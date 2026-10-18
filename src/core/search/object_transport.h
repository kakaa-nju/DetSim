#pragma once
#include "protocol.h"
#include <chrono>
#include <memory>
#include <vector>

namespace detsim::search::wire {
struct ObjectLimits {
    uint64_t cache_bytes = uint64_t{64} << 20;
    size_t cache_segments = 128;
    uint64_t segment_bytes = uint64_t{8} << 20;
    size_t confirmed_keys = 16'384;
};
struct ObjectStats {
    uint64_t send_calls = 0, recv_calls = 0, poll_calls = 0;
    uint64_t memfds_created = 0, memfd_bytes_created = 0, segments_received = 0;
    uint64_t reference_bytes = 0, missing_bytes = 0, peak_inflight_bytes = 0;
    uint64_t cache_bytes = 0, cache_segments = 0, peak_cache_bytes = 0;
};
struct BlobSpec { hash_type key; uint64_t size; bool reference; };

// Runtime cache only. All mappings are immutable/sealed; eviction is independent
// of snapshot identity and missing objects are explicitly retransmitted. Callers
// serialize access (the native coordinator and each worker have one main loop).
class ObjectCache {
public:
    explicit ObjectCache(ObjectLimits limits = {});
    ~ObjectCache();
    ObjectCache(const ObjectCache&) = delete;
    ObjectCache& operator=(const ObjectCache&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class ObjectTransport;
};

// Owns a SOCK_SEQPACKET descriptor dedicated to SCM_RIGHTS. The control stream
// carries BlobSpecs; this side channel never contains mutable StateStore files.
// At most one <=segment_bytes descriptor is in flight before SegmentAck.
class ObjectTransport {
public:
    using Deadline = std::chrono::steady_clock::time_point;
    ObjectTransport(int fd, Limits limits, std::shared_ptr<ObjectCache> cache = {}, ObjectLimits bounds = {});
    ~ObjectTransport();
    ObjectTransport(const ObjectTransport&) = delete;
    ObjectTransport& operator=(const ObjectTransport&) = delete;
    bool supports(const snapshot::Bundle& bundle) const;
    std::vector<BlobSpec> plan(const std::vector<snapshot::Blob>& blobs);
    void send(const std::vector<snapshot::Blob>& blobs, const std::vector<BlobSpec>& specs, Deadline deadline);
    std::vector<snapshot::Blob> receive(const std::vector<BlobSpec>& specs, Deadline deadline);
    // Required after the ordinary import_bundle validation/durability barrier.
    void confirm();
    void abort() noexcept;
    bool pending() const;
    ObjectStats statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace detsim::search::wire
