#pragma once
#include "types.h"
#include <memory>
#include <cstddef>
#include <string>
#include <vector>

namespace detsim::snapshot {
// Same-host immutable CAS. publish makes a complete object atomically visible;
// flush is the explicit durability barrier. It is never implicit on hot saves.
// Durability/error reporting requires Linux >=5.8 syncfs semantics. The barrier
// synchronizes each backing filesystem (including its unrelated dirty files),
// plus ancestor directory entries; unsupported syscalls and I/O errors fail.
// The caller owns a private pool lifetime: published objects must not be edited
// or removed until all users are closed. A bounded immutable cache retains
// publication proofs only within this pool instance; reopen validates cold files.
class SharedObjectPool {
public:
    using Buffer = std::shared_ptr<const std::vector<uint8_t>>;
    explicit SharedObjectPool(std::string dir, bool create = true, bool read_only = false);
    ~SharedObjectPool();
    SharedObjectPool(const SharedObjectPool&) = delete;
    SharedObjectPool& operator=(const SharedObjectPool&) = delete;
    const std::string& path() const;
    bool is_read_only() const noexcept;
    void publish_blob(hash_type key, const void* bytes, size_t size);
    Buffer load_blob(hash_type key);
    // Ready payload is opaque. The caller validates its semantic closure before
    // publishing; its semantic key is independent of its raw checksum.
    void publish_ready(hash_type key, const std::vector<uint8_t>& validated_record);
    Buffer load_ready(hash_type key);
    void flush();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
