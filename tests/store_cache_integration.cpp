#include "state.h"
#include "state_store.h"
#include "state_store_packed.h"
#include "sysstate_store.h"
#include "debug.h"
#include "ncurses_ui.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }
static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
static std::mutex gate_mutex;
static std::condition_variable gate_cv;
static bool hold_compression = false, compression_entered = false;
static bool fail_compression = false;
static std::atomic<bool> fail_sync{false};
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
  if (fail_sync.load()) {errno = EIO; return -1;}
  return __real_fsync(fd);
}
// Count metadata loads performed by the prefetch path. The queue-prefetch
// regression asserts that a known-hint enqueue does not enter this oracle.
static std::atomic<unsigned> sysstate_load_calls{0};
extern "C" bool real_sysstate_load(SysStateStore *, hash_type, std::vector<uint8_t> &)
  asm("__real__ZN13SysStateStore4loadEmRSt6vectorIhSaIhEE");
extern "C" bool wrap_sysstate_load(SysStateStore *store, hash_type hash, std::vector<uint8_t> &data)
  asm("__wrap__ZN13SysStateStore4loadEmRSt6vectorIhSaIhEE");
extern "C" bool wrap_sysstate_load(SysStateStore *store, hash_type hash, std::vector<uint8_t> &data) {
  ++sysstate_load_calls;
  return real_sysstate_load(store, hash, data);
}
template <typename F> static void rejects(F operation, const char *message) {
  bool failed = false; try {operation();} catch (const std::exception &) {failed = true;}
  require(failed, message);
}
extern "C" std::vector<uint8_t> real_compress(StateStorePacked *, const std::vector<uint8_t> &)
  asm("__real__ZN16StateStorePacked8compressERKSt6vectorIhSaIhEE");
extern "C" std::vector<uint8_t> wrap_compress(StateStorePacked *store, const std::vector<uint8_t> &data)
  asm("__wrap__ZN16StateStorePacked8compressERKSt6vectorIhSaIhEE");
extern "C" std::vector<uint8_t> wrap_compress(StateStorePacked *store, const std::vector<uint8_t> &data) {
  std::unique_lock lock(gate_mutex);
  compression_entered = true;
  gate_cv.notify_all();
  gate_cv.wait(lock, [] { return !hold_compression; });
  if (fail_compression) throw std::runtime_error("injected compression failure");
  lock.unlock();
  return real_compress(store, data);
}
static void release_compression() {
  std::lock_guard lock(gate_mutex);
  hold_compression = false;
  gate_cv.notify_all();
}
static void await_compression() {
  std::unique_lock lock(gate_mutex);
  require(gate_cv.wait_for(lock, std::chrono::seconds(3), [] {return compression_entered;}), "worker did not reach compression gate");
}
static std::vector<uint8_t> payload(unsigned seed) {
  std::vector<uint8_t> result(4096);
  for (auto &byte : result) { seed = seed * 1664525 + 1013904223; byte = seed >> 24; }
  return result;
}
static StateStore::Config config(size_t hot = 8192, size_t warm = 8192) {
  StateStore::Config result;
  result.hot_cache_size = hot; result.warm_cache_size = warm;
  result.prefetch_threads = 0; result.prefetch_window = 0;
  result.packed_storage_path = "packed";
  return result;
}
static bool await_l2(StateStore &store, hash_type hash) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!store.in_l2(hash) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return store.in_l2(hash);
}
// Persist sys-state metadata whose only nonzero component is ts. This makes the
// hash-only legacy path observable: it must load this record to find ts.
static sys_state metadata_with_component(hash_type ts) {
  sys_state state;
  state.ts_hash[0] = ts;
  state.ss_hash = state.identity_hash();
  require(state.save_metadata() == 1, "queue hints: metadata save");
  return state;
}
static int run_child(const char *executable, const char *mode, const char *hash = nullptr) {
  const pid_t pid = fork();
  require(pid >= 0, "fork");
  if (!pid) { execl(executable, executable, mode, hash, nullptr); _exit(127); }
  int status; require(waitpid(pid, &status, 0) == pid, "waitpid");
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
static void run_case(const char *mode, const char *executable, const char *hash_arg) {
  auto &store = StateStore::instance();
  if (!strcmp(mode, "reader")) {
    StateStorePacked::Config cfg; cfg.storage_path = "packed"; cfg.read_only = true;
    auto &packed = StateStorePacked::instance(); packed.init(cfg);
    std::vector<uint8_t> result;
    require(packed.load(std::stoull(hash_arg), result) == 4096 && result == payload(11), "fresh process cannot read completed write");
    packed.shutdown(); return;
  }
  if (!strcmp(mode, "reference")) {
    store.init(config(0, 0)); hold_compression = true;
    auto data = payload(11); auto hash = store.save(data.data(), data.size());
    await_compression();
    require(store.save(data.data(), data.size(), hash) == hash, "pending reference changed hash");
    require(store.get_stats().save_reference_hits == 1, "pending reference did not avoid hashing");
    auto other = payload(12);
    auto other_hash = store.save(other.data(), other.size(), hash);
    require(other_hash != hash && store.get_stats().save_reference_hits == 1, "unequal reference reused hash");
    require(store.save(data.data(), data.size() - 1, hash) != hash, "short reference reused hash");
    release_compression(); store.wait_for_completion();
    const auto before = store.get_stats();
    require(store.save(data.data(), data.size(), hash) == hash, "cold reference changed hash");
    const auto after = store.get_stats();
    require(after.save_reference_hits == before.save_reference_hits && after.load_requests == before.load_requests,
            "cold reference used speculative load");
    require(store.save(data.data(), data.size(), 123) == hash, "missing reference changed hash");
    store.shutdown(); store.init(config());
    require(*store.load_shared(hash) == data, "reference reload");
    require(store.save(data.data(), data.size(), hash) == hash && store.get_stats().save_reference_hits == 1,
            "resident reference did not avoid hashing");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "barrier")) {
    store.init(config());
    hold_compression = true;
    auto data = payload(11); auto hash = store.save(data.data(), data.size());
    await_compression();
    auto barrier = std::async(std::launch::async, [&] { store.wait_for_completion(); });
    bool blocked = barrier.wait_for(std::chrono::milliseconds(150)) == std::future_status::timeout;
    release_compression(); barrier.get();
    require(blocked, "completion barrier returned while worker held before persistence");
    auto encoded = std::to_string(hash);
    require(run_child(executable, "reader", encoded.c_str()) == 0, "fresh-process reload after wait failed");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "lifetime")) {
    store.init(config(4096, 0));
    auto data = payload(11); auto hash = store.save(data.data(), data.size());
    auto held = store.load_shared(hash);
    require(held && held.use_count() > 0, "load_shared returned a non-owning pointer");
    store.wait_for_completion();
    for (unsigned i = 20; i < 40; ++i) {auto other = payload(i); store.save(other.data(), other.size());}
    store.wait_for_completion();
    require(*held == data, "eviction mutated retained shared bytes");
    require(store.get_l1_usage() <= 4096 && store.get_l2_usage() == 0, "cache exceeded byte budget");
    store.shutdown(); require(*held == data, "shutdown invalidated retained shared bytes"); return;
  }
  if (!strcmp(mode, "errors")) {
    store.init(config()); fail_compression = true;
    auto data = payload(11); store.save(data.data(), data.size());
    bool failed = false; try {store.wait_for_completion();} catch (const std::exception &) {failed = true;}
    require(failed, "worker failure was not propagated");
    failed = false; try {store.save(data.data(), data.size());} catch (const std::exception &) {failed = true;}
    require(failed, "save accepted data after worker failure");
    failed = false; try {store.shutdown();} catch (const std::exception &) {failed = true;}
    require(failed, "shutdown concealed worker failure");
    fail_compression = false; store.init(config());
    auto hash = store.save(data.data(), data.size()); store.wait_for_completion();
    require(*store.load_shared(hash) == data, "reinit after failure did not recover"); store.shutdown(); return;
  }
  if (!strcmp(mode, "concurrent")) {
    store.init(config(0, 0)); hold_compression = true;
    auto data = payload(11); auto hash = store.save(data.data(), data.size()); await_compression();
    std::vector<std::future<void>> work;
    for (unsigned i = 0; i < 8; ++i) work.push_back(std::async(std::launch::async, [&] {
      for (unsigned j = 0; j < 40; ++j) {
        require(store.save(data.data(), data.size()) == hash, "concurrent duplicate hash");
        require(*store.load_shared(hash) == data, "pending write not readable");
      }
    }));
    for (auto &task : work) task.get();
    require(store.get_stats().save_new == 1, "concurrent dedup queued multiple writes");
    release_compression(); store.wait_for_completion();
    require(*store.load_shared(hash) == data, "zero-cache disk read");
    require(store.get_l1_usage() == 0 && store.get_l2_usage() == 0, "zero cache retained data");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "shutdown")) {
    store.init(config(0, 0)); hold_compression = true;
    auto data = payload(11); auto hash = store.save(data.data(), data.size()); await_compression();
    for (unsigned i = 1; i < 20; ++i) {auto other=payload(i); store.save(other.data(),other.size());}
    auto stopping = std::async(std::launch::async,[&]{store.shutdown();});
    require(stopping.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout, "shutdown did not wait for accepted writes");
    release_compression(); stopping.get();
    store.init(config(0, 0));
    require(*store.load_shared(hash) == data, "reinit lost first write");
    for (unsigned i = 1; i < 20; ++i) {auto other=payload(i); auto key=store.save(other.data(),other.size()); require(store.get_stats().save_new == 0, "shutdown dropped queued write"); require(*store.load_shared(key)==other,"reinit bytes differ");}
    store.shutdown(); return;
  }
  if (!strcmp(mode, "backpressure")) {
    auto cfg = config(0,0); cfg.max_pending_bytes = 4096; store.init(cfg);
    hold_compression = true;
    auto first = payload(11); store.save(first.data(), first.size()); await_compression();
    auto second = std::async(std::launch::async, [&] {auto bytes=payload(12); return store.save(bytes.data(),bytes.size());});
    require(second.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout, "pending-byte budget excluded active worker");
    release_compression(); auto second_hash = second.get(); store.wait_for_completion();
    require(*store.load_shared(second_hash) == payload(12), "backpressure lost write");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "multiworker")) {
    auto cfg = config(4096,4096); cfg.io_threads = 4; store.init(cfg);
    std::vector<std::future<void>> tasks;
    for (unsigned thread=0; thread<4; ++thread) tasks.push_back(std::async(std::launch::async, [&,thread] {
      for (unsigned i=1; i<=40; ++i) {
        auto bytes=payload(thread*100+i); auto hash=store.save(bytes.data(),bytes.size());
        auto held=store.load_shared(hash); require(held && *held==bytes,"multiworker read bytes");
      }
    }));
    for (auto &task : tasks) task.get();
    store.wait_for_completion();
    require(store.get_l1_usage()<=4096 && store.get_l2_usage()<=4096,"multiworker cache cap");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "prefetch")) {
    auto cfg=config(0,8192); cfg.prefetch_threads=2; cfg.prefetch_window=2;
    store.init(cfg);
    auto first=payload(11); auto hash=store.save(first.data(),first.size());
    store.wait_for_completion(); store.shutdown(); store.init(cfg);
    store.queue_push_back(1); store.queue_push_back(hash); store.queue_pop_front();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!store.in_l2(hash) && std::chrono::steady_clock::now()<deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(store.in_l2(hash),"prefetch did not populate L2");
    require(*store.load_shared(hash)==first,"prefetched bytes differ");
    require(store.get_stats().prefetch_issued==1,"prefetch issue accounting");
    store.shutdown(); return;
  }
  if (!strcmp(mode, "queue_hints")) {
    auto cfg=config(0,1u<<16); cfg.prefetch_threads=1; cfg.prefetch_window=4; cfg.max_inflight_prefetch=8;
    store.init(cfg);
    auto d1=payload(21), d2=payload(22), d3=payload(23), d4=payload(24);
    const hash_type h1=store.save(d1.data(),d1.size()), h2=store.save(d2.data(),d2.size());
    const hash_type h3=store.save(d3.data(),d3.size()), h4=store.save(d4.data(),d4.size());
    store.wait_for_completion();
    store.shutdown();
    require(store.queue_size()==0,"queue survived shutdown");
    store.init(cfg); // reopen packed data with cold L1/L2 and an empty queue
    SysStateStore::instance().init();
    auto fs1=metadata_with_component(h1), fs2=metadata_with_component(h2);
    auto fs3=metadata_with_component(h3), fs4=metadata_with_component(h4);
    sysstate_load_calls.store(0);

    // Known hints alone: FIFO order and cold bytes must not cost a metadata load.
    sys_state trigger1; trigger1.ss_hash=0x1111;
    state_queue_append(trigger1);
    state_queue_append(fs1);
    // The queue must own the hint array: mutating the producer's sys_state after
    // enqueue must not change what the later trigger pop prefetches.
    fs1.ts_hash[0] = h2;
    require(store.queue_size()==2,"known hints: queue size");
    require(store.queue_peek(0)==trigger1.ss_hash && store.queue_peek(1)==fs1.ss_hash,
            "known hints: peek order");
    require(store.queue_try_pop_front()==trigger1.ss_hash,"known hints: trigger FIFO");
    require(await_l2(store,h1),"known hints: cold component not prefetched");
    require(sysstate_load_calls.load()==0,"known hints: metadata was loaded");
    require(store.get_stats().prefetch_issued==1,"known hints: prefetch accounting");
    auto got1=store.load_shared(h1);
    require(got1 && *got1==d1,"known hints: prefetched bytes differ");
    require(store.get_stats().l2_hits==1,"known hints: prefetch was not a cold L2 hit");
    require(store.queue_try_pop_front()==fs1.ss_hash,"known hints: pop order");
    require(store.queue_size()==0,"known hints: queue not drained");

    // Mixed queue: known hint plus the hash-only legacy overload. Only the
    // legacy entry may resolve component hashes through SysStateStore::load.
    sysstate_load_calls.store(0);
    sys_state trigger2; trigger2.ss_hash=0x2222;
    state_queue_append(trigger2);
    state_queue_append(fs2);
    store.queue_push_back(fs3.ss_hash);
    require(store.queue_size()==3,"mixed hints: queue size");
    require(store.queue_peek(1)==fs2.ss_hash && store.queue_peek(2)==fs3.ss_hash,
            "mixed hints: peek order");
    require(store.queue_try_pop_front()==trigger2.ss_hash,"mixed hints: trigger FIFO");
    require(await_l2(store,h2) && await_l2(store,h3),"mixed hints: components not prefetched");
    require(sysstate_load_calls.load()==1,"mixed hints: legacy metadata load count");
    auto got2=store.load_shared(h2), got3=store.load_shared(h3);
    require(got2 && *got2==d2,"mixed hints: known bytes differ");
    require(got3 && *got3==d3,"mixed hints: legacy bytes differ");
    require(store.queue_try_pop_front()==fs2.ss_hash,"mixed hints: known FIFO");
    require(store.queue_try_pop_front()==fs3.ss_hash,"mixed hints: legacy FIFO");

    // All-zero component hints are valid entries: neither prefetch nor metadata.
    store.queue_clear();
    sysstate_load_calls.store(0);
    const auto issued_before_zero=store.get_stats().prefetch_issued;
    sys_state trigger3; trigger3.ss_hash=0x3333;
    sys_state zero_hint; zero_hint.ss_hash=0x4444;
    state_queue_append(trigger3);
    state_queue_append(zero_hint);
    require(store.queue_try_pop_front()==trigger3.ss_hash,"zero hints: trigger FIFO");
    // submit_prefetch_tasks runs inside the pop; no asynchronous grace is needed
    // for the issue/load counters. Byte waits use await_l2 elsewhere.
    require(store.get_stats().prefetch_issued==issued_before_zero,"zero hints: prefetch issued");
    require(sysstate_load_calls.load()==0,"zero hints: metadata loaded");
    require(store.queue_try_pop_front()==zero_hint.ss_hash,"zero hints: FIFO");
    require(store.queue_size()==0,"zero hints: queue not drained");

    // Known hints must not bypass metadata validation. Store a valid metadata
    // payload under the wrong key: extract must still load it and reject the
    // identity mismatch instead of trusting the queued component hint.
    std::vector<uint8_t> fs1_bytes;
    require(SysStateStore::instance().load(fs1.ss_hash, fs1_bytes),
            "malformed hints: read metadata payload");
    const hash_type bad_ss = 0xBAD5EED5EED5EEDULL;
    require(!SysStateStore::instance().exists(bad_ss),
            "malformed hints: fixed key already present");
    require(SysStateStore::instance().save(bad_ss, fs1_bytes),
            "malformed hints: save mismatched metadata");
    sys_state bad_hint; bad_hint.ss_hash=bad_ss; bad_hint.ts_hash[0]=h3;
    state_queue_append(bad_hint);
    require(store.queue_size()==1,"malformed hints: queue size");
    sysstate_load_calls.store(0);
    rejects([&]{ state_queue_extract(); }, "known hints bypassed metadata validation");
    require(sysstate_load_calls.load()==1,"malformed hints: metadata not loaded");
    require(store.queue_size()==0,"malformed hints: entry not consumed");

    // Clear must drop pending known-hint entries without issuing prefetch.
    state_queue_append(fs4);
    require(store.queue_size()==1,"clear: queue size");
    sysstate_load_calls.store(0);
    const auto issued_before_clear=store.get_stats().prefetch_issued;
    store.queue_clear();
    require(store.queue_size()==0,"clear: retained entries");
    require(store.queue_try_pop_front()==0,"clear: popped a cleared entry");
    require(store.get_stats().prefetch_issued==issued_before_clear,"clear: issued prefetch");
    require(sysstate_load_calls.load()==0,"clear: loaded metadata");

    // Shutdown drops the queue; reopen must not resurrect it, and a fresh
    // known-hint enqueue must still prefetch cold bytes with no metadata load.
    state_queue_append(fs4);
    require(store.queue_size()==1,"shutdown: queue size");
    store.shutdown();
    require(store.queue_size()==0,"shutdown: retained queue entry");
    store.init(cfg);
    require(store.queue_size()==0,"reinit: retained queue entry");
    require(store.queue_try_pop_front()==0,"reinit: popped a stale entry");
    require(!store.in_l2(h4),"reinit: retained warm bytes");
    sysstate_load_calls.store(0);
    const auto issued_before_reinit=store.get_stats().prefetch_issued;
    sys_state trigger4; trigger4.ss_hash=0x5555;
    state_queue_append(trigger4);
    state_queue_append(fs4);
    require(store.queue_try_pop_front()==trigger4.ss_hash,"reinit: trigger FIFO");
    require(await_l2(store,h4),"reinit: known hint did not prefetch");
    require(sysstate_load_calls.load()==0,"reinit: metadata loaded");
    require(store.get_stats().prefetch_issued==issued_before_reinit+1,"reinit: prefetch accounting");
    auto got4=store.load_shared(h4);
    require(got4 && *got4==d4,"reinit: prefetched bytes differ");
    require(store.queue_try_pop_front()==fs4.ss_hash,"reinit: FIFO");
    store.shutdown();
    SysStateStore::instance().shutdown();
    return;
  }
  if (!strcmp(mode, "sync_error")) {
    store.init(config()); auto data=payload(11); auto hash=store.save(data.data(),data.size());
    fail_sync=true;
    rejects([&]{store.wait_for_completion();},"sync error hidden"); fail_sync=false;
    rejects([&]{store.save(data.data(),data.size());},"cached save after failed barrier");
    rejects([&]{store.save(data.data(),data.size(),hash);},"reference save after failed barrier");
    rejects([&]{store.load_shared(hash);},"cached load after failed barrier");
    rejects([&]{store.wait_for_completion();},"repeat barrier after failure");
    rejects([&]{store.shutdown();},"shutdown hid barrier failure");
    return;
  }
  if (!strcmp(mode, "config")) {
    auto cfg=config(); cfg.io_threads=0;
    rejects([&]{store.init(cfg);},"zero workers accepted");
    cfg.io_threads=-1; rejects([&]{store.init(cfg);},"negative workers accepted");
    cfg.io_threads=1; cfg.max_pending_bytes=0;
    rejects([&]{store.init(cfg);},"zero pending budget accepted");
    store.init(config()); auto bytes=payload(11);
    rejects([&]{store.save(bytes.data(), size_t(UINT32_MAX)+1);},"oversized snapshot accepted");
    auto hash=state_store_save(bytes.data(),bytes.size());
    require(hash==store.save(bytes.data(),bytes.size()),"C interface truncated hash");
    std::vector<uint8_t> small(1);
    rejects([&]{state_store_load(hash,small.data(),small.size());},"C interface silently truncated state");
    store.shutdown();
    cfg=config(); cfg.read_only=true; store.init(cfg);
    require(*store.load_shared(hash)==bytes,"readonly reload");
    rejects([&]{store.save(bytes.data(),bytes.size());},"readonly save accepted");
    rejects([&]{store.save(bytes.data(),bytes.size(),hash);},"readonly reference save accepted");
    store.shutdown(); return;
  }
  throw std::runtime_error("unknown case");
}
int main(int argc, char **argv) {
  loglevel = 4;
  alarm(30);
  try {
    const std::string executable = std::filesystem::canonical("/proc/self/exe");
    if (argc > 1) {run_case(argv[1], executable.c_str(), argc > 2 ? argv[2] : nullptr); return 0;}
    for (auto mode : {"reference", "lifetime", "barrier", "concurrent", "shutdown", "errors", "backpressure", "multiworker", "prefetch", "queue_hints", "sync_error", "config"}) {
      auto old = std::filesystem::current_path();
      char pattern[] = "/tmp/detsim-store-cache-XXXXXX";
      require(mkdtemp(pattern) != nullptr,"mkdtemp");
      std::filesystem::current_path(pattern);
      int status = run_child(executable.c_str(), mode);
      std::filesystem::current_path(old); std::filesystem::remove_all(pattern);
      require(status == 0, mode); std::printf("PASS store cache: %s\n", mode);
    }
  } catch (const std::exception &error) {std::fprintf(stderr,"FAIL store cache: %s\n",error.what()); return 1;}
}
