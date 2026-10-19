#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "parallel.h"
#include "protocol.h"
#include "object_transport.h"
#include "compatibility.h"
#include "worker.h"
#include "config.h"
#include "monitor.h"
#include "runtime_profile.h"
#include "state/sysstate_store.h"
#include "state/state_store.h"
#include "state/shared_object_pool.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern int sigint_received;
namespace detsim::search {
namespace {
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;
namespace fs = std::filesystem;
using ActionKey = std::tuple<int,int,int>;
void check_interrupted() {
    if (sigint_received) { sigint_received = 0; throw std::runtime_error("Parallel exploration interrupted"); }
}
ActionKey action_key(const ActorAction& a) { return {a.process,a.thread,static_cast<int>(a.action)}; }
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
void checked(int error, const char* operation) {
    if (error) throw std::runtime_error(std::string(operation) + ": " + strerror(error));
}
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    int release() { int result = value; value = -1; return result; }
};
std::set<pid_t> direct_children() {
    std::ifstream input("/proc/self/task/" + std::to_string(getpid()) + "/children");
    require(bool(input), "Cannot enumerate coordinator child ownership");
    std::set<pid_t> result; pid_t child;
    while (input >> child) result.insert(child);
    return result;
}
struct Subreaper {
    int previous = 0;
    std::set<pid_t> baseline = direct_children();
    Subreaper() {
        if (prctl(PR_GET_CHILD_SUBREAPER, &previous) || prctl(PR_SET_CHILD_SUBREAPER, 1))
            throw std::runtime_error("Cannot establish owned worker subreaper");
    }
    // run_parallel is synchronous: all children created during this scope are
    // its workers or their adopted descendants. Original tracees are baseline.
    // This also covers worker death before its descendants were first observed.
    void cleanup() {
        for (;;) {
            bool found = false;
            for (pid_t child : direct_children()) {
                if (baseline.count(child)) continue;
                Fd fd(syscall(SYS_pidfd_open, child, 0));
                if (fd.value < 0) { require(errno == ESRCH, "Cannot bind adopted worker descendant"); continue; }
                found = true;
                if (syscall(SYS_pidfd_send_signal, fd.value, SIGKILL, nullptr, 0))
                    require(errno == ESRCH, "Cannot terminate adopted worker descendant");
                siginfo_t info{};
                int result;
                do { result = waitid(P_PIDFD, fd.value, &info, WEXITED); } while (result < 0 && errno == EINTR);
                require(result == 0 || errno == ECHILD, "Cannot reap adopted worker descendant");
            }
            if (!found) return;
        }
    }
    ~Subreaper() {
        try { cleanup(); } catch (...) {}
        prctl(PR_SET_CHILD_SUBREAPER, previous);
    }
};
struct Worker {
    WorkerId id;
    int host_cpu = -1;
    pid_t pid = -1;
    unsigned incarnation = 0, startup_failures = 0;
    std::unique_ptr<wire::Channel> channel;
    bool hello = false;
    std::optional<Ticket> ticket;
    Clock::time_point deadline;
    std::set<ActionKey> actors;
    std::map<ActionKey,std::set<int>> choices;
    std::map<ActionKey,uint32_t> choice_counts;
    std::vector<StateKey> successors;
    std::optional<StateKey> violation;
    int source_budget = 0;
    bool source_violation = false;
    std::map<pid_t,int> descendants;
    explicit Worker(WorkerId worker) : id(worker) {}
    Worker(const Worker&) = delete;
    ~Worker() { terminate(); }
    void observe_descendants() {
        if (pid <= 0) return;
        std::vector<pid_t> pending{pid};
        std::set<pid_t> seen;
        while (!pending.empty()) {
            auto parent = pending.back(); pending.pop_back();
            if (!seen.insert(parent).second) continue;
            std::ifstream children("/proc/" + std::to_string(parent) + "/task/" + std::to_string(parent) + "/children");
            pid_t child;
            while (children >> child) {
                if (!descendants.count(child)) {
                    const int fd = syscall(SYS_pidfd_open, child, 0);
                    if (fd >= 0) descendants.emplace(child, fd);
                }
                pending.push_back(child);
            }
        }
    }
    void terminate() noexcept {
        // Final inventory is unconditional. Subreaper cleanup also owns any
        // descendants orphaned before an observation could see them.
        try { observe_descendants(); } catch (...) {}
        channel.reset();
        if (pid > 0) kill(-pid, SIGKILL);
        for (const auto& [child, fd] : descendants) {
            (void)child;
            syscall(SYS_pidfd_send_signal, fd, SIGKILL, nullptr, 0);
        }
        if (pid > 0) {
            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            pid = -1;
        }
        // Descendant leaders are now adopted. P_PIDFD prevents accidental
        // consumption of unrelated tracee stops or recycled numeric PIDs.
        for (const auto& [child, fd] : descendants) {
            (void)child;
            siginfo_t info{};
            while (waitid(P_PIDFD, fd, &info, WEXITED) < 0 && errno == EINTR) {}
            close(fd);
        }
        descendants.clear();
    }
    void clear_job() {
        ticket.reset(); actors.clear(); choices.clear(); choice_counts.clear(); successors.clear(); violation.reset();
    }
};
class Journal {
    std::ofstream output_;
public:
    explicit Journal(const fs::path& path) : output_(path) { require(bool(output_), "Cannot create parallel journal"); }
    void write(json record) {
        output_ << record.dump() << '\n'; output_.flush();
        require(bool(output_), "Cannot persist parallel journal");
    }
};
void spawn(Worker& worker, const fs::path& base, const fs::path& executable,
           const fs::path& configuration, int timeout_ms, Journal& journal, const std::shared_ptr<wire::ObjectCache>& cache,
           const std::string& shared_pool_path) {
    if (worker.host_cpu >= 0) require_bootstrap_cpu_affinity();
    const auto directory = base / ("worker-" + std::to_string(worker.id) + "-" + std::to_string(++worker.incarnation));
    fs::create_directory(directory);
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) throw std::runtime_error("Worker socketpair failed");
    Fd original_parent(sockets[0]), original_child(sockets[1]);
    Fd parent(fcntl(sockets[0], F_DUPFD_CLOEXEC, 5)), child(fcntl(sockets[1], F_DUPFD_CLOEXEC, 5));
    int objects[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, objects)) throw std::runtime_error("Worker object socketpair failed");
    Fd object_parent(objects[0]), original_object_child(objects[1]);
    Fd object_child(fcntl(objects[1], F_DUPFD_CLOEXEC, 5));
    require(parent.value >= 0 && child.value >= 0 && object_child.value >= 0, "Worker socket duplication failed");
    posix_spawn_file_actions_t actions;
    checked(posix_spawn_file_actions_init(&actions), "spawn actions init");
    posix_spawnattr_t attrs;
    const int attr_error = posix_spawnattr_init(&attrs);
    if (attr_error) { posix_spawn_file_actions_destroy(&actions); checked(attr_error, "spawn attr init"); }
    try {
        checked(posix_spawn_file_actions_addchdir_np(&actions, directory.c_str()), "spawn chdir");
        checked(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0), "spawn stdin");
        const auto log = directory / "worker.log";
        checked(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0600), "spawn stdout");
        checked(posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO), "spawn stderr");
        checked(posix_spawn_file_actions_adddup2(&actions, child.value, 3), "spawn socket");
        checked(posix_spawn_file_actions_adddup2(&actions, object_child.value, 4), "spawn object socket");
        checked(posix_spawn_file_actions_addclosefrom_np(&actions, 5), "spawn close inherited fds");
        checked(posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP), "spawn flags");
        checked(posix_spawnattr_setpgroup(&attrs, 0), "spawn group");
        std::string exe = executable.string(), config = configuration.string(), target_cwd = tracee_native_cwd.empty() ? fs::current_path().string() : tracee_native_cwd;
        std::string io_timeout = std::to_string(timeout_ms), host_cpu = std::to_string(worker.host_cpu);
        std::vector<char*> args = {exe.data(), const_cast<char*>("--worker-fd"), const_cast<char*>("3"),
                        const_cast<char*>("--worker-blob-fd"), const_cast<char*>("4"),
                        const_cast<char*>("-n"), const_cast<char*>("-c"), config.data(),
                        const_cast<char*>("--target-cwd"), target_cwd.data(),
                        const_cast<char*>("--worker-io-timeout-ms"), io_timeout.data()};
        if (!shared_pool_path.empty()) {
            args.push_back(const_cast<char*>("--worker-shared-pool"));
            args.push_back(const_cast<char*>(shared_pool_path.c_str()));
        }
        if (worker.host_cpu >= 0) {
            args.push_back(const_cast<char*>("--worker-host-cpu"));
            args.push_back(host_cpu.data());
        }
        args.push_back(nullptr);
        std::vector<char*> environment;
        for (const auto& entry : bootstrap_environment()) environment.push_back(const_cast<char*>(entry.c_str()));
        environment.push_back(nullptr);
        checked(posix_spawn(&worker.pid, exe.c_str(), &actions, &attrs, args.data(), environment.data()), "spawn worker");
    } catch (...) {
        posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attrs); throw;
    }
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attrs);
    wire::Limits limits; limits.io_timeout_ms = timeout_ms;
    worker.channel = std::make_unique<wire::Channel>(parent.release(), limits, object_parent.release(), cache);
    worker.hello = false; worker.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    journal.write({{"event","worker_spawn"},{"worker",worker.id},{"pid",worker.pid},{"incarnation",worker.incarnation},{"directory",directory.string()},{"host_cpu",worker.host_cpu}});
}
void transport_report(Worker& worker, Journal& journal) {
    if (!worker.channel) return;
    const auto wire = worker.channel->counters();
    const auto objects = worker.channel->object_statistics();
    journal.write({{"event","transport"},{"worker",worker.id},{"incarnation",worker.incarnation},
        {"stream_send_calls",wire.send_calls},{"stream_recv_calls",wire.recv_calls},{"stream_poll_calls",wire.poll_calls},
        {"stream_bytes_sent",wire.bytes_sent},{"stream_bytes_received",wire.bytes_received},
        {"shared_frames",wire.shared_frames},{"raw_fallback_frames",wire.raw_fallback_frames},{"raw_fallback_bytes",wire.raw_fallback_bytes},
        {"object_send_calls",objects.send_calls},{"object_recv_calls",objects.recv_calls},{"object_poll_calls",objects.poll_calls},
        {"memfds_created",objects.memfds_created},{"memfd_bytes_created",objects.memfd_bytes_created},
        {"segments_received",objects.segments_received},{"reference_bytes",objects.reference_bytes},{"missing_bytes",objects.missing_bytes},
        {"peak_inflight_bytes",objects.peak_inflight_bytes},{"cache_bytes",objects.cache_bytes},
        {"cache_segments",objects.cache_segments},{"peak_cache_bytes",objects.peak_cache_bytes}});
}
const char* phase_name(NodePhase phase) {
    switch (phase) {
    case NodePhase::Pending:return "pending"; case NodePhase::Inflight:return "inflight";
    case NodePhase::Expanded:return "expanded"; case NodePhase::Terminal:return "terminal";
    case NodePhase::DepthCut:return "depth_cut"; case NodePhase::ModelBudget:return "model_budget";
    case NodePhase::Failure:return "failure";
    }
    return "invalid";
}
}
Report run_parallel(const sys_state& root, const Options& options) {
    Report report;
    const auto started = Clock::now();
    std::unique_ptr<Frontier> frontier;
    std::unique_ptr<Journal> journal;
    auto object_cache = std::make_shared<wire::ObjectCache>();
    std::shared_ptr<snapshot::SharedObjectPool> pool;
    std::string shared_pool_path;
    try {
        check_interrupted();
        std::vector<int> placement_cpus;
        if (options.worker_placement == Options::WorkerPlacement::SameCpu) {
            require_bootstrap_cpu_affinity();
            placement_cpus = worker_cpu_order(bootstrap_cpu_affinity());
        } else require(options.worker_placement == Options::WorkerPlacement::None, "Invalid worker placement");
        require(options.workers > 0 && options.workers <= 32 && options.timeout_ms > 0, "Invalid parallel worker count or timeout");
        require(runtime_profile().fixed_layout, "Parallel exploration requires RuntimeProfile.fixed_layout=true");
        require(!root.model_identity.empty() && root.model_identity == ptmc_state.model_identity,
                "Parallel root model differs from initialized coordinator");
        require(root.ss_hash && root.identity_hash() == root.ss_hash, "Invalid parallel root identity");
        const auto compatibility = compatibility_manifest();
        const auto configuration = fs::absolute(cfg_file ? cfg_file : "config.json");
        std::ifstream config_stream(configuration); json config; config_stream >> config;
        if (config.contains("StateStore") && config["StateStore"].contains("packed_storage_path")) {
            const fs::path storage = config["StateStore"]["packed_storage_path"].get<std::string>();
            require(!storage.empty() && !storage.is_absolute(), "Parallel worker storage path must be relative");
            for (const auto& part : storage) require(part != "..", "Parallel worker storage path escapes its directory");
        }
        std::string temporary = (fs::current_path() / ".parallel-XXXXXX").string();
        require(mkdtemp(temporary.data()) != nullptr, "Cannot create parallel artifact directory");
        report.artifacts = temporary;
        const fs::path base(temporary), executable = base / "tracer";
        fs::copy_file("/proc/self/exe", executable);
        fs::permissions(executable, fs::perms::owner_read | fs::perms::owner_exec);
        journal = std::make_unique<Journal>(base / "journal.jsonl");
        journal->write({{"event","bootstrap_environment"},{"changed_names",environment_change_names()}});
        frontier = std::make_unique<Frontier>(options.depth, options.policy, options.priority);
        // Validate/export once before launching; per-task transport reads saved
        // state, never coordinator live memory or current UI bindings.
        if (options.storage == Options::Storage::Shared) {
            auto& store = StateStore::instance();
            shared_pool_path = fs::absolute(fs::path(store.storage_path()) / "shared-objects").lexically_normal().string();
            pool = std::make_shared<snapshot::SharedObjectPool>(shared_pool_path, true);
            store.attach_shared_pool(shared_pool_path, false);
            snapshot::publish_shared_state(root.ss_hash, *pool);
        } else (void)snapshot::export_bundle(root.ss_hash);
        frontier->admit(root.ss_hash);
        Subreaper subreaper;
        std::vector<std::unique_ptr<Worker>> workers;
        std::map<StateKey,unsigned> failures;
        using EdgeKey = std::tuple<StateKey,int,int,int,int>;
        std::map<EdgeKey,std::tuple<StateKey,int,uint32_t>> verified_edges;
        for (unsigned index = 0; index < options.workers; ++index) {
            auto worker = std::make_unique<Worker>(index + 1);
            if (!placement_cpus.empty()) worker->host_cpu = placement_cpus[index % placement_cpus.size()];
            workers.push_back(std::move(worker));
        }
        auto lost = [&](Worker& worker, const std::string& reason) {
            check_interrupted();
            journal->write({{"event","worker_lost"},{"worker",worker.id},{"pid",worker.pid},
                           {"lease",worker.ticket ? worker.ticket->lease_id : 0},{"reason",reason}});
            bool exhausted = false;
            if (worker.ticket) {
                exhausted = ++failures[worker.ticket->state] > options.retries;
                if (exhausted) frontier->commit(*worker.ticket, {ResultKind::Failure,{},reason});
                else frontier->worker_lost(worker.id);
            } else exhausted = ++worker.startup_failures > options.retries;
            transport_report(worker, *journal);
            worker.terminate(); worker.clear_job();
            if (exhausted) throw std::runtime_error("Parallel retry budget exhausted: " + reason);
            ++report.retries;
        };
        bool violation = false;
        size_t next_worker = 0;
        while (frontier->status() == Status::Running && !violation) {
            check_interrupted();
            bool progressed = false;
            const size_t dispatch_start = next_worker;
            for (size_t scan = 0; scan < workers.size(); ++scan) {
                const size_t index = (dispatch_start + scan) % workers.size();
                auto& owned = workers[index];
                Worker& worker = *owned;
                check_interrupted();
                if (!worker.channel) {
                    try { spawn(worker, base, executable, configuration, options.timeout_ms, *journal, object_cache, shared_pool_path); }
                    catch (const std::exception& error) { lost(worker,error.what()); continue; }
                }
                if (worker.hello && !worker.ticket) {
                    if (auto ticket = frontier->lease(worker.id)) {
                        worker.ticket = ticket;
                        auto ready = pool ? snapshot::load_shared_state(ticket->state, root.model_identity, *pool)
                                          : snapshot::ReadyState{sys_state(ticket->state), {}};
                        const auto& source = ready.state;
                        if (!pool) ready.actors = enabled_actor_actions(source);
                        worker.source_budget = source.error_bound; worker.source_violation = source.property_failed;
                        for (const auto& actor : ready.actors) worker.actors.insert(action_key(actor));
                        journal->write({{"event","lease"},{"worker",worker.id},{"lease",ticket->lease_id},
                                       {"state",ticket->state},{"depth",ticket->depth},{"may_expand",ticket->may_expand}});
                        next_worker = (index + 1) % workers.size();
                        const auto kind = classify_enumeration(source, ready.actors, ticket->may_expand);
                        if (pool && kind != EnumerationKind::Expanded) {
                            if (kind == EnumerationKind::Violation) {
                                report.violation_state = ticket->state;
                                report.property_violation = violation = true;
                            } else {
                                Expansion expansion;
                                switch (kind) {
                                case EnumerationKind::Terminal: expansion.kind = ResultKind::Terminal; break;
                                case EnumerationKind::DepthCut: expansion.kind = ResultKind::DepthCut; break;
                                case EnumerationKind::ModelBudget:
                                    expansion.kind = ResultKind::ModelBudget;
                                    expansion.reason = "Saved model budget exhausted";
                                    break;
                                default: throw std::runtime_error("Invalid local completion kind");
                                }
                                frontier->commit(*ticket, std::move(expansion));
                            }
                            journal->write({{"event","done"},{"worker",worker.id},{"lease",ticket->lease_id},
                                {"source",ticket->state},{"kind",static_cast<int>(completion_kind(kind))},
                                {"emitted",0},{"local",true}});
                            ++report.local_completions;
                            worker.clear_job();
                            progressed = true;
                            if (violation) break;
                            continue;
                        }
                        auto bundle = pool ? snapshot::export_shared_bundle(ticket->state, *pool)
                                           : snapshot::export_bundle(ticket->state);
                        check_interrupted();
                        try { worker.channel->send(wire::Task{*ticket,std::move(bundle)}); }
                        catch (const std::exception& error) { lost(worker,error.what()); continue; }
                        check_interrupted();
                        worker.deadline = Clock::now() + std::chrono::milliseconds(options.timeout_ms);
                        progressed = true;
                        // Drain ready result streams between large task uploads;
                        // never queue 32 synchronous closure sends in one pass.
                        break;
                    }
                }
            }
            if (violation) break;
            std::vector<pollfd> fds; std::vector<Worker*> active;
            for (auto& worker : workers) if (worker->channel) {
                fds.push_back({worker->channel->fd(), POLLIN, 0}); active.push_back(worker.get());
            }
            const bool buffered = std::any_of(active.begin(), active.end(),
                [](const Worker* worker) { return worker->channel->has_buffered_input(); });
            const int ready = poll(fds.data(), fds.size(), buffered || progressed ? 0 : 20);
            check_interrupted();
            if (ready < 0 && errno != EINTR) throw std::runtime_error("Parallel coordinator poll failed");
            for (size_t index = 0; index < active.size() && !violation; ++index) {
                Worker& worker = *active[index];
                if ((!worker.hello || worker.ticket) && Clock::now() >= worker.deadline) {
                    lost(worker, "Worker startup/task deadline exceeded"); continue;
                }
                if (!fds[index].revents && !worker.channel->has_buffered_input()) continue;
                std::optional<wire::Message> message;
                check_interrupted();
                try { message = worker.channel->receive(); }
                catch (const std::exception& error) { lost(worker,error.what()); continue; }
                check_interrupted();
                if (!message) { lost(worker,"Worker control stream closed"); continue; }
                if (const auto* hello = std::get_if<wire::Hello>(&*message)) {
                    if (hello->compatibility != compatibility) {
                        const auto expected = json::parse(compatibility, nullptr, false);
                        const auto actual = json::parse(hello->compatibility, nullptr, false);
                        journal->write({{"event","compatibility_mismatch"},{"worker",worker.id},
                            {"manifest_diff", expected.is_discarded() || actual.is_discarded() ?
                                json("invalid manifest JSON") : json::diff(expected, actual)}});
                    }
                    require(!worker.hello && !worker.ticket && hello->model_identity == root.model_identity &&
                            hello->compatibility == compatibility, "Worker startup model/compatibility/phase mismatch");
                    worker.observe_descendants();
                    worker.hello = true; continue;
                }
                if (const auto* error = std::get_if<wire::Error>(&*message); error && !worker.ticket) {
                    require(error->lease_id == 0 && error->source == 0, "Idle worker error has stale lease");
                    lost(worker, error->reason); continue;
                }
                require(worker.hello && worker.ticket.has_value(), "Worker response has no active lease");
                const auto ticket = *worker.ticket;
                if (auto* edge = std::get_if<wire::Edge>(&*message)) {
                    require(!worker.violation, "Worker emitted edges after property violation");
                    require(edge->lease_id == ticket.lease_id && edge->source == ticket.state && ticket.may_expand,
                            "Worker edge source/lease mismatch");
                    require(worker.actors.count(action_key(edge->actor)), "Worker edge actor is not enabled at source");
                    require((edge->actor.action == TransitionAction::Timeout && edge->choice == -2) ||
                            (edge->actor.action == TransitionAction::Normal && edge->choice >= -1), "Invalid worker edge choice");
                    require(edge->result_code >= CKPT_NO && edge->result_code <= CKPT_STOP,
                            "Invalid/discarded worker edge result");
                    require(edge->target && edge->target == edge->bundle.system_key, "Worker edge bundle identity mismatch");
                    require((edge->choice < 0 && edge->choice_count == 0) ||
                            (edge->actor.action == TransitionAction::Normal && edge->choice >= 0 &&
                             static_cast<uint64_t>(edge->choice) < edge->choice_count), "Worker choice outside declared cardinality");
                    const auto [count, inserted] = worker.choice_counts.emplace(action_key(edge->actor),edge->choice_count);
                    require(inserted || count->second == edge->choice_count, "Worker actor cardinality changed across edges");
                    require(worker.choices[action_key(edge->actor)].insert(edge->choice).second, "Duplicate worker action/choice response");
                    const EdgeKey edge_key{edge->source,edge->actor.process,edge->actor.thread,
                                           static_cast<int>(edge->actor.action),edge->choice};
                    const auto previous = verified_edges.find(edge_key);
                    require(previous == verified_edges.end() || previous->second == std::make_tuple(edge->target, int(edge->result_code),edge->choice_count),
                            "Nondeterministic transition: same source/action/choice produced a different result");
                    const auto target = pool ? snapshot::import_shared_bundle(edge->bundle, root.model_identity, *pool).state
                                             : snapshot::import_bundle(edge->bundle, root.model_identity);
                    require(edge->property_violation == target.property_failed, "Worker property result differs from saved state");
                    worker.channel->confirm_objects();
                    if (edge->property_violation) worker.violation = edge->target;
                    worker.successors.push_back(edge->target); ++report.attempted_edges;
                    if (verified_edges.emplace(edge_key,std::make_tuple(edge->target,int(edge->result_code),edge->choice_count)).second) ++report.edges;
                    journal->write({{"event","edge"},{"worker",worker.id},{"lease",ticket.lease_id},
                                   {"source",edge->source},{"target",edge->target},{"process",edge->actor.process},
                                   {"thread",edge->actor.thread},{"choice",edge->choice},{"choice_count",edge->choice_count},
                                   {"action",edge->actor.action == TransitionAction::Timeout ? "timeout" : "normal"},
                                   {"result_code",edge->result_code},{"violation",edge->property_violation}});
                    // A validated counterexample is existential evidence. It
                    // survives loss of the worker before its completion receipt;
                    // Done is needed only to claim a complete source expansion.
                    if (edge->property_violation) {
                        report.violation_state = edge->target;
                        report.property_violation = violation = true;
                    }
                    continue;
                }
                if (const auto* error = std::get_if<wire::Error>(&*message)) {
                    require(error->lease_id == ticket.lease_id && error->source == ticket.state, "Worker error lease mismatch");
                    throw std::runtime_error("Worker expansion failed: " + error->reason);
                }
                const auto* done = std::get_if<wire::Done>(&*message);
                require(done && done->lease_id == ticket.lease_id && done->source == ticket.state &&
                        done->emitted == worker.successors.size(), "Invalid worker completion receipt");
                Expansion expansion;
                if (done->kind == wire::DoneKind::Violation) {
                    require(worker.violation.has_value() || (worker.source_violation && done->emitted == 0),
                            "Worker violation receipt lacks saved violation");
                    report.violation_state = worker.violation.value_or(ticket.state);
                    report.property_violation = violation = true;
                } else {
                    require(!worker.violation && !worker.source_violation, "Worker completion omitted property violation");
                    if (done->kind == wire::DoneKind::Expanded) {
                        require(ticket.may_expand && worker.source_budget > 0 && !worker.actors.empty() &&
                                worker.choices.size() == worker.actors.size(), "Incomplete worker actor expansion");
                        for (const auto& [actor, choices] : worker.choices) {
                            const auto count = worker.choice_counts.at(actor);
                            if (count == 0) require(choices.size() == 1 && *choices.begin() < 0, "Invalid fixed worker choice set");
                            else require(choices.size() == count && *choices.begin() == 0 &&
                                         static_cast<uint64_t>(*choices.rbegin()) + 1 == count,
                                         "Incomplete worker configured choice set");
                        }
                        expansion = {ResultKind::Expanded,std::move(worker.successors),{}};
                    } else {
                        require(done->emitted == 0, "Non-expansion completion contains edges");
                        if (done->kind == wire::DoneKind::Terminal) {
                            require(worker.actors.empty(), "False terminal completion"); expansion.kind = ResultKind::Terminal;
                        } else if (done->kind == wire::DoneKind::DepthCut) {
                            require(!ticket.may_expand && !worker.actors.empty() && worker.source_budget > 0, "False depth cutoff");
                            expansion.kind = ResultKind::DepthCut;
                        } else if (done->kind == wire::DoneKind::ModelBudget) {
                            require(!worker.actors.empty() && worker.source_budget <= 0, "False model budget cutoff");
                            expansion.kind = ResultKind::ModelBudget; expansion.reason = "Saved model budget exhausted";
                        } else throw std::runtime_error("Invalid worker completion kind");
                    }
                    frontier->commit(ticket,std::move(expansion));
                }
                journal->write({{"event","done"},{"worker",worker.id},{"lease",ticket.lease_id},{"source",ticket.state},
                               {"kind",static_cast<int>(done->kind)},{"emitted",done->emitted}});
                worker.clear_job();
            }
        }
        report.status = violation ? ParallelStatus::Violation : frontier->status() == Status::Complete ?
            ParallelStatus::Complete : frontier->status() == Status::Bounded ? ParallelStatus::Bounded : ParallelStatus::Failed;
        // Worker destruction kills/reaps only owned process incarnations; the
        // coordinator's running tracees and selected saved state remain intact.
        if (violation) for (const auto& worker : workers) frontier->worker_lost(worker->id);
        for (const auto& worker : workers) transport_report(*worker, *journal);
        workers.clear();
        subreaper.cleanup();
        check_interrupted();
    } catch (const std::exception& error) {
        report.status = ParallelStatus::Failed; report.error = error.what();
        // Unfinished leases no longer have live workers after scope cleanup.
        // Retain them as pending in the journal rather than implying expansion.
        if (frontier) {
            try {
                std::set<WorkerId> owners;
                for (const auto& [key,node] : frontier->nodes()) {
                    (void)key; if (node.ticket) owners.insert(node.ticket->worker);
                }
                for (auto owner : owners) frontier->worker_lost(owner);
            } catch (...) {} // A poisoned frontier stays explicitly Failed.
        }
    }
    report.elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (frontier) { report.nodes = frontier->nodes().size(); report.frontier = frontier->counters(); }
    if (journal) {
        try {
            if (frontier) for (const auto& [key,node] : frontier->nodes())
                journal->write({{"event","node"},{"state",key},{"depth",node.depth},{"phase",phase_name(node.phase)}});
            journal->write({{"event","summary"},{"status",static_cast<int>(report.status)},{"nodes",report.nodes},
                           {"edges",report.edges},{"attempted_edges",report.attempted_edges},{"retries",report.retries},{"elapsed_seconds",report.elapsed_seconds},
                           {"error",report.error},{"violation",report.property_violation}});
        } catch (const std::exception& error) { report.status = ParallelStatus::Failed; report.error = error.what(); }
    }
    return report;
}
} // namespace detsim::search
