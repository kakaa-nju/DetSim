#include "protocol.h"
#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>

namespace detsim::search::wire {
namespace {
constexpr uint32_t magic = 0x50575344; // DSWP, little endian
constexpr uint16_t version = 1;
enum class Type : uint16_t { Hello = 1, Task, Edge, Done, Error, Shutdown };
[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
[[noreturn]] void io_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
using Clock = std::chrono::steady_clock;
void await_ready(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto left = deadline - Clock::now();
        if (left <= Clock::duration::zero()) fail("Worker protocol frame deadline exceeded");
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
        const int timeout = static_cast<int>(std::min<int64_t>(millis + 1, INT32_MAX));
        pollfd descriptor{fd, events, 0};
        const int ready = poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) io_error("Worker protocol poll");
        if (!ready) continue;
        if (descriptor.revents & POLLNVAL) fail("Worker protocol descriptor is invalid");
        return; // recv/send report EOF or the socket error after HUP/ERR.
    }
}
void write_exact(int fd, const void* data, size_t length, Clock::time_point deadline) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (length) {
        await_ready(fd, POLLOUT, deadline);
        const auto written = ::send(fd, bytes, length, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (written < 0) io_error("Worker protocol send");
        if (!written) fail("Worker protocol write made no progress");
        bytes += written; length -= static_cast<size_t>(written);
    }
}
bool read_exact(int fd, void* data, size_t length, Clock::time_point deadline, bool allow_eof = false) {
    auto* bytes = static_cast<uint8_t*>(data);
    size_t offset = 0;
    while (offset < length) {
        await_ready(fd, POLLIN, deadline);
        const auto received = ::recv(fd, bytes + offset, length - offset, MSG_DONTWAIT);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (received < 0) io_error("Worker protocol receive");
        if (!received) {
            if (!offset && allow_eof) return false;
            fail("Truncated worker protocol frame");
        }
        offset += static_cast<size_t>(received);
    }
    return true;
}
template<class T> std::array<uint8_t, sizeof(T)> little(T value) {
    static_assert(std::is_unsigned_v<T>);
    std::array<uint8_t, sizeof(T)> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(value >> (i * 8));
    return bytes;
}
template<class T> T number(const uint8_t* bytes) {
    static_assert(std::is_unsigned_v<T>);
    T result = 0;
    for (size_t i = 0; i < sizeof(T); ++i) result |= T(bytes[i]) << (i * 8);
    return result;
}
struct Writer {
    int fd;
    uint64_t size = 0;
    const Limits& limits;
    Clock::time_point deadline;
    void bytes(const void* data, uint64_t length) {
        if (length > limits.max_frame_bytes - size || length > SIZE_MAX)
            fail("Worker protocol frame exceeds transfer limit");
        size += length;
        if (fd >= 0 && length) write_exact(fd, data, static_cast<size_t>(length), deadline);
    }
    template<class T> void scalar(T value) { const auto encoded = little(value); bytes(encoded.data(), encoded.size()); }
    void text(const std::string& text) {
        if (text.size() > limits.max_text_bytes) fail("Worker protocol text exceeds transfer limit");
        scalar(uint64_t(text.size())); bytes(text.data(), text.size());
    }
    void bundle(const snapshot::Bundle& bundle) {
        if (bundle.metadata.size() > limits.max_metadata_bytes || bundle.blobs.size() > limits.max_blobs)
            fail("Worker bundle metadata/object count exceeds transfer limit");
        scalar(bundle.version); scalar(uint64_t(bundle.system_key));
        scalar(uint64_t(bundle.metadata.size())); bytes(bundle.metadata.data(), bundle.metadata.size());
        scalar(uint64_t(bundle.blobs.size()));
        for (const auto& blob : bundle.blobs) {
            if (blob.bytes.size() > limits.max_blob_bytes) fail("Worker bundle blob exceeds transfer limit");
            scalar(uint64_t(blob.key)); scalar(uint64_t(blob.bytes.size())); bytes(blob.bytes.data(), blob.bytes.size());
        }
    }
};
void encode(Writer& out, const Message& message) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Hello>) { out.text(value.model_identity); out.text(value.compatibility); }
        else if constexpr (std::is_same_v<T, Task>) {
            out.scalar(value.ticket.state); out.scalar(value.ticket.worker);
            out.scalar(value.ticket.lease_id); out.scalar(value.ticket.depth);
            out.scalar(uint8_t(value.ticket.may_expand)); out.bundle(value.bundle);
        } else if constexpr (std::is_same_v<T, Edge>) {
            out.scalar(value.lease_id); out.scalar(value.source); out.scalar(value.target);
            out.scalar(uint32_t(value.actor.process)); out.scalar(uint32_t(value.actor.thread));
            out.scalar(uint8_t(value.actor.action)); out.scalar(uint32_t(value.choice)); out.scalar(value.choice_count);
            out.scalar(uint8_t(value.property_violation)); out.scalar(uint8_t(value.result_code)); out.bundle(value.bundle);
        } else if constexpr (std::is_same_v<T, Done>) {
            out.scalar(value.lease_id); out.scalar(value.source); out.scalar(uint8_t(value.kind)); out.scalar(value.emitted);
        } else if constexpr (std::is_same_v<T, Error>) {
            out.scalar(value.lease_id); out.scalar(value.source); out.text(value.reason);
        }
    }, message);
}
struct Reader {
    int fd;
    uint64_t remaining;
    const Limits& limits;
    Clock::time_point deadline;
    void bytes(void* data, uint64_t length) {
        if (length > remaining || length > SIZE_MAX) fail("Worker protocol payload length mismatch");
        remaining -= length;
        if (length) read_exact(fd, data, static_cast<size_t>(length), deadline);
    }
    template<class T> T scalar() { std::array<uint8_t, sizeof(T)> encoded{}; bytes(encoded.data(), encoded.size()); return number<T>(encoded.data()); }
    bool boolean() { auto value = scalar<uint8_t>(); if (value > 1) fail("Invalid worker protocol boolean"); return value; }
    uint64_t length(uint64_t limit) {
        const auto length = scalar<uint64_t>();
        if (length > limit || length > remaining || length > SIZE_MAX)
            fail("Worker protocol length exceeds transfer limit or payload");
        return length;
    }
    std::string text() {
        std::string value(static_cast<size_t>(length(limits.max_text_bytes)), '\0');
        bytes(value.data(), value.size()); return value;
    }
    snapshot::Bundle bundle() {
        snapshot::Bundle result;
        result.version = scalar<uint32_t>(); result.system_key = scalar<uint64_t>();
        result.metadata.resize(static_cast<size_t>(length(limits.max_metadata_bytes)));
        bytes(result.metadata.data(), result.metadata.size());
        const auto count = scalar<uint64_t>();
        if (count > limits.max_blobs || count > remaining / 16 || count > SIZE_MAX / sizeof(snapshot::Blob))
            fail("Worker bundle object count exceeds transfer limit or payload");
        result.blobs.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            snapshot::Blob blob; blob.key = scalar<uint64_t>();
            blob.bytes.resize(static_cast<size_t>(length(limits.max_blob_bytes)));
            bytes(blob.bytes.data(), blob.bytes.size()); result.blobs.push_back(std::move(blob));
        }
        return result;
    }
};
Message decode(Reader& in, Type type) {
    switch (type) {
        case Type::Hello: { auto model = in.text(); return Hello{std::move(model), in.text()}; }
        case Type::Task: {
            Ticket ticket{}; ticket.state = in.scalar<uint64_t>(); ticket.worker = in.scalar<uint64_t>();
            ticket.lease_id = in.scalar<uint64_t>(); ticket.depth = in.scalar<uint64_t>(); ticket.may_expand = in.boolean();
            return Task{ticket, in.bundle()};
        }
        case Type::Edge: {
            Edge edge; edge.lease_id = in.scalar<uint64_t>(); edge.source = in.scalar<uint64_t>(); edge.target = in.scalar<uint64_t>();
            const auto process = in.scalar<uint32_t>(), thread = in.scalar<uint32_t>();
            if (process > INT32_MAX || thread > INT32_MAX) fail("Invalid worker protocol actor");
            edge.actor.process = int(process); edge.actor.thread = int(thread);
            const auto action = in.scalar<uint8_t>();
            if (action > uint8_t(TransitionAction::Timeout)) fail("Invalid worker protocol action");
            edge.actor.action = TransitionAction(action);
            const auto choice = in.scalar<uint32_t>();
            if (choice > INT32_MAX && choice != uint32_t(-1) && choice != uint32_t(-2))
                fail("Invalid worker protocol choice");
            edge.choice = choice == uint32_t(-1) ? -1 : choice == uint32_t(-2) ? -2 : int(choice);
            edge.choice_count = in.scalar<uint32_t>();
            if (edge.choice_count > INT32_MAX) fail("Invalid worker protocol choice count");
            edge.property_violation = in.boolean();
            const auto result_code = in.scalar<uint8_t>();
            if (result_code > uint8_t(CKPT_DISCARD)) fail("Invalid worker protocol transition result code");
            edge.result_code = TransitionResultCode(result_code); edge.bundle = in.bundle(); return edge;
        }
        case Type::Done: {
            Done done; done.lease_id = in.scalar<uint64_t>(); done.source = in.scalar<uint64_t>();
            const auto kind = in.scalar<uint8_t>();
            if (kind > uint8_t(DoneKind::Violation)) fail("Invalid worker protocol completion kind");
            done.kind = DoneKind(kind); done.emitted = in.scalar<uint64_t>(); return done;
        }
        case Type::Error: { auto lease = in.scalar<uint64_t>(), source = in.scalar<uint64_t>(); return Error{lease, source, in.text()}; }
        case Type::Shutdown: return Shutdown{};
    }
    fail("Unknown worker protocol message type");
}
} // namespace
Channel::Channel(int fd, Limits limits) : fd_(fd), limits_(limits) {
    try {
    if (fd < 0 || !limits.max_frame_bytes || !limits.max_blob_bytes || !limits.max_metadata_bytes ||
        !limits.max_blobs || !limits.max_text_bytes || limits.io_timeout_ms <= 0)
        fail("Invalid worker protocol descriptor or limits");
    int type = 0; socklen_t size = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_STREAM)
        fail("Worker protocol requires a stream socket");
    sockaddr_storage address{}; socklen_t address_size = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) || address.ss_family != AF_UNIX)
        fail("Worker protocol requires a Unix socket");
    const timeval timeout{limits.io_timeout_ms / 1000, (limits.io_timeout_ms % 1000) * 1000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) io_error("Worker protocol timeout setup");
    } catch (...) { if (fd_ >= 0) close(fd_); fd_ = -1; throw; }
}
Channel::~Channel() { if (fd_ >= 0) close(fd_); }
Channel::Channel(Channel&& other) noexcept : fd_(other.fd_), limits_(other.limits_), failed_(other.failed_) { other.fd_ = -1; }
Channel& Channel::operator=(Channel&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) close(fd_);
        fd_ = other.fd_; limits_ = other.limits_; failed_ = other.failed_; other.fd_ = -1;
    }
    return *this;
}
void Channel::send(const Message& message) const {
    if (failed_ || fd_ < 0) fail("Worker protocol stream is unusable");
    try {
    const auto deadline = Clock::now() + std::chrono::milliseconds(limits_.io_timeout_ms);
    Writer measure{-1, 0, limits_, deadline}; encode(measure, message);
    const auto m = little(magic); const auto v = little(version);
    const auto t = little(uint16_t(message.index() + 1)); const auto size = little(measure.size);
    std::array<uint8_t, 16> header{};
    std::memcpy(header.data(), m.data(), 4); std::memcpy(header.data() + 4, v.data(), 2);
    std::memcpy(header.data() + 6, t.data(), 2); std::memcpy(header.data() + 8, size.data(), 8);
    write_exact(fd_, header.data(), header.size(), deadline);
    Writer out{fd_, 0, limits_, deadline}; encode(out, message);
    } catch (...) { failed_ = true; throw; }
}
std::optional<Message> Channel::receive() const {
    if (failed_ || fd_ < 0) fail("Worker protocol stream is unusable");
    try {
    const auto deadline = Clock::now() + std::chrono::milliseconds(limits_.io_timeout_ms);
    std::array<uint8_t, 16> header{};
    if (!read_exact(fd_, header.data(), header.size(), deadline, true)) { failed_ = true; return std::nullopt; }
    if (number<uint32_t>(header.data()) != magic || number<uint16_t>(header.data() + 4) != version)
        fail("Worker protocol magic/version mismatch");
    const auto type = number<uint16_t>(header.data() + 6);
    const auto size = number<uint64_t>(header.data() + 8);
    if (type < uint16_t(Type::Hello) || type > uint16_t(Type::Shutdown)) fail("Unknown worker protocol message type");
    if (size > limits_.max_frame_bytes) fail("Worker protocol frame exceeds transfer limit");
    Reader in{fd_, size, limits_, deadline};
    auto message = decode(in, Type(type));
    if (in.remaining) fail("Trailing worker protocol payload bytes");
    return message;
    } catch (...) { failed_ = true; throw; }
}
} // namespace detsim::search::wire
