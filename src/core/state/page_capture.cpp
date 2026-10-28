#include "page_capture.h"
#include "snapshot_memory.h"
#include "resident_memory.h"
#include <sys/mman.h>
#include "soft_dirty_experiment.h"
#include <cstdlib>
#include <climits>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <sys/uio.h>

namespace {
struct SavedPage {
    PageRef ref;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};
struct Scratch {
    uint64_t generation = 0;
    std::vector<uint8_t> memory;
    std::vector<SavedPage> previous;
};
// Only a byte-equality hint: every capture still reads every supported region.
// Fixed live tracees own at most one working image each; no search states pinned.
std::unordered_map<pid_t, Scratch> scratch;
struct Stats {
    uint64_t captures=0, pages=0, reused=0, saved=0, read_bytes=0, oracle_bytes=0, resident_captures=0, protected_bytes=0;
} stats;

void read_regions(pid_t pid, const std::vector<detsim::experiment::Region>& ranges,
                  const std::vector<RegionInfo>& regions, std::span<const uint8_t> skip = {}) {
    if (ranges.size() != regions.size()) throw std::runtime_error("Page capture region mismatch");
    if (!skip.empty() && skip.size() != regions.size()) throw std::runtime_error("Page capture skip mask mismatch");
    detsim::GuardMemoryReader guard_memory(pid);
    for (size_t start=0; start<ranges.size(); start+=256) {
        iovec local[256], remote[256];
        const size_t count=std::min(size_t(256),ranges.size()-start);
        size_t expected=0, readable_count=0;
        for (size_t i=0;i<count;++i) {
            if (!skip.empty() && skip[start+i]) continue;
            const auto& r=ranges[start+i];
            if (regions[start+i].prot == PROT_NONE) {
                guard_memory.read(r.address,r.data,r.size);
                continue;
            }
            if (r.size>size_t(SSIZE_MAX)-expected) throw std::runtime_error("Page capture transfer overflow");
            expected+=r.size;
            local[readable_count]={r.data,r.size};
            remote[readable_count]={reinterpret_cast<void*>(r.address),r.size};
            ++readable_count;
        }
        if (!readable_count) continue;
        const auto got=process_vm_readv(pid,local,readable_count,remote,readable_count,0);
        if (got<0 || size_t(got)!=expected) throw std::runtime_error("Page capture read failed or short");
    }
}
}

bool page_capture_enabled() {
    const char* mode=getenv("DETSIM_SNAPSHOT_BACKEND");
    if (!mode || !strcmp(mode,"full")) return false;
    if (strcmp(mode,"pages") && strcmp(mode,"verify-pages"))
        throw std::runtime_error("Invalid DETSIM_SNAPSHOT_BACKEND");
    return true;
}

hash_type save_paged_snapshot(pid_t pid, const std::vector<RegionInfo>& regions,
    std::span<const uint8_t> metadata, std::string_view maps,
    const user_regs_struct& regs,
    hash_type reference_hash) {
    if (sysconf(_SC_PAGESIZE)!=4096) throw std::runtime_error("Page backend requires 4KiB host pages");
    auto& store=StateStore::instance();
    auto& work=scratch[pid];
    if (work.generation!=store.generation()) {work=Scratch{};work.generation=store.generation();}
    size_t total=0, page_count=0;
    for (const auto& r:regions) {
        if (r.end<=r.start || r.end-r.start>SIZE_MAX-total) throw std::runtime_error("Page capture extent overflow");
        total+=r.end-r.start;
        page_count += (r.end - 1) / 4096 - r.start / 4096 + 1;
    }
    auto check_oracle = [&](hash_type root) {
        std::vector<uint8_t> oracle(total);
        std::vector<detsim::experiment::Region> oracle_reads;
        size_t cursor = 0;
        for (const auto& region : regions) {
            const size_t length = region.end - region.start;
            oracle_reads.push_back({uintptr_t(region.start), length, oracle.data() + cursor});
            cursor += length;
        }
        read_regions(pid, oracle_reads, regions); stats.oracle_bytes += total;
        const auto view = SnapshotView::open(root);
        if (view->maps_text() != maps || view->metadata().size() != metadata.size() ||
            memcmp(view->metadata().data(), metadata.data(), metadata.size()) ||
            memcmp(&view->main_regs(), &regs, sizeof regs))
            throw std::runtime_error("Page snapshot metadata disagrees with full capture");
        cursor = 0;
        std::vector<uint8_t> decoded;
        for (const auto& region : regions) {
            decoded.resize(region.end - region.start);
            view->read_into(region.start, decoded.data(), decoded.size());
            if (memcmp(decoded.data(), oracle.data() + cursor, decoded.size()))
                throw std::runtime_error("Page snapshot disagrees with full memory oracle");
            cursor += decoded.size();
        }
    };
    const auto resident = detsim::resident::get(pid);
    bool same_layout = resident.snapshot && resident.snapshot->regions().size() == regions.size();
    if (same_layout) for (size_t i = 0; i < regions.size(); ++i) {
        const auto& old = resident.snapshot->regions()[i];
        const auto& now = regions[i];
        if (old.start != now.start || old.end != now.end || old.prot != now.prot ||
            memcmp(old.flags, now.flags, sizeof old.flags)) same_layout = false;
    }
    if (same_layout && resident.snapshot->format_version() == PAGE_SNAPSHOT_VERSION &&
        resident.snapshot->maps_text() == maps) {
        const auto previous = resident.snapshot->page_references();
        std::vector<PageRef> refs(previous.begin(), previous.end());
        const auto manifest = encode_page_manifest(resident.snapshot->regions(), metadata, maps, regs, refs);
        const auto root = store.save(manifest.data(), manifest.size(), reference_hash);
        const char* backend = getenv("DETSIM_SNAPSHOT_BACKEND");
        if (detsim::resident::verify() || (backend && !strcmp(backend, "verify-pages")))
            check_oracle(root);
        ++stats.captures; ++stats.resident_captures;
        stats.pages += refs.size(); stats.reused += refs.size();
        return root;
    }
    work.memory.resize(total);
    std::vector<detsim::experiment::Region> reads;
    reads.reserve(regions.size());
    const auto read_only = detsim::resident::get_read_only(pid);
    std::vector<uint8_t> protected_regions(regions.size(), 0);
    size_t protected_size = 0;
    if (read_only.matches(maps) && !detsim::experiment::configured_capture()) {
        for (size_t i = 0; i < regions.size(); ++i) {
            const auto& region = regions[i];
            if (region.start % 4096 || region.end % 4096) continue;
            if (!read_only.region(i, region.start, region.end, region.prot)) continue;
            // Direct immutable-page references avoid both remote reads and
            // redundant local memcmp. Flat owners use the baseline for now.
            bool paged = true;
            const size_t count = (region.end - region.start + 4095) / 4096;
            for (size_t page = 0; page < count; ++page) {
                const auto& old = read_only.page(i, page);
                if (!old.key || old.data != old.owner->data() || old.length != old.owner->size()) paged = false;
            }
            if (paged) { protected_regions[i] = 1; protected_size += region.end - region.start; }
        }
    }
    size_t offset=0;
    for (const auto& r:regions) {
        reads.push_back({uintptr_t(r.start),size_t(r.end-r.start),work.memory.data()+offset});
        offset+=r.end-r.start;
    }
    if (auto* experiment=detsim::experiment::configured_capture()) {
        for (const auto& region:regions)
            if (region.prot == PROT_NONE)
                throw std::runtime_error("Soft-dirty capture does not support PROT_NONE mappings");
        const auto before=experiment->stats().bytes_read;
        experiment->capture(pid,reads,std::string(maps));
        stats.read_bytes+=experiment->stats().bytes_read-before;
    } else {
        read_regions(pid,reads,regions,protected_regions);
        stats.read_bytes+=total-protected_size;
        stats.protected_bytes+=protected_size;
    }

    std::vector<RegionInfo> descriptors=regions;
    std::vector<PageRef> refs;
    std::vector<SavedPage> next;
    // Count current chunks, including unaligned endpoints, rather than retaining
    // a former large layout's capacity after mappings shrink.
    refs.reserve(page_count);
    next.reserve(page_count);
    offset=0;
    for (size_t region_index = 0; region_index < descriptors.size(); ++region_index) {
        auto& r = descriptors[region_index];
        r.offset=refs.size();
        size_t region_page = 0;
        for (uint64_t address=r.start;address<r.end;) {
            const auto length=uint32_t(std::min(r.end-address,4096-address%4096));
            const uint8_t* bytes=work.memory.data()+offset;
            const size_t index=refs.size();
            if (protected_regions[region_index]) {
                const auto& old = read_only.page(region_index, region_page++);
                if (old.address != address || old.length != length)
                    throw std::runtime_error("Protected capture page geometry differs");
                SavedPage saved{{address, length, old.key}, old.owner};
                refs.push_back(saved.ref); next.push_back(std::move(saved));
                ++stats.reused;
                address += length; offset += length;
                continue;
            }
            const bool same=index<work.previous.size() &&
                work.previous[index].ref.address==address && work.previous[index].ref.length==length &&
                !memcmp(work.previous[index].bytes->data(),bytes,length);
            SavedPage saved;
            if (same) {saved=work.previous[index];++stats.reused;}
            else {
                const auto key=store.save(bytes,length);
                auto owner=store.load_shared(key);
                if (!owner || owner->size()!=length) throw std::runtime_error("Cannot retain captured page");
                saved={{address,length,key},std::move(owner)};
                ++stats.saved;
            }
            refs.push_back(saved.ref);next.push_back(std::move(saved));
            ++region_page;
            address+=length;offset+=length;
        }
    }
    auto manifest=encode_page_manifest(descriptors,metadata,maps,regs,refs);
    const auto root=store.save(manifest.data(),manifest.size(),reference_hash);
    const char* mode=getenv("DETSIM_SNAPSHOT_BACKEND");
    if ((mode && !strcmp(mode,"verify-pages")) || detsim::resident::verify()) check_oracle(root);
    work.previous=std::move(next);
    work.generation=store.generation();
    ++stats.captures;stats.pages+=refs.size();
    return root;
}

void reset_page_capture_stats() {stats={};}
void report_page_capture_stats() {
    fprintf(stdout,"PAGE_CAPTURE {\"captures\":%llu,\"pages\":%llu,\"reused\":%llu,\"saved\":%llu,\"read_bytes\":%llu,\"oracle_bytes\":%llu,\"resident_captures\":%llu,\"protected_bytes\":%llu}\n",
        (unsigned long long)stats.captures,(unsigned long long)stats.pages,(unsigned long long)stats.reused,
        (unsigned long long)stats.saved,(unsigned long long)stats.read_bytes,(unsigned long long)stats.oracle_bytes,(unsigned long long)stats.resident_captures,(unsigned long long)stats.protected_bytes);
}
