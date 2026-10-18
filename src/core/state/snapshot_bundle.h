#pragma once
#include "state.h"
#include <string>
#include <vector>

namespace detsim::snapshot {
struct Blob {
    hash_type key = 0;
    std::vector<uint8_t> bytes;
};
// This transport owns one raw copy of its complete closure; peak transfer
// memory scales with closure size. Streaming/chunked transport is not provided.
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
} // namespace detsim::snapshot
