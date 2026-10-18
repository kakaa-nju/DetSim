#include "core/search/object_transport.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <future>
#include <poll.h>
#include <array>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include "utils.h"
namespace wire = detsim::search::wire;
using namespace std::chrono_literals;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static size_t fd_count() {
    DIR* dir = opendir("/proc/self/fd"); require(dir, "opendir fd");
    size_t count = 0; while (readdir(dir)) ++count; closedir(dir); return count;
}
static detsim::snapshot::Bundle bundle(size_t count, size_t size, unsigned salt = 0) {
    detsim::snapshot::Bundle value; value.system_key = 123; value.metadata = {3, 1, 4};
    for (size_t i = 0; i < count; ++i) {
        detsim::snapshot::Blob blob{}; blob.bytes.resize(size);
        for (size_t j = 0; j < size; ++j) blob.bytes[j] = uint8_t((i + salt) * 13 + j * 17 + (j >> 8));
        if (size >= sizeof(i)) std::memcpy(blob.bytes.data(), &i, sizeof(i));
        blob.key = XXHash64::hash(blob.bytes.data(), blob.bytes.size()); value.blobs.push_back(std::move(blob));
    }
    return value;
}
static void equal(const detsim::snapshot::Bundle& a, const detsim::snapshot::Bundle& b) {
    require(a.version == b.version && a.system_key == b.system_key && a.metadata == b.metadata && a.blobs.size() == b.blobs.size(), "bundle header mismatch");
    for (size_t i = 0; i < a.blobs.size(); ++i)
        require(a.blobs[i].key == b.blobs[i].key && a.blobs[i].bytes == b.blobs[i].bytes, "bundle bytes mismatch");
}
struct Pair {
    std::unique_ptr<wire::Channel> sender, receiver;
    explicit Pair(std::shared_ptr<wire::ObjectCache> left = {}, std::shared_ptr<wire::ObjectCache> right = {}, int timeout_ms = 1000) {
        int stream[2], data[2];
        require(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, stream), "stream pair");
        require(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, data), "data pair");
        wire::Limits limits; limits.io_timeout_ms = timeout_ms;
        sender = std::make_unique<wire::Channel>(stream[0], limits, data[0], std::move(left));
        receiver = std::make_unique<wire::Channel>(stream[1], limits, data[1], std::move(right));
    }
};
static void transfer(Pair& pair, const detsim::snapshot::Bundle& value, bool gate = false) {
    std::atomic<bool> done = false;
    auto sent = std::async(std::launch::async, [&] { pair.sender->send(wire::Task{{123, 1, 2, 0, true}, value}); done = true; });
    auto message = pair.receiver->receive(); require(message.has_value(), "missing task");
    equal(std::get<wire::Task>(*message).bundle, value);
    if (gate) { std::this_thread::sleep_for(10ms); require(!done, "sender returned before import confirmation"); }
    pair.receiver->confirm_objects(); sent.get(); require(done, "sender did not finish");
}
struct SharedMode {
    std::optional<std::string> previous;
    explicit SharedMode(const char* value) {
        if (const auto* old = std::getenv("DETSIM_SHARED_OBJECTS")) previous = old;
        require(!(value ? setenv("DETSIM_SHARED_OBJECTS", value, 1) : unsetenv("DETSIM_SHARED_OBJECTS")), "set shared transport mode");
    }
    ~SharedMode() {
        if (previous) setenv("DETSIM_SHARED_OBJECTS", previous->c_str(), 1);
        else unsetenv("DETSIM_SHARED_OBJECTS");
    }
};
static void diagnostic_modes() {
    {
        SharedMode unset(nullptr);
        Pair defaults;
        transfer(defaults, bundle(2, 4096));
        require(defaults.sender->counters().shared_frames == 0 && defaults.receiver->counters().shared_frames == 0 &&
                defaults.sender->counters().raw_fallback_frames == 1 && defaults.receiver->counters().raw_fallback_frames == 1,
                "unset mode must default to V1");
        require(defaults.sender->object_statistics().memfds_created == 0 && defaults.receiver->object_statistics().segments_received == 0,
                "unset mode used the object data channel");
    }
    Pair pair;
    const auto value = bundle(2, 4096);
    {
        SharedMode off("off"); transfer(pair, value);
        require(pair.sender->counters().shared_frames == 0 && pair.receiver->counters().shared_frames == 0,
                "off mode sent a shared frame");
        require(pair.sender->counters().raw_fallback_frames == 1 && pair.receiver->counters().raw_fallback_frames == 1 &&
                pair.sender->counters().raw_fallback_bytes == 8192 && pair.receiver->counters().raw_fallback_bytes == 8192,
                "explicit V1 selection must count raw frames and bytes");
        require(pair.sender->object_statistics().memfds_created == 0 && pair.receiver->object_statistics().segments_received == 0,
                "off mode used the object data channel");
    }
    transfer(pair, value, true);
    require(pair.sender->counters().shared_frames == 1 && pair.receiver->counters().shared_frames == 1,
            "on mode failed after diagnostic V1 selection");
    // The sender has already chosen/encoded V2 before off is installed. This
    // verifies that off affects local sends without rejecting peer V2 input.
    auto sent = std::async(std::launch::async, [&] { pair.sender->send(wire::Task{{123,1,2,0,true},value}); });
    pollfd ready{pair.receiver->fd(), POLLIN, 0};
    require(poll(&ready, 1, 1000) == 1 && (ready.revents & POLLIN), "missing V2 header before receive-only off");
    {
        SharedMode off("off");
        auto message = pair.receiver->receive(); require(message.has_value(), "off receiver rejected V2");
        equal(std::get<wire::Task>(*message).bundle,value); pair.receiver->confirm_objects(); sent.get();
    }
    require(pair.receiver->counters().shared_frames == 2, "off receiver did not accept V2");
    SharedMode invalid("invalid");
    bool rejected = false;
    try { pair.sender->send(wire::Shutdown{}); } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("Invalid DETSIM_SHARED_OBJECTS") != std::string::npos;
    }
    require(rejected, "invalid diagnostic mode did not fail explicitly");
}
static void cold_hot_fallback() {
    Pair pair;
    transfer(pair, bundle(0, 0), true);
    const auto value = bundle(600, 4096); // multiple <=256-object batches
    transfer(pair, value, true);
    const auto cold = pair.sender->object_statistics();
    require(cold.memfds_created == 3, "cold batch count");
    transfer(pair, value, true);
    require(pair.sender->object_statistics().memfds_created == cold.memfds_created, "hot transfer allocated new memfds");
    require(pair.receiver->object_statistics().reference_bytes == 600 * 4096, "hot transfer missed references");
    const auto large = bundle(1, 9 * 1024 * 1024);
    transfer(pair, large);
    require(pair.sender->counters().raw_fallback_frames == 1 && pair.sender->counters().raw_fallback_bytes == large.blobs[0].bytes.size(), "large-object raw fallback not counted");
    require(pair.receiver->counters().raw_fallback_frames == 1 && pair.receiver->counters().raw_fallback_bytes == large.blobs[0].bytes.size(), "incoming raw fallback not counted");
    transfer(pair, value, true);
    require(pair.receiver->object_statistics().reference_bytes == 2 * 600 * 4096, "V1 fallback disrupted V2 cache progress");
    require(pair.sender->object_statistics().peak_inflight_bytes <= (8u << 20), "inflight bound exceeded");
    pair.sender->send(wire::Shutdown{}); require(std::holds_alternative<wire::Shutdown>(*pair.receiver->receive()), "shutdown after objects");
}
static void eviction_and_shared_backing() {
    wire::ObjectLimits bounds; bounds.cache_bytes = 12000; bounds.cache_segments = 1;
    auto small = std::make_shared<wire::ObjectCache>(bounds);
    auto sender_cache = std::make_shared<wire::ObjectCache>();
    const auto first = bundle(2, 4096), other = bundle(2, 4096, 7);
    Pair pair(sender_cache, small);
    transfer(pair, first); transfer(pair, other); transfer(pair, first);
    require(pair.sender->object_statistics().memfds_created == 2, "sender did not reuse old immutable backing after peer eviction");
    const auto stats = pair.receiver->object_statistics();
    require(stats.reference_bytes == 0 && stats.missing_bytes == 6 * 4096, "missing-object fallback after eviction failed");
    require(stats.cache_bytes <= bounds.cache_bytes && stats.cache_segments <= 1, "receiver cache budget exceeded");
    // A different peer shares source backing but must get actual objects before references.
    Pair next(sender_cache);
    transfer(next, first);
    require(next.sender->object_statistics().memfds_created == 0 && next.receiver->object_statistics().segments_received == 1,
            "shared coordinator cache did not forward same immutable backing to new peer");
    for (unsigned i = 10; i < 35; ++i) transfer(pair, bundle(2, 4096, i));
    require(pair.receiver->object_statistics().cache_segments <= 1, "cache retained historical descriptors");
}
static void rejected_ack_and_poison() {
    Pair pair;
    const auto value = bundle(2, 4096);
    auto sent = std::async(std::launch::async, [&] {
        try { pair.sender->send(wire::Task{{123, 1, 2, 0, true}, value}); } catch (const std::runtime_error&) { return; }
        throw std::runtime_error("aborted import reported success");
    });
    require(pair.receiver->receive().has_value(), "missing abort task"); pair.receiver->abort_objects(); sent.get();
    bool failed = false; try { pair.sender->send(wire::Shutdown{}); } catch (const std::runtime_error&) { failed = true; }
    require(failed, "sender stream reused after rejected import");
    require(pair.sender->object_statistics().cache_bytes == 0 && pair.receiver->object_statistics().cache_bytes == 0,
            "unconfirmed objects retained after import abort");
}
static void ack_timeout_and_disconnect() {
    for (bool disconnect : {false, true}) {
        Pair pair({}, {}, 60);
        const auto value = bundle(2, 4096);
        auto sent = std::async(std::launch::async, [&] {
            try { pair.sender->send(wire::Task{{123, 1, 2, 0, true}, value}); } catch (const std::runtime_error&) { return; }
            throw std::runtime_error("missing final ACK reported success");
        });
        require(pair.receiver->receive().has_value(), "missing timeout task");
        if (disconnect) pair.receiver.reset();
        const auto begin = std::chrono::steady_clock::now(); sent.get();
        require(std::chrono::steady_clock::now() - begin < 400ms, "object ACK deadline did not bound stalled import");
        require(pair.sender->object_statistics().cache_bytes == 0, "timeout published unconfirmed objects");
    }
}
static void malformed_descriptor() {
    for (int kind = 0; kind < 6; ++kind) {
        int data[2]; require(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, data), "malformed pair");
        wire::Limits limits; limits.io_timeout_ms = 100;
        wire::ObjectTransport receiver(data[1], limits);
        const auto value = bundle(1, 32);
        auto peer = std::async(std::launch::async, [&, kind] {
            std::array<uint8_t, 2048> need{};
            require(recv(data[0], need.data(), need.size(), 0) > 0, "missing Need packet");
            const int fd = int(syscall(SYS_memfd_create, "malformed-test", MFD_CLOEXEC | MFD_ALLOW_SEALING));
            require(fd >= 0 && !ftruncate(fd, 80), "test memfd");
            std::array<uint8_t, 80> segment{};
            auto put = [](uint8_t* p, uint64_t value, size_t n) { for (size_t i = 0; i < n; ++i) p[i] = uint8_t(value >> (8 * i)); };
            put(segment.data(), 0x314745534d495344ULL, 8); put(segment.data()+8, 1, 8); put(segment.data()+16, 80, 8);
            put(segment.data()+24, value.blobs[0].key, 8); put(segment.data()+32, 48, 8); put(segment.data()+40, 32, 8);
            std::memcpy(segment.data()+48, value.blobs[0].bytes.data(), 32);
            if (kind == 1) segment[79] ^= 1; // sealed but corrupted content hash
            require(pwrite(fd, segment.data(), segment.size(), 0) == ssize_t(segment.size()), "write test segment");
            if (kind != 0) require(!fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL), "seal test segment");
            std::array<uint8_t, 36> packet{};
            put(packet.data(), 0x444f5344, 4); put(packet.data()+4, 1, 2); put(packet.data()+6, 2, 2);
            put(packet.data()+8, kind == 2 ? 2 : 1, 8); put(packet.data()+20, 1, 4);
            if (kind == 3) put(packet.data()+28, 8, 4); // unrequested object index
            iovec io{packet.data(), kind == 5 ? 0 : packet.size()}; msghdr message{}; message.msg_iov = &io; message.msg_iovlen = 1;
            alignas(cmsghdr) std::array<char, CMSG_SPACE(2 * sizeof(int))> ancillary{};
            message.msg_control = ancillary.data(); message.msg_controllen = ancillary.size();
            auto* header = CMSG_FIRSTHDR(&message); header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
            header->cmsg_len = CMSG_LEN(kind == 4 ? 2 * sizeof(int) : sizeof(int));
            std::memcpy(CMSG_DATA(header), &fd, sizeof(fd));
            if (kind == 4) std::memcpy(CMSG_DATA(header)+sizeof(int), &fd, sizeof(fd));
            message.msg_controllen = CMSG_SPACE(kind == 4 ? 2 * sizeof(int) : sizeof(int));
            require(sendmsg(data[0], &message, MSG_NOSIGNAL) == ssize_t(io.iov_len), "malformed send");
            close(fd);
        });
        bool rejected = false;
        try { receiver.receive({{value.blobs[0].key, 32, false}}, std::chrono::steady_clock::now()+100ms); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "malformed immutable descriptor accepted"); peer.get(); close(data[0]);
        rejected = false;
        try { receiver.receive({}, std::chrono::steady_clock::now()+100ms); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "failed object endpoint reused");
    }
}
int main() {
    SharedMode on("on");
    const auto before = fd_count();
    diagnostic_modes(); cold_hot_fallback(); eviction_and_shared_backing(); rejected_ack_and_poison(); ack_timeout_and_disconnect(); malformed_descriptor();
    require(fd_count() == before, "object channel/memfd leak");
    std::cout << "parallel object transport: on/off/invalid modes, cold/hot, ACK, eviction, peer reuse, V1 fallback, bounds, malformed descriptors, timeout, abort and fd cleanup PASS\n";
}
