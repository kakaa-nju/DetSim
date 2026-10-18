#include "core/search/object_transport.h"
#include "page_snapshot.h"
#include "sysstate_store.h"
#include "utils.h"
#include "ncurses_ui.h"
#include "cereal/archives/binary.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <sstream>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
namespace wire = detsim::search::wire;
using namespace detsim::snapshot;
extern "C" detsim::ui::NCursesUI* get_ncurses_ui() { return nullptr; }
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void start(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory); std::filesystem::current_path(directory);
    StateStore::Config config; config.hot_cache_size = config.warm_cache_size = 0; config.prefetch_threads = 0;
    StateStore::instance().init(config); SysStateStore::instance().init();
}
static void stop() { SysStateStore::instance().shutdown(); StateStore::instance().shutdown(); }
template<class T> static std::vector<uint8_t> encode(const T& value) {
    std::ostringstream stream; { cereal::BinaryOutputArchive archive(stream); archive(value); }
    const auto text = stream.str(); return {text.begin(), text.end()};
}
static Bundle fixture(bool pages) {
    tracee_state saved;
    auto& file = saved.fs_state.filesystem["/shared-payload"];
    file.metadata.st_mode = S_IFREG | 0600; file.content.assign(4096, 'v'); file.metadata.st_size = file.content.size();
    saved.validate_io_state();
    std::vector<uint8_t> metadata; { FileContentSnapshotScope scope; metadata = encode(saved); }
    RegionInfo region{}; region.start = 0x1000; region.end = 0x2000; region.prot = PROT_READ; memcpy(region.flags, "r--p", 5);
    const std::vector<uint8_t> memory(4096, 'm'); user_regs_struct regs{};
    auto& store = StateStore::instance(); std::vector<uint8_t> root;
    if (pages) {
        const auto key = store.save(memory.data(), memory.size());
        root = encode_page_manifest({region}, metadata, "", regs, {{0x1000,4096,key}});
    } else {
        StateDataHeader header{}; header.magic = 0x4453544D; header.version = FLAT_SNAPSHOT_VERSION; header.num_regions = 1;
        header.tstate_offset = sizeof(header) + sizeof(region); header.tstate_size = metadata.size();
        header.maps_offset = header.tstate_offset + metadata.size(); region.offset = header.maps_offset;
        header.regs_offset = region.offset + memory.size(); root.resize(header.regs_offset + sizeof(regs));
        memcpy(root.data(), &header, sizeof(header)); memcpy(root.data()+sizeof(header), &region, sizeof(region));
        memcpy(root.data()+header.tstate_offset, metadata.data(), metadata.size());
        memcpy(root.data()+region.offset, memory.data(), memory.size()); memcpy(root.data()+header.regs_offset, &regs, sizeof(regs));
    }
    sys_state state; const auto key = store.save(root.data(), root.size());
    for (auto& node : state.ts_hash) node = key;
    state.model_identity = "parallel-bundle-fixture/model1"; state.monitor_states["property"] = {1,2,3};
    state.ss_hash = state.identity_hash(); state.save_metadata();
    store.wait_for_completion(); SysStateStore::instance().flush_index();
    return export_bundle(state.ss_hash);
}
static void check(const sys_state& state) {
    require(state.model_identity == "parallel-bundle-fixture/model1" && state.monitor_states.at("property") == std::vector<uint8_t>({1,2,3}), "semantic metadata changed");
    for (int node = 0; node < NP; ++node) {
        const auto& file = state.child[node].fs_state.filesystem.at("/shared-payload");
        require(file.content.size() == 4096 && file.content[4095] == 'v' && file.content.saved_blob_key(), "external VFS payload changed");
        std::vector<char> memory(4096); SnapshotView::open(state.ts_hash[node])->read_into(0x1000,memory.data(),memory.size());
        require(memory == std::vector<char>(4096,'m'), "snapshot memory changed");
    }
}
static void transfer(const Bundle& exported) {
    int stream[2], data[2]; require(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,stream), "stream pair");
    require(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,data), "data pair");
    wire::Channel sender(stream[0],{},data[0]), receiver(stream[1],{},data[1]);
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto sent = std::async(std::launch::async,[&] { sender.send(wire::Task{{exported.system_key,1,2,0,true},exported}); });
        try {
            auto message = receiver.receive(); require(message && std::holds_alternative<wire::Task>(*message), "missing Bundle task");
            const auto& received = std::get<wire::Task>(*message).bundle;
            require(received.metadata == exported.metadata && received.system_key == exported.system_key && received.blobs.size() == exported.blobs.size(), "closure changed");
            for (size_t i = 0; i < received.blobs.size(); ++i) {
                require(received.blobs[i].key == exported.blobs[i].key && received.blobs[i].bytes == exported.blobs[i].bytes, "object changed");
                require(received.blobs[i].key == XXHash64::hash(received.blobs[i].bytes.data(),received.blobs[i].bytes.size()), "production hash differs");
            }
            check(import_bundle(received,"parallel-bundle-fixture/model1")); receiver.confirm_objects(); sent.get();
        } catch (...) { receiver.abort_objects(); if (sent.valid()) sent.wait(); throw; }
    }
    uint64_t total = 0; for (const auto& blob : exported.blobs) total += blob.bytes.size();
    const auto objects = receiver.object_statistics();
    require(sender.counters().shared_frames == 2 && sender.counters().raw_fallback_frames == 0, "real Bundle did not use V2");
    require(objects.reference_bytes == total && objects.missing_bytes == total, "real cold/hot closure coverage differs");
}
int main(int argc,char** argv) {
    try {
        require(!setenv("DETSIM_SHARED_OBJECTS", "on", 1), "enable real shared Bundle transfer");
        if (argc == 4 && !strcmp(argv[1],"--read")) { start(argv[2]); check(sys_state(std::stoull(argv[3]))); stop(); return 0; }
        char temporary[] = "/tmp/detsim-parallel-bundle-XXXXXX"; require(mkdtemp(temporary),"mkdtemp");
        const std::filesystem::path directory(temporary), original = std::filesystem::current_path();
        for (bool pages : {false,true}) {
            const std::string suffix = pages ? "pages" : "full";
            start(directory/("source-"+suffix)); const auto exported = fixture(pages); stop();
            require(exported.blobs.size() == (pages ? 3u : 2u), "real export closure shape changed");
            const auto destination = directory/("destination-"+suffix); start(destination); transfer(exported); stop();
            const auto child = fork(); require(child >= 0,"fork reader");
            if (!child) { const auto key = std::to_string(exported.system_key); execl("/proc/self/exe","parallel-bundle-reader","--read",destination.c_str(),key.c_str(),nullptr); _exit(127); }
            int status{}; require(waitpid(child,&status,0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,"fresh process read failed");
        }
        std::filesystem::current_path(original); std::filesystem::remove_all(directory);
        puts("parallel Bundle: real full/pages export -> V2 cold/hot -> validated import -> fresh-process read PASS"); return 0;
    } catch (const std::exception& error) { fprintf(stderr,"parallel Bundle test: %s\n",error.what()); return 1; }
}
