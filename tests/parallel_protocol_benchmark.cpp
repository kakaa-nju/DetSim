#include "protocol.h"
#include "object_transport.h"
#include <cstring>
#include "utils.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>

namespace wire = detsim::search::wire;
int main(int argc, char** argv) {
    const bool shared = argc == 2 && !std::strcmp(argv[1], "--shared");
    if (argc > 1 && !shared) return 2;
    try {
        if (setenv("DETSIM_SHARED_OBJECTS", shared ? "on" : "off", 1))
            throw std::runtime_error("Cannot select protocol benchmark transport mode");
        constexpr unsigned frames = 4, pages = 2048, page_size = 4096;
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) throw std::runtime_error("socketpair failed");
        int data[2] = {-1, -1};
        if (shared && socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, data)) throw std::runtime_error("object socketpair failed");
        wire::Channel reader(sockets[1], {}, data[1]);
        std::exception_ptr error;
#ifdef DETSIM_PROTOCOL_BUFFER_STATS
        wire::Counters sent;
        wire::ObjectStats sender_objects;
#endif
        const auto started = std::chrono::steady_clock::now();
        std::thread writer([&] {
            try {
                wire::Channel sender(sockets[0], {}, data[0]);
                detsim::snapshot::Bundle bundle; bundle.system_key = 7; bundle.metadata.assign(32, 0x57);
                for (unsigned page = 0; page < pages; ++page) {
                    std::vector<uint8_t> bytes(page_size, uint8_t(page));
                    std::memcpy(bytes.data(), &page, sizeof(page));
                    const auto key = XXHash64::hash(bytes.data(), bytes.size());
                    bundle.blobs.push_back({key, std::move(bytes)});
                }
                wire::Message task = wire::Task{{7, 1, 2, 0, true}, std::move(bundle)};
                for (unsigned frame = 0; frame < frames; ++frame) sender.send(task);
                sender.send(wire::Shutdown{});
#ifdef DETSIM_PROTOCOL_BUFFER_STATS
                sent = sender.counters();
                sender_objects = sender.object_statistics();
#endif
            } catch (...) { error = std::current_exception(); }
        });
        bool valid = true;
        try {
            for (unsigned frame = 0; frame < frames; ++frame) {
                auto message = reader.receive();
                if (!message || !std::holds_alternative<wire::Task>(*message)) throw std::runtime_error("missing task");
                const auto& bundle = std::get<wire::Task>(*message).bundle;
                if (bundle.metadata != std::vector<uint8_t>(32, 0x57) || bundle.blobs.size() != pages)
                    throw std::runtime_error("metadata/object count mismatch");
                for (unsigned page = 0; page < pages; ++page) {
                    const auto& blob = bundle.blobs[page];
                    std::vector<uint8_t> expected(page_size, uint8_t(page));
                    std::memcpy(expected.data(), &page, sizeof(page));
                    if (blob.key != XXHash64::hash(expected.data(), expected.size()) || blob.bytes != expected)
                        throw std::runtime_error("page transport mismatch");
                }
                reader.confirm_objects();
            }
            auto end = reader.receive();
            valid = end && std::holds_alternative<wire::Shutdown>(*end);
        } catch (...) { valid = false; }
        writer.join();
        if (error) std::rethrow_exception(error);
        if (!valid) throw std::runtime_error("transport verification failed");
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::printf("{\"frames\":%u,\"pages_per_frame\":%u,\"payload_bytes\":%llu,\"seconds\":%.9f",
            frames, pages, static_cast<unsigned long long>(frames) * pages * page_size, elapsed);
#ifdef DETSIM_PROTOCOL_BUFFER_STATS
        const auto received = reader.counters();
        if ((!shared && sent.bytes_sent != 33685956) || received.bytes_received != sent.bytes_sent ||
            !sent.send_calls || !received.recv_calls ||
            sent.send_calls + received.recv_calls + sent.poll_calls + received.poll_calls >= 20000)
            throw std::runtime_error("buffered page transport exceeded the syscall budget or changed wire bytes");
        std::printf(",\"send_calls\":%llu,\"recv_calls\":%llu,\"poll_calls\":%llu,\"bytes_sent\":%llu,\"bytes_received\":%llu",
            static_cast<unsigned long long>(sent.send_calls), static_cast<unsigned long long>(received.recv_calls),
            static_cast<unsigned long long>(sent.poll_calls + received.poll_calls),
            static_cast<unsigned long long>(sent.bytes_sent), static_cast<unsigned long long>(received.bytes_received));
#endif
#ifdef DETSIM_PROTOCOL_BUFFER_STATS
        if (shared) {
            const auto objects = reader.object_statistics();
            if (sender_objects.memfds_created != 8 || objects.reference_bytes != uint64_t(frames - 1) * pages * page_size ||
                objects.peak_cache_bytes > (64u << 20) || objects.cache_segments > 128)
                throw std::runtime_error("shared object cache/batch/reference budget mismatch");
            std::printf(",\"shared\":true,\"object_send_calls\":%llu,\"object_recv_calls\":%llu,\"object_poll_calls\":%llu,\"memfds_created\":%llu,\"memfd_bytes_created\":%llu,\"reference_bytes\":%llu,\"peak_cache_bytes\":%llu",
                static_cast<unsigned long long>(sender_objects.send_calls + objects.send_calls),
                static_cast<unsigned long long>(sender_objects.recv_calls + objects.recv_calls),
                static_cast<unsigned long long>(sender_objects.poll_calls + objects.poll_calls),
                static_cast<unsigned long long>(sender_objects.memfds_created),
                static_cast<unsigned long long>(sender_objects.memfd_bytes_created),
                static_cast<unsigned long long>(objects.reference_bytes),
                static_cast<unsigned long long>(objects.peak_cache_bytes));
        }
#endif
        std::puts("}");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
