#pragma once

#include "frontier.h"
#include "state/snapshot_bundle.h"
#include "state/state_transition.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace detsim::search::wire {
// Limits apply before allocation and to the entire frame, including all bundle
// objects. A single absolute deadline covers the header and entire streamed
// payload even when the peer makes partial progress. The coordinator separately
// owns job deadlines and may kill a stalled worker.
struct Limits {
    uint64_t max_frame_bytes = uint64_t{8} << 30;
    uint64_t max_blob_bytes = uint64_t{2} << 30;
    uint64_t max_metadata_bytes = uint64_t{64} << 20;
    uint64_t max_blobs = 1'000'000;
    uint64_t max_text_bytes = uint64_t{1} << 20;
    int io_timeout_ms = 30'000;
};
struct Hello { std::string model_identity; std::string compatibility = {}; };
struct Task { Ticket ticket; snapshot::Bundle bundle; };
struct Edge {
    uint64_t lease_id = 0;
    StateKey source = 0, target = 0;
    ActorAction actor{};
    int choice = -1;
    uint32_t choice_count = 0; // 0: fixed -1 normal/-2 timeout; N: exact configured alternatives
    bool property_violation = false;
    TransitionResultCode result_code = CKPT_NO;
    snapshot::Bundle bundle;
};
enum class DoneKind : uint8_t { Expanded, Terminal, DepthCut, ModelBudget, Violation };
struct Done {
    uint64_t lease_id = 0;
    StateKey source = 0;
    DoneKind kind = DoneKind::Expanded;
    uint64_t emitted = 0;
};
struct Error { uint64_t lease_id = 0; StateKey source = 0; std::string reason; };
struct Shutdown {};
using Message = std::variant<Hello, Task, Edge, Done, Error, Shutdown>;

// Owns an exclusively owned Unix stream socket. Bundle
// vectors are read/written directly, without a second encoded packet or base64.
class Channel {
public:
    explicit Channel(int fd, Limits limits = {});
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&& other) noexcept;
    Channel& operator=(Channel&& other) noexcept;
    void send(const Message& message) const;
    // nullopt means clean EOF BETWEEN frames only. Truncation, invalid encodings,
    // timeouts and I/O failures throw; a failed stream must never be reused.
    std::optional<Message> receive() const;
    int fd() const { return fd_; }
private:
    int fd_;
    Limits limits_;
    mutable bool failed_ = false;
};
} // namespace detsim::search::wire
