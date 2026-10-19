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
#include <set>
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
static std::atomic<hash_type> fail_ready_key{0}, watched_ready_key{0};
static std::atomic<size_t> watched_ready_reads{0};
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
    if (fail_ready_publication.exchange(false) || (key && fail_ready_key.load() == key && fail_ready_key.exchange(0)))
        throw std::runtime_error("Injected failure at Ready publication boundary");
    real_publish_ready(pool, key, bytes);
}
extern "C" SharedObjectPool::Buffer real_load_ready(SharedObjectPool*, hash_type)
    asm("__real__ZN6detsim8snapshot16SharedObjectPool10load_readyEm");
extern "C" SharedObjectPool::Buffer wrapped_load_ready(SharedObjectPool*, hash_type)
    asm("__wrap__ZN6detsim8snapshot16SharedObjectPool10load_readyEm");
extern "C" SharedObjectPool::Buffer wrapped_load_ready(SharedObjectPool* pool, hash_type key) {
    if (key && key == watched_ready_key.load()) ++watched_ready_reads;
    return real_load_ready(pool, key);
}
static std::atomic<bool> fail_registration{false};
extern "C" bool real_metadata_save(SysStateStore*, hash_type, const std::vector<uint8_t>&)
    asm("__real__ZN13SysStateStore4saveEmRKSt6vectorIhSaIhEE");
extern "C" bool wrapped_metadata_save(SysStateStore*, hash_type, const std::vector<uint8_t>&)
    asm("__wrap__ZN13SysStateStore4saveEmRKSt6vectorIhSaIhEE");
extern "C" bool wrapped_metadata_save(SysStateStore* store, hash_type key, const std::vector<uint8_t>& bytes) {
    if (fail_registration.exchange(false)) return false;
    return real_metadata_save(store, key, bytes);
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
                            hash_type last_root = 0, const std::string& model = "bundle-fixture/model1") {
    static uint64_t nonce = 0;
    sys_state state;
    for (auto& key : state.ts_hash) key = root;
    if (last_root) state.ts_hash[NP - 1] = last_root;
    state.model_identity = model;
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
static void publisher_import_proofs(bool pages, const std::filesystem::path& directory) {
    const std::string model = "bundle-fixture/model1";
    const auto a = make_root(pages), b = make_root(pages, 'n'), c = make_root(pages, 'o');
    SharedObjectPool pool(directory.string());
    const auto ref_a = export_shared_bundle(new_system(a), pool);
    const auto ref_b = export_shared_bundle(new_system(b), pool);
    SharedPublisher imported(pool, 1);
    const auto bare = imported.import_bundle(ref_a, model);
    check_actors(bare);
    for (const auto& child : bare.state.child) require(child.threads.empty(), "Bare import remains lazy");
    reset_calls(); imported.export_bundle(new_system(a)); hit_calls();
    imported.import_bundle(ref_b, model);
    reset_calls(); imported.export_bundle(new_system(b)); hit_calls();
    reset_calls(); imported.export_bundle(new_system(a)); full_calls(); // Successful import evicted a.
    reset_calls(); const auto violation = imported.export_bundle(new_system(a, true)); hit_calls();
    require(load_shared_state(violation.system_key, model, pool).actors.empty(), "Imported proofs must not cache actors");
    reset_calls(); const auto exited = imported.export_bundle(new_system(a, false, true)); hit_calls();
    require(load_shared_state(exited.system_key, model, pool).actors.size() == 2 * (NP - 1),
            "Imported proof must respect current process status");
    SharedPublisher disabled(pool, 0);
    disabled.import_bundle(ref_a, model);
    reset_calls(); disabled.export_bundle(new_system(a)); full_calls();

    auto failed_import_preserves = [&](SharedObjectPool& target, auto attempt) {
        SharedPublisher context(target, 1);
        context.export_bundle(new_system(a));
        rejects([&] { attempt(context); });
        reset_calls(); context.export_bundle(new_system(a)); hit_calls();
        reset_calls(); context.export_bundle(new_system(b)); full_calls();
    };
    failed_import_preserves(pool, [&](auto& context) { context.import_bundle(ref_b, "wrong model"); });
    failed_import_preserves(pool, [&](auto& context) { context.import_bundle(ref_b, ""); });
    auto wrong_metadata = ref_b; wrong_metadata.metadata.push_back(0);
    failed_import_preserves(pool, [&](auto& context) { context.import_bundle(wrong_metadata, model); });
    auto wrong_version = ref_b; wrong_version.version = 1;
    failed_import_preserves(pool, [&](auto& context) { context.import_bundle(wrong_version, model); });
    Bundle unready; unready.version = 2; unready.system_key = new_system(b);
    require(SysStateStore::instance().load(unready.system_key, unready.metadata), "Read unready fixture metadata");
    failed_import_preserves(pool, [&](auto& context) { context.import_bundle(unready, model); });
    // A materialization request without this pool attached must fail before seeding.
    require(!StateStore::instance().shared_pool(), "Fixture starts without an attached pool");
    failed_import_preserves(pool, [&](auto& context) { context.import_bundle(ref_b, model, true); });
    SharedObjectPool other((directory.string() + "-other"));
    failed_import_preserves(other, [&](auto& context) { context.import_bundle(ref_b, model); });

    StateStore::instance().attach_shared_pool(pool.path(), false);
    SharedPublisher materialized(pool);
    const auto full = materialized.import_bundle(ref_a, model, true);
    check_state(full.state); check_actors(full);
    reset_calls(); materialized.export_bundle(new_system(a)); hit_calls();
    failed_import_preserves(pool, [&](auto& context) {
        fail_registration = true;
        context.import_bundle(ref_b, model, true);
    });
    require(!fail_registration.load(), "Registration failure must reach actual SysStateStore::save");
    // Failed registration of an already-proven root cannot promote its LRU position.
    SharedPublisher ordered(pool, 2);
    ordered.export_bundle(new_system(a)); ordered.export_bundle(new_system(b));
    fail_registration = true;
    rejects([&] { ordered.import_bundle(ref_a, model); });
    require(!fail_registration.load(), "Warm registration fault was exercised");
    ordered.export_bundle(new_system(c));
    reset_calls(); ordered.export_bundle(new_system(b)); hit_calls();
    reset_calls(); ordered.export_bundle(new_system(a)); full_calls();

    // Even cached Ready bytes/root proofs cannot bypass the pool fail-stop state.
    pool.flush(); fail_sync = true;
    rejects([&] { pool.flush(); }); fail_sync = false;
    rejects([&] { materialized.import_bundle(ref_a, model); });
    reset_calls(); rejects([&] { materialized.export_bundle(new_system(a)); }); hit_calls();
    puts("shared publisher import: ready seeds, lazy/full import, bounded proofs, rejection and poison passed");
}

static std::set<hash_type> ready_keys(const SharedObjectPool& pool) {
    std::set<hash_type> keys;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(pool.path()) / "ready"))
        if (entry.path().extension() == ".obj") keys.insert(std::stoull(entry.path().stem().string(), nullptr, 16));
    return keys;
}
static Blob added_root_proof(SharedObjectPool& pool, const std::set<hash_type>& before, hash_type system) {
    auto added = ready_keys(pool);
    for (auto key : before) added.erase(key);
    added.erase(system);
    require(added.size() == 1, "Exactly one certificate for one distinct root");
    const auto key = *added.begin();
    const auto bytes = pool.load_ready(key);
    require(bytes && XXHash64::hash(bytes->data(), bytes->size()) == key, "Root certificate descriptor-derived key");
    return {key, *bytes};
}
static Bundle reference_for(hash_type key) {
    Bundle ref; ref.version = 2; ref.system_key = key;
    require(SysStateStore::instance().load(key, ref.metadata), "Reference metadata exists");
    return ref;
}
static void global_root_proofs(bool pages, const std::filesystem::path& directory) {
    const std::string model = "bundle-fixture/model1:bootstrap-v1:0123456789abcdef";
    const auto a = make_root(pages), b = make_root(pages, 'n');
    auto state = [&](hash_type root, const std::string& identity) { return new_system(root, false, false, 0, identity); };
    SharedObjectPool pool(directory.string());
    SharedPublisher publisher(pool);
    auto before = ready_keys(pool);
    reset_calls(); const auto ref_a = publisher.export_bundle(state(a, model)); full_calls();
    const auto proof_a = added_root_proof(pool, before, ref_a.system_key);
    before = ready_keys(pool);
    const auto ref_b = publisher.export_bundle(state(b, model));
    const auto proof_b = added_root_proof(pool, before, ref_b.system_key);

    SharedPublisher fresh(pool, 1);
    reset_calls(); const auto shared = fresh.export_bundle(state(a, model)); hit_calls();
    check_actors(load_shared_state(shared.system_key, model, pool));
    reset_calls(); fresh.export_bundle(state(b, model)); hit_calls();
    reset_calls(); fresh.export_bundle(state(a, model)); hit_calls(); // LRU eviction falls back to global proof.
    SharedObjectPool reopened(pool.path(), false);
    SharedPublisher cold(reopened);
    reset_calls(); cold.export_bundle(state(a, model)); hit_calls();
    SharedPublisher property(pool), status(pool);
    reset_calls(); const auto failed = property.export_bundle(new_system(a, true, false, 0, model)); hit_calls();
    require(load_shared_state(failed.system_key, model, pool).actors.empty(), "Global proof still derives property admission");
    reset_calls(); const auto exited = status.export_bundle(new_system(a, false, true, 0, model)); hit_calls();
    require(load_shared_state(exited.system_key, model, pool).actors.size() == 2 * (NP - 1), "Global proof still derives process admission");

    watched_ready_key = proof_a.key; watched_ready_reads = 0;
    SharedPublisher disabled(pool, 0);
    reset_calls(); disabled.export_bundle(state(a, model)); full_calls();
    disabled.import_bundle(ref_a, model);
    reset_calls(); export_shared_bundle(state(a, model), pool); full_calls();
    const auto old_bundle = export_bundle(ref_a.system_key);
    import_bundle(old_bundle, model);
    require(watched_ready_reads.load() == 0, "cap0/free/v1 must not consult RootProof namespace");
    watched_ready_key = 0;

    // A different valid complete model needs its own certificate, even with identical roots.
    const std::string other_model = "another-model:bootstrap-v1:0123456789abcdef";
    SharedPublisher different_model(pool);
    reset_calls(); different_model.export_bundle(state(a, other_model)); full_calls();
    SharedObjectPool empty(directory.string() + "-empty");
    SharedPublisher empty_context(empty);
    reset_calls(); empty_context.export_bundle(state(a, model)); full_calls();

    // A v4 pool has complete SystemReady but no optional RootProof. Read-only
    // imports must retain the old successful behavior without changing the pool.
    SharedObjectPool legacy(directory.string() + "-legacy");
    const auto legacy_ref = export_shared_bundle(state(a, model), legacy);
    legacy.flush();
    const auto legacy_keys = ready_keys(legacy);
    require(legacy_keys.size() == 1, "Legacy fixture contains only SystemReady");
    SharedObjectPool readonly_legacy(legacy.path(), false, true);
    SharedPublisher readonly_reader(readonly_legacy);
    check_actors(readonly_reader.import_bundle(legacy_ref, model));
    require(ready_keys(readonly_legacy) == legacy_keys, "Read-only import must not add optional proofs");

    // Guest blobs may have exactly the descriptor bytes/key; only KindReady is a proof.
    SharedObjectPool forged_blob(directory.string() + "-blob");
    forged_blob.publish_blob(proof_a.key, proof_a.bytes.data(), proof_a.bytes.size());
    SharedPublisher blob_context(forged_blob);
    reset_calls(); blob_context.export_bundle(state(a, model)); full_calls();
    require(*forged_blob.load_ready(proof_a.key) == proof_a.bytes, "Real proof published after full validation");

    // Private descriptor layout is exercised without adding a product introspection API.
    // Domain[16], version u32, NP u32, root u64, model length u64, complete model.
    const std::vector<size_t> wrong_fields{0, 16, 20, 24, proof_a.bytes.size() - 1};
    for (size_t field : wrong_fields) {
        SharedObjectPool invalid(directory.string() + "-field-" + std::to_string(field));
        auto bytes = proof_a.bytes;
        if (field == bytes.size() - 1) bytes[field] = 'e'; else bytes[field] ^= 1;
        invalid.publish_ready(proof_a.key, bytes);
        SharedPublisher reject(invalid);
        const auto key = state(a, model);
        rejects([&] { reject.export_bundle(key); });
        require(!invalid.load_ready(key), "Invalid descriptor rejected before SystemReady");
    }
    SharedObjectPool wrong_type(directory.string() + "-type");
    wrong_type.publish_ready(proof_a.key, *pool.load_ready(ref_a.system_key));
    SharedPublisher reject_type(wrong_type);
    rejects([&] { reject_type.export_bundle(state(a, model)); });
    const auto colliding_system = state(a, model);
    wrong_type.publish_ready(colliding_system, proof_a.bytes);
    rejects([&] { reject_type.export_bundle(colliding_system); });

    SharedObjectPool failed_system(directory.string() + "-system-failure");
    SharedPublisher failed_context(failed_system);
    const auto failed_key = state(a, model);
    fail_ready_publication = true;
    rejects([&] { failed_context.export_bundle(failed_key); });
    require(ready_keys(failed_system).empty(), "Failed SystemReady must not publish RootProof");

    if constexpr (NP >= 2) {
        for (bool importing : {false, true}) {
            SharedObjectPool partial(directory.string() + (importing ? "-partial-import" : "-partial-export"));
            const auto key = new_system(a, false, false, b, model);
            const auto ref = importing ? export_shared_bundle(key, partial) : reference_for(key);
            SharedPublisher context(partial);
            fail_ready_key = proof_b.key;
            rejects([&] {
                if (importing) (void)context.import_bundle(ref, model);
                else (void)context.export_bundle(key);
            });
            require(!fail_ready_key.load(), "Injected second-root publication failure reached");
            require(partial.load_ready(key) && partial.load_ready(proof_a.key) && !partial.load_ready(proof_b.key),
                    "Keep valid SystemReady/first proof, do not fabricate second proof");
            SharedPublisher reuse(partial);
            reset_calls(); reuse.export_bundle(state(a, model)); hit_calls();
            SharedPublisher repair(partial);
            repair.import_bundle(ref, model); // Existing complete SystemReady certifies both roots.
            require(partial.load_ready(proof_b.key) != nullptr, "Successful import completes remaining proof");
            reset_calls(); context.export_bundle(state(b, model)); hit_calls(); // No negative cache from failed attempt.
        }
    }

    const std::vector<std::string> unknown_models{
        "base", ":bootstrap-v1:0123456789abcdef", "base:bootstrap-v2:0123456789abcdef",
        "base:bootstrap-v1:0123456789abcde", "base:bootstrap-v1:0123456789abcdef0",
        "base:bootstrap-v1:0123456789abcdeF", "base:bootstrap-v1:0123456789abcdef:tail"};
    for (size_t index = 0; index < unknown_models.size(); ++index) {
        const auto& unknown = unknown_models[index];
        SharedObjectPool fallback(directory.string() + "-unknown-" + std::to_string(index));
        auto fake = proof_a.bytes;
        fake.resize(40); const uint64_t length = unknown.size(); std::memcpy(fake.data() + 32, &length, 8);
        fake.insert(fake.end(), unknown.begin(), unknown.end());
        const auto fake_key = XXHash64::hash(fake.data(), fake.size());
        fallback.publish_ready(fake_key, fake); // Even an exact descriptor must not enable an unknown model.
        watched_ready_key = fake_key; watched_ready_reads = 0;
        SharedPublisher first(fallback), second(fallback);
        reset_calls(); first.export_bundle(state(a, unknown)); full_calls();
        reset_calls(); first.export_bundle(state(a, unknown)); hit_calls(); // v4 local behavior retained.
        reset_calls(); second.export_bundle(state(a, unknown)); full_calls();
        require(watched_ready_reads.load() == 0, "Unknown model must not read global RootProof");
        require(ready_keys(fallback).size() == 4, "Unknown model publishes only three SystemReady records");
        watched_ready_key = 0;
    }
    pool.flush(); fail_sync = true;
    rejects([&] { pool.flush(); }); fail_sync = false;
    rejects([&] { publisher.import_bundle(ref_a, model); });
    rejects([&] { publisher.export_bundle(state(a, model)); });
    puts("global root proof: namespace/model/type isolation, global/cold reuse, failure ordering, partial repair and fallback passed");
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
            publisher_import_proofs(pages, base/(suffix+"-import-pool"));
            global_root_proofs(pages, base/(suffix+"-global-pool"));
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
