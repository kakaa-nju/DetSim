#include "object_transport.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <list>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include "utils.h"

namespace detsim::search::wire {
namespace {
using Deadline = ObjectTransport::Deadline;
constexpr uint64_t segment_magic = 0x314745534d495344ULL; // DSIMSEG1
constexpr uint32_t packet_magic = 0x444f5344;
constexpr size_t header_bytes = 24, row_bytes = 24, max_objects = 256;
constexpr size_t packet_header = 28, packet_capacity = packet_header + max_objects * 8;
constexpr int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
enum class Op : uint16_t { Need = 1, Segment, SegmentAck, ChunkEnd, CommitAck, Abort };
[[noreturn]] void fail(const char* reason) { throw std::runtime_error(reason); }
void require(bool condition, const char* reason) { if (!condition) fail(reason); }
struct Fd {
    int value = -1;
    explicit Fd(int descriptor = -1) : value(descriptor) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(other.value) { other.value = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { if (value >= 0) close(value); value = other.value; other.value = -1; }
        return *this;
    }
};
void put(uint8_t* output, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) output[i] = uint8_t(value >> (i * 8));
}
uint64_t get(const uint8_t* input, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value |= uint64_t(input[i]) << (i * 8);
    return value;
}
void time_left(Deadline deadline) {
    require(std::chrono::steady_clock::now() < deadline, "Snapshot object transfer deadline exceeded");
}
void wait_ready(int fd, short events, Deadline deadline, ObjectStats& stats) {
    for (;;) {
        time_left(deadline);
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        pollfd descriptor{fd, events, 0};
        ++stats.poll_calls;
        const int result = poll(&descriptor, 1, int(std::min<int64_t>(std::max<int64_t>(left + 1, 1), INT32_MAX)));
        if (result < 0 && errno == EINTR) continue;
        require(result >= 0, "Snapshot object poll failed");
        if (!result) continue;
        require(!(descriptor.revents & POLLNVAL), "Snapshot object socket is invalid");
        return;
    }
}
struct Row { hash_type key; uint64_t offset, size; };
struct Segment {
    Fd descriptor;
    void* mapping = MAP_FAILED;
    uint64_t size = 0;
    std::vector<Row> rows;
    Segment(Fd fd, const Limits& limits, const ObjectLimits& bounds) : descriptor(std::move(fd)) {
        struct stat status{};
        require(fstat(descriptor.value, &status) == 0 && S_ISREG(status.st_mode) && status.st_size >= int64_t(header_bytes),
                "Invalid immutable snapshot descriptor");
        size = static_cast<uint64_t>(status.st_size);
        require(size <= bounds.segment_bytes && size <= SIZE_MAX && size <= limits.max_frame_bytes,
                "Snapshot segment exceeds hard size bound");
        const int seals = fcntl(descriptor.value, F_GET_SEALS);
        require(seals >= 0 && (seals & required_seals) == required_seals, "Snapshot descriptor is not permanently immutable");
        mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, descriptor.value, 0);
        require(mapping != MAP_FAILED, "Cannot map immutable snapshot segment");
        try {
            const auto* bytes = static_cast<const uint8_t*>(mapping);
            require(get(bytes, 8) == segment_magic && get(bytes + 16, 8) == size, "Invalid snapshot segment header");
            const auto count = get(bytes + 8, 8);
            require(count && count <= max_objects && count <= (size - header_bytes) / row_bytes,
                    "Invalid snapshot segment object count");
            uint64_t next = header_bytes + count * row_bytes;
            for (uint64_t i = 0; i < count; ++i) {
                const auto* entry = bytes + header_bytes + i * row_bytes;
                Row row{get(entry, 8), get(entry + 8, 8), get(entry + 16, 8)};
                require(row.key && row.size && row.size <= limits.max_blob_bytes && row.offset == next && row.size <= size - next,
                        "Invalid snapshot segment object range");
                require(XXHash64::hash(bytes + row.offset, row.size) == row.key, "Snapshot segment object hash mismatch");
                next += row.size; rows.push_back(row);
            }
            require(next == size, "Trailing snapshot segment bytes");
        } catch (...) { munmap(mapping, size); mapping = MAP_FAILED; throw; }
    }
    ~Segment() { if (mapping != MAP_FAILED) munmap(mapping, size); }
    const uint8_t* bytes(size_t row) const { return static_cast<const uint8_t*>(mapping) + rows.at(row).offset; }
};
std::shared_ptr<Segment> make_segment(const std::vector<const snapshot::Blob*>& blobs, const Limits& limits,
                                      const ObjectLimits& bounds, ObjectStats& stats) {
    require(!blobs.empty() && blobs.size() <= max_objects, "Invalid snapshot segment batch");
    uint64_t size = header_bytes + blobs.size() * row_bytes;
    for (const auto* blob : blobs) {
        require(!blob->bytes.empty() && blob->bytes.size() <= bounds.segment_bytes - size,
                "Snapshot segment batch exceeds hard size bound");
        size += blob->bytes.size();
    }
    Fd fd(static_cast<int>(syscall(SYS_memfd_create, "detsim-immutable-objects", MFD_CLOEXEC | MFD_ALLOW_SEALING)));
    require(fd.value >= 0 && size <= uint64_t(std::numeric_limits<off_t>::max()) && ftruncate(fd.value, off_t(size)) == 0,
            "Cannot allocate immutable snapshot segment");
    struct WritableMap {
        void* address;
        size_t size;
        ~WritableMap() { if (address != MAP_FAILED) munmap(address, size); }
    } writable{mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.value, 0), size_t(size)};
    require(writable.address != MAP_FAILED, "Cannot populate snapshot segment");
    auto* bytes = static_cast<uint8_t*>(writable.address);
    put(bytes, segment_magic, 8); put(bytes + 8, blobs.size(), 8); put(bytes + 16, size, 8);
    uint64_t offset = header_bytes + blobs.size() * row_bytes;
    for (size_t i = 0; i < blobs.size(); ++i) {
        const auto& blob = *blobs[i];
        put(bytes + header_bytes + i * row_bytes, blob.key, 8);
        put(bytes + header_bytes + i * row_bytes + 8, offset, 8);
        put(bytes + header_bytes + i * row_bytes + 16, blob.bytes.size(), 8);
        std::memcpy(bytes + offset, blob.bytes.data(), blob.bytes.size()); offset += blob.bytes.size();
    }
    require(munmap(writable.address, size) == 0, "Cannot unmap writable snapshot segment");
    writable.address = MAP_FAILED;
    require(fcntl(fd.value, F_ADD_SEALS, required_seals) == 0, "Cannot permanently seal snapshot segment");
    ++stats.memfds_created; stats.memfd_bytes_created += size;
    stats.peak_inflight_bytes = std::max(stats.peak_inflight_bytes, size);
    return std::make_shared<Segment>(std::move(fd), limits, bounds);
}
struct Packet {
    Op op;
    uint64_t transfer;
    uint32_t chunk, extra;
    std::vector<std::pair<uint32_t, uint32_t>> values;
    Fd fd;
};
void send_packet(int fd, Op op, uint64_t transfer, uint32_t chunk, uint32_t extra,
                 const std::vector<std::pair<uint32_t,uint32_t>>& values, int shared_fd,
                 Deadline deadline, ObjectStats& stats) {
    require(values.size() <= max_objects, "Snapshot packet exceeds bounded batch");
    std::array<uint8_t, packet_capacity> bytes{};
    put(bytes.data(), packet_magic, 4); put(bytes.data() + 4, 1, 2); put(bytes.data() + 6, uint16_t(op), 2);
    put(bytes.data() + 8, transfer, 8); put(bytes.data() + 16, chunk, 4);
    put(bytes.data() + 20, values.size(), 4); put(bytes.data() + 24, extra, 4);
    const size_t width = op == Op::Need ? 4 : 8;
    for (size_t i = 0; i < values.size(); ++i) {
        put(bytes.data() + packet_header + i * width, values[i].first, 4);
        if (width == 8) put(bytes.data() + packet_header + i * width + 4, values[i].second, 4);
    }
    iovec io{bytes.data(), packet_header + values.size() * width};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
    msghdr message{}; message.msg_iov = &io; message.msg_iovlen = 1;
    if (shared_fd >= 0) {
        message.msg_control = control.data(); message.msg_controllen = control.size();
        auto* header = CMSG_FIRSTHDR(&message); header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int)); std::memcpy(CMSG_DATA(header), &shared_fd, sizeof(shared_fd));
    }
    for (;;) {
        time_left(deadline); ++stats.send_calls;
        const auto count = sendmsg(fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { wait_ready(fd, POLLOUT, deadline, stats); continue; }
        require(count == ssize_t(io.iov_len), "Snapshot packet send failed"); return;
    }
}
Packet receive_packet(int fd, Deadline deadline, ObjectStats& stats) {
    std::array<uint8_t, packet_capacity> bytes{};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(4 * sizeof(int))> control{};
    iovec io{bytes.data(), bytes.size()}; msghdr message{};
    message.msg_iov = &io; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    ssize_t count;
    for (;;) {
        time_left(deadline); ++stats.recv_calls;
        message.msg_controllen = control.size(); message.msg_flags = 0;
        count = recvmsg(fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { wait_ready(fd, POLLIN, deadline, stats); continue; }
        require(count >= 0, "Snapshot object stream failed"); break;
    }
    std::vector<Fd> descriptors;
    bool valid_control = true;
    for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS || header->cmsg_len < CMSG_LEN(0)) {
            valid_control = false; continue;
        }
        const size_t length = header->cmsg_len - CMSG_LEN(0);
        if (length % sizeof(int)) valid_control = false;
        for (size_t i = 0; i < length / sizeof(int); ++i) {
            int descriptor; std::memcpy(&descriptor, CMSG_DATA(header) + i * sizeof(int), sizeof(descriptor));
            descriptors.emplace_back(descriptor);
        }
    }
    require(count > 0, "Snapshot object stream closed or empty packet");
    require(valid_control && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) && descriptors.size() <= 1,
            "Truncated or invalid snapshot ancillary data");
    require(count >= ssize_t(packet_header) && get(bytes.data(), 4) == packet_magic && get(bytes.data() + 4, 2) == 1,
            "Invalid snapshot object packet header");
    const auto operation = get(bytes.data() + 6, 2), entries = get(bytes.data() + 20, 4);
    require(operation >= uint16_t(Op::Need) && operation <= uint16_t(Op::Abort) && entries <= max_objects,
            "Invalid snapshot object packet type/count");
    const Op op = Op(operation); const size_t width = op == Op::Need ? 4 : 8;
    require(size_t(count) == packet_header + entries * width && (op == Op::Segment) == !descriptors.empty(),
            "Snapshot packet descriptor/length mismatch");
    Packet packet{op, get(bytes.data() + 8, 8), uint32_t(get(bytes.data() + 16, 4)), uint32_t(get(bytes.data() + 24, 4)), {}, Fd{}};
    if (!descriptors.empty()) packet.fd = std::move(descriptors[0]);
    for (size_t i = 0; i < entries; ++i)
        packet.values.emplace_back(uint32_t(get(bytes.data() + packet_header + i * width, 4)),
            width == 8 ? uint32_t(get(bytes.data() + packet_header + i * width + 4, 4)) : 0);
    if (op == Op::Abort) fail("Peer aborted snapshot object import");
    return packet;
}
} // namespace

struct ObjectCache::Impl {
    struct Entry {
        std::shared_ptr<Segment> segment;
        std::vector<size_t> rows;
        bool published = false, active = true;
        std::list<std::shared_ptr<Entry>>::iterator position;
    };
    struct View { std::shared_ptr<Segment> segment; size_t row; };
    struct Index { std::weak_ptr<Entry> entry; size_t row; };
    using Token = std::weak_ptr<Entry>;
    ObjectLimits limits;
    uint64_t bytes = 0, peak = 0;
    std::list<std::shared_ptr<Entry>> lru;
    std::unordered_map<hash_type, Index> index;
    explicit Impl(ObjectLimits settings) : limits(settings) {}
    std::optional<View> find(hash_type key) {
        const auto found = index.find(key);
        if (found == index.end()) return std::nullopt;
        auto entry = found->second.entry.lock();
        if (!entry || !entry->active) { index.erase(found); return std::nullopt; }
        lru.splice(lru.end(), lru, entry->position);
        return View{entry->segment, found->second.row};
    }
    static bool equals(const View& view, const snapshot::Blob& blob) {
        return view.segment->rows[view.row].size == blob.bytes.size() &&
            !std::memcmp(view.segment->bytes(view.row), blob.bytes.data(), blob.bytes.size());
    }
    void erase(const std::shared_ptr<Entry>& entry) {
        if (!entry->active) return;
        for (size_t row : entry->rows) {
            const auto key = entry->segment->rows[row].key;
            const auto found = index.find(key);
            if (found != index.end() && found->second.entry.lock() == entry) index.erase(found);
        }
        entry->active = false; bytes -= entry->segment->size; lru.erase(entry->position);
    }
    Token stage(const std::shared_ptr<Segment>& segment, std::vector<size_t> rows) {
        if (!limits.cache_segments || segment->size > limits.cache_bytes) return {};
        std::vector<size_t> selected;
        std::unordered_map<hash_type, size_t> keys;
        for (size_t row : rows) {
            require(row < segment->rows.size(), "Invalid snapshot cache row");
            const auto& data = segment->rows[row];
            if (auto prior = find(data.key)) {
                require(prior->segment->rows[prior->row].size == data.size &&
                    !std::memcmp(prior->segment->bytes(prior->row), segment->bytes(row), data.size),
                    "Snapshot object cache byte conflict");
            } else {
                const auto [duplicate, inserted] = keys.emplace(data.key, row);
                if (inserted) selected.push_back(row);
                else {
                    const auto& previous = segment->rows[duplicate->second];
                    require(previous.size == data.size && !std::memcmp(segment->bytes(duplicate->second), segment->bytes(row), data.size),
                            "Duplicate snapshot object cache byte conflict");
                }
            }
        }
        if (selected.empty()) return {};
        auto entry = std::make_shared<Entry>(); entry->segment = segment; entry->rows = std::move(selected);
        while (!lru.empty() && (bytes > limits.cache_bytes - segment->size || lru.size() >= limits.cache_segments))
            erase(lru.front());
        lru.push_back(entry); entry->position = std::prev(lru.end()); bytes += segment->size; peak = std::max(peak, bytes);
        return entry;
    }
    void publish(const std::vector<Token>& entries) {
        for (const auto& token : entries) {
            auto entry = token.lock();
            if (!entry || !entry->active || entry->published) continue;
            std::vector<size_t> selected;
            for (size_t row : entry->rows) {
                const auto& data = entry->segment->rows[row];
                if (auto prior = find(data.key)) {
                    require(prior->segment->rows[prior->row].size == data.size &&
                        !std::memcmp(prior->segment->bytes(prior->row), entry->segment->bytes(row), data.size),
                        "Snapshot object cache publish conflict");
                } else {
                    index.emplace(data.key, Index{entry, row}); selected.push_back(row);
                }
            }
            entry->rows = std::move(selected); entry->published = true;
            if (entry->rows.empty()) erase(entry);
        }
    }
    void cancel(const std::vector<Token>& entries) noexcept {
        for (const auto& token : entries) if (auto entry = token.lock(); entry && !entry->published) erase(entry);
    }
};
ObjectCache::ObjectCache(ObjectLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
ObjectCache::~ObjectCache() = default;

struct ObjectTransport::Impl {
    Fd socket;
    Limits limits;
    ObjectLimits bounds;
    std::shared_ptr<ObjectCache> cache;
    ObjectStats stats;
    bool failed = false;
    uint64_t tx = 0, rx = 0, pending_rx = 0;
    Deadline rx_deadline{};
    std::vector<ObjectCache::Impl::Token> incoming, outgoing;
    std::unordered_set<hash_type> confirmed;
    std::deque<hash_type> confirmation_order;
    Impl(Fd fd, Limits settings, std::shared_ptr<ObjectCache> store, ObjectLimits resource)
        : socket(std::move(fd)), limits(settings), bounds(resource), cache(store ? std::move(store) : std::make_shared<ObjectCache>(resource)) {
        int type = 0; socklen_t size = sizeof(type);
        require(getsockopt(socket.value, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == SOCK_SEQPACKET,
                "Snapshot object transport requires SOCK_SEQPACKET");
        sockaddr_storage address{}; socklen_t address_size = sizeof(address);
        require(getsockname(socket.value, reinterpret_cast<sockaddr*>(&address), &address_size) == 0 && address.ss_family == AF_UNIX,
                "Snapshot object transport requires a Unix socket");
        require(bounds.segment_bytes > header_bytes + row_bytes && bounds.segment_bytes <= limits.max_frame_bytes &&
                bounds.confirmed_keys > 0, "Invalid snapshot object transport bounds");
    }
    ~Impl() { cache->impl_->cancel(incoming); cache->impl_->cancel(outgoing); }
    void hint(hash_type key) {
        if (!confirmed.insert(key).second) return;
        confirmation_order.push_back(key);
        while (confirmed.size() > bounds.confirmed_keys) {
            confirmed.erase(confirmation_order.front()); confirmation_order.pop_front();
        }
    }
    Packet read(Deadline deadline, uint64_t transfer) {
        auto packet = receive_packet(socket.value, deadline, stats);
        require(packet.transfer == transfer, "Snapshot object transfer generation mismatch"); return packet;
    }
    void emit_segment(const std::shared_ptr<Segment>& segment,
                      const std::vector<std::pair<uint32_t,uint32_t>>& mapping,
                      uint64_t transfer, uint32_t chunk, uint32_t ordinal, Deadline deadline) {
        send_packet(socket.value, Op::Segment, transfer, chunk, ordinal, mapping, segment->descriptor.value, deadline, stats);
        auto ack = read(deadline, transfer);
        require(ack.op == Op::SegmentAck && ack.chunk == chunk && ack.extra == ordinal && ack.values.empty(),
                "Invalid snapshot segment acknowledgement");
    }
};
ObjectTransport::ObjectTransport(int fd, Limits limits, std::shared_ptr<ObjectCache> cache, ObjectLimits bounds) {
    Fd owner(fd); impl_ = std::make_unique<Impl>(std::move(owner), limits, std::move(cache), bounds);
}
ObjectTransport::~ObjectTransport() = default;
bool ObjectTransport::supports(const snapshot::Bundle& bundle) const {
    return std::all_of(bundle.blobs.begin(), bundle.blobs.end(), [&](const snapshot::Blob& blob) {
        return blob.bytes.size() <= impl_->bounds.segment_bytes - header_bytes - row_bytes;
    });
}
std::vector<BlobSpec> ObjectTransport::plan(const std::vector<snapshot::Blob>& blobs) {
    require(!impl_->failed, "Snapshot object transport is unusable");
    std::vector<BlobSpec> result; result.reserve(blobs.size());
    for (const auto& blob : blobs) {
        bool reference = false;
        if (auto view = impl_->cache->impl_->find(blob.key)) {
            require(ObjectCache::Impl::equals(*view, blob), "Snapshot object cache differs from current exported bytes");
            reference = impl_->confirmed.count(blob.key) != 0;
        }
        result.push_back({blob.key, blob.bytes.size(), reference});
    }
    return result;
}

void ObjectTransport::send(const std::vector<snapshot::Blob>& blobs, const std::vector<BlobSpec>& specs, Deadline deadline) {
    auto& state = *impl_;
    require(!state.failed && !state.pending_rx && blobs.size() <= state.limits.max_blobs && blobs.size() == specs.size() && state.tx != UINT64_MAX, "Invalid snapshot object send plan");
    for (size_t i = 0; i < blobs.size(); ++i)
        require(specs[i].key == blobs[i].key && specs[i].size == blobs[i].bytes.size(), "Changed snapshot send plan");
    const uint64_t transfer = ++state.tx;
    std::vector<bool> requested(blobs.size());
    try {
        uint32_t chunk = 0;
        for (;;) {
            auto need = state.read(deadline, transfer);
            require(need.op == Op::Need && need.chunk == chunk && need.extra <= 1 &&
                    (!need.values.empty() || need.extra), "Invalid snapshot missing-object request");
            std::map<std::shared_ptr<Segment>, std::vector<std::pair<uint32_t,uint32_t>>> groups;
            std::vector<uint32_t> fresh;
            for (const auto& [index, unused] : need.values) {
                (void)unused;
                require(index < blobs.size() && !requested[index], "Duplicate or unknown missing snapshot object");
                requested[index] = true;
                const auto& blob = blobs[index];
                require(specs[index].key == blob.key && specs[index].size == blob.bytes.size(), "Changed snapshot send plan");
                if (auto view = state.cache->impl_->find(blob.key)) {
                    require(ObjectCache::Impl::equals(*view, blob), "Snapshot cache byte conflict during send");
                    groups[view->segment].emplace_back(index, uint32_t(view->row));
                } else if (!blob.bytes.empty()) fresh.push_back(index);
            }
            uint32_t ordinal = 0;
            for (const auto& [segment, mapping] : groups)
                state.emit_segment(segment, mapping, transfer, chunk, ordinal++, deadline);
            groups.clear(); // No pinned historical mappings while creating a new batch.
            size_t next = 0;
            while (next < fresh.size()) {
                std::vector<const snapshot::Blob*> batch;
                std::vector<std::pair<uint32_t,uint32_t>> mapping;
                uint64_t bytes = header_bytes;
                while (next < fresh.size() && batch.size() < max_objects) {
                    const auto index = fresh[next]; const auto& blob = blobs[index];
                    if (row_bytes + blob.bytes.size() > state.bounds.segment_bytes - bytes) break;
                    bytes += row_bytes + blob.bytes.size();
                    mapping.emplace_back(index, uint32_t(batch.size())); batch.push_back(&blob); ++next;
                }
                require(!batch.empty(), "Snapshot object requires raw transfer fallback");
                auto segment = make_segment(batch, state.limits, state.bounds, state.stats);
                std::vector<size_t> rows;
                for (size_t i = 0; i < batch.size(); ++i) rows.push_back(i);
                state.outgoing.push_back(state.cache->impl_->stage(segment, std::move(rows)));
                state.emit_segment(segment, mapping, transfer, chunk, ordinal++, deadline);
                // SegmentAck permits release of this in-flight fd/map. Only the
                // explicitly budgeted cache may retain it pending import ACK.
            }
            send_packet(state.socket.value, Op::ChunkEnd, transfer, chunk, 0, {}, -1, deadline, state.stats);
            if (need.extra) break;
            require(chunk != UINT32_MAX, "Snapshot request chunk overflow"); ++chunk;
        }
        for (size_t i = 0; i < specs.size(); ++i)
            require(specs[i].reference || requested[i], "Peer omitted an unconfirmed snapshot object");
        auto committed = state.read(deadline, transfer);
        require(committed.op == Op::CommitAck && !committed.chunk && !committed.extra && committed.values.empty(),
                "Snapshot objects were not acknowledged after import");
        state.cache->impl_->publish(state.outgoing); state.outgoing.clear();
        for (const auto& blob : blobs) state.hint(blob.key);
    } catch (...) {
        state.failed = true; state.cache->impl_->cancel(state.outgoing); state.outgoing.clear(); throw;
    }
}
std::vector<snapshot::Blob> ObjectTransport::receive(const std::vector<BlobSpec>& specs, Deadline deadline) {
    auto& state = *impl_;
    require(!state.failed && !state.pending_rx && state.rx != UINT64_MAX && specs.size() <= state.limits.max_blobs,
            "Unconfirmed or invalid snapshot object receive");
    state.pending_rx = ++state.rx; state.rx_deadline = deadline;
    try {
        std::vector<snapshot::Blob> blobs;
        std::vector<uint32_t> missing;
        uint64_t total = 0;
        for (size_t i = 0; i < specs.size(); ++i) {
            const auto& spec = specs[i];
            require(spec.size <= state.limits.max_blob_bytes && spec.size <= SIZE_MAX &&
                    spec.size <= state.limits.max_frame_bytes - total, "Snapshot object receive exceeds transfer bound");
            total += spec.size;
            snapshot::Blob blob{spec.key, std::vector<uint8_t>(size_t(spec.size))};
            auto found = spec.reference ? state.cache->impl_->find(spec.key) : std::nullopt;
            if (found) {
                require(found->segment->rows[found->row].size == spec.size, "Snapshot cache reference length mismatch");
                if (spec.size) std::memcpy(blob.bytes.data(), found->segment->bytes(found->row), size_t(spec.size));
                state.stats.reference_bytes += spec.size;
            } else { missing.push_back(uint32_t(i)); state.stats.missing_bytes += spec.size; }
            blobs.push_back(std::move(blob));
        }
        size_t position = 0;
        uint32_t chunk = 0;
        do {
            const auto count = std::min(max_objects, missing.size() - position);
            std::vector<std::pair<uint32_t,uint32_t>> wanted;
            std::unordered_set<uint32_t> pending;
            for (size_t i = 0; i < count; ++i) { wanted.emplace_back(missing[position + i], 0); pending.insert(missing[position + i]); }
            const bool final = position + count == missing.size();
            send_packet(state.socket.value, Op::Need, state.pending_rx, chunk, final, wanted, -1, deadline, state.stats);
            uint32_t ordinal = 0;
            for (;;) {
                auto packet = state.read(deadline, state.pending_rx);
                require(packet.chunk == chunk, "Snapshot response chunk mismatch");
                if (packet.op == Op::ChunkEnd) {
                    require(!packet.extra && packet.values.empty(), "Malformed snapshot chunk ending"); break;
                }
                require(packet.op == Op::Segment && packet.extra == ordinal++ && !packet.values.empty(),
                        "Invalid snapshot segment response");
                auto segment = std::make_shared<Segment>(std::move(packet.fd), state.limits, state.bounds);
                ++state.stats.segments_received;
                state.stats.peak_inflight_bytes = std::max(state.stats.peak_inflight_bytes, segment->size);
                std::vector<size_t> rows;
                for (const auto& [index, row] : packet.values) {
                    require(pending.erase(index) == 1 && row < segment->rows.size(), "Unrequested or duplicate snapshot object response");
                    const auto& object = segment->rows[row];
                    require(object.key == specs[index].key && object.size == specs[index].size, "Snapshot object descriptor differs from closure");
                    std::memcpy(blobs[index].bytes.data(), segment->bytes(row), size_t(object.size)); rows.push_back(row);
                }
                state.incoming.push_back(state.cache->impl_->stage(segment, std::move(rows)));
                send_packet(state.socket.value, Op::SegmentAck, state.pending_rx, chunk, packet.extra, {}, -1, deadline, state.stats);
            }
            for (auto index : pending) require(specs[index].size == 0, "Missing snapshot object response");
            position += count;
            if (final) break;
            require(chunk != UINT32_MAX, "Snapshot receive chunk overflow"); ++chunk;
        } while (true);
        return blobs;
    } catch (...) { abort(); throw; }
}
void ObjectTransport::confirm() {
    auto& state = *impl_;
    require(!state.failed, "Snapshot object transport is unusable");
    if (!state.pending_rx) return;
    try {
        state.cache->impl_->publish(state.incoming);
        send_packet(state.socket.value, Op::CommitAck, state.pending_rx, 0, 0, {}, -1, state.rx_deadline, state.stats);
        state.incoming.clear(); state.pending_rx = 0;
    } catch (...) { abort(); throw; }
}
void ObjectTransport::abort() noexcept {
    auto& state = *impl_; state.failed = true;
    if (state.pending_rx) {
        try { send_packet(state.socket.value, Op::Abort, state.pending_rx, 0, 0, {}, -1,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(1), state.stats); } catch (...) {}
    }
    state.cache->impl_->cancel(state.incoming); state.incoming.clear(); state.pending_rx = 0;
}
bool ObjectTransport::pending() const { return impl_->pending_rx != 0; }
ObjectStats ObjectTransport::statistics() const {
    auto result = impl_->stats;
    result.cache_bytes = impl_->cache->impl_->bytes;
    result.cache_segments = impl_->cache->impl_->lru.size();
    result.peak_cache_bytes = impl_->cache->impl_->peak;
    return result;
}
} // namespace detsim::search::wire
