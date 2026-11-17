/*
 * search/audit.h - opt-in, machine-readable receipt of one search command.
 *
 * The completeness gates in perf/raft/ cannot be judged from the human log:
 * per-state text logging changes both the timing and the log contents, and
 * `show_syscall_history()` prints only the single branch that ends the search.
 * This module emits one line per committed edge (in commit order) plus one
 * summary line per search, so two builds can be compared on
 *
 *   * the ordered edge sequence (L2: throughput refactors must not reorder),
 *   * the multiset of edges (L3: reductions may drop independent interleavings),
 *   * the set of discovered state identities (L3: must be identical).
 *
 * It is off unless DETSIM_SEARCH_AUDIT is set and unset-like, costs one
 * getenv() at startup otherwise, and never feeds anything back into the search.
 * Parallel workers audit their own process; the coordinator receipt for that
 * mode stays `Report{nodes,edges,attempted_edges,status}`.
 */
#ifndef DETSIM_SEARCH_AUDIT_H
#define DETSIM_SEARCH_AUDIT_H

#include "types.h"
#include <cstddef>
#include <unordered_set>

namespace detsim::search::audit
{

inline constexpr int kSchema = 1;

bool enabled();

/* Count one executed syscall (-2 for an explicit Timeout action). summary()
 * reports the histogram so a reduction policy can be scoped against the
 * workload it will actually meet. */
void syscall(int nr);

/* Identity-purity diagnostics.
 *
 * Whether a component of the state identity is worth purifying is a measurement,
 * not an opinion: sample the identity a capture *would* have if one component
 * were excluded and count how many distinct identities the whole search sees.
 * A variant that collapses the count is a candidate reduction; one that does not
 * is provably a no-op on this workload. Nothing here feeds back into the search:
 * the samples are read-only projections of the same captured bytes, so enabling
 * it cannot change which states are explored.
 *
 * Variants are the capture-level slices of one node plus the whole-state monitor
 * blob. They are only produced by the pages backend, which is what the
 * benchmarks run. */
enum IdentityVariant
{
  kIdentityBase = 0,   /* the committed identity */
  kIdentityNoUserState,/* monitor (UserState) blob excluded */
  kIdentityNoMetadata, /* node: serialized tracee_state excluded */
  kIdentityNoMaps,     /* node: /proc/<pid>/maps text excluded */
  kIdentityNoRegs,     /* node: main-thread registers excluded */
  kIdentityNoPendingPhase, /* node: pending-syscall phase normalized away */
  kIdentityVariantCount
};

bool identity_diagnostics();
const char *identity_variant_name(int variant);
/* One node's capture reports what its identity would be without each slice. */
void identity_node_variant(int node, int variant, hash_type value);
/* The slice recorded for `node` since its last capture (0 when none). */
hash_type identity_node_value(int node, int variant);
/* One product state reports the whole-state variants it would have. */
void identity_sample(int variant, hash_type value);

/* Coverage bookkeeping of one DFS run: how many frame entries were skipped by a
 * memo and how many were re-expansions of an already seen state. Sizes the
 * "replay known successors instead of re-executing" opportunity. */
enum CoverageEvent
{
  kCoverageEntry = 0,
  kCoverageSkipFully,   /* state fully covered at any depth */
  kCoverageSkipDepth,   /* covered at this depth */
  kCoverageExpand,      /* frame actually enumerates transitions */
  kCoverageReexpand,    /* ... and the state had been entered before */
  kCoverageEventCount
};

void coverage(int event);

/* One committed product edge, in the order the search committed it. The
 * cardinality is the syscall choice count (-1 when the action had none). */
void edge(hash_type source, hash_type target, int process, int thread,
          int choice, int cardinality, int result_code, bool violation);

/* One search command: receipt plus set/multiset fingerprints of everything it
 * discovered. `states` must contain every identity the search reached,
 * including the initial state. `root` is the state the search started from, so a
 * second search in the same process can be re-entered at exactly that state.
 * `por_skipped` counts successors a reduction policy did not attempt; it is a
 * receipt field, never an edge, so a reduced receipt can be told apart from a
 * duplicate-suppressed one. */
void summary(const char *mode, long depth, hash_type root, size_t searched,
             size_t unique, const char *status,
             const std::unordered_set<hash_type> &states,
             unsigned long long por_skipped = 0);

} // namespace detsim::search::audit

#endif
