#pragma once
#include "state.h"
#include "state_transition.h"
#include <string>
#include <vector>

namespace detsim::snapshot {
class SharedObjectPool;
struct ReadyState {
    sys_state state; // Metadata only unless materialization explicitly requested.
    std::vector<ActorAction> actors;
};
struct Blob {
    hash_type key = 0;
    std::vector<uint8_t> bytes;
};
// Version 1 owns one raw copy of its complete closure (transfer memory scales
// with closure size). Version 2 is metadata-only and requires a ready certificate
// in the caller-selected private shared pool; it never carries blob payloads.
struct Bundle {
    uint32_t version = 1;
    hash_type system_key = 0;
    std::vector<uint8_t> metadata;
    std::vector<Blob> blobs;
};
// Complete immutable closure only: no physical bindings or live-process access.
Bundle export_bundle(hash_type system_key);
// All dependencies must be declared even if the destination already has them.
// Failure can leave orphan immutable blobs; metadata is published only after
// complete semantic validation and the data durability barrier. An I/O failure
// is fail-stop; publication is not a transaction or an exactly-once guarantee.
sys_state import_bundle(const Bundle& bundle, const std::string& expected_model_identity);
// A ready record certifies a fully validated closure in one private immutable
// pool generation. Removing/replacing published objects violates that contract.
// These APIs provide process visibility; they do not replace v1 durability.
void publish_shared_state(hash_type system_key, SharedObjectPool& pool);
ReadyState load_shared_state(hash_type system_key, const std::string& expected_model_identity,
                             SharedObjectPool& pool);
Bundle export_shared_bundle(hash_type system_key, SharedObjectPool& pool);
ReadyState import_shared_bundle(const Bundle& bundle, const std::string& expected_model_identity,
                               SharedObjectPool& pool, bool materialize_child = false);
} // namespace detsim::snapshot
