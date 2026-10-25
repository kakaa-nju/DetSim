#include "config.h"
#include "state/state_transition.h"
#include "monitor.h"
#include "ncurses_ui.h"
#include "proc_status.h"
#include "state/sysstate_store.h"
#include "state/page_snapshot.h"
#include "cereal/archives/binary.hpp"
#include "cereal/types/map.hpp"
#include "cereal/types/vector.hpp"
#include "utils.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

extern TSS state_tree;
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

static void require(bool ok, const char *message)
{
  if (!ok) throw std::runtime_error(message);
}

template <typename F> static void rejects(F operation, const char *message)
{
  bool rejected = false;
  try { operation(); }
  catch (const std::exception &) { rejected = true; }
  require(rejected, message);
}

struct StoreDirectory
{
  std::filesystem::path old = std::filesystem::current_path();
  std::filesystem::path path;
  StoreDirectory()
  {
    char pattern[] = "/tmp/detsim-identity-XXXXXX";
    require(mkdtemp(pattern) != nullptr, "temporary store directory");
    path = pattern;
    std::filesystem::current_path(path);
  }
  ~StoreDirectory()
  {
    StateStore::instance().shutdown();
    SysStateStore::instance().shutdown();
    std::filesystem::current_path(old);
    std::filesystem::remove_all(path);
  }
};

static void check_tracee_archive_compatibility()
{
  // Independent old member-wise wire encoder: never serialize syscall struct
  // padding, and include the suffix to catch block-size/alignment mistakes.
  const auto legacy = [](const tracee_state& state) {
    std::ostringstream output;
    {
      cereal::BinaryOutputArchive ar(output);
      const bool has_futex = state.futex_state.has_value();
      ar(has_futex);
      if (has_futex) ar(*state.futex_state);
      for (const auto& call : state.si) ar(call.nr, call.rval, call.args);
      ar(state.brk, state.tv.tv_sec, state.tv.tv_usec, state.fs_state,
         state.sock_state, state.process_state, state.fd_manager_state,
         state.threads, state.thread_create_records, state.main_tid,
         state.current_thread_idx);
    }
    return output.str();
  };
  const auto encode = [](const tracee_state& state) {
    std::ostringstream output;
    { cereal::BinaryOutputArchive ar(output); ar(state); }
    return output.str();
  };
  tracee_state fixture;
  for (size_t i = 0; i < MAX_THREADS_PER_PROCESS; ++i) {
    fixture.si[i].nr = i % 2 ? -int(i + 1) : int(0x70000000 + i);
    fixture.si[i].rval = uintptr_t(-4095) + i;
    for (size_t j = 0; j < 6; ++j)
      fixture.si[i].args[j] = 0xfedcba9800000000ULL | (i << 8) | j;
  }
  fixture.brk = 0x123456789abcdef0ULL;
  fixture.tv.tv_sec = 1234567;
  fixture.tv.tv_usec = 987654;
  fixture.process_state.random_state = 0xf123456789abcdefULL;
  for (bool with_futex : {false, true}) {
    if (with_futex) fixture.futex_state.emplace();
    const auto expected = legacy(fixture);
    require(encode(fixture) == expected, "tracee archive changed legacy member bytes");
    tracee_state decoded;
    std::istringstream input(expected);
    { cereal::BinaryInputArchive ar(input); ar(decoded); }
    require(legacy(decoded) == expected, "legacy tracee archive failed roundtrip");
    require(input.peek() == std::char_traits<char>::eof(), "tracee archive left trailing bytes");
    // First record and final record truncations, plus a truncated suffix.
    if (!with_futex) {
      constexpr size_t record_size = sizeof(int) + 7 * sizeof(uintptr_t);
      for (size_t length : {size_t(1), size_t(1 + record_size - 1),
                            size_t(1 + MAX_THREADS_PER_PROCESS * record_size - 1),
                            expected.size() - 1}) {
        rejects([&] {
          tracee_state incomplete;
          std::istringstream short_input(expected.substr(0, length));
          cereal::BinaryInputArchive ar(short_input); ar(incomplete);
        }, "truncated tracee archive accepted");
      }
    }
  }
}

int main()
{
  static_assert(NP == 2);
  loglevel = 4;
  try {
    StoreDirectory directory;
    StateStore::instance().init(StateStore::Config{});
    check_tracee_archive_compatibility();
    auto snapshot = [](uint64_t seed) {
      tracee_state saved;saved.process_state.random_state=seed;
      std::ostringstream output;{cereal::BinaryOutputArchive ar(output);ar(saved);}
      const auto bytes=output.str();
      const std::vector<uint8_t> metadata(bytes.begin(),bytes.end());
      const auto manifest=encode_page_manifest({},metadata,"",user_regs_struct{},{});
      return StateStore::instance().save(manifest.data(),manifest.size());
    };
    auto filesystem_snapshot = [](const FileSystemState& fs) {
      tracee_state saved;saved.fs_state=fs;
      std::ostringstream output;{cereal::BinaryOutputArchive ar(output);ar(saved);}
      const auto bytes=output.str();
      const std::vector<uint8_t> metadata(bytes.begin(),bytes.end());
      const auto manifest=encode_page_manifest({}, metadata, "", user_regs_struct{}, {});
      return StateStore::instance().save(manifest.data(),manifest.size());
    };
    FileSystemState empty_fs(std::make_shared<FdManager>());
    auto used_fs=empty_fs;
    const int created=used_fs.do_open("/temporary",O_CREAT|O_RDWR,0600);
    require(created>=0 && used_fs.do_close(created)==0 && used_fs.do_unlinkat(AT_FDCWD,"/temporary")==0,
            "create/delete inode allocator fixture");
    // Remove parent timestamp changes, leaving only the saved allocation cursor.
    used_fs.filesystem=empty_fs.filesystem;
    require(filesystem_snapshot(used_fs)!=filesystem_snapshot(empty_fs),
            "deleted object's inode allocator disappeared from machine identity");
    FileSystemState moved_fs;
    moved_fs.filesystem["/source"].metadata.st_mode=S_IFREG|0600;
    require(moved_fs.do_renameat2(AT_FDCWD,"/source",AT_FDCWD,"/target",0)==0,
            "initial inode relocation fixture");
    auto pathname_fs=moved_fs;
    pathname_fs.filesystem.at("/target").metadata.st_ino=0;
    require(filesystem_snapshot(moved_fs)!=filesystem_snapshot(pathname_fs),
            "preserved inode override disappeared from machine identity");
    sys_state original;
    original.ts_hash[0] = snapshot(1);
    original.ts_hash[1] = snapshot(2);
    original.status[0] = original.status[1] = dstatus_exit(0);
    original.error_bound = 5;
    auto ordered = original.identity_hash();
    const auto legacy_identity = [](const sys_state& state) {
      std::ostringstream encoded;
      { cereal::BinaryOutputArchive archive(encoded);
        const uint32_t domain=0x44535335, nodes=NP;
        archive(domain,nodes,state); }
      const auto bytes=encoded.str();
      return XXHash64::hash(bytes.data(),bytes.size());
    };
    const auto check_legacy_encoding = [&](sys_state state) {
      std::ostringstream payload;
      { cereal::BinaryOutputArchive archive(payload); archive(state); }
      const auto expected=payload.str();
      const auto key=legacy_identity(state);
      require(state.identity_hash()==key,"identity changed from legacy encoding");
      state.commit_metadata();
      std::vector<uint8_t> actual;
      require(state.ss_hash==key && SysStateStore::instance().load(key,actual) &&
              std::string(actual.begin(),actual.end())==expected,
              "commit changed legacy hash or persisted archive bytes");
    };
    auto encoding_fixture=original;
    encoding_fixture.error_bound=417;
    check_legacy_encoding(encoding_fixture);
    encoding_fixture.model_identity=std::string("model\0context",13);
    encoding_fixture.monitor_states["encoding-monitor"]={0,1,255,0,42};
    encoding_fixture.property_failed=true;
    check_legacy_encoding(encoding_fixture);
    auto swapped = original;
    std::swap(swapped.ts_hash[0], swapped.ts_hash[1]);
    require(swapped.identity_hash() != ordered, "node order disappeared from identity");
    auto exited = original;
    exited.status[0] = dstatus_exit(1);
    require(exited.identity_hash() != ordered, "exit status disappeared from identity");
    auto budget = original;
    budget.error_bound = 4;
    require(budget.identity_hash() != ordered, "remaining error budget disappeared from identity");
    auto repeated = original;
    repeated.ts_hash[0] = repeated.ts_hash[1] = original.ts_hash[0];
    auto other = repeated;
    other.ts_hash[0] = other.ts_hash[1] = original.ts_hash[1];
    require(repeated.identity_hash() != other.identity_hash(), "equal node hashes cancelled out");
    require(original.identity_hash() == ordered, "identity is not deterministic");
    auto monitored = original;
    monitored.monitor_states["monitor:v1"] = {1,2,3};
    require(monitored.identity_hash() != ordered, "monitor state omitted from identity");
    auto branch = monitored;
    branch.monitor_states.at("monitor:v1")[0] = 9;
    require(monitored.monitor_states.at("monitor:v1")[0] == 1 &&
            branch.identity_hash() != monitored.identity_hash(), "monitor branches alias or merge");
    auto failed = original;
    failed.property_failed = true;
    require(failed.identity_hash() != ordered, "terminal verdict omitted from identity");
    sys_state parent_a = original, parent_b = original;
    parent_a.error_bound = 4; parent_b.error_bound = 3;
    parent_a.ss_hash = parent_a.identity_hash(); parent_b.ss_hash = parent_b.identity_hash();
    auto passed_target = original; passed_target.ss_hash = passed_target.identity_hash();
    failed.ss_hash = failed.identity_hash();
    state_tree.clear();
    state_tree_add(parent_a, passed_target, 0, 0);
    state_tree_add(parent_b, failed, 0, 0);
    require(std::get<0>(state_tree.at(failed.ss_hash)) == parent_b.ss_hash,
            "failing edge reused an earlier valid witness");
    auto failed_live=failed;failed_live.status[0]=dstatus_normal();
    bool terminal_rejected=false;
    try {(void)state_transition(failed_live,0);}
    catch(const std::runtime_error& error) {
      terminal_rejected=std::string(error.what()).find("terminal property violation")!=std::string::npos;
    }
    require(terminal_rejected,"terminal property verdict did not reject at entry");
    original.ss_hash = ordered;
    require(original.save_metadata() == 1, "save original metadata");
    require(original.save_metadata() == 0, "identical metadata deduplicates");
    swapped.ss_hash = swapped.identity_hash();
    require(swapped.save_metadata() == 1, "swapped metadata not merged");
    sys_state loaded(original.ss_hash);
    require(loaded.ts_hash[0] == original.ts_hash[0] &&
            loaded.ts_hash[1] == original.ts_hash[1] &&
            loaded.error_bound == original.error_bound &&
            loaded.status[0] == original.status[0], "metadata round trip");
    auto dangling=original;dangling.ts_hash[0]=0x1111;dangling.commit_metadata();
    rejects([&] {sys_state missing_image(dangling.ss_hash);},
            "terminated node with missing snapshot was accepted");
    monitored.commit_metadata();
    sys_state monitor_loaded(monitored.ss_hash);
    require(monitor_loaded.monitor_states == monitored.monitor_states, "monitor archive roundtrip");
    auto model = monitored;
    model.model_identity = "different model";
    require(model.identity_hash() != monitored.identity_hash(), "model context omitted from identity");
    failed.commit_metadata();
    require(sys_state(failed.ss_hash).property_failed, "terminal verdict lost on reload");
    auto wrong = original;
    wrong.error_bound--;
    rejects([&] { wrong.save_metadata(); }, "modified metadata reused stale identity");
    rejects([] { sys_state missing(hash_type{0}); }, "zero identity accepted");

    std::ostringstream encoded;
    { cereal::BinaryOutputArchive archive(encoded); archive(original); }
    const std::string bytes = encoded.str();
    // Inject a valid archive under a conflicting key, exercising the boundary
    // without relying on finding a probabilistic 64-bit hash collision.
    auto &store = SysStateStore::instance();
    require(store.save(budget.identity_hash(), {bytes.begin(), bytes.end()}), "inject metadata mismatch");
    rejects([&] { sys_state wrong_key(budget.identity_hash()); }, "wrong-key metadata accepted");
    budget.ss_hash = budget.identity_hash();
    rejects([&] { budget.save_metadata(); }, "conflicting metadata silently merged");
    std::puts("PASS system identity: ordered nodes, exit/budget, no XOR cancellation, metadata round trip, conflicts rejected");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL system identity: %s\n", error.what());
    return 1;
  }
}
