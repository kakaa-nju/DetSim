#include "process_state.h"
#include "monitor.h"
#include "guest.h"
#include "net/emu.h"
#include "engine/thread_pool.h"
#include "engine/thread_control.h"
#include "cereal/archives/binary.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <limits>
#include <array>
#include <optional>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <sys/random.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>
#include <vector>

namespace {
constexpr uint64_t action_flags=0xdc000007ULL;
constexpr uint64_t unmaskable=(1ULL<<(SIGKILL-1))|(1ULL<<(SIGSTOP-1));
bool valid_action(int signal,const KernelSigaction& action) {
    return signal>=1 && signal<=64 && signal!=SIGKILL && signal!=SIGSTOP &&
        !(action.flags&~action_flags) && !(action.mask&unmaskable) && action!=KernelSigaction{};
}
void validate_clock(int clock) {
    switch(clock) {
    case CLOCK_REALTIME:case CLOCK_MONOTONIC:case CLOCK_REALTIME_COARSE:case CLOCK_MONOTONIC_COARSE:return;
    default:throw std::runtime_error("Unsupported modeled clock ID");
    }
}
bool valid_timer(const timeval& value) {
    return value.tv_sec>=0 && value.tv_usec>=0 && value.tv_usec<1000000;
}
uint64_t random_word(uint64_t& state) {
    state^=state>>12;state^=state<<25;state^=state>>27;
    return state*2685821657736338717ULL;
}
}

template<class Archive> void ProcessKernelState::serialize(Archive& ar) {
    uint64_t seed=random_state;
    uint32_t count=signal_actions.size();
    ar(seed,count);
    if(!seed || count>62) throw std::runtime_error("Invalid process kernel state archive");
    std::map<int,KernelSigaction> actions;
    if constexpr(Archive::is_loading::value) {
        for(uint32_t i=0;i<count;++i) {
            int signal;KernelSigaction action;
            ar(signal,action.handler,action.flags,action.restorer,action.mask);
            if(!valid_action(signal,action) || !actions.emplace(signal,action).second)
                throw std::runtime_error("Invalid signal action archive");
        }
    } else {
        for(const auto& [signal,action]:signal_actions) {
            if(!valid_action(signal,action)) throw std::runtime_error("Invalid signal action state");
            ar(signal,action.handler,action.flags,action.restorer,action.mask);
        }
    }
    auto limits=resource_limits;
    for(auto& limit:limits) {
        ar(limit.current,limit.maximum);
        if(limit.current>limit.maximum) throw std::runtime_error("Invalid captured resource limit");
    }
    uint32_t count_caps=capability_count;
    uint64_t mask_caps=capability_mask;
    ar(count_caps,mask_caps);
    if(count_caps>64 || (count_caps<64 && (mask_caps>>count_caps)))
        throw std::runtime_error("Invalid captured capability boundary");
    if constexpr(Archive::is_loading::value) {
        random_state=seed;signal_actions=std::move(actions);resource_limits=limits;
        capability_count=count_caps;capability_mask=mask_caps;
    }
}
template void ProcessKernelState::serialize(cereal::BinaryInputArchive&);
template void ProcessKernelState::serialize(cereal::BinaryOutputArchive&);

std::array<char,16> capture_native_thread_comm(int pid) {
    // Initial low-level adoption precedes the stop barrier: never inject here.
    std::ifstream input("/proc/"+std::to_string(pid)+"/comm",std::ios::binary);
    if(!input) throw std::runtime_error("Cannot read initial native thread name");
    std::string bytes((std::istreambuf_iterator<char>(input)),{});
    if(input.bad() || bytes.empty() || bytes.back()!='\n')
        throw std::runtime_error("Invalid initial native thread name");
    bytes.pop_back(); // Preserve any newlines inside the actual name.
    if(bytes.size()>15 || bytes.find('\0')!=std::string::npos)
        throw std::runtime_error("Invalid initial native thread name bytes");
    std::array<char,16> name{};
    std::copy(bytes.begin(),bytes.end(),name.begin());
    return name;
}

void capture_process_kernel_state(int pid,int node) {
    if(node<0 || node>=NP) throw std::runtime_error("Invalid kernel process capture node");
    size_t threads=0;
    for(const auto& entry:std::filesystem::directory_iterator("/proc/"+std::to_string(pid)+"/task")) {
        (void)entry;
        if(++threads>1) throw std::runtime_error("Already multithreaded startup boundaries are unsupported");
    }
    if(threads!=1) throw std::runtime_error("Cannot inspect startup process thread set");
    adopt_thread_signal_mask(node,pid);
    ProcessKernelState captured;
    std::array<char,16> captured_comm{};
    uint32_t inherited_altstack_flags=SS_DISABLE;
    const uintptr_t address=scratch_page+0x800;
    std::array<unsigned char,128> original{};
    if(!guest_read_exact(pid,reinterpret_cast<void*>(address),original.data(),original.size()))
        throw std::runtime_error("Cannot preserve kernel capture scratch bytes");
    auto query=[&](int nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
        const long result=tracee_do_syscall(pid,nr,a,b,c,d,0,0);
        if(result<0) throw std::runtime_error("Cannot query inherited process kernel state");
    };
    auto read=[&](auto& value) {
        if(!guest_read_exact(pid,reinterpret_cast<void*>(address),&value,sizeof(value)))
            throw std::runtime_error("Cannot read inherited process kernel state");
    };
    try {
        // Trusted prelude syscalls may have renamed the physical task since
        // early adoption. Publish its final boundary name only on full success.
        query(SYS_prctl,PR_GET_NAME,address,0,0);read(captured_comm);
        const auto end=std::find(captured_comm.begin(),captured_comm.end(),'\0');
        if(end==captured_comm.end()) throw std::runtime_error("Unterminated bootstrap thread name");
        std::fill(end,captured_comm.end(),'\0');
        for(unsigned resource=0;resource<captured.resource_limits.size();++resource) {
            query(SYS_prlimit64,0,resource,0,address);
            auto& limit=captured.resource_limits[resource];read(limit);
            if(limit.current>limit.maximum) throw std::runtime_error("Invalid inherited resource limit");
        }
        for(unsigned capability=0;capability<=64;++capability) {
            const long result=tracee_do_syscall(pid,SYS_prctl,PR_CAPBSET_READ,capability,0,0,0,0);
            if(result==-EINVAL) {captured.capability_count=capability;break;}
            if(result<0 || result>1 || capability==64)
                throw std::runtime_error("Unsupported inherited capability boundary");
            captured.capability_mask|=uint64_t(result)<<capability;
        }
        if(!captured.capability_count) throw std::runtime_error("Cannot capture supported capability range");
        for(int signal=1;signal<=64;++signal) {
            query(SYS_rt_sigaction,signal,0,address,8);
            KernelSigaction action{};read(action);
            if(action!=KernelSigaction{}) {
                if(!valid_action(signal,action)) throw std::runtime_error("Unsupported inherited signal disposition");
                captured.signal_actions.emplace(signal,action);
            }
        }
        uint64_t mask=0;
        query(SYS_rt_sigprocmask,SIG_SETMASK,0,address,8);read(mask);
        if(mask!=ptmc_state.running_state.child[node].threads.front().signal_mask)
            throw std::runtime_error("Bootstrap signal mask changed during capture");
        query(SYS_rt_sigpending,address,8,0,0);read(mask);
        if(mask) throw std::runtime_error("Inherited pending signals are unsupported");
        for(int timer=ITIMER_REAL;timer<=ITIMER_PROF;++timer) {
            query(SYS_getitimer,timer,address,0,0);
            itimerval value{};read(value);
            if(value.it_interval.tv_sec || value.it_interval.tv_usec || value.it_value.tv_sec || value.it_value.tv_usec)
                throw std::runtime_error("Inherited interval timers are unsupported");
        }
        query(SYS_sigaltstack,0,address,0,0);
        stack_t stack{};read(stack);
        if(!(stack.ss_flags&SS_DISABLE)) throw std::runtime_error("Inherited alternate signal stacks are unsupported");
        inherited_altstack_flags=SS_DISABLE | (static_cast<uint32_t>(stack.ss_flags)&0x80000000u);
    } catch(...) {
        if(!guest_write_exact(pid,reinterpret_cast<void*>(address),original.data(),original.size()))
            throw std::runtime_error("Cannot restore kernel capture scratch bytes after failure");
        throw;
    }
    if(!guest_write_exact(pid,reinterpret_cast<void*>(address),original.data(),original.size()))
        throw std::runtime_error("Cannot restore kernel capture scratch bytes");
    std::ifstream status("/proc/"+std::to_string(pid)+"/status");
    std::string line;unsigned creation_mask=0;bool found=false;
    while(std::getline(status,line)) {
        if(line.starts_with("Umask:")) {
            std::istringstream input(line.substr(6));input>>std::oct>>creation_mask;
            if(!input || creation_mask>0777) throw std::runtime_error("Invalid inherited process umask");
            found=true;
        }
    }
    if(!found) throw std::runtime_error("Cannot read inherited process umask");
    require_no_pending_signals(pid);
    auto& task=ptmc_state.running_state.child[node].threads.front();
    task.altstack_sp=task.altstack_size=0;
    task.altstack_flags=inherited_altstack_flags;
    task.comm=captured_comm;
    ptmc_state.process_states[node]=std::move(captured);
    ptmc_state.fs_states[node].set_creation_mask(creation_mask);
}

/* ==============================================================================
 * Process kernel models
 *
 * Each modelled syscall owns its own function, and the dispatcher at the bottom
 * of this block only selects the model. Value-class syscalls additionally look
 * for a configured ChooseFunc inside their own emulation (the
 * *_with_choice() variants), exactly like SockState::do_recvfrom_with_choice():
 * the branch the user plugin materializes replaces the whole result, so no
 * part of the default model runs on the chosen path.
 * ==============================================================================
 */

namespace {

/* What one process-model syscall needs to touch the guest and the model clock. */
struct emu_context {
    int node;
    pid_t pid;
    ProcessKernelState& state;
    const timeval& now;

    bool write(uint64_t address, const auto& value) const {
        return guest_write_exact(pid, reinterpret_cast<void*>(address), &value, sizeof(value));
    }
    bool read(uint64_t address, auto& value) const {
        return guest_read_exact(pid, reinterpret_cast<const void*>(address), &value, sizeof(value));
    }
};

emu_context current_emu_context() {
    const int node=ptmc_state.cursor;
    return emu_context{node, ptmc_state.get_current_tid(node),
                       ptmc_state.process_states[node], ptmc_state.time[node]};
}

/* Branch index of the running choice point; it is only consumed while
 * ptmc_state.n_choose says a choice point is active. */
int active_choice() {
    return ptmc_state.choose;
}

/* Ask the configured choose_<syscall> plugin for the selected outcome. Returns
 * nothing when no branch applies, so the caller runs its default model. */
std::optional<long> emu_choose_value(const emu_context& ctx, const syscall_info& info, int choice) {
    if (ptmc_state.n_choose <= 0 || !choose_syswhat[info.nr]) return std::nullopt;
    uint64_t args[6];
    for (int i = 0; i < 6; ++i)
        args[i] = static_cast<uint64_t>(info.args[i]);
    choose_in in(info.nr, ctx.pid, ctx.node, args, ctx.now);
    choose_out *out = choose_syswhat[info.nr](ctx.pid, choice, in);
    if (!out) return std::nullopt;
    const long result = emu_apply_choose_out(ctx.pid, info, *out);
    delete out;
    return result;
}

long emu_prctl(const emu_context& ctx, const syscall_info& info) {
    const uint32_t option=info.args[0];
    if(option!=PR_CAPBSET_READ && option!=PR_SET_NAME && option!=PR_GET_NAME)
        throw std::runtime_error("Unsupported modeled prctl option");
    if(option==PR_CAPBSET_READ) {
        if(!ctx.state.capability_count) throw std::runtime_error("Capability boundary has not been captured");
        const uint64_t capability=info.args[1];
        return capability>=ctx.state.capability_count ? -EINVAL : (ctx.state.capability_mask>>capability)&1;
    }
    auto& threads=ptmc_state.running_state.child[ctx.node].threads;
    const auto found=std::find_if(threads.begin(),threads.end(),[&ctx](const thread_state& thread) {
        return thread.alive && thread.physical_tid==ctx.pid;
    });
    if(found==threads.end()) throw std::runtime_error("Thread name has no live logical owner");
    const uint64_t address=info.args[1];
    const size_t size=option==PR_GET_NAME?16:15;
    if(address<scratch_page+4096 && address>scratch_page-size)
        throw std::runtime_error("Thread name buffer overlaps reserved syscall scratch page");
    if(option==PR_GET_NAME) return ctx.write(address,found->comm)?0:-EFAULT;
    std::array<char,16> name{};
    for(unsigned offset=0;offset<15;++offset) {
        if(address>UINT64_MAX-offset || !ctx.read(address+offset,name[offset])) return -EFAULT;
        if(!name[offset]) break;
    }
    found->comm=name;
    return 0;
}

/* getrlimit and prlimit64 share one table; only their argument layout differs. */
long emu_resource_limit(const emu_context& ctx, const syscall_info& info, bool extended) {
    if(extended && (info.args[2] || (static_cast<int32_t>(info.args[0])!=0 &&
        static_cast<int32_t>(info.args[0])!=logical_process_id(ctx.node))))
        throw std::runtime_error("Only owned-process resource limit queries are supported");
    const uint32_t resource=info.args[extended?1:0];
    if(resource>=ctx.state.resource_limits.size()) return -EINVAL;
    const uint64_t destination=info.args[extended?3:1];
    if(extended && !destination) return 0;
    if(destination<scratch_page+4096 && destination>scratch_page-sizeof(KernelResourceLimit))
        throw std::runtime_error("Resource limit output overlaps reserved syscall scratch page");
    return ctx.write(destination,ctx.state.resource_limits[resource])?0:-EFAULT;
}

long emu_getrlimit(const emu_context& ctx, const syscall_info& info) {
    return emu_resource_limit(ctx,info,false);
}

long emu_prlimit64(const emu_context& ctx, const syscall_info& info) {
    return emu_resource_limit(ctx,info,true);
}

long emu_clock_gettime_with_choice(const emu_context& ctx, const syscall_info& info, int choice) {
    if(auto chosen=emu_choose_value(ctx,info,choice)) return *chosen;
    validate_clock(static_cast<int>(info.args[0]));
    const timespec value=info.nr==SYS_clock_getres ? timespec{0,1000}
                                                  : timespec{ctx.now.tv_sec,ctx.now.tv_usec*1000};
    if(info.nr==SYS_clock_getres && !info.args[1]) return 0;
    return ctx.write(info.args[1],value)?0:-EFAULT;
}

long emu_time(const emu_context& ctx, const syscall_info& info) {
    const time_t value=ctx.now.tv_sec;
    return !info.args[0] || ctx.write(info.args[0],value)?value:-EFAULT;
}

long emu_getrandom_with_choice(const emu_context& ctx, const syscall_info& info, int choice) {
    if(auto chosen=emu_choose_value(ctx,info,choice)) return *chosen;
    const unsigned flags=info.args[2];
    if((flags&~(GRND_NONBLOCK|GRND_RANDOM|GRND_INSECURE)) ||
       ((flags&GRND_RANDOM) && (flags&GRND_INSECURE))) return -EINVAL;
    // Linux permits partial results; bound staging memory per transition.
    const size_t count=std::min<uint64_t>(info.args[1],256*1024);
    if(!count) return 0;
    std::vector<unsigned char> bytes(count);
    uint64_t seed=ctx.state.random_state;
    for(size_t i=0;i<count;) {
        const uint64_t word=random_word(seed);
        for(unsigned j=0;j<8 && i<count;++j,++i) bytes[i]=word>>(j*8);
    }
    if(!guest_write_exact(ctx.pid,reinterpret_cast<void*>(info.args[0]),bytes.data(),bytes.size()))
        return -EFAULT;
    ctx.state.random_state=seed;
    return count;
}

/* True when the affinity target names the calling actor or a live logical thread. */
bool emu_affinity_target_alive(const emu_context& ctx, int32_t target) {
    for(int node=0;node<NP;++node)
        for(const auto& thread:ptmc_state.running_state.child[node].threads)
            if(thread.alive && ((target==0 && node==ctx.node && thread.physical_tid==ctx.pid) ||
                (target>0 && logical_thread_id(node,thread.tid)==target)))
                return true;
    return false;
}

long emu_sched_getaffinity_with_choice(const emu_context& ctx, const syscall_info& info, int choice) {
    if(auto chosen=emu_choose_value(ctx,info,choice)) return *chosen;
    // Fixed x86-64 machine profile: one available CPU, one kernel mask word.
    // Linux accepts unsigned-int len (including its bit-count wrap), checks
    // word alignment before PID lookup, and returns bytes copied, not zero.
    const uint32_t length=info.args[1];
    if(uint32_t(length*8U)<1 || length%sizeof(uint64_t)) return -EINVAL;
    if(!emu_affinity_target_alive(ctx,info.args[0])) return -ESRCH;
    const uint64_t mask=1;
    return ctx.write(info.args[2],mask)?long(sizeof(mask)):-EFAULT;
}

long emu_sysinfo_with_choice(const emu_context& ctx, const syscall_info& info, int choice) {
    if(auto chosen=emu_choose_value(ctx,info,choice)) return *chosen;
    struct sysinfo value{};
    value.uptime=ctx.now.tv_sec;value.totalram=1ULL<<30;value.freeram=1ULL<<29;
    value.procs=NP;value.mem_unit=1;
    return ctx.write(info.args[0],value)?0:-EFAULT;
}

long emu_rt_sigaction(const emu_context& ctx, const syscall_info& info) {
    const int signal=info.args[0];
    if(signal<1 || signal>64 || info.args[3]!=8) return -EINVAL;
    KernelSigaction action{};
    if(info.args[1]) {
        if(signal==SIGKILL || signal==SIGSTOP) return -EINVAL;
        if(!ctx.read(info.args[1],action)) return -EFAULT;
        if(action.flags&~action_flags) throw std::runtime_error("Unsupported signal action flags");
        action.mask&=~unmaskable;
    }
    const auto found=ctx.state.signal_actions.find(signal);
    const KernelSigaction old=found==ctx.state.signal_actions.end()?KernelSigaction{}:found->second;
    if(info.args[2] && !ctx.write(info.args[2],old)) return -EFAULT;
    if(info.args[1]) {
        if(action==KernelSigaction{}) ctx.state.signal_actions.erase(signal);
        else ctx.state.signal_actions[signal]=action;
    }
    return 0;
}

long emu_getitimer(const emu_context& ctx, const syscall_info& info) {
    if(info.args[0]>ITIMER_PROF) return -EINVAL;
    const itimerval value{};
    return ctx.write(info.args[1],value)?0:-EFAULT;
}

long emu_setitimer(const emu_context& ctx, const syscall_info& info) {
    if(info.args[0]>ITIMER_PROF) return -EINVAL;
    itimerval value{};
    if(info.args[1] && !ctx.read(info.args[1],value)) return -EFAULT;
    if(!valid_timer(value.it_interval) || !valid_timer(value.it_value)) return -EINVAL;
    if(value.it_interval.tv_sec || value.it_interval.tv_usec || value.it_value.tv_sec || value.it_value.tv_usec)
        throw std::runtime_error("Armed interval timers are unsupported");
    return !info.args[2] || ctx.write(info.args[2],value)?0:-EFAULT;
}

long emu_umask(const emu_context& ctx, const syscall_info& info) {
    auto& fs=ptmc_state.fs_states[ctx.node];
    const auto previous=fs.creation_mask();
    fs.set_creation_mask(info.args[0]);
    return previous;
}

} // namespace

long emu_process_syscall(const syscall_info& info) {
    const emu_context ctx=current_emu_context();
    const int choice=active_choice();
    switch(info.nr) {
    case SYS_prctl: return emu_prctl(ctx,info);
    case SYS_getrlimit: return emu_getrlimit(ctx,info);
    case SYS_prlimit64: return emu_prlimit64(ctx,info);
    case SYS_clock_gettime: case SYS_clock_getres:
        return emu_clock_gettime_with_choice(ctx,info,choice);
    case SYS_time: return emu_time(ctx,info);
    case SYS_getrandom: return emu_getrandom_with_choice(ctx,info,choice);
    case SYS_sched_getaffinity: return emu_sched_getaffinity_with_choice(ctx,info,choice);
    case SYS_sysinfo: return emu_sysinfo_with_choice(ctx,info,choice);
    case SYS_rt_sigaction: return emu_rt_sigaction(ctx,info);
    case SYS_setitimer: return emu_setitimer(ctx,info);
    case SYS_getitimer: return emu_getitimer(ctx,info);
    case SYS_umask: return emu_umask(ctx,info);
    default: throw std::runtime_error("Unknown process kernel model syscall");
    }
}
