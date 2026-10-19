#pragma once
#include "state.h"
#include "state_transition.h"
#include <list>
#include <unordered_map>
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
// One worker-loop publication context. The caller must keep this exact pool
// instance alive until the publisher is destroyed. Published pool objects must
// remain immutable. Proof eviction drops only an optimization, never objects.
class SharedPublisher final {
public:
    explicit SharedPublisher(SharedObjectPool& pool, size_t root_capacity = 1024);
    SharedPublisher(const SharedPublisher&) = delete;
    SharedPublisher& operator=(const SharedPublisher&) = delete;
    SharedPublisher(SharedPublisher&&) = delete;
    SharedPublisher& operator=(SharedPublisher&&) = delete;
    Bundle export_bundle(hash_type system_key);
    // A same-pool Ready certifies its roots. Seed proofs only after validation,
    // optional materialization and local metadata registration all succeed.
    ReadyState import_bundle(const Bundle& bundle, const std::string& expected_model_identity,
                             bool materialize_child = false);
private:
    void remember(const hash_type (&roots)[NP]);
    SharedObjectPool& pool_;
    size_t capacity_;
    std::list<hash_type> recent_;
    std::unordered_map<hash_type, std::list<hash_type>::iterator> proven_;
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
