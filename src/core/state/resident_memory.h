#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>
#include <string_view>
#include <sys/types.h>

class SnapshotView;
namespace detsim::resident {
// Only native automatic exploration owns a closed execution interval. Public
// capture/restore never assumes that a debugger or another caller left RAM alone.
class Scope {
public:
    Scope();
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
struct Image {
    struct Page {
        uint64_t address;
        uint32_t length;
        const uint8_t* data;
        std::shared_ptr<const std::vector<uint8_t>> owner;
        uint64_t key = 0;
    };
    struct Pages {
        std::vector<Page> values;
        std::vector<size_t> offsets;
    };
    std::shared_ptr<const SnapshotView> snapshot;
    uint64_t root = 0;
    uint64_t brk = 0;
    std::shared_ptr<const Pages> pages;
    const Page& page(size_t region, size_t index) const;
};
bool enabled();
bool verify();
Image get(pid_t pid);
// The heap break is an independent transient proof. It is established only by
// successful actual capture/restore publication for this scope/process/store
// generation, and it proves the endpoint alone: no bytes, mappings, registers or
// stoppedness. Demotion and ordinary guest writes keep it because only the full
// image lost live authority; explicit invalidation, incarnation/store changes,
// scope entry/exit and non-read-only resumes revoke it.
std::optional<uint64_t> known_brk(pid_t pid);
// This capability exposes only selected immutable regions, never a complete
// live image, mappings, brk or register state after user instructions ran.
class ReadOnlyImage {
    Image image_;
    std::vector<bool> valid_;
    friend ReadOnlyImage get_read_only(pid_t);
public:
    bool matches(std::string_view maps) const;
    bool region(size_t index, uint64_t start, uint64_t end, unsigned protection) const;
    const Image::Page& page(size_t region, size_t index) const;
};
ReadOnlyImage get_read_only(pid_t pid);
void publish(pid_t pid, uint64_t root, uint64_t brk);
void invalidate(pid_t tid);
void invalidate_all();
// Must be called before a possibly partial write. Reserved injection scratch
// is outside captured memory, but an overlapping write still invalidates.
void writing(pid_t tid, uintptr_t address, size_t length);
void resuming(pid_t tid);
class ReadOnlyInjection {
    bool read_only_;
    int exceptions_;
public:
    explicit ReadOnlyInjection(bool read_only);
    ~ReadOnlyInjection();
    ReadOnlyInjection(const ReadOnlyInjection&) = delete;
};
// Authorizes one user-to-entry resume, or a verified suppressed entry-to-exit
// resume. Signals/events and all other resumes revoke this weaker proof.
class ProtectedResume {
    bool preserve_;
public:
    explicit ProtectedResume(bool preserve = true);
    ~ProtectedResume();
    ProtectedResume(const ProtectedResume&) = delete;
};
}
