#include "compatibility.h"
#include "monitor.h"
#include "runtime_profile.h"
#include <nlohmann/json.hpp>
#include <xxhash.h>
#include <array>
#include <algorithm>
#include <set>
#include <cerrno>
#include <cpuid.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>

extern char** environ;
namespace detsim::search {
namespace {
using Json = nlohmann::json;
bool initialized = false;
std::string manifest, error;
std::vector<std::string> initial_environment;
[[noreturn]] void failed(const std::string& text) { throw std::runtime_error("Parallel compatibility: " + text); }
std::string hex(uint64_t value) {
    char result[17]; std::snprintf(result, sizeof(result), "%016llx", static_cast<unsigned long long>(value)); return result;
}
class Hash {
    std::unique_ptr<XXH64_state_t, decltype(&XXH64_freeState)> state_{XXH64_createState(), XXH64_freeState};
public:
    Hash() { if (!state_ || XXH64_reset(state_.get(), 0) != XXH_OK) failed("cannot initialize content hash"); }
    void add(const void* data, size_t size) { if (XXH64_update(state_.get(), data, size) != XXH_OK) failed("content hash failed"); }
    std::string value() const { return hex(XXH64_digest(state_.get())); }
};
struct Descriptor {
    int fd;
    ~Descriptor() { if (fd >= 0) close(fd); }
};
std::string file_hash(const std::string& path, dev_t expected_device = 0, ino_t expected_inode = 0) {
    Descriptor source{open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    struct stat before{}, after{};
    if (source.fd < 0 || fstat(source.fd, &before) || !S_ISREG(before.st_mode)) failed("unreadable backing file: " + path);
    if (expected_inode && (before.st_dev != expected_device || before.st_ino != expected_inode))
        failed("backing file identity changed: " + path);
    Hash hash; std::array<char, 65536> buffer{};
    for (;;) {
        const auto count = read(source.fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) failed("cannot hash backing file: " + path);
        if (!count) break;
        hash.add(buffer.data(), static_cast<size_t>(count));
    }
    if (fstat(source.fd, &after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) failed("backing changed while hashing: " + path);
    return hash.value();
}
std::string proc_bytes_hash(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) failed("cannot read " + path);
    Hash hash; std::array<char, 65536> bytes{};
    do {
        file.read(bytes.data(), bytes.size());
        hash.add(bytes.data(), static_cast<size_t>(file.gcount()));
    } while (file);
    if (!file.eof()) failed("failed reading " + path);
    return hash.value();
}
std::string native_cwd(pid_t pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/cwd";
    std::vector<char> bytes(256);
    for (;;) {
        const auto count = readlink(path.c_str(), bytes.data(), bytes.size());
        if (count < 0) failed("cannot inspect target cwd");
        if (static_cast<size_t>(count) < bytes.size()) return std::string(bytes.data(), static_cast<size_t>(count));
        if (bytes.size() >= 1024 * 1024) failed("target cwd exceeds profile bound");
        bytes.resize(bytes.size() * 2);
    }
}
Json mapped_files(pid_t pid, bool native_layout = true) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/maps");
    if (!input) failed("cannot inspect bootstrap mappings");
    Json maps = Json::array(); std::string line;
    struct stat self{};
    if (!native_layout && stat("/proc/self/exe", &self)) failed("cannot inspect tracer executable backing");
    std::map<std::pair<dev_t, ino_t>, std::string> hashes;
    while (std::getline(input, line)) {
        unsigned long long first = 0, last = 0, offset = 0, inode = 0;
        unsigned major_number = 0, minor_number = 0; char permissions[5]{}; int consumed = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx %4s %llx %x:%x %llu%n", &first, &last, permissions,
                        &offset, &major_number, &minor_number, &inode, &consumed) != 7)
            failed("invalid bootstrap mapping record");
        if (!inode) continue;
        std::string path = line.substr(static_cast<size_t>(consumed));
        const auto start = path.find_first_not_of(' ');
        if (start == std::string::npos) failed("file mapping lacks backing path");
        path.erase(0, start);
        if (path.empty() || path[0] != '/' || path.ends_with(" (deleted)")) failed("unsupported bootstrap file backing");
        const dev_t device = makedev(major_number, minor_number);
        const auto key = std::make_pair(device, static_cast<ino_t>(inode));
        if (!native_layout && device == self.st_dev && static_cast<ino_t>(inode) == self.st_ino) continue;
        auto found = hashes.find(key);
        if (found == hashes.end()) {
            const std::string rooted = "/proc/" + std::to_string(pid) + "/root" + path;
            found = hashes.emplace(key, file_hash(rooted, device, static_cast<ino_t>(inode))).first;
        }
        Json record = {{"offset", offset}, {"permissions", permissions}, {"file_xxh64", found->second}};
        if (native_layout) {
            record["start"] = first; record["end"] = last;
            record["device_major"] = major_number; record["device_minor"] = minor_number; record["inode"] = inode;
        } else record["size"] = last - first;
        maps.push_back(std::move(record));
    }
    if (!input.eof()) failed("bootstrap map read failed");
    if (!native_layout)
        std::sort(maps.begin(), maps.end(), [](const Json& left, const Json& right) { return left.dump() < right.dump(); });
    return maps;
}
Json cpu_profile() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid(0, a, b, c, d); const auto maximum = a;
    Json profile = {{"vendor", {b, d, c}}, {"maximum_leaf", maximum}};
    if (maximum < 1) failed("missing native x86 CPU feature leaf");
    __cpuid(1, a, b, c, d);
    // Initial APIC ID is a scheduling location, not an ISA capability. This
    // profile does not virtualize arbitrary guest CPUID/RDTSC observations.
    profile["signature"] = a; profile["features"] = {c, d};
    profile["cacheline_bytes"] = ((b >> 8) & 0xff) * 8;
    uint64_t xcr0 = 0;
    if (c & bit_OSXSAVE) { unsigned low, high; asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0)); xcr0 = low | (uint64_t(high) << 32); }
    profile["xcr0"] = xcr0;
    profile["structured_features"] = Json::array();
    if (maximum >= 7) {
        __cpuid_count(7, 0, a, b, c, d); const auto count = a;
        if (count > 64) failed("CPU feature subleaves exceed profile bound");
        for (unsigned index = 0; index <= count; ++index) {
            __cpuid_count(7, index, a, b, c, d); profile["structured_features"].push_back({index, a, b, c, d});
        }
    }
    profile["xsave_layout"] = Json::array();
    if (maximum >= 0xd) {
        __cpuid_count(0xd, 0, a, b, c, d); const uint64_t supported = a | (uint64_t(d) << 32);
        __cpuid_count(0xd, 1, a, b, c, d); const uint64_t supervisor = c | (uint64_t(d) << 32);
        for (unsigned index = 0; index < 64; ++index)
            if (index < 2 || ((supported | supervisor) & (uint64_t{1} << index))) {
                __cpuid_count(0xd, index, a, b, c, d); profile["xsave_layout"].push_back({index, a, b, c, d});
            }
    }
    cpu_set_t affinity{};
    if (sched_getaffinity(0, sizeof(affinity), &affinity)) failed("cannot inspect CPU affinity");
    profile["affinity"] = Json::array();
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &affinity)) profile["affinity"].push_back(cpu);
    return profile;
}
Json build_manifest(const sys_state& first_state) {
    utsname host{};
    if (uname(&host)) failed("cannot inspect kernel profile");
    uid_t real_uid, effective_uid, saved_uid; gid_t real_gid, effective_gid, saved_gid;
    if (getresuid(&real_uid, &effective_uid, &saved_uid) || getresgid(&real_gid, &effective_gid, &saved_gid))
        failed("cannot inspect credentials");
    const int count = getgroups(0, nullptr);
    if (count < 0) failed("cannot inspect supplementary groups");
    std::vector<gid_t> groups(static_cast<size_t>(count));
    if (getgroups(count, groups.data()) != count) failed("supplementary groups changed");
    Hash environment;
    for (const auto& entry : initial_environment) environment.add(entry.c_str(), entry.size() + 1);
    Json result = {{"schema", 1}, {"scope", "same-host-canonical-seed"}, {"hash", "noncryptographic-xxh64"},
        {"tracer_xxh64", file_hash("/proc/self/exe")}, {"tracer_dso_backings", mapped_files(getpid(), false)}, {"kernel", {host.sysname, host.release, host.version, host.machine}},
        {"uid", {real_uid, effective_uid, saved_uid}}, {"gid", {real_gid, effective_gid, saved_gid}},
        {"groups", groups}, {"ordered_environment_xxh64", environment.value()}, {"cpu", cpu_profile()}};
    result["rlimits"] = Json::array();
    for (int resource = 0; resource < RLIM_NLIMITS; ++resource) {
        rlimit limit{}; if (getrlimit(resource, &limit)) failed("cannot inspect resource limit");
        result["rlimits"].push_back({resource, uint64_t(limit.rlim_cur), uint64_t(limit.rlim_max)});
    }
    result["nodes"] = Json::array();
    for (int node = 0; node < NP; ++node) {
        if (!tracee_has_fixed_layout(ptmc_state.pids[node])) failed("target is not fixed-layout");
        result["nodes"].push_back({{"files", mapped_files(ptmc_state.pids[node])},
            {"native_cwd", native_cwd(ptmc_state.pids[node])},
            {"ordered_environment_xxh64", proc_bytes_hash("/proc/" + std::to_string(ptmc_state.pids[node]) + "/environ")},
            {"xstate_size", first_state.child[node].threads.at(0).xstate.size()}});
    }
    return result;
}
} // namespace
void initialize_compatibility_manifest(sys_state& first_state) noexcept {
    if (initialized) return;
    initialized = true;
    try {
        if (!runtime_profile().fixed_layout) failed("RuntimeProfile.fixed_layout is required");
        for (char** entry = environ; *entry; ++entry) initial_environment.emplace_back(*entry);
        manifest = build_manifest(first_state).dump();
        Hash identity; identity.add(manifest.data(), manifest.size());
        auto first_bound = ptmc_state.model_identity + ":bootstrap-v1:" + identity.value();
        auto configured_bound = first_bound;
        // Prepare both allocating copies before publishing either identity.
        // std::string::swap is noexcept, so failure leaves both base identities.
        first_state.model_identity.swap(first_bound);
        ptmc_state.model_identity.swap(configured_bound);
    } catch (const std::exception& failure) { error = failure.what(); }
    catch (...) { error = "Unknown bootstrap compatibility failure"; }
}
const std::string& compatibility_manifest() {
    if (!initialized) failed("bootstrap manifest was not initialized before exploration");
    if (!error.empty()) failed(error);
    return manifest;
}
const std::vector<std::string>& bootstrap_environment() {
    (void)compatibility_manifest();
    return initial_environment;
}
std::vector<std::string> environment_change_names() {
    (void)compatibility_manifest();
    std::map<std::string, std::vector<std::pair<size_t, std::string>>> before, after;
    const auto add = [](auto& entries, size_t index, const std::string& entry) {
        entries[entry.substr(0, entry.find('='))].emplace_back(index, entry);
    };
    for (size_t i = 0; i < initial_environment.size(); ++i) add(before, i, initial_environment[i]);
    for (size_t i = 0; environ[i]; ++i) add(after, i, environ[i]);
    std::set<std::string> names;
    for (const auto& [name, entries] : before) if (!after.count(name) || after.at(name) != entries) names.insert(name);
    for (const auto& [name, entries] : after) if (!before.count(name) || before.at(name) != entries) names.insert(name);
    return {names.begin(), names.end()};
}
} // namespace detsim::search
