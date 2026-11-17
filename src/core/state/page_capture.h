#pragma once
#include "page_snapshot.h"

bool page_capture_enabled();
hash_type save_paged_snapshot(pid_t pid, const std::vector<RegionInfo>& regions,
    std::span<const uint8_t> metadata, std::string_view maps,
    const user_regs_struct& regs, hash_type reference_hash,
    /* Identity-diagnostic only: the same metadata re-encoded with the pending
     * phase normalized away. Empty disables that variant. */
    std::span<const uint8_t> metadata_without_pending_phase = {});
void reset_page_capture_stats();
void report_page_capture_stats();
