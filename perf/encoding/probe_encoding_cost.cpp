/*
 * probe_encoding_cost.cpp - what does the state-capture metadata cost?
 *
 * The capture path pays two metadata costs per captured tracee that the
 * checker work sits on top of:
 *
 *  1. `capture_stopped()` serializes the *whole* tracee_state into every
 *     snapshot (cereal BinaryOutputArchive through std::ostringstream), and
 *     `state_transition()` calls `commit_metadata()`, which serializes the
 *     *whole sys_state* a second time to compute the identity hash. The
 *     captured file bytes therefore appear twice in the identity encoding
 *     (once in the VFS node, once in the sha256-like digest of that encoding),
 *     and the in-memory metadata is built twice.
 *  2. xxhash64 runs over the whole payload (regions + metadata + maps).
 *
 * This probe measures (1) on a representative shape and (2) over the payload
 * sizes the raft example produces (7 regions, ~1.19 MB per tracee), so the
 * numbers can be compared directly against the measured phase profile
 * (capture ~= 3.4 ms of a 4.4 ms state).
 *
 * Build:  make -C <repo> NP=3 <probe target>  (see the Makefile entry) or
 *   g++ -O2 -std=gnu++20 -DNP=3 -I. -Isrc -Isrc/core -Isrc/core/state \
 *       -Isrc/subsys -Isrc/subsys/fs -Isrc/subsys/net -Isrc/utils \
 *       -Ithird_party -Ithird_party/cereal/include \
 *       perf/probe_encoding_cost.cpp -o /tmp/probe -lxxhash
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#include "cereal/archives/binary.hpp"
#include "cereal/types/map.hpp"
#include "cereal/types/vector.hpp"
#include "state/state.h"
#include "utils.h"

/* Benchmarks link the tracer objects without main.cpp. */
namespace detsim { namespace ui { class NCursesUI; } }
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

static double now_ms()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int main()
{
  /* Shape taken from a real raft capture: one file with a few hundred bytes
   * of log content per node, two open sockets, ~7 KB of serialized metadata. */
  tracee_state ts;
  for (int i = 0; i < 3; ++i)
  {
    VFSNode node;
    const std::vector<char> log_bytes(512, 'x');
    node.content.assign(log_bytes.begin(), log_bytes.end());
    ts.fs_state.filesystem["/tmp/raft-log-" + std::to_string(i)] = node;
  }
  ts.sock_state.sockets()[100 + 0] = Socket{};
  ts.sock_state.sockets()[100 + 1] = Socket{};

  const int iters = 5000;
  double total = 0;
  size_t encoded = 0;
  for (int i = 0; i < iters; ++i)
  {
    std::ostringstream oss;
    const double start = now_ms();
    {
      cereal::BinaryOutputArchive ar(oss);
      ar(ts);
    }
    total += now_ms() - start;
    encoded = std::move(oss).str().size();
  }
  std::printf("tracee_state cereal encode: %.2f us/state, %zu bytes\n",
              total * 1000.0 / iters, encoded);

  /* Payload hashing: one raft tracee snapshot is ~1.19 MB. */
  std::vector<uint8_t> payload(1187974, 0x5a);
  for (size_t i = 0; i < payload.size(); i += 97)
    payload[i] = static_cast<uint8_t>(i);
  const int hashes = 2000;
  double hash_total = 0;
  for (int i = 0; i < hashes; ++i)
  {
    const double start = now_ms();
    volatile uint64_t digest = XXHash64::hash(payload.data(), payload.size());
    (void)digest;
    hash_total += now_ms() - start;
  }
  std::printf("xxh3_64 over %zu bytes: %.2f us/tracee-state (%.1f GB/s)\n",
              payload.size(), hash_total * 1000.0 / hashes,
              (payload.size() * double(hashes) / 1e9) /
                  (hash_total / 1000.0));
  return 0;
}
