#pragma once
#include <string>
#include <vector>

struct sys_state;

namespace detsim::search {
// Call after capturing first_state, before checking or publishing that initial state.
// Unsupported profiles are cached as errors so ordinary serial use is unaffected.
// Success binds the bootstrap digest into both configured and initial model identity.
void initialize_compatibility_manifest(sys_state& first_state) noexcept;
// Strict same-host, canonical-seed compatibility, not cross-host portability or
// independent-root semantic normalization. Throws when bootstrap was unavailable.
const std::string& compatibility_manifest();
// Exact pre-UI exec environment, preserving entry order and all values. Values
// are for exec only; diagnostics must report names, never credential contents.
const std::vector<std::string>& bootstrap_environment();
std::vector<std::string> environment_change_names();
// Reads the already-admitted bootstrap mask; never changes its identity.
std::vector<int> bootstrap_cpu_affinity();
void require_bootstrap_cpu_affinity();
// Topology is only an ordering hint. Any missing/inconsistent sysfs data falls
// back to ascending allowed CPUs. The root parameter supports synthetic tests.
std::vector<int> worker_cpu_order(const std::vector<int>& allowed,
                                 const std::string& sysfs_root = "/sys/devices/system/cpu");
}
