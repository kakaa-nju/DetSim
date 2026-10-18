#include "snapshot_bundle.h"
#include "page_snapshot.h"
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
template<class Visit> void validate_closure(sys_state& state, Visit visit) {
    for (int node = 0; node < NP; ++node) {
        const auto root = state.ts_hash[node];
        visit(root);
        const auto view = SnapshotView::open(root);
        for (auto key : view->referenced_blob_keys()) visit(key);
        // Manifest validation is lazy for page payloads; force every page and
        // its declared size before publication, including unobserved regions.
        for (size_t region = 0; region < view->regions().size(); ++region)
            for (size_t page = 0; page < view->page_count(region); ++page)
                (void)view->page(region, page);
        state.child[node] = tracee_state(root);
        for (const auto& [path, file] : state.child[node].fs_state.filesystem) {
            (void)path;
            if (auto key = file.content.saved_blob_key()) visit(key);
        }
    }
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
} // namespace detsim::snapshot
