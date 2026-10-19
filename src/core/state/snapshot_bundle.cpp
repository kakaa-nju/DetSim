#include "snapshot_bundle.h"
#include "page_snapshot.h"
#include "shared_object_pool.h"
#include "proc_status.h"
#include <cstring>
#include <tuple>
#include "sysstate_store.h"
#include "cereal/archives/binary.hpp"
#include "utils.h"
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace detsim::snapshot {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
sys_state decode(const Bundle& bundle) {
    require(bundle.version == 1 && bundle.system_key, "Invalid snapshot bundle header");
    std::istringstream encoded(std::string(bundle.metadata.begin(), bundle.metadata.end()));
    sys_state state;
    { cereal::BinaryInputArchive archive(encoded); archive(state); }
    require(encoded.peek() == std::char_traits<char>::eof() && state.identity_hash() == bundle.system_key,
            "Invalid snapshot bundle system identity");
    for (auto root : state.ts_hash) require(root != 0, "Missing bundle tracee root");
    state.ss_hash = bundle.system_key;
    return state;
}
// Loads only saved bytes using the normal validators. The visitor enforces
// supplied-closure membership on import, independently of destination caches.
template<class Visit> tracee_state validate_root(hash_type root, Visit& visit) {
    visit(root);
    const auto view = SnapshotView::open(root);
    for (auto key : view->referenced_blob_keys()) visit(key);
    // Manifest validation is lazy for page payloads; force every page and
    // its declared size before publication, including unobserved regions.
    for (size_t region = 0; region < view->regions().size(); ++region)
        for (size_t page = 0; page < view->page_count(region); ++page)
            (void)view->page(region, page);
    tracee_state child(root);
    for (const auto& [path, file] : child.fs_state.filesystem) {
        (void)path;
        if (auto key = file.content.saved_blob_key()) visit(key);
    }
    return child;
}
template<class Visit> void validate_closure(sys_state& state, Visit visit) {
    for (int node = 0; node < NP; ++node)
        state.child[node] = validate_root(state.ts_hash[node], visit);
}
}
Bundle export_bundle(hash_type system_key) {
    Bundle bundle; bundle.system_key = system_key;
    require(SysStateStore::instance().load(system_key, bundle.metadata), "Missing bundle system metadata");
    auto state = decode(bundle);
    std::set<hash_type> seen;
    validate_closure(state, [&](hash_type key) {
        if (!seen.insert(key).second) return;
        auto bytes = StateStore::instance().load_shared(key);
        require(bytes && !bytes->empty(), "Missing snapshot bundle dependency");
        bundle.blobs.push_back({key, *bytes});
    });
    return bundle;
}
sys_state import_bundle(const Bundle& bundle, const std::string& expected_model_identity) {
    auto state = decode(bundle);
    require(!expected_model_identity.empty() && state.model_identity == expected_model_identity,
            "Snapshot bundle execution model mismatch");
    std::map<hash_type, const std::vector<uint8_t>*> provided;
    for (const auto& blob : bundle.blobs) {
        require(blob.key && !blob.bytes.empty() &&
                XXHash64::hash(blob.bytes.data(), blob.bytes.size()) == blob.key,
                "Invalid snapshot bundle blob hash");
        auto [it, inserted] = provided.emplace(blob.key, &blob.bytes);
        require(inserted || *it->second == blob.bytes, "Conflicting duplicate bundle blob");
    }
    for (auto key : state.ts_hash) require(provided.count(key), "Undeclared bundle root");
    auto& store = StateStore::instance();
    // Preflight existing-key equality, too: StateStore dedup is not a proof that
    // an existing object has identical bytes (a corrupt/colliding store rejects).
    for (const auto& [key, bytes] : provided) {
        if (store.exists(key)) {
            const auto existing = store.load_shared(key);
            require(existing && *existing == *bytes, "Destination bundle blob conflict");
        }
    }
    for (const auto& [key, bytes] : provided)
        require(store.save(bytes->data(), bytes->size()) == key, "Bundle blob registration mismatch");
    std::set<hash_type> referenced;
    validate_closure(state, [&](hash_type key) {
        require(provided.count(key), "Undeclared snapshot bundle dependency");
        referenced.insert(key);
    });
    require(referenced.size() == provided.size(), "Extraneous snapshot bundle objects");
    store.wait_for_completion();
    auto& metadata_store = SysStateStore::instance();
    require(metadata_store.save(bundle.system_key, bundle.metadata), "Failed to publish snapshot bundle metadata");
    metadata_store.flush_index();
    return state;
}
namespace {
// Local same-host wire record. Metadata carries model identity and ordered node
// roots; a checksum also covers derived actor admission data and framing.
constexpr uint32_t ready_magic = 0x44535244;
constexpr uint32_t ready_version = 1;
template<class T> void append(std::vector<uint8_t>& out, T value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(value));
}
template<class T> T take(const std::vector<uint8_t>& bytes, size_t& offset) {
    require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "Truncated shared ready record");
    T result; std::memcpy(&result, bytes.data() + offset, sizeof(result));
    offset += sizeof(result); return result;
}
struct DecodedReady { ReadyState ready; std::vector<uint8_t> metadata; };
DecodedReady decode_ready(hash_type key, const std::vector<uint8_t>& bytes,
                          const std::string& expected) {
    require(!expected.empty(), "Empty shared snapshot execution model");
    require(bytes.size() >= sizeof(uint64_t), "Truncated shared ready checksum");
    size_t checksum_offset = bytes.size() - sizeof(uint64_t);
    size_t tail = checksum_offset;
    require(take<uint64_t>(bytes, tail) == XXHash64::hash(bytes.data(), checksum_offset),
            "Corrupt shared ready checksum");
    size_t offset = 0;
    require(take<uint32_t>(bytes, offset) == ready_magic &&
            take<uint32_t>(bytes, offset) == ready_version &&
            take<uint64_t>(bytes, offset) == key && key, "Invalid shared ready header");
    const auto metadata_size = take<uint64_t>(bytes, offset);
    const auto actor_count = take<uint32_t>(bytes, offset);
    require(actor_count <= NP * MAX_THREADS_PER_PROCESS && offset <= checksum_offset &&
            metadata_size <= checksum_offset - offset &&
            checksum_offset - offset - metadata_size == uint64_t(actor_count) * 3 * sizeof(int32_t),
            "Invalid shared ready lengths");
    DecodedReady result;
    result.metadata.assign(bytes.begin() + offset, bytes.begin() + offset + metadata_size);
    offset += metadata_size;
    Bundle bundle; bundle.system_key = key; bundle.metadata = result.metadata;
    result.ready.state = decode(bundle);
    require(result.ready.state.model_identity == expected, "Shared snapshot execution model mismatch");
    std::pair<int, int> previous{-1, -1};
    for (uint32_t i = 0; i < actor_count; ++i) {
        const auto process = take<int32_t>(bytes, offset);
        const auto thread = take<int32_t>(bytes, offset);
        const auto action = take<int32_t>(bytes, offset);
        require(process >= 0 && process < NP && thread >= 0 && thread < MAX_THREADS_PER_PROCESS &&
                (action == int(TransitionAction::Normal) || action == int(TransitionAction::Timeout)) &&
                std::make_pair(process, thread) > previous &&
                !DISDEAD(result.ready.state.status[process]) && !result.ready.state.property_failed,
                "Invalid shared ready actor admission");
        previous = {process, thread};
        result.ready.actors.push_back({process, thread, TransitionAction(action)});
    }
    return result;
}
std::vector<uint8_t> encode_ready(const Bundle& bundle, const std::vector<ActorAction>& actors) {
    std::vector<uint8_t> bytes;
    append(bytes, ready_magic); append(bytes, ready_version); append<uint64_t>(bytes, bundle.system_key);
    append<uint64_t>(bytes, bundle.metadata.size()); append<uint32_t>(bytes, actors.size());
    bytes.insert(bytes.end(), bundle.metadata.begin(), bundle.metadata.end());
    for (const auto& actor : actors) {
        append<int32_t>(bytes, actor.process); append<int32_t>(bytes, actor.thread);
        append<int32_t>(bytes, int(actor.action));
    }
    append<uint64_t>(bytes, XXHash64::hash(bytes.data(), bytes.size()));
    return bytes;
}
}
namespace {
template<class Proven, class Commit>
Bundle publish_reference(hash_type key, SharedObjectPool& pool, Proven proven, Commit commit) {
    Bundle bundle; bundle.system_key = key;
    require(SysStateStore::instance().load(key, bundle.metadata), "Missing shared system metadata");
    auto state = decode(bundle);
    require(!state.model_identity.empty(), "Empty shared snapshot execution model");
    // This also checks pool poison before any context proof can be used.
    if (const auto existing = pool.load_ready(key)) {
        const auto saved = decode_ready(key, *existing, state.model_identity);
        require(saved.metadata == bundle.metadata, "Conflicting shared ready metadata");
        bundle.version = 2;
        return bundle; // Whole-system certificate; do not seed local root proofs.
    }
    std::set<hash_type> seen;
    auto publish = [&](hash_type blob_key) {
        if (!seen.insert(blob_key).second) return;
        const auto bytes = StateStore::instance().load_shared(blob_key);
        require(bytes && !bytes->empty(), "Missing shared snapshot dependency");
        pool.publish_blob(blob_key, bytes->data(), bytes->size());
    };
    for (int node = 0; node < NP; ++node) {
        const auto root = state.ts_hash[node];
        // A root proof certifies only its immutable closure in this pool.
        // Decode metadata again: actors depend on this system's status/property.
        state.child[node] = proven(root) ? tracee_state(root) : validate_root(root, publish);
    }
    const auto actors = enabled_actor_actions(state);
    const auto record = encode_ready(bundle, actors);
    (void)decode_ready(key, record, state.model_identity);
    pool.publish_ready(key, record);
    commit(state.ts_hash); // No proof or LRU change before successful Ready publication.
    bundle.version = 2;
    return bundle;
}
}
void publish_shared_state(hash_type key, SharedObjectPool& pool) {
    (void)publish_reference(key, pool, [](hash_type) { return false; }, [](const auto&) {});
}
SharedPublisher::SharedPublisher(SharedObjectPool& pool, size_t root_capacity)
    : pool_(pool), capacity_(root_capacity) {}
void SharedPublisher::remember(const hash_type (&roots)[NP]) {
    if (!capacity_) return;
    for (const auto root : roots) {
        if (const auto found = proven_.find(root); found != proven_.end()) {
            recent_.splice(recent_.begin(), recent_, found->second);
            continue;
        }
        recent_.push_front(root);
        try { proven_.emplace(root, recent_.begin()); }
        catch (...) { recent_.pop_front(); throw; }
        if (proven_.size() > capacity_) {
            proven_.erase(recent_.back());
            recent_.pop_back();
        }
    }
}
Bundle SharedPublisher::export_bundle(hash_type key) {
    return publish_reference(key, pool_,
        [&](hash_type root) { return proven_.contains(root); },
        [&](const auto& roots) { remember(roots); });
}
ReadyState SharedPublisher::import_bundle(const Bundle& bundle, const std::string& expected,
                                         bool materialize_child) {
    auto ready = import_shared_bundle(bundle, expected, pool_, materialize_child);
    remember(ready.state.ts_hash);
    return ready;
}
ReadyState load_shared_state(hash_type key, const std::string& expected, SharedObjectPool& pool) {
    const auto record = pool.load_ready(key);
    require(bool(record), "Shared snapshot is not ready");
    return decode_ready(key, *record, expected).ready;
}
Bundle export_shared_bundle(hash_type key, SharedObjectPool& pool) {
    return publish_reference(key, pool, [](hash_type) { return false; }, [](const auto&) {});
}
ReadyState import_shared_bundle(const Bundle& bundle, const std::string& expected,
                               SharedObjectPool& pool, bool materialize_child) {
    require(bundle.version == 2 && bundle.blobs.empty(), "Invalid shared reference bundle");
    const auto record = pool.load_ready(bundle.system_key);
    require(bool(record), "Shared snapshot is not ready");
    auto decoded = decode_ready(bundle.system_key, *record, expected);
    require(decoded.metadata == bundle.metadata, "Shared reference metadata mismatch");
    if (materialize_child) {
        const auto attached = StateStore::instance().shared_pool();
        require(attached && attached->path() == pool.path(), "Shared materialization requires the same attached pool");
        for (int node = 0; node < NP; ++node)
            decoded.ready.state.child[node] = tracee_state(decoded.ready.state.ts_hash[node]);
    }
    require(SysStateStore::instance().save(bundle.system_key, bundle.metadata),
            "Failed to register shared system metadata");
    // Process-visible registration only. Explicit caller checkpoints own durability.
    return std::move(decoded.ready);
}
} // namespace detsim::snapshot
