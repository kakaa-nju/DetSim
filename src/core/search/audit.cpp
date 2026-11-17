#include "search/audit.h"

#include "utils.h"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace detsim::search::audit
{
namespace
{

bool read_flag()
{
  const char *value = std::getenv("DETSIM_SEARCH_AUDIT");
  return value && *value && std::strcmp(value, "0") != 0;
}

std::string edge_line(hash_type source, hash_type target, int process,
                      int thread, int choice, int cardinality, int result_code,
                      bool violation)
{
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer),
                "%016lx => %016lx p=%d t=%d c=%d n=%d code=%d bad=%d", source,
                target, process, thread, choice, cardinality, result_code,
                violation ? 1 : 0);
  return buffer;
}

std::string fingerprint(const std::vector<std::string> &lines)
{
  std::vector<std::string> sorted = lines;
  std::sort(sorted.begin(), sorted.end());
  std::string joined;
  for (const auto &line : sorted)
  {
    joined += line;
    joined += '\n';
  }
  const hash_type value = XXHash64::hash(joined.data(), joined.size());
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016lx", value);
  return buffer;
}

} // namespace

bool enabled()
{
  static const bool on = read_flag();
  return on;
}

namespace {
bool read_identity_flag()
{
  const char *value = std::getenv("DETSIM_IDENTITY_DIAG");
  return value && *value && std::strcmp(value, "0") != 0;
}

/* node variants for the capture in flight, and per-variant identity sets for the
 * whole search. Single-threaded exploration; a worker audits its own process. */
std::array<std::array<hash_type, NP>, kIdentityVariantCount> node_variants{};
std::array<std::unordered_set<hash_type>, kIdentityVariantCount> identity_sets{};

std::array<unsigned long long, kCoverageEventCount>& coverage_counts()
{
  static std::array<unsigned long long, kCoverageEventCount> counts{};
  return counts;
}

std::array<unsigned long long, 512>& syscall_counts()
{
  static std::array<unsigned long long, 512> counts{};
  return counts;
}
}  // namespace

bool identity_diagnostics()
{
  static const bool on = read_identity_flag();
  return on;
}

const char *identity_variant_name(int variant)
{
  static const char *const names[kIdentityVariantCount] = {
      "base", "no_user_state", "no_metadata", "no_maps", "no_regs",
      "no_pending_phase"};
  return variant >= 0 && variant < kIdentityVariantCount ? names[variant] : "unknown";
}

void identity_node_variant(int node, int variant, hash_type value)
{
  if (!identity_diagnostics() || node < 0 || node >= NP ||
      variant < 0 || variant >= kIdentityVariantCount)
    return;
  node_variants[variant][node] = value;
}

hash_type identity_node_value(int node, int variant)
{
  if (node < 0 || node >= NP || variant < 0 || variant >= kIdentityVariantCount)
    return 0;
  return node_variants[variant][node];
}

void identity_sample(int variant, hash_type value)
{
  if (!identity_diagnostics() || variant < 0 || variant >= kIdentityVariantCount)
    return;
  identity_sets[variant].insert(value);
}

void coverage(int event)
{
  if (!enabled() || event < 0 || event >= kCoverageEventCount) return;
  ++coverage_counts()[event];
}

void syscall(int nr)
{
  if (!enabled()) return;
  /* An explicit Timeout action has no syscall number of its own; the top slot is
   * above the highest x86_64 syscall number, so it cannot alias a real one. */
  const int index = nr == -2 ? static_cast<int>(syscall_counts().size()) - 1 : nr;
  if (index < 0 || index >= static_cast<int>(syscall_counts().size())) return;
  ++syscall_counts()[index];
}

void edge(hash_type source, hash_type target, int process, int thread,
          int choice, int cardinality, int result_code, bool violation)
{
  if (!enabled())
    return;
  const std::string line = edge_line(source, target, process, thread, choice,
                                     cardinality, result_code, violation);
  std::fprintf(stderr, "SEARCH_EDGE %s\n", line.c_str());
}

void summary(const char *mode, long depth, hash_type root, size_t searched,
             size_t unique, const char *status,
             const std::unordered_set<hash_type> &states,
             unsigned long long por_skipped)
{
  if (!enabled())
    return;
  std::vector<std::string> state_lines;
  state_lines.reserve(states.size());
  for (hash_type state : states)
  {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016lx", state);
    state_lines.emplace_back(buffer);
  }
  const std::string states_fp = fingerprint(state_lines);
  {
    std::string histogram;
    for (int nr = 0; nr < static_cast<int>(syscall_counts().size()); ++nr)
    {
      if (!syscall_counts()[nr]) continue;
      char entry[48];
      const int index = nr == static_cast<int>(syscall_counts().size()) - 1 ? -2 : nr;
      std::snprintf(entry, sizeof(entry), "%s%d:%llu", histogram.empty() ? "" : ",",
                    index, syscall_counts()[nr]);
      histogram += entry;
      syscall_counts()[nr] = 0;
    }
    std::fprintf(stderr, "SEARCH_SYSCALLS {\"counts\":\"%s\"}\n", histogram.c_str());
  }
  {
    std::fprintf(stderr,
                 "SEARCH_COVERAGE {\"entries\":%llu,\"skipped_fully\":%llu,"
                 "\"skipped_depth\":%llu,\"expanded\":%llu,\"re_expanded\":%llu}\n",
                 coverage_counts()[kCoverageEntry], coverage_counts()[kCoverageSkipFully],
                 coverage_counts()[kCoverageSkipDepth], coverage_counts()[kCoverageExpand],
                 coverage_counts()[kCoverageReexpand]);
    coverage_counts() = {};
  }
  if (identity_diagnostics())
  {
    std::fprintf(stderr, "IDENTITY_DIAG {");
    for (int variant = 0; variant < kIdentityVariantCount; ++variant)
      std::fprintf(stderr, "%s\"%s\":%zu", variant ? "," : "",
                   identity_variant_name(variant), identity_sets[variant].size());
    std::fprintf(stderr, "}\n");
    for (auto &set : identity_sets) set.clear();
  }
  std::fprintf(stderr,
               "SEARCH_AUDIT {\"schema\":%d,\"mode\":\"%s\",\"depth\":%ld,"
               "\"root\":\"%016lx\",\"searched\":%zu,\"unique\":%zu,"
               "\"states\":%zu,\"states_fp\":\"%s\",\"status\":\"%s\","
               "\"por_skipped\":%llu}\n",
               kSchema, mode, depth, root, searched, unique, states.size(),
               states_fp.c_str(), status, por_skipped);
}

} // namespace detsim::search::audit
