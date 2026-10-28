#include "state_view.h"
#include "page_snapshot.h"
#include "guest.h"
#include "monitor.h"
#include "proc_status.h"
#include <stdexcept>
#include "dwarf_info.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <list>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>
#include <utility>
#include <sys/stat.h>
namespace detsim {
namespace {
struct MemoValue { int64_t value; bool symbols; };
struct MemoRoot {
  size_t node;
  hash_type key;
  std::string executable;
  std::shared_ptr<const SnapshotView> owner;
  std::unordered_map<std::string,MemoValue,ExpressionStringHash,std::equal_to<>> values;
};
struct ObservationMemo {
  static constexpr size_t max_roots=16, max_values=256, max_expression=1024;
  static constexpr size_t max_root_bytes=64*1024, max_executable=4096;
  uint64_t store=0, symbols=0;
  std::list<MemoRoot> roots;
  ObservationMemoStats stats;
  void sync(uint64_t store_epoch, uint64_t symbol_epoch) {
    if (store!=store_epoch || symbols!=symbol_epoch) {
      roots.clear(); store=store_epoch; symbols=symbol_epoch;
    }
  }
  auto find(size_t node, hash_type key, const std::string& executable) {
    return std::find_if(roots.begin(),roots.end(),[&](const auto& root) {
      return root.node==node && root.key==key && root.executable==executable;
    });
  }
  ~ObservationMemo() {
    const char* report=std::getenv("DETSIM_OBSERVATION_CACHE_STATS");
    if (report && !std::strcmp(report,"1"))
      std::fprintf(stderr,"OBSERVATION_CACHE_PROCESS {\"lookups\":%llu,\"hits\":%llu,\"misses\":%llu,\"admitted\":%llu,\"nonlocal\":%llu,\"roots\":%zu}\n",
        (unsigned long long)stats.lookups,(unsigned long long)stats.hits,
        (unsigned long long)stats.misses,(unsigned long long)stats.admitted,
        (unsigned long long)stats.nonlocal,roots.size());
  }
};
thread_local ObservationMemo memo;
}
ObservationMemoStats observation_memo_stats() {
  auto result=memo.stats; result.roots=memo.roots.size(); return result;
}
void clear_observation_memo() {
  memo.roots.clear(); memo.stats={}; memo.store=memo.symbols=0;
}
StateView::MemoMode StateView::read_memo_mode() {
  const char* mode=std::getenv("DETSIM_OBSERVATION_CACHE");
  if (!mode || !std::strcmp(mode,"off")) return MemoMode::off;
  if (!std::strcmp(mode,"on")) return MemoMode::on;
  if (!std::strcmp(mode,"verify")) return MemoMode::verify;
  throw std::runtime_error("DETSIM_OBSERVATION_CACHE must be off, on or verify");
}
StateView::StateView(const sys_state& state) : memo_mode_(read_memo_mode()) {
  store_generation_=StateStore::instance().snapshot_generation();
  for (size_t i=0; i<NP; ++i) {
    executables_[i]=ptmc_state.tracee[i].executable ? ptmc_state.tracee[i].executable : "";
    snapshots_[i]=state.ts_hash[i];
    statuses_[i]=state.status[i];
    // Saved metadata is authoritative, including nodes omitted by cold system loading.
  }
}
StateView StateView::stopped_live() {
  StateView result;
  result.live_=true;
  for (size_t i=0; i<NP; ++i) {
    result.executables_[i]=ptmc_state.tracee[i].executable ? ptmc_state.tracee[i].executable : "";
    result.live_pids_[i]=ptmc_state.pids[i];
    result.statuses_[i]=ptmc_state.status[i];
    result.clocks_[i]=ptmc_state.time[i];

  }
  return result;
}
void StateView::read(size_t node, uint64_t address, void* destination, size_t length) const {
  if (node>=NP) throw std::out_of_range("Observation process index");
  if (evaluation_reads_ && length) {
    evaluation_reads_->memory=true;
    evaluation_reads_->local &= node==evaluation_reads_->node;
  }
  if (live_) {
    if (!guest_read_exact(live_pids_[node], reinterpret_cast<const void*>(address), destination, length))
      throw std::runtime_error("Cannot read stopped guest memory");
  } else {
    check_snapshot_lifetime();
    if (!snapshots_[node]) throw std::runtime_error("Observation has no saved process snapshot");
    // Pin this node's validated immutable root for the view's lifetime: later
    // reads skip open's store locks and global LRU. read_into still verifies the
    // root's store generation; stopped_live never enters this branch.
    auto& view=snapshot_views_[node];
    if (!view) view=SnapshotView::open(snapshots_[node]);
    view->read_into(address, destination, length);
  }
}
void StateView::require_symbols(size_t node) const {
  if (node>=NP) throw std::out_of_range("Observation process index");
  if (evaluation_reads_) {
    evaluation_reads_->symbols=true;
    evaluation_reads_->local &= node==evaluation_reads_->node;
  }
  const auto& configured = executables_[node];
  const auto& loaded = dwarf_executable_path();
  std::error_code error;
  if (configured.empty() || loaded.empty() ||
      (configured != loaded && !std::filesystem::equivalent(configured, loaded, error)))
    throw std::runtime_error("Observation symbols do not match the configured node executable");
  // The symbol service resolves the configured main executable at its own
  // runtime base, so PIE images are supported for that single image. Other
  // library contexts still require per-node relocation support.
  (void)dwarf_fixed_executable();
}
void StateView::require_symbol_address(size_t node, uint64_t address) const {
  require_symbols(node);
  if (!dwarf_main_image_contains(address))
    throw std::runtime_error("Observation cannot use symbols outside the configured fixed executable");
}
namespace {
/* Node qualifier spellings accepted in the assertion syntax: tracee0(expr) and
 * tracee0:expr. The colon form is an alternative spelling of the documented
 * traceeN(...) selector and is normalized before parsing; a time literal keeps
 * its own colon, so only a leading traceeN: prefix is rewritten. */
std::string normalize_node_qualifier(std::string_view expression) {
  std::string text(expression);
  size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
  size_t i = start + 6;  // after "tracee"
  if (text.compare(start, 6, "tracee") != 0) return text;
  size_t digits = i;
  while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) ++digits;
  if (digits == i || digits >= text.size() || text[digits] != ':') return text;
  const int qualifier = std::atoi(text.c_str() + i);
  if (qualifier < 0 || qualifier >= NP)
    throw std::runtime_error("Observation node " + std::to_string(qualifier) +
        " is outside this fixed-NP build (Nodes " + std::to_string(NP) +
        "); every node of a run executes the same target");
  return text.substr(0, digits) + "(" + text.substr(digits + 1) + ")";
}
}

int64_t StateView::evaluate(size_t node, std::string_view expression) const {
  if (node>=NP) throw std::out_of_range("Observation process index");
  const std::string normalized = normalize_node_qualifier(expression);
  return evaluate_normalized(node, std::string_view(normalized));
}
int64_t StateView::evaluate_normalized(size_t node, std::string_view expression) const {
  const auto mode=memo_mode_;
  // Nested evaluations keep recording into the outer dependency set. Constants
  // and stale/live views retain the original evaluator's validation behavior.
  if (mode==MemoMode::off || live_ || evaluation_reads_ || !snapshots_[node] ||
      expression.size()>ObservationMemo::max_expression ||
      executables_[node].size()>ObservationMemo::max_executable ||
      store_generation_!=StateStore::instance().generation())
    return evaluate_observation(*this,node,expression);
  const auto symbols=dwarf_symbol_generation();
  memo.sync(store_generation_,symbols);
  ++memo.stats.lookups;
  auto root=memo.find(node,snapshots_[node],executables_[node]);
  std::optional<MemoValue> cached;
  if (root!=memo.roots.end()) {
    const auto found=root->values.find(expression);
    if (found!=root->values.end()) {
      cached=found->second;
      // A hit is a retained validated observation, not a fallback to live data.
      if (cached->symbols) require_symbols(node);
      check_snapshot_lifetime();
      snapshot_views_[node]=root->owner;
      memo.roots.splice(memo.roots.begin(),memo.roots,root);
      ++memo.stats.hits;
      if (mode==MemoMode::on) return cached->value;
    }
  }
  if (!cached) ++memo.stats.misses;
  EvaluationReads reads{node};
  struct TraceScope {
    EvaluationReads*& slot;
    EvaluationReads* previous;
    TraceScope(EvaluationReads*& slot, EvaluationReads& current)
        : slot(slot),previous(std::exchange(slot,&current)) {}
    ~TraceScope() {slot=previous;}
  } trace(evaluation_reads_,reads);
  const int64_t value=evaluate_observation(*this,node,expression);
  if (cached && (cached->value!=value || !reads.local || !reads.memory))
    throw std::runtime_error("Saved observation memo disagrees with original evaluation");
  if (!reads.local) {++memo.stats.nonlocal;return value;}
  const auto owner=snapshot_views_[node];
  if (!reads.memory || !owner || store_generation_!=StateStore::instance().generation() ||
      symbols!=dwarf_symbol_generation() || owner->encoded_size()>ObservationMemo::max_root_bytes)
    return value;
  // Re-find after evaluation: nested symbol/type work may invalidate admission,
  // and no cache iterator is kept across a callback into the evaluator.
  root=memo.find(node,snapshots_[node],executables_[node]);
  if (root==memo.roots.end()) {
    if (memo.roots.size()>=ObservationMemo::max_roots) memo.roots.pop_back();
    memo.roots.push_front({node,snapshots_[node],executables_[node],owner,{}});
    root=memo.roots.begin();
  } else memo.roots.splice(memo.roots.begin(),memo.roots,root);
  if (root->values.size()<ObservationMemo::max_values &&
      root->values.emplace(std::string(expression),MemoValue{value,reads.symbols}).second)
    ++memo.stats.admitted;
  return value;
}
void StateView::check_snapshot_lifetime() const {
  if (!live_ && StateStore::instance().generation()!=store_generation_)
    throw std::runtime_error("Observation belongs to a closed or failed snapshot store");
}
const tracee_state& StateView::metadata(size_t node) const {
  check_snapshot_lifetime();
  if (node>=NP) throw std::out_of_range("Observation process index");
  if (!metadata_[node]) {
    if (live_) {
      auto state=std::make_shared<tracee_state>();
      state->tv=clocks_[node];
      state->fs_state=ptmc_state.fs_states[node];
      state->sock_state=ptmc_state.sock_states[node];
      state->process_state=ptmc_state.process_states[node];
      if (!ptmc_state.fd_managers[node]) throw std::runtime_error("Missing live descriptor namespace");
      state->fd_manager_state=*ptmc_state.fd_managers[node];
      metadata_[node]=std::move(state);
    } else {
      if (!snapshots_[node]) throw std::runtime_error("Observation has no saved process snapshot");
      metadata_[node]=std::make_shared<tracee_state>(snapshots_[node]);
    }
  }
  return *metadata_[node];
}
timeval StateView::time(size_t node) const {
  if (node>=NP) throw std::out_of_range("Observation process index");
  return live_ ? clocks_[node] : metadata(node).tv;
}
namespace {
using Json=nlohmann::json;
Json endpoint(const sockaddr_in& address) {
  return {{"family",address.sin_family},{"ipv4",ntohl(address.sin_addr.s_addr)},{"port",ntohs(address.sin_port)}};
}
Json queue_lengths(const std::deque<std::string>& queue) {
  auto result=Json::array();
  for (const auto& chunk:queue) result.push_back({{"length",chunk.size()},{"eof",chunk.empty()}});
  return result;
}
const char* fd_kind(FdType kind) {
  switch (kind) {
    case FdType::FILE:return "file"; case FdType::SOCKET:return "socket";
    case FdType::EPOLL:return "epoll"; case FdType::PIPE:return "pipe";
    case FdType::STDIO:return "stdio"; case FdType::UNKNOWN:return "unknown";
  }
  throw std::runtime_error("Unknown descriptor kind");
}
Json registers(const user_regs_struct& registers) {
  Json result=Json::object();
#define REG(name) result[#name]=registers.name
  REG(r15); REG(r14); REG(r13); REG(r12); REG(rbp); REG(rbx);
  REG(r11); REG(r10); REG(r9); REG(r8); REG(rax); REG(rcx); REG(rdx);
  REG(rsi); REG(rdi); REG(orig_rax); REG(rip); REG(cs); REG(eflags);
  REG(rsp); REG(ss); REG(fs_base); REG(gs_base); REG(ds); REG(es); REG(fs); REG(gs);
#undef REG
  return result;
}
}
std::string StateView::inspect_json(size_t node) const {
  const auto& state=metadata(node);
  const auto& fs=state.fs_state;
  const auto& network=state.sock_state;
  const auto& manager=state.fd_manager_state;
  Json result={{"schema",1},{"process",node},{"status",statuses_[node]},{"clock",{{"seconds",state.tv.tv_sec},{"microseconds",state.tv.tv_usec}}},
    {"cwd",fs.cwd},{"registers_available",!live_},{"futex_available",!live_}};
  const int status = statuses_[node];
  result["lifecycle"] = DISEXITED(status) ? "exited" : DISCRASHED(status) ? "crashed" :
      DISSIGNALED(status) ? "signaled" : (status & DIFSTOPPED) ? "stopped" : "running";
  result["exit_code"] = DISEXITED(status) ? Json(DEXITSTATUS(status)) : Json(nullptr);
  result["signal"] = (status & (DIFCRASHED | DIFSIGNALED | DIFSTOPPED)) ? Json(DSTOPSIG(status)) : Json(nullptr);
  result["creation_mask"] = fs.creation_mask();
  result["kernel_process"] = {{"random_state",state.process_state.random_state},{"signal_actions",Json::array()}};
  for(const auto& [signal,action]:state.process_state.signal_actions)
    result["kernel_process"]["signal_actions"].push_back({{"signal",signal},{"handler",action.handler},
      {"flags",action.flags},{"restorer",action.restorer},{"mask",action.mask}});
  auto descriptors=Json::array(); auto fds=manager.get_allocated_fds();
  std::vector<int> sorted(fds.begin(),fds.end()); std::sort(sorted.begin(),sorted.end());
  for (int fd:sorted) descriptors.push_back({{"fd",fd},{"kind",fd_kind(manager.get_fd_type(fd))},
    {"status_flags",manager.get_status_flags(fd)},{"descriptor_flags",manager.get_descriptor_flags(fd)}});
  result["descriptors"]=std::move(descriptors);
  auto files=Json::array();
  for (const auto& [path,file]:fs.filesystem) files.push_back({{"path",path},{"mode",file.metadata.st_mode},{"size",file.metadata.st_size},{"buffer_length",file.content.size()}});
  result["files"]=std::move(files);
  auto open_files=Json::array();
  for (const auto& [fd,file]:fs.open_files) open_files.push_back({{"fd",fd},{"path",file.path},{"offset",file.offset}});
  result["open_files"]=std::move(open_files);
  auto file_mappings=Json::array();
  for (const auto& [start,end]:fs.virtual_file_mappings)
    file_mappings.push_back({{"start",start},{"end",end}});
  result["virtual_file_mappings"]=std::move(file_mappings);
  auto pipes=Json::array();
  for (const auto& [id,pipe]:fs.pipes_) pipes.push_back({{"id",id},{"length",pipe.buffer.size()},{"read_closed",pipe.read_closed},{"write_closed",pipe.write_closed}});
  result["pipes"]=std::move(pipes);
  auto pipe_fds=Json::array();
  for (const auto& [fd,id]:fs.pipe_fds_) pipe_fds.push_back({{"fd",fd},{"pipe",id>0?id:-id},{"direction",id>0?"read":"write"}});
  result["pipe_endpoints"]=std::move(pipe_fds);
  auto sockets=Json::array();
  for (const auto& [fd,socket]:network.sockets()) {
    Json item={{"fd",fd},{"domain",socket.domain},{"type",socket.type},{"protocol",socket.protocol},
      {"bound",socket.bound},{"connected",socket.connected},{"listening",socket.listening},
      {"local",endpoint(socket.local_addr)},{"peer",endpoint(socket.peer_addr)},{"backlog",socket.backlog},
      {"pending",Json::array()},{"options",{{"reuse_address",socket.reuse_address},{"no_delay",socket.no_delay},
        {"keep_alive",socket.keep_alive},{"keep_idle",socket.keep_idle},{"keep_interval",socket.keep_interval},{"keep_count",socket.keep_count}}}};
    for (const auto key:socket.pending_connections) item["pending"].push_back(std::to_string(key));
    auto pending=Json::array();
    for (const auto& [key,connection]:socket.established_connections) pending.push_back({{"key",std::to_string(key)},{"local",endpoint(connection.local_addr)},{"peer",endpoint(connection.peer_addr)},{"queue",queue_lengths(connection.recv_buffer)}});
    item["pending_connections"]=std::move(pending); sockets.push_back(std::move(item));
  }
  result["sockets"]=std::move(sockets);
  auto tcp=Json::array();
  for (const auto& [fd,connection]:network.tcp_connections()) tcp.push_back({{"fd",fd},{"local",endpoint(connection.local_addr)},{"peer",endpoint(connection.peer_addr)},{"queue",queue_lengths(connection.recv_buffer)}});
  result["tcp"]=std::move(tcp);
  auto udp=Json::array();
  for (const auto& [fd,queue]:network.udp_recv_buffers()) {
    auto messages=Json::array(); for (const auto& message:queue) messages.push_back({{"length",message.content.size()},{"from",endpoint(message.from)}});
    udp.push_back({{"fd",fd},{"queue",std::move(messages)}});
  }
  result["udp"]=std::move(udp);
  auto epolls=Json::array();
  for (const auto& [fd,instance]:network.epoll_instances()) {
    auto watches=Json::array(); for (const auto& [watched,watch]:instance.watched_fds) watches.push_back({{"fd",watched},{"events",watch.events},{"data",watch.data}});
    epolls.push_back({{"fd",fd},{"watches",std::move(watches)}});
  }
  result["epolls"]=std::move(epolls);
  result["threads"]=live_ ? Json(nullptr) : Json::array();
  if (!live_) for (const auto& thread:state.threads) {
    Json pending=nullptr;
    if (thread.pending_syscall) {
      const auto& call=*thread.pending_syscall;
      pending={{"phase",call.phase==PendingPhase::Waiting?"waiting":call.phase==PendingPhase::Retry?"retry":"ready"},
               {"reason",call.reason==PendingReason::Futex?"futex":call.reason==PendingReason::Io?"io":"timer"},
               {"syscall",call.nr},{"args",call.args},
               {"result",call.phase==PendingPhase::Ready?Json(call.result):Json(nullptr)}};
      if(call.deadline) pending["deadline"]={{"clock",call.deadline->clock_id},
          {"seconds",call.deadline->sec},{"nanoseconds",call.deadline->nsec},
          {"timeout_result",call.deadline->timeout_result}};
      else pending["deadline"]=nullptr;
    }
    result["threads"].push_back({{"pending_syscall",pending},{"id",thread.tid},{"alive",thread.alive},{"signal_mask",thread.signal_mask},{"robust_list_head",thread.robust_list_head},{"clear_child_tid",thread.clear_child_tid},{"child_settid_pending",thread.child_settid_pending},{"pending_child_settid",thread.pending_child_settid},{"exit_code",thread.alive?Json(nullptr):Json(thread.exit_code)},{"is_main",thread.is_main},{"registers",registers(thread.regs)}});
  }
  result["futex"]=live_ ? Json(nullptr) : Json::array();
  if (!live_ && state.futex_state) for (const auto& [address,queue]:state.futex_state->wait_queues()) {
    auto waiters=Json::array(); for (const auto& waiter:queue) waiters.push_back({{"thread",waiter.tid},{"value",waiter.val},{"bitset",waiter.bitset},{"timeout",state.find_thread(waiter.tid) && state.find_thread(waiter.tid)->pending_syscall && state.find_thread(waiter.tid)->pending_syscall->deadline.has_value()},{"private",waiter.private_wait}});
    result["futex"].push_back({{"address",address},{"waiters",std::move(waiters)}});
  }
  return result.dump();
}
}
