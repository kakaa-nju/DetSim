#include "snapshot_bundle.h"
#include "shared_object_pool.h"
#include <sys/syscall.h>
#include <type_traits>
#include "proc_status.h"
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
// Linker wrappers count the actual calls, including warm-cache publication.
static std::atomic<size_t> page_calls{0}, blob_publications{0};
static std::atomic<bool> fail_ready_publication{false};
extern "C" SnapshotView::OwnedSpan real_snapshot_page(const SnapshotView*, size_t, size_t)
    asm("__real__ZNK12SnapshotView4pageEmm");
extern "C" SnapshotView::OwnedSpan wrapped_snapshot_page(const SnapshotView*, size_t, size_t)
    asm("__wrap__ZNK12SnapshotView4pageEmm");
extern "C" SnapshotView::OwnedSpan wrapped_snapshot_page(const SnapshotView* view, size_t region, size_t page) {
    ++page_calls;
    return real_snapshot_page(view, region, page);
}
extern "C" void real_publish_blob(SharedObjectPool*, hash_type, const void*, size_t)
    asm("__real__ZN6detsim8snapshot16SharedObjectPool12publish_blobEmPKvm");
extern "C" void wrapped_publish_blob(SharedObjectPool*, hash_type, const void*, size_t)
    asm("__wrap__ZN6detsim8snapshot16SharedObjectPool12publish_blobEmPKvm");
extern "C" void wrapped_publish_blob(SharedObjectPool* pool, hash_type key, const void* bytes, size_t size) {
    ++blob_publications;
    real_publish_blob(pool, key, bytes, size);
}
extern "C" void real_publish_ready(SharedObjectPool*, hash_type, const std::vector<uint8_t>&)
    asm("__real__ZN6detsim8snapshot16SharedObjectPool13publish_readyEmRKSt6vectorIhSaIhEE");
extern "C" void wrapped_publish_ready(SharedObjectPool*, hash_type, const std::vector<uint8_t>&)
    asm("__wrap__ZN6detsim8snapshot16SharedObjectPool13publish_readyEmRKSt6vectorIhSaIhEE");
extern "C" void wrapped_publish_ready(SharedObjectPool* pool, hash_type key, const std::vector<uint8_t>& bytes) {
    if (fail_ready_publication.exchange(false))
        throw std::runtime_error("Injected failure at Ready publication boundary");
    real_publish_ready(pool, key, bytes);
}
static_assert(!std::is_copy_constructible_v<SharedPublisher> && !std::is_move_constructible_v<SharedPublisher>);
static_assert(!std::is_copy_assignable_v<SharedPublisher> && !std::is_move_assignable_v<SharedPublisher>);
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
static hash_type make_root(bool pages, char memory_value = 'm') {
    tracee_state saved;
    saved.threads.resize(2);
    for (int slot=0; slot<2; ++slot) {
        saved.threads[slot].alive = true; saved.threads[slot].tid = slot + 1;
    }
    PendingSyscall pending; pending.nr = SYS_nanosleep; pending.reason = PendingReason::Timer;
    pending.args[0] = 0x1000; pending.deadline = PendingDeadline{CLOCK_MONOTONIC, 1, 0, 0};
    saved.threads[1].pending_syscall = pending;
    auto& file = saved.fs_state.filesystem["/payload"];
    file.metadata.st_mode = S_IFREG | 0600;
    file.content.assign(4096, 'v'); file.metadata.st_size = file.content.size();
    saved.validate_io_state();
    std::vector<uint8_t> metadata;
    { FileContentSnapshotScope scope; metadata = encode(saved); }
    RegionInfo region{}; region.start = 0x1000; region.end = 0x2000; region.prot = PROT_READ;
    memcpy(region.flags, "r--p", 5);
    std::vector<uint8_t> memory(4096, memory_value); user_regs_struct regs{};
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

static void check_actors(const ReadyState& ready) {
    require(ready.actors.size()==2*NP, "All typed actors preserved");
    for (int node=0; node<NP; ++node) {
        const auto& normal=ready.actors[node*2]; const auto& timer=ready.actors[node*2+1];
        require(normal.process==node && normal.thread==0 && normal.action==TransitionAction::Normal &&
                timer.process==node && timer.thread==1 && timer.action==TransitionAction::Timeout,
                "Canonical process/thread/action order preserved");
    }
}
static hash_type new_system(hash_type root, bool failed = false, bool first_dead = false,
                            hash_type last_root = 0) {
    static uint64_t nonce = 0;
    sys_state state;
    for (auto& key : state.ts_hash) key = root;
    if (last_root) state.ts_hash[NP - 1] = last_root;
    state.model_identity = "bundle-fixture/model1";
    state.monitor_states["property"] = {1,2,3};
    state.monitor_states["nonce"] = encode(++nonce);
    state.property_failed = failed;
    if (first_dead) state.status[0] = DIFEXITED;
    state.ss_hash = state.identity_hash();
    state.save_metadata();
    return state.ss_hash;
}
static void reset_calls() { page_calls = 0; blob_publications = 0; }
static void full_calls() {
    require(page_calls.load() > 0 && blob_publications.load() > 0,
            "Cold proof must perform actual page validation and blob publication");
}
static void hit_calls() {
    require(page_calls.load() == 0 && blob_publications.load() == 0,
            "Root proof hit must skip page access and blob publication");
}
static void publisher_proofs(bool pages, const std::filesystem::path& directory) {
    const auto a = make_root(pages), b = make_root(pages, 'n'), c = make_root(pages, 'o');
    SharedObjectPool pool(directory.string());
    SharedPublisher publisher(pool);
    const auto first = new_system(a);
    reset_calls(); publisher.export_bundle(first); full_calls();
    const auto second = new_system(a);
    require(first != second, "Use fresh system keys, not whole-system ready hits");
    reset_calls(); const auto cached = publisher.export_bundle(second); hit_calls();
    check_actors(load_shared_state(cached.system_key, "bundle-fixture/model1", pool));
    // Global admission still comes from this system, not a cached actor list.
    reset_calls(); const auto failed = publisher.export_bundle(new_system(a, true)); hit_calls();
    require(load_shared_state(failed.system_key, "bundle-fixture/model1", pool).actors.empty(),
            "Property failure must suppress actors despite root proof hit");
    reset_calls(); const auto exited = publisher.export_bundle(new_system(a, false, true)); hit_calls();
    const auto admitted = load_shared_state(exited.system_key, "bundle-fixture/model1", pool);
    require(admitted.actors.size() == 2 * (NP - 1), "Status must filter cached-root actors");
    for (const auto& actor : admitted.actors) require(actor.process != 0, "Exited process admitted");

    // cap=0 and the free function remain independent complete-validation paths.
    SharedPublisher uncached(pool, 0);
    for (int i = 0; i < 2; ++i) {
        reset_calls(); uncached.export_bundle(new_system(a)); full_calls();
    }
    reset_calls(); export_shared_bundle(new_system(a), pool); full_calls();
    SharedObjectPool reference((directory.string() + "-reference"));
    const auto expected = export_shared_bundle(second, reference);
    require(expected.metadata == cached.metadata && *reference.load_ready(second) == *pool.load_ready(second),
            "Memoized and stateless paths must publish identical ready bytes");

    // Changing just one process must validate that root without rewalking a.
    publisher.export_bundle(new_system(a, false, false, b));
    const auto mixed_key = new_system(a, false, false, c);
    reset_calls(); const auto mixed = publisher.export_bundle(mixed_key); full_calls();
    const auto mixed_pages = page_calls.load(), mixed_blobs = blob_publications.load();
    reset_calls(); const auto mixed_reference = export_shared_bundle(mixed_key, reference); full_calls();
    require(mixed_pages < page_calls.load() && mixed_blobs < blob_publications.load(),
            "Mixed-root proof must avoid validating the unchanged process");
    require(mixed.metadata == mixed_reference.metadata &&
            *pool.load_ready(mixed_key) == *reference.load_ready(mixed_key),
            "Mixed-root publication must match independent stateless validation");
    reset_calls(); publisher.export_bundle(new_system(a, false, false, c)); hit_calls();

    SharedPublisher tiny(pool, 1);
    reset_calls(); tiny.export_bundle(new_system(a)); full_calls();
    reset_calls(); tiny.export_bundle(new_system(b)); full_calls();
    reset_calls(); tiny.export_bundle(new_system(a)); full_calls(); // a was evicted.
    reset_calls(); tiny.export_bundle(new_system(a)); hit_calls();

    // The first root's objects can be published while Ready itself fails. That
    // failure must not install b's proof or evict the existing a proof.
    const auto rejected = new_system(b);
    fail_ready_publication = true;
    reset_calls(); rejects([&] { tiny.export_bundle(rejected); }); full_calls();
    require(!pool.load_ready(rejected), "Failed publication must not expose Ready");
    reset_calls(); tiny.export_bundle(new_system(a)); hit_calls();
    reset_calls(); tiny.export_bundle(rejected); full_calls();
    reset_calls(); tiny.export_bundle(new_system(b)); hit_calls();

    // A failed cache hit must not promote LRU ordering either.
    SharedPublisher ordering(pool, 2);
    ordering.export_bundle(new_system(a)); ordering.export_bundle(new_system(b));
    fail_ready_publication = true;
    reset_calls(); rejects([&] { ordering.export_bundle(new_system(a)); }); hit_calls();
    ordering.export_bundle(new_system(c)); // Evicts a, not b.
    reset_calls(); ordering.export_bundle(new_system(b)); hit_calls();
    reset_calls(); ordering.export_bundle(new_system(a)); full_calls();

    // No static registry can carry proofs into a new context or pool instance.
    SharedPublisher fresh_context(pool);
    reset_calls(); fresh_context.export_bundle(new_system(a)); full_calls();
    SharedObjectPool same_path(directory.string(), false);
    SharedPublisher other_instance(same_path);
    reset_calls(); other_instance.export_bundle(new_system(a)); full_calls();
    pool.flush();
    fail_sync = true;
    rejects([&] { pool.flush(); });
    fail_sync = false;
    reset_calls(); rejects([&] { publisher.export_bundle(new_system(a)); }); hit_calls();
}
int main(int argc,char** argv) {
    try {
        if(argc==5 && !strcmp(argv[1],"--cold")) {
            start(argv[2]); SharedObjectPool pool(argv[3],false,true);
            StateStore::instance().attach_shared_pool(argv[3],false);
            const auto key=std::stoull(argv[4]);
            const auto before=StateStore::instance().get_stats().load_requests;
            auto bare=load_shared_state(key,"bundle-fixture/model1",pool); check_actors(bare);
            require(StateStore::instance().get_stats().load_requests==before,"Bare ready must not load snapshots");
            for(const auto& child:bare.state.child)require(child.threads.empty(),"Bare metadata has no eager child");
            Bundle ref;ref.version=2;ref.system_key=key;ref.metadata=encode(bare.state);
            fail_sync=true;
            check_actors(import_shared_bundle(ref,"bundle-fixture/model1",pool));
            fail_sync=false; // The new registration path must not implicitly flush.
            auto full=import_shared_bundle(ref,"bundle-fixture/model1",pool,true);
            check_state(full.state);check_actors(full);
            check_state(sys_state(key)); // Existing snapshot-load/replay admission works after registration.
            stop();return 0;
        }
        char temporary[]="/tmp/detsim-ready-XXXXXX";require(mkdtemp(temporary),"mkdtemp");
        const std::filesystem::path base(temporary),original=std::filesystem::current_path();
        for(bool pages:{false,true}) {
            const std::string suffix=pages?"pages":"full";
            const auto source=base/(suffix+"-private"),pool_path=base/(suffix+"-pool");
            start(source,1<<20);const auto bundle=fixture(pages);
            SharedObjectPool pool(pool_path.string());
            rejects([&]{load_shared_state(bundle.system_key,"bundle-fixture/model1",pool);});
            publisher_proofs(pages, base/(suffix+"-publisher-pool"));
            const auto ref=export_shared_bundle(bundle.system_key,pool);
            require(ref.version==2 && ref.blobs.empty() && ref.metadata==bundle.metadata,"Reference carries metadata only");
            const auto before=StateStore::instance().get_stats().load_requests;
            check_actors(load_shared_state(ref.system_key,"bundle-fixture/model1",pool));
            require(before==StateStore::instance().get_stats().load_requests,"Ready read does not scan closure");
            publish_shared_state(bundle.system_key,pool); // Idempotent certified publication.
            for(const auto& blob:bundle.blobs)require(pool.load_blob(blob.key) && *pool.load_blob(blob.key)==blob.bytes,
                                                     "Every actual closure blob is globally present");
            rejects([&]{load_shared_state(ref.system_key,"",pool);});
            rejects([&]{load_shared_state(ref.system_key,"different",pool);});
            rejects([&]{load_shared_state(ref.system_key+1,"bundle-fixture/model1",pool);});
            auto bad=ref;bad.metadata.push_back(0);
            rejects([&]{import_shared_bundle(bad,"bundle-fixture/model1",pool);});
            bad=ref;bad.version=1;rejects([&]{import_shared_bundle(bad,"bundle-fixture/model1",pool);});
            bad=ref;bad.blobs=bundle.blobs;rejects([&]{import_shared_bundle(bad,"bundle-fixture/model1",pool);});
            const auto record=*pool.load_ready(ref.system_key);
            SharedObjectPool invalid((base/(suffix+"-invalid-ready")).string());
            auto corrupt=record;corrupt.back()^=1;invalid.publish_ready(ref.system_key,corrupt);
            rejects([&]{load_shared_state(ref.system_key,"bundle-fixture/model1",invalid);});
            SharedObjectPool bad_actor((base/(suffix+"-invalid-actor")).string());
            corrupt=record;
            // First actor action follows the 28-byte header, metadata and process/thread.
            int32_t invalid_action=42;memcpy(corrupt.data()+28+ref.metadata.size()+8,&invalid_action,4);
            const auto checksum=XXHash64::hash(corrupt.data(),corrupt.size()-8);
            memcpy(corrupt.data()+corrupt.size()-8,&checksum,8);bad_actor.publish_ready(ref.system_key,corrupt);
            rejects([&]{load_shared_state(ref.system_key,"bundle-fixture/model1",bad_actor);});
            SharedObjectPool bad_order((base/(suffix+"-invalid-order")).string());
            corrupt=record;int32_t duplicate_thread=0;
            memcpy(corrupt.data()+28+ref.metadata.size()+12+4,&duplicate_thread,4);
            const auto order_checksum=XXHash64::hash(corrupt.data(),corrupt.size()-8);
            memcpy(corrupt.data()+corrupt.size()-8,&order_checksum,8);
            bad_order.publish_ready(ref.system_key,corrupt);
            rejects([&]{load_shared_state(ref.system_key,"bundle-fixture/model1",bad_order);});
            SharedObjectPool wrong_key((base/(suffix+"-wrong-key")).string());
            wrong_key.publish_ready(ref.system_key+1,record);
            rejects([&]{load_shared_state(ref.system_key+1,"bundle-fixture/model1",wrong_key);});
            pool.flush();stop();
            // The pool generation remains immutable; private worker stores do not.
            std::filesystem::remove_all(source);
            const auto child=fork();require(child>=0,"fork cold reader");
            if(!child) {
                const auto key=std::to_string(ref.system_key);const auto destination=base/(suffix+"-reader");
                execl("/proc/self/exe","ready-reader","--cold",destination.c_str(),pool_path.c_str(),key.c_str(),nullptr);
                _exit(127);
            }
            int status=0;require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
                                 "Cold read-only pool survives deleted worker private store");
            // Warm private bytes cannot certify a pool that has no ready record.
            start(base/(suffix+"-unready"),1<<20);
            for(const auto& blob:bundle.blobs)StateStore::instance().save(blob.bytes.data(),blob.bytes.size());
            SharedObjectPool unready((base/(suffix+"-unready-pool")).string());
            rejects([&]{import_shared_bundle(ref,"bundle-fixture/model1",unready);});
            require(!SysStateStore::instance().exists(ref.system_key),"Unready import cannot publish metadata");
            stop();
            start(base/(suffix+"-metadata-conflict"));
            require(SysStateStore::instance().save(ref.system_key,{9,9,9}),"Install conflicting metadata");
            rejects([&]{import_shared_bundle(ref,"bundle-fixture/model1",pool);});
            std::vector<uint8_t> previous;
            require(SysStateStore::instance().load(ref.system_key,previous) && previous==std::vector<uint8_t>({9,9,9}),
                    "Reference import must preserve conflicting existing metadata");stop();
            // A real incomplete closure cannot publish its ready certificate.
            start(base/(suffix+"-missing-dependency"));
            // Install only roots; external VFS (and page payload) remain absent.
            sys_state bare;std::istringstream encoded(std::string(ref.metadata.begin(),ref.metadata.end()));
            {cereal::BinaryInputArchive archive(encoded);archive(bare);}
            for(const auto& blob:bundle.blobs)
                if(std::find(std::begin(bare.ts_hash),std::end(bare.ts_hash),blob.key)!=std::end(bare.ts_hash))
                    StateStore::instance().save(blob.bytes.data(),blob.bytes.size());
            require(SysStateStore::instance().save(ref.system_key,ref.metadata),"Install fixture metadata");
            SharedObjectPool missing((base/(suffix+"-missing-pool")).string());
            rejects([&]{publish_shared_state(ref.system_key,missing);});
            require(!missing.load_ready(ref.system_key),"Incomplete closure must not become ready");stop();
        }
        std::filesystem::current_path(original);std::filesystem::remove_all(base);
        puts("shared ready: full/pages VFS closure, typed actors, lazy references, corruption, missing closure, cold read-only reopen passed");
        return 0;
    }catch(const std::exception& error){fprintf(stderr,"shared ready test: %s\n",error.what());return 1;}
}
