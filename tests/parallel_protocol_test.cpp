#include "core/search/protocol.h"
#include <array>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace wire = detsim::search::wire;
static void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class F> static void rejected(F fn, const char* reason) {
    bool failed = false;
    try { fn(); } catch (const std::runtime_error&) { failed = true; }
    require(failed, reason);
}
static std::array<int, 2> sockets() {
    std::array<int, 2> pair{};
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) == 0, "socketpair failed");
    return pair;
}
static detsim::snapshot::Bundle bundle() {
    detsim::snapshot::Bundle result; result.system_key = 11;
    result.metadata = {1, 2, 3}; result.blobs.push_back({12, std::vector<uint8_t>(1 << 20, 0x73)});
    return result;
}
static void round_trip() {
    const auto fds = sockets(); wire::Channel receiver(fds[1]);
    std::exception_ptr failure;
    std::thread writer([&] {
        try {
            wire::Channel sender(fds[0]);
            sender.send(wire::Hello{"model", "compatibility-profile"});
            sender.send(wire::Task{{11, 4, 8, 3, true}, bundle()});
            wire::Edge edge; edge.lease_id = 8; edge.source = 11; edge.target = 12;
            edge.actor = {1, 3, TransitionAction::Timeout}; edge.choice = -2; edge.choice_count = 0;
            edge.property_violation = true; edge.result_code = CKPT_STOP; edge.bundle = bundle();
            sender.send(std::move(edge));
            sender.send(wire::Done{8, 11, wire::DoneKind::Violation, 1});
            sender.send(wire::Error{8, 11, "unsupported"});
            sender.send(wire::Shutdown{});
        } catch (...) { failure = std::current_exception(); }
    });
    auto hello = receiver.receive(); require(hello && std::get<wire::Hello>(*hello).model_identity == "model" &&
        std::get<wire::Hello>(*hello).compatibility == "compatibility-profile", "hello round trip");
    auto task = receiver.receive(); require(task && std::get<wire::Task>(*task).ticket.depth == 3 &&
        std::get<wire::Task>(*task).ticket.may_expand && std::get<wire::Task>(*task).bundle.blobs[0].bytes == bundle().blobs[0].bytes,
        "task/bundle round trip");
    auto edge = receiver.receive(); require(edge && std::get<wire::Edge>(*edge).actor.thread == 3 &&
        std::get<wire::Edge>(*edge).choice == -2 && std::get<wire::Edge>(*edge).choice_count == 0 && std::get<wire::Edge>(*edge).property_violation &&
        std::get<wire::Edge>(*edge).result_code == CKPT_STOP, "edge round trip");
    auto done = receiver.receive(); require(done && std::get<wire::Done>(*done).kind == wire::DoneKind::Violation &&
        std::get<wire::Done>(*done).emitted == 1, "done round trip");
    auto error = receiver.receive(); require(error && std::get<wire::Error>(*error).reason == "unsupported", "error round trip");
    auto shutdown = receiver.receive(); require(shutdown && std::holds_alternative<wire::Shutdown>(*shutdown), "shutdown round trip");
    require(!receiver.receive(), "clean EOF missing");
    writer.join(); if (failure) std::rethrow_exception(failure);
}
static void malformed() {
    // Shutdown with a trailing byte, unknown message/version and partial headers.
    for (int mode = 0; mode < 5; ++mode) {
        const auto fds = sockets(); wire::Channel receiver(fds[1]);
        std::array<unsigned char, 17> bytes{0x44, 0x53, 0x57, 0x50, 1, 0, 6, 0};
        size_t length = 16;
        if (mode == 0) { bytes[8] = 1; length = 17; }
        if (mode == 1) bytes[6] = 77;
        if (mode == 2) bytes[4] = 2;
        if (mode == 3) length = 7;
        if (mode == 4) { bytes[6] = 1; bytes[8] = 8; } // missing Hello length/body
        require(send(fds[0], bytes.data(), length, MSG_NOSIGNAL) == static_cast<ssize_t>(length), "raw malformed send");
        close(fds[0]);
        rejected([&] { receiver.receive(); }, "malformed frame accepted");
        rejected([&] { receiver.receive(); }, "failed channel was reused");
    }
}
static void limits_and_timeout() {
    {
        const auto fds = sockets(); wire::Limits limits; limits.max_metadata_bytes = 2;
        wire::Channel sender(fds[0], limits); wire::Channel receiver(fds[1]);
        rejected([&] { sender.send(wire::Task{{11, 1, 2, 0, true}, bundle()}); }, "oversize metadata sent");
        rejected([&] { sender.send(wire::Shutdown{}); }, "send continued after error");
    }
    {
        const auto fds = sockets(); wire::Limits limits; limits.io_timeout_ms = 20;
        wire::Channel receiver(fds[1], limits);
        rejected([&] { receiver.receive(); }, "idle socket timeout ignored"); close(fds[0]);
    }
    {
        const auto fds = sockets(); wire::Channel sender(fds[0]); close(fds[1]);
        rejected([&] { sender.send(wire::Shutdown{}); }, "closed peer send accepted");
    }
    {
        const auto fds = sockets();
        const int moved = fds[0];
        { wire::Channel original(fds[0]); wire::Channel owner(std::move(original)); }
        require(fcntl(moved, F_GETFD) == -1 && errno == EBADF, "Channel did not own/close moved descriptor");
        close(fds[1]);
    }
}
static void configured_choice_round_trip() {
    const auto fds = sockets(); wire::Channel sender(fds[0]), receiver(fds[1]);
    wire::Edge edge; edge.lease_id = 5; edge.source = 1; edge.target = 2;
    edge.actor = {0, 1, TransitionAction::Normal}; edge.choice = 2; edge.choice_count = 3;
    edge.result_code = CKPT_YES;
    sender.send(std::move(edge));
    auto message = receiver.receive();
    require(message && std::get<wire::Edge>(*message).choice == 2 &&
            std::get<wire::Edge>(*message).choice_count == 3 &&
            std::get<wire::Edge>(*message).result_code == CKPT_YES, "configured choice cardinality round trip");
}

static void receive_limits() {
    for (int mode = 0; mode < 5; ++mode) {
        const auto fds = sockets(); wire::Limits limits;
        if (mode == 0) limits.max_metadata_bytes = 2;
        if (mode == 1) limits.max_blob_bytes = 4;
        if (mode == 2) limits.max_blobs = 1;
        if (mode == 3) limits.max_frame_bytes = 16;
        if (mode == 4) limits.max_text_bytes = 2;
        wire::Channel sender(fds[0]), receiver(fds[1], limits);
        auto value = bundle(); value.blobs[0].bytes.resize(8);
        if (mode == 2) value.blobs.push_back({13, {4}});
        if (mode == 4) sender.send(wire::Hello{"model", "compatibility"});
        else sender.send(wire::Task{{11, 1, 2, 0, true}, std::move(value)});
        rejected([&] { receiver.receive(); }, "oversize incoming frame/object accepted");
    }
}

static void partial_progress_deadline() {
    for (bool slow_header : {false, true}) {
        const auto fds = sockets(); wire::Limits limits; limits.io_timeout_ms = 45;
        wire::Channel receiver(fds[1], limits);
        std::atomic<bool> stop = false;
        std::thread writer([&] {
            std::array<unsigned char, 36> bytes{0x44, 0x53, 0x57, 0x50, 1, 0, 1, 0};
            bytes[8] = 20; bytes[16] = 4;
            bytes[24] = 't'; bytes[25] = 'e'; bytes[26] = 's'; bytes[27] = 't';
            size_t offset = 0;
            if (!slow_header) {
                if (send(fds[0], bytes.data(), 24, MSG_NOSIGNAL) != 24) { close(fds[0]); return; }
                offset = 24;
            }
            for (; offset < bytes.size() && !stop; ++offset) {
                if (send(fds[0], bytes.data() + offset, 1, MSG_NOSIGNAL) != 1) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            close(fds[0]);
        });
        bool deadline = false;
        try { (void)receiver.receive(); }
        catch (const std::runtime_error& error) {
            deadline = std::string(error.what()).find("frame deadline") != std::string::npos;
        }
        stop = true; writer.join();
        require(deadline, "partial progress extended the complete-frame deadline");
    }
}

int main() {
    try {
        round_trip(); malformed(); limits_and_timeout(); configured_choice_round_trip(); receive_limits(); partial_progress_deadline();
        std::puts("parallel protocol round-trip, bounds, truncation, timeout, RAII: OK"); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
