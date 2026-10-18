#include "resident_memory.h"
#include "page_snapshot.h"
#include "monitor.h"
#include "guest.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <cstdio>
#include <sstream>
#include <string>
#include <poll.h>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <unistd.h>

namespace detsim::resident {
namespace {
struct Entry {
    pid_t pid = 0;
    int pidfd = -1;
    uint64_t generation = 0;
    Image image;
    Image protected_image;
    Image object_hint; // immutable saved-object cache only, never a live proof
    std::vector<bool> protected_valid;
    std::string eligible_maps;
    std::vector<bool> eligible;
    void clear() {
        image = {}; object_hint = {};
        protected_image = {}; protected_valid.clear(); eligible_maps.clear(); eligible.clear();
        if (pidfd >= 0) close(pidfd);
        pidfd = -1; pid = 0; generation = 0;
    }
};
std::array<Entry, NP> entries;
unsigned scopes = 0;
bool active = false, oracle = false;
thread_local unsigned read_only_injections = 0;
thread_local unsigned protected_resumes = 0;
int node_for(pid_t tid) {
    for (int node = 0; node < NP; ++node) {
        if (ptmc_state.pids[node] == tid) return node;
        for (const auto& thread : ptmc_state.running_state.child[node].threads)
            if (thread.physical_tid == tid) return node;
    }
    return -1;
}
Entry* checked(pid_t pid) {
    if (!enabled()) return nullptr;
    const char* capture = std::getenv("DETSIM_CAPTURE_MODE");
    if (capture && !std::strcmp(capture, "full")) return nullptr;
    const int node = node_for(pid);
    if (node < 0) return nullptr;
    auto& entry = entries[node];
    if (entry.pid != ptmc_state.pids[node] || entry.generation != StateStore::instance().snapshot_generation()) {
        entry.clear(); return nullptr;
    }
    pollfd descriptor{entry.pidfd, POLLIN, 0};
    if (entry.pidfd < 0 || poll(&descriptor, 1, 0) != 0) {
        entry.clear(); return nullptr;
    }
    return &entry;
}
void demote(Entry& entry) {
    if (entry.image.snapshot) {
        entry.protected_image = entry.image;
        entry.protected_valid = entry.eligible;
    }
    entry.image = {};
}
bool same_geometry(const SnapshotView& before, const SnapshotView& after) {
    const auto& left = before.regions();
    const auto& right = after.regions();
    if (before.maps_text() != after.maps_text() || left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        const auto& old = left[i];
        const auto& current = right[i];
        if (old.start != current.start || old.end != current.end || old.offset != current.offset ||
            old.prot != current.prot || std::memcmp(old.flags, current.flags, sizeof old.flags)) return false;
    }
    return true;
}
void describe_protected(Entry& entry, const SnapshotView& snapshot, pid_t pid) {
    if (entry.eligible_maps == snapshot.maps_text()) return;
    std::vector<bool> eligible(snapshot.regions().size(), false);
    std::istringstream records(std::string(snapshot.maps_text()));
    std::string line;
    while (std::getline(records, line)) {
        unsigned long start, end, offset, inode;
        unsigned major, minor; char flags[5]{}; int consumed = 0;
        if (sscanf(line.c_str(), "%lx-%lx %4s %lx %x:%x %lu %n", &start, &end, flags,
                   &offset, &major, &minor, &inode, &consumed) != 7) continue;
        if (!inode || flags[0] != 'r' || flags[1] == 'w' || flags[3] != 'p') continue;
        const auto path = line.substr(consumed);
        if (path.empty() || path.front() != '/') continue;
        struct stat status{};
        const auto rooted = "/proc/" + std::to_string(pid) + "/root" + path;
        if (stat(rooted.c_str(), &status) || !S_ISREG(status.st_mode) || status.st_ino != inode ||
            status.st_dev != makedev(major, minor)) continue;
        for (size_t i = 0; i < snapshot.regions().size(); ++i) {
            const auto& region = snapshot.regions()[i];
            if (region.start == start && region.end == end && (region.prot & PROT_READ) &&
                !(region.prot & PROT_WRITE)) eligible[i] = true;
        }
    }
    entry.eligible_maps = snapshot.maps_text(); entry.eligible = std::move(eligible);
}
}
Scope::Scope() {
    if (scopes++) return;
    invalidate_all();
    const char* mode = std::getenv("DETSIM_RESIDENT_MEMORY");
    if (mode && std::strcmp(mode, "on") && std::strcmp(mode, "off") && std::strcmp(mode, "verify")) {
        --scopes;
        throw std::runtime_error("Invalid DETSIM_RESIDENT_MEMORY: expected on, off or verify");
    }
    active = !mode || std::strcmp(mode, "off");
    oracle = mode && !std::strcmp(mode, "verify");
    // The previous public command could have changed live bytes. Establish the
    // first resident proof via real restore, even if the selected hash matches.
    if (active) for (int node = 0; node < NP; ++node) ptmc_state.tracee_image_dirty[node] = true;
}
Scope::~Scope() {
    if (!--scopes) { invalidate_all(); active = oracle = false; }
}
bool enabled() { return scopes && active; }
bool verify() { return enabled() && oracle; }
const Image::Page& Image::page(size_t region, size_t index) const {
    if (!pages || region >= pages->offsets.size())
        throw std::runtime_error("Resident page region out of range");
    const size_t begin = pages->offsets[region];
    const size_t end = region + 1 < pages->offsets.size() ? pages->offsets[region + 1] : pages->values.size();
    if (index >= end - begin)
        throw std::runtime_error("Resident page index out of range");
    return pages->values[begin + index];
}
void invalidate_all() { for (auto& entry : entries) entry.clear(); }
void invalidate(pid_t tid) {
    if (!enabled()) return;
    const int node = node_for(tid);
    if (node < 0) { invalidate_all(); return; }
    entries[node].image = {};
    entries[node].protected_image = {}; entries[node].protected_valid.clear();
}
void writing(pid_t tid, uintptr_t address, size_t length) {
    if (!length) return;
    if (address >= scratch_page && address - scratch_page < PAGE_SIZE &&
        length <= PAGE_SIZE - (address - scratch_page)) return;
    if (!enabled()) return;
    const int node = node_for(tid);
    if (node < 0 || length > UINTPTR_MAX - address) { invalidate_all(); return; }
    auto& entry = entries[node];
    if (entry.pid != ptmc_state.pids[node] || entry.generation != StateStore::instance().generation()) {
        entry.clear(); return;
    }
    demote(entry);
    if (entry.protected_image.snapshot) {
        const auto& regions = entry.protected_image.snapshot->regions();
        for (size_t i = 0; i < regions.size(); ++i)
            if (address < regions[i].end && regions[i].start < address + length)
                entry.protected_valid[i] = false;
    }
}
void resuming(pid_t tid) {
    if (read_only_injections) return;
    if (protected_resumes) {
        if (auto* entry = checked(tid)) demote(*entry);
        else invalidate(tid);
    } else invalidate(tid);
}
ProtectedResume::ProtectedResume(bool preserve) : preserve_(preserve) { if (preserve_) ++protected_resumes; }
ProtectedResume::~ProtectedResume() { if (preserve_) --protected_resumes; }
ReadOnlyInjection::ReadOnlyInjection(bool read_only)
    : read_only_(read_only), exceptions_(std::uncaught_exceptions()) {
    if (read_only_) ++read_only_injections;
}
ReadOnlyInjection::~ReadOnlyInjection() {
    if (read_only_) --read_only_injections;
    if (std::uncaught_exceptions() > exceptions_) invalidate_all();
}
Image get(pid_t pid) {
    const auto* entry = checked(pid);
    return entry ? entry->image : Image{};
}
ReadOnlyImage get_read_only(pid_t pid) {
    ReadOnlyImage result;
    if (const auto* entry = checked(pid)) {
        result.image_ = entry->protected_image;
        result.valid_ = entry->protected_valid;
    }
    return result;
}
bool ReadOnlyImage::matches(std::string_view maps) const {
    return image_.snapshot && image_.snapshot->maps_text() == maps;
}
bool ReadOnlyImage::region(size_t index, uint64_t start, uint64_t end, unsigned protection) const {
    if (!image_.snapshot || index >= valid_.size() || !valid_[index]) return false;
    const auto& region = image_.snapshot->regions().at(index);
    return region.start == start && region.end == end && region.prot == protection;
}
const Image::Page& ReadOnlyImage::page(size_t region, size_t index) const {
    if (!valid_.at(region)) throw std::runtime_error("Readonly residency does not cover this region");
    return image_.page(region, index);
}
void publish(pid_t pid, uint64_t root, uint64_t brk) {
    if (!enabled()) return;
    const int node = node_for(pid);
    if (node < 0 || pid != ptmc_state.pids[node]) return;
    auto& entry = entries[node];
    if (entry.pid != pid || entry.generation != StateStore::instance().generation()) entry.clear();
    auto snapshot = SnapshotView::open(root);
    if (entry.pid == pid && entry.image.root == root &&
        entry.generation == StateStore::instance().generation()) return;
    std::shared_ptr<const Image::Pages> pages;
    if (entry.image.snapshot && snapshot->format_version() == PAGE_SNAPSHOT_VERSION &&
        entry.image.snapshot->format_version() == PAGE_SNAPSHOT_VERSION) {
        const auto before = entry.image.snapshot->page_references();
        const auto after = snapshot->page_references();
        if (before.size() == after.size() && same_geometry(*entry.image.snapshot, *snapshot) &&
            (before.empty() || !std::memcmp(before.data(), after.data(), before.size_bytes()))) pages = entry.image.pages;
    }
    if (!pages) {
        auto loaded = std::make_shared<Image::Pages>();
        const auto refs = snapshot->format_version() == PAGE_SNAPSHOT_VERSION ?
            snapshot->page_references() : std::span<const PageRef>{};
        const auto& hint = entry.object_hint;
        const bool matching_geometry = hint.snapshot &&
            hint.snapshot->format_version() == PAGE_SNAPSHOT_VERSION && !refs.empty() &&
            same_geometry(*hint.snapshot, *snapshot);
        for (size_t region = 0; region < snapshot->regions().size(); ++region) {
            loaded->offsets.push_back(loaded->values.size());
            const size_t count = snapshot->page_count(region);
            for (size_t index = 0; index < count; ++index) {
                const uint64_t key = refs.empty() ? 0 : refs[loaded->values.size()].key;
                if (key && matching_geometry) {
                    const auto& old = hint.page(region, index);
                    // Equal validated object IDs reuse the immutable saved
                    // object, not a claim that the tracee still holds its bytes.
                    const auto& desired = refs[loaded->values.size()];
                    if (old.key == key && old.address == desired.address && old.length == desired.length) {
                        loaded->values.push_back(old); continue;
                    }
                }
                const auto page = snapshot->page(region, index);
                loaded->values.push_back({page.address, page.length, page.data, page.owner, key});
            }
        }
        pages = std::move(loaded);
    }
    if (entry.pid != pid || entry.pidfd < 0) {
        entry.clear();
        entry.pidfd = syscall(SYS_pidfd_open, pid, 0);
        if (entry.pidfd < 0) throw std::runtime_error("Cannot bind resident image to owned process");
        entry.pid = pid;
    }
    entry.generation = StateStore::instance().generation();
    describe_protected(entry, *snapshot, pid);
    entry.image = {std::move(snapshot), root, brk, std::move(pages)};
    entry.object_hint = entry.image;
    entry.protected_image = {}; entry.protected_valid.clear();
}
}
