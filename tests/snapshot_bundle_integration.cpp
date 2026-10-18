#include "snapshot_bundle.h"
#include "page_snapshot.h"
#include "sysstate_store.h"
#include "utils.h"
#include "ncurses_ui.h"
#include "cereal/archives/binary.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace detsim::snapshot;
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static std::atomic<bool> fail_sync{false};
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (fail_sync.load()) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
static void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> static void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Expected explicit rejection");
}
static void start(const std::filesystem::path& directory, size_t cache = 0) {
    std::filesystem::create_directories(directory);
    std::filesystem::current_path(directory);
    StateStore::Config config; config.hot_cache_size = cache; config.warm_cache_size = cache;
    config.prefetch_threads = 0;
    StateStore::instance().init(config);
    SysStateStore::instance().init();
}
static void stop() {
    SysStateStore::instance().shutdown();
    StateStore::instance().shutdown();
}
template<class T> static std::vector<uint8_t> encode(const T& object) {
    std::ostringstream out;
    { cereal::BinaryOutputArchive archive(out); archive(object); }
    const auto bytes = out.str(); return {bytes.begin(), bytes.end()};
}
static hash_type make_root(bool pages) {
    tracee_state saved;
    auto& file = saved.fs_state.filesystem["/payload"];
    file.metadata.st_mode = S_IFREG | 0600;
    file.content.assign(4096, 'v'); file.metadata.st_size = file.content.size();
    saved.validate_io_state();
    std::vector<uint8_t> metadata;
    { FileContentSnapshotScope scope; metadata = encode(saved); }
    RegionInfo region{}; region.start = 0x1000; region.end = 0x2000; region.prot = PROT_READ;
    memcpy(region.flags, "r--p", 5);
    std::vector<uint8_t> memory(4096, 'm'); user_regs_struct regs{};
    std::vector<uint8_t> root;
    auto& store = StateStore::instance();
    if (pages) {
        const auto key = store.save(memory.data(), memory.size());
        root = encode_page_manifest({region}, metadata, "", regs, {{0x1000, 4096, key}});
    } else {
        StateDataHeader header{}; header.magic = 0x4453544D; header.version = FLAT_SNAPSHOT_VERSION;
        header.num_regions = 1; header.tstate_offset = sizeof(header) + sizeof(region);
        header.tstate_size = metadata.size(); header.maps_offset = header.tstate_offset + metadata.size();
        region.offset = header.maps_offset; header.regs_offset = region.offset + memory.size();
        root.resize(header.regs_offset + sizeof(regs));
        memcpy(root.data(), &header, sizeof(header));
        memcpy(root.data() + sizeof(header), &region, sizeof(region));
        memcpy(root.data() + header.tstate_offset, metadata.data(), metadata.size());
        memcpy(root.data() + region.offset, memory.data(), memory.size());
        memcpy(root.data() + header.regs_offset, &regs, sizeof(regs));
    }
    return store.save(root.data(), root.size());
}
static Bundle fixture(bool pages) {
    sys_state state;
    const auto root = make_root(pages);
    for (int i = 0; i < NP; ++i) state.ts_hash[i] = root;
    state.model_identity = "bundle-fixture/model1";
    state.monitor_states["property"] = {1,2,3};
    state.ss_hash = state.identity_hash(); state.save_metadata();
    StateStore::instance().wait_for_completion(); SysStateStore::instance().flush_index();
    const char orphan[] = "unused object"; StateStore::instance().save(orphan, sizeof(orphan));
    auto bundle = export_bundle(state.ss_hash);
    require(bundle.blobs.size() == (pages ? 3u : 2u), "Only unique root/page/VFS closure exported");
    return bundle;
}
static Bundle replace_root(Bundle bundle, std::vector<uint8_t> bytes) {
    sys_state state;
    std::istringstream input(std::string(bundle.metadata.begin(), bundle.metadata.end()));
    { cereal::BinaryInputArchive archive(input); archive(state); }
    const auto old = state.ts_hash[0];
    const auto key = XXHash64::hash(bytes.data(), bytes.size());
    for (auto& blob : bundle.blobs) if (blob.key == old) blob = {key, bytes};
    for (auto& root : state.ts_hash) root = key;
    bundle.system_key = state.identity_hash(); bundle.metadata = encode(state);
    return bundle;
}
static void check_state(const sys_state& state) {
    require(state.monitor_states.at("property") == std::vector<uint8_t>({1,2,3}), "Monitor state preserved");
    for (int i = 0; i < NP; ++i) {
        const auto& file = state.child[i].fs_state.filesystem.at("/payload");
        require(file.content.size() == 4096 && file.content[4095] == 'v', "External VFS bytes preserved");
        require(file.content.saved_blob_key() != 0, "Saved file reference retained");
        char memory = 0; SnapshotView::open(state.ts_hash[i])->read_into(0x1fff, &memory, 1);
        require(memory == 'm', "Saved memory preserved");
    }
}
int main(int argc, char** argv) {
    try {
        if (argc == 4 && !strcmp(argv[1], "--read")) {
            start(argv[2]); auto state = sys_state(std::stoull(argv[3])); check_state(state); stop(); return 0;
        }
        char temporary[] = "/tmp/detsim-bundle-XXXXXX";
        require(mkdtemp(temporary), "mkdtemp");
        const std::filesystem::path directory(temporary), original = std::filesystem::current_path();
        for (bool pages : {false, true}) {
            const auto suffix = pages ? "pages" : "full";
            start(directory / (std::string("source-") + suffix));
            const auto bundle = fixture(pages); stop();
            start(directory / (std::string("negative-") + suffix), 1024 * 1024);
            // Destination data is deliberately warm, but there is no metadata.
            for (const auto& blob : bundle.blobs) StateStore::instance().save(blob.bytes.data(), blob.bytes.size());
            StateStore::instance().wait_for_completion();
            const auto unchanged = [&] { require(!SysStateStore::instance().exists(bundle.system_key), "Invalid import published metadata"); };
            for (size_t removed = 0; removed < bundle.blobs.size(); ++removed) {
                auto bad = bundle; bad.blobs.erase(bad.blobs.begin() + removed);
                rejects([&] { import_bundle(bad, "bundle-fixture/model1"); }); unchanged();
            }
            rejects([&] { import_bundle(bundle, "wrong model"); }); unchanged();
            auto bad = bundle; bad.blobs[0].bytes[0] ^= 1;
            rejects([&] { import_bundle(bad, "bundle-fixture/model1"); }); unchanged();
            bad = bundle; bad.blobs.push_back(bad.blobs[0]); bad.blobs.back().bytes[0] ^= 1;
            rejects([&] { import_bundle(bad, "bundle-fixture/model1"); }); unchanged();
            bad = bundle; bad.metadata.push_back(0);
            rejects([&] { import_bundle(bad, "bundle-fixture/model1"); }); unchanged();
            bad = bundle; bad.version = 99;
            rejects([&] { import_bundle(bad, "bundle-fixture/model1"); }); unchanged();
            // Correct content hash with invalid root format still rejects.
            bad = bundle;
            auto corrupt_root = bad.blobs.front().bytes;
            corrupt_root[0] ^= 1;
            bad = replace_root(bad, std::move(corrupt_root));
            rejects([&] { import_bundle(bad, "bundle-fixture/model1"); });
            require(!SysStateStore::instance().exists(bad.system_key), "Corrupt root published metadata");
            bad = bundle;
            Blob extra{0, {'e','x','t','r','a'}};
            extra.key = XXHash64::hash(extra.bytes.data(), extra.bytes.size());
            bad.blobs.push_back(extra);
            rejects([&] { import_bundle(bad, "bundle-fixture/model1"); }); unchanged();
            rejects([&] { import_bundle(bundle, ""); }); unchanged();
            stop();
            start(directory / (std::string("metadata-conflict-") + suffix));
            require(SysStateStore::instance().save(bundle.system_key, {9,9,9}), "Insert conflicting metadata fixture");
            rejects([&] { import_bundle(bundle, "bundle-fixture/model1"); });
            std::vector<uint8_t> previous;
            require(SysStateStore::instance().load(bundle.system_key, previous) && previous == std::vector<uint8_t>({9,9,9}),
                    "Conflicting metadata must not be overwritten");
            stop();
            const auto destination = directory / (std::string("destination-") + suffix);
            start(destination);
            check_state(import_bundle(bundle, "bundle-fixture/model1"));
            auto duplicate = bundle; duplicate.blobs.push_back(bundle.blobs[0]);
            check_state(import_bundle(duplicate, "bundle-fixture/model1"));
            require(SysStateStore::instance().get_entry_count() == 1, "Idempotent metadata import");
            stop();
            const auto child = fork(); require(child >= 0, "fork reader");
            if (!child) {
                const auto key = std::to_string(bundle.system_key);
                execl("/proc/self/exe", "bundle-reader", "--read", destination.c_str(), key.c_str(), nullptr);
                _exit(127);
            }
            int status = 0;
            require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "Fresh process reads complete imported closure");
            start(directory / (std::string("io-failure-") + suffix));
            fail_sync = true;
            rejects([&] { import_bundle(bundle, "bundle-fixture/model1"); });
            fail_sync = false;
            require(!SysStateStore::instance().exists(bundle.system_key), "Data barrier failure published metadata");
            rejects([&] { StateStore::instance().load_shared(bundle.blobs[0].key); });
            SysStateStore::instance().shutdown();
            try { StateStore::instance().shutdown(); } catch (const std::exception&) {}
        }
        std::filesystem::current_path(original); std::filesystem::remove_all(directory);
        puts("snapshot bundle: full/pages closure, fresh process, warm omissions, idempotence, corruption and I/O failure passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "bundle test: %s\n", error.what()); return 1;
    }
}
