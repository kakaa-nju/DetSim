# Correctness backlog

These are source-supported findings, not all reproduced end to end. Component/build
checks do not close these items. Priority reflects risk to exploration soundness.
Use one item per focused change, with a failing regression before the fix.
The [state/atom contract assessment](model-checking-contract.md) explains their
relationship to the native-binary model-checking goal.

| ID | Priority | Evidence / problem | Evidence required to close |
| --- | --- | --- | --- |
| C01 | Fixed (unchanged live thread set) | Futex queues are branch-owned, captured and archived; restore reconstructs blocked state, including same-hash restores. | `check-futex-snapshot` exercises capture/copy/assignment/archive, persisted reload and repeated ptrace restore/wake. Raw lifecycle/carrier restore has separate C02 tests; combined lifecycle and futex waiting still needs wider coverage. |
| C02 | P0 / partial | Capture preserves virtual slots and creation records; raw clone/exit now restore logical contexts over retained carriers; unexpected physical death or untracked creation rejects. Physical TIDs now stay in transient bindings; explicit liveness and logical parent IDs enter archives. Guest-visible physical IDs in register/memory contents are not normalized. | Create A/B, exit A, create C, repeatedly restore with correct logical identity and TLS/stop phases. `check-thread-lifecycle` now covers that sequence, TLS, persisted replay, main-first exit and actual carrier reuse. TID writeback/clear-tid now have join and replay coverage; ordinary pthread TLS/join/condition and non-PI robust owner-death have scoped coverage; full pthread, PI and rseq critical sections remain unsupported. |
| C03 | Partial: actual CLI actors fixed | Actual CLI BFS and iterative DFS enumerate saved runnable/Ready/Retry threads and each eligible finite timeout, with explicit post-restore actors and per-action choices. Iterative DFS now reopens a hash when a later command has more remaining depth and reports depth/budget cuts instead of claiming completion; a following BFS enqueues known finite frontiers. RAND is not covered. | Native actor gate:28 full/pages rows,76 direct root actions,228 same-source replays and actual BFS terminal projections; pure saved-policy/readiness tests, DFS residency, depth-reopen and DFS-then-BFS recovery regressions pass. Consolidation and complete outcome/atom coverage remain open. |
| C04 | Partial | Ordered system metadata identity includes status/budget; storage now compares duplicate bytes in cache, packed records and metadata instead of trusting hashes alone. | Identity and all storage gates cover ordering and duplicate conflicts. Logical thread identity and equivalence of differently encoded semantic states remain open. |
| C05 | Fixed (scoped) | Removed filename heuristics; corrected libelf string-table index in Go-section detection. | Native `test_probe` starts at the same boundary as `probe`; ordinary/Go-section ELF fixtures and two existing gc Go binaries classified correctly. gccgo and actual Go runtime execution remain unverified. |
| C06 | Fixed (framework boundary) | Removed Raft fields, implicit role pruning, message inference and example parser from the production link. Versioned plugins inspect explicit snapshots; branch-owned opaque monitors enter product identity before successor publication. | `check-state-view`, `check-plugins`, `check-identity` and native counter-plugin restore/violation scenarios cover readonly inspection, ABI failures, history isolation, terminal witnesses and decoder noninterference. Raft examples have separate projection/history contracts; this does not certify every Raft property. |
| C07 | Fixed | Snapshot reads now require saved bytes and reject missing/corrupt data; no live-memory substitution. | `check-state-io` covers missing/invalid archives and reads spanning saved regions. |
| C08 | Partial | Native same-edge replay preserves hash; storage tests now cover immutable reader lifetime, cache pressure/zero budgets, short I/O, prefetch and fresh-process reload. | `check-storage`, `check-state-io`, `check-capture` full-byte oracle, mapping and native replay gates pass. Full exploration equivalence across cache policies and general multi-node semantic canonicalization remain unproven. Virtual-exit/fatal-stop branch closure now has real ptrace regressions: persisted terminated images load and restore along with live peers; recapture preserves identity. Physical process death still rejects; lifecycle reconstruction remains C02. |
| C09 | Fixed (scoped) | Replaced process-group termination with owned-child kill/reap; signal handler avoids store shutdown locks. | NP=1 scenarios verify unrelated sibling survival and full/pages pthread normal/SIGTERM cleanup with all owned TIDs gone. Dedicated host groups let cleanup reap parked workers before leader. Unsupported prelude group changes reject. Fork descendants and broader initialization races need additional coverage. |
| C10 | Fixed | Migrated removed libdwarf calls to supported APIs; validated with installed libdwarf 0.11.1 and GCC 14.2. | Real production parser tests pass DWARF 4/5, two CUs, sizes/member offsets, symbols, arrays and cleanup/reinit; tracer compiles and links. Split DWARF and PIE relocation remain outside coverage. |
| C11 | Fixed (distinct addresses) | Direct `futex_requeue` now updates `waiter_map_` when moving a waiter. | Both entry points agree on queue/address; archive round trip followed by `wake_thread` succeeds. Same-address and syscall requeue ABI now have C16 regressions. |
| C12 | Fixed | `handle_exit_group` wrote a raw wait status without the DetSim exited flag; successful exit became “running” and the next step crashed. | Finite program reaches counter 1/2/3, exits virtually, and further stepping reports exit status 0. |
| C13 | Fixed (BFS/DFS/RAND) | BFS formerly continued after violation; DFS ignored its stop code and RAND only broke an inner loop. All now stop the entire search, retaining a failed product-state witness. | Native scenarios verify reachable assertion violations stop each mode without a false completion; counter-plugin replay preserves terminal identity. Ordinary finite BFS still explores the expected five states. |
| C14 | Fixed | Batch command parser pointed at the NUL inserted by strtok and discarded all arguments. | Real batch handles whitespace, `sw 0`, `p counter` and `q`. |
| C15 | Fixed (captured streams) | STDIO descriptors share the virtual FD registry; stdin is deterministic EOF and stdout/stderr content is archived VFS state. | `check-io-boundary` covers access modes, writes, close, archive and branch isolation; terminal input/output and interactive stdin remain unsupported. |
| C16 | Fixed (scoped) | WAKE unblocks removed waiters, WAIT saves an unfinished successor; wake grants a separately published return; requeue uses arg4 count/arg6 comparison, returns total affected and handles same address safely. | `check-futex` covers real ptrace WAIT/WAKE/bitset/requeue/errors; timed wait deadlines and explicit expiry are separately tested; PI operations explicitly fail. Exhaustive wake-order outcomes remain outside coverage. |
| C17 | P1 | The glibc-private CPU-feature patch has been removed; general nondeterministic CPU instructions/runtime inputs still need a supported execution contract. | Define and test which nondeterministic CPU/runtime state must be normalized; avoid assuming this patch worked from a successful scenario. |
| C20 | Fixed (host user XSTATE) | Capture/archive/restore NT_X86_XSTATE for each thread; snapshot version 5 rejects old formats. | `check-xstate` covers XMM/AVX/x87/MXCSR; native SIMD scenario repeatedly restores observed=0 and replays observed=11. Cross-host layouts and supervisor state are outside coverage. |
| C21 | Fixed (scoped) | Save private readable mappings and unnamed anonymous guards including hidden bytes; restore protections/backing and canonicalize equivalent VMA partitions. Reject shared and other unreadable maps and missing file/named backing. | `check-mappings` covers RO contents, permissions, split/merge, anonymous recreation, explicit unsupported cases and wide map fields. Native replay preserves successor identity. |
| C22 | Fixed (snapshot I/O) | Exact read/write counts and archive bounds required; failures propagate, without zero-fill or live fallback. | `check-state-io` fault-injects short/failed process_vm I/O and corrupt/missing/old archives. Failed restore is fatal; no successor may execute afterward. |
| C23 | P0 / partial | TCP source ports now derive from saved socket state, independent of search RNG; clocks/getrandom now use saved virtual state; per-task masks now persist; custom signal delivery and asynchronous kernel inputs remain unsupported. | `check-port` covers replay, RNG independence, source addresses, uniqueness and exhaustion. Closed syscall policy and process-model regressions cover rejection, deterministic replay and archives; this does not close general asynchronous inputs. |
| C24 | Fixed | Populate current syscall number/arguments before the entry handler. | `check-exec` exercises real ptrace FUTEX_WAKE suppression using current arguments. |
| C25 | P0 / partial | Auto execution returns after each syscall boundary or explicit pending completion, including CKPT_NO. Actual CLI BFS/iterative DFS now enumerate supported saved actors/timeouts, distinguish depth/budget-bounded returns and recover a recorded DFS frontier in a following BFS; all futex/message outcomes, instruction-level shared-memory interleavings and bounded native execution remain unproven. | `check-exec`, native actor/timeout and pending gates cover those scoped actions. Pure user loops, blocking native calls, unexpanded-frontier reporting and instruction atoms still need separate evidence. |
| C26 | Fixed | Pending accounting covers queued and executing work; completion waits and synchronizes packed data/index. Async and sync errors poison subsequent operations. | `check-store-cache` holds a worker at compression, proves barrier stays blocked, then opens a fresh read-only process while writer remains alive. Sync failure and shutdown/reinit also covered. |

| C27 | Fixed | Non-owning shared cache buffers and eviction mutations caused wrong bytes and crashes under pressure. Immutable owned buffers replace mutable cross-thread entries; cache and pending budgets are explicit. | `check-store-cache` covers pointer lifetime, zero/tiny caches, concurrent dedup, multiworker and prefetch. Fixed-data constrained-cache benchmark passes every byte comparison. |
| C28 | Fixed (validated local storage) | Packed raw checksum mismatch, unsafe index/journal publication and destructive maintenance; metadata index merge could lose records and accepted conflicting hashes. | `check-store-packed`, `check-store-metadata`, `check-store-tools` cover short I/O, faults, recovery/append/barrier/reopen, corruption and non-destructive maintenance. Partial files fail explicitly; arbitrary power-loss recovery is not established. |

## Next development slice

Page snapshot branch (2026-10-15): opt-in page manifests (now v12) and page objects use the
existing barrier; flat snapshots (now v11) stay default. Exact decoded flat/page ptrace oracles, cold/tiny
reads, fresh-process inspection, missing/corrupt pages and warmed-cache poisoning
have regressions. Both formats compare live bytes and restore only differing pages.
This strengthens C08 evidence without closing general canonicalization, complete
exploration equivalence or asynchronous dirty tracking. See [page snapshot scope
and whole-chain timings](page-snapshots.md).

Soft-dirty experiment (2026-10-15): opt-in verify reconstructed all11,127 Raft
captures without observed omitted pages; native fixtures also reject a deliberately
lost dirty epoch. This does not close C08 or certify asynchronous/pinned writes:
the baseline-read/clear_refs window, epoch exclusivity and PID/exec lifetime need
a supported contract. The full backend remains default; end-to-end timings show
no gain in the measured workload. See [measurement and limits](redisraft-benchmark.md).

Redis smoke evidence (2026-10-14): [redis-smoke.md](redis-smoke.md).
Redis findings and resolution:

- **C18 / fixed:** Indexed type definitions once, removed nested CU scans, and
  guarded recursive aliases. Forward/opaque same-name typedef and C++ class4/5
  regressions pass; original Redis debug initialization takes about 0.34 seconds
  and 64 MiB RSS under a 1-GiB cap (previously >45 seconds / 6.2 GiB sampled RSS).
- **C19 / fixed for the supported path:** Checked guest sockaddr/length copies,
  length writeback, truncation, fd/peer matching and stale queue handling pass 12
  cross-process cases; original Redis SET/GET now completes. Pending connections now use captured endpoint keys; same numeric client FDs
  across nodes no longer alias. Snapshot formats are now11/12 (I/O repair first introduced7/8).
  EFAULT retains the virtual pending connection; partial copyout is not rolled back.
  These error-path modeling limits and accept4 flag fidelity need further coverage.

The first state/atom repair pass adds XSTATE, strict snapshot I/O, scoped mapping
restoration, ordered system identity and a completed-syscall reference boundary.
Next define and enforce the supported binary/environment/property profile, close
remaining host inputs (C23), and complete thread lifecycle restoration C02 and explicit
actor enumeration C03/C25. C01/C16 now have scoped branch-state/syscall regressions. Consolidate schedulers only with successor-set
and property-observation comparisons. Eviction and wider
mapping coverage (C08) remain necessary for broader workloads.

Current scenario evidence is NP=1, native non-PIE, single-thread, with inherited
ASLR unchanged. It verifies repeated restore within one live process, not cross-run
canonical hashes, multi-node symmetry, complete syscall fidelity or general search
soundness. The early single-thread commit (`c21c496`) remains a design reference,
not a proven oracle.

## Joint virtual I/O boundary slice (2026-10-15)

A shared typed descriptor registry now owns allocation and flags. Files, pipes,
sockets and epoll dispatch through one read/write/close boundary. Guest copies
are exact; failed copyout retains offsets/queued payloads (guest memory may have
a partial prefix written). Archives include pipe endpoints/queues, pre-accept
TCP payloads, socket options and epoll opaque userdata. Old format5/6 is rejected.
Unsupported descriptor/path syscalls stop before host kernel effects; this is a
supported profile, not complete Linux emulation.

Still open: blocking read/accept/epoll wakeups/timeouts; dup/shared open-file
descriptions; cross-node shared filesystem ownership; symlinks; advanced epoll
ET/ONESHOT/nesting and temporary signal masks; TCP timers/Nagle/keepalive effects.
Private VFS mmap copies initial bytes and restores permissions, but does not track
subsequent file writes into clean mapped pages. fsync validates a virtual regular
file without modeling filesystem crashes/durability. These require new transition
and snapshot contracts, rather than success-only syscall stubs.

## Generic framework slice (2026-10-15)

Machine snapshots no longer contain application/checker fields. Flat9/page10 share
SnapshotView chunk readers; opaque per-plugin history is persisted in version2
system metadata with model context and terminal failure identity. Model fingerprints
cover executables, semantic plugin binaries/configuration and runtime profile,
excluding caches, UI and decoder-only plugins. Incompatible states fail explicitly.

Known support limits: trusted native plugin ABI, fixed NP in core, symbolic
inspection of a matching non-PIE main image only, immutable executable files during
a run, explicit Linux syscall bootstrap outside exploration, and time-choice-only
external environment callbacks. Generic network/futex outcomes remain in platform
models. Signal delivery and universal CPU nondeterminism are not solved by removing
workload patches. C02/C03/C08/C23/C25 remain open at their stated scope.

## Closed exploration syscall profile (2026-10-15)

Unknown calls now reject before kernel execution; the default route no longer
passes through. The explicit native allowlist retains owned-process identity and
credential queries, query-only own resource limits and supported private memory
operations. Mapping operations reject scratch-page overlap and range overflow.
Clone/clone3 and thread exit reject until lifecycle restoration is implemented.
The execution entry rejects i386/x32 ABIs and noncanonical wide syscall numbers.

Flat11/page12 include deterministic RNG state and signal dispositions; filesystem
umask is saved with VFS state. Clocks and sysinfo read a fixed virtual environment.
Startup imports actual dispositions/umask and rejects preexisting multiple threads,
blocked/pending signals, active interval timers and alternate stacks. Nonzero
sleep, armed timers and custom signal delivery fail explicitly. Trusted startup
and internal restore injections remain separate from modeled exploration.

`check-syscall-policy` uses native ptrace to prove PR_SET_NAME cannot escape to
host state, validates allowed queries/errors and guards reserved memory.
`check-process-state` covers copy/archive, RNG replay and EFAULT rollback, signal
policy and actual startup inheritance. `check-capture` perturbs RNG/actions to
prove identity changes and repeated restore for flat/page snapshots. These tests
close the default unknown-call escape; C02/C03/C08/C23/C25 remain scoped/open.
General signal delivery and exhaustive random outcomes are not implemented.

## Parked terminal images (2026-10-15)

Virtual exit and intercepted fatal signals leave an owned physical task parked.
Loading and restoring their product states must include those images: skipping
terminated-node metadata or memory mixes a different branch into later captures.
The restore path now loads every node and restores every differing image. Even
same-hash restores validate the owned stopped physical thread set; a genuinely
dead task rejects instead of reporting success. The transition entry rejects an
invalid or terminated actor before selecting/restoring it. The kernel-model
compatibility marker advances to v2; snapshot byte layouts stay flat11/page12.

`check-capture` reproduces the old virtual-exit memory leak, then covers a real
`exit_group` and `ud2`/SIGILL, alternate memory/RNG/umask/time branches, persisted
reload, exact recapture identity, predecessor restore and actual OS death. Both
normal and tiny caches run against flat and verify-pages backends. This closes
that C08 branch-contamination case; changed physical thread sets, stop-all
coordination and general canonicalization remain open.

The original terminal-image slice retained descriptors. Normal last-thread/group
exit now closes supported modeled resources; fatal-crash teardown and TCP reset
cases remain outside that later extension. The
regressions above establish branch-image isolation, not complete exit semantics.

## Raw thread carrier lifecycle

Logical clone/exit reconstructs thread contexts across births and exits using
retained stopped carriers; the physical group leader remains an anchor. Pool
spares do not enter snapshots. Frozen logical-main registers define the header,
so anchor injections cannot change terminal-main identity. Flat15/page16 adds
thread exit codes; kernel model v4 includes logical syscall-visible IDs.

The bounded profile requires explicit raw sharing flags, no inherited clear-tid or
rseq for birth, and no robust ownership at exit. It does not claim pthread support.
All-node ownership/stop preflight precedes restore; physical dirty flags force a
full rollback after failed guest execution or partial restore. Entry cancellation
skips a pending syscall before restore injection; capture refuses such a phase.
C02 remains partial for full Linux lifecycle; C03/C25 actor and instruction-level
coverage remain open despite the explicit transition actor API.

## Cross-runtime follow-up

The [Linux task contract audit](development.md#runtime-independent-task-contract-audit-2026-10-15)
separates kernel ABI coverage from user-level scheduler coverage. Next bounded
slice: clone/clone3 TID copyout + modeled clear-tid/join, with initial-thread
registration handled from exec rather than the prelude's114514 return shortcut.
Guard mappings, per-task masks/altstack, timed waits, robust and signal delivery
remain generic capabilities. The rseq whole-region masking/live-byte preservation
shortcut remains unverified semantic debt, including on the single-thread path;
raw clone currently rejects rseq rather than certifying that shortcut.

## TID/clear-tid slice completed

clone3 and clone now share TID birth/clear semantics, with explicit pending child
write state and initial-thread identity handled from syscall entry. The prelude
114514 return and uncontrolled thread creation path are removed. Shared/private
futex waiters are distinct and persist across wake/requeue/restore. Native-oracle
and full/pages replay tests pass; format17/18 and kernel modelv5 reject prior
execution contexts. Parent-first syscall atoms and fixed group cleanup order
remain scoped. The next observed blocker for unmodified pthread is rt_sigprocmask;
guard mappings, rseq policy, robust ownership and full signal delivery remain open.

## Per-task mask slice completed

Logical task masks now have capture/copy/archive/identity/restore coverage,
including initial inheritance, clone from a non-anchor task, carrier reuse and
same-hash restore. `rt_sigprocmask` uses native argument/error ordering, including
committing a new mask before an oldset copyout fault. Flat19/page20 and kernel
model v6 record this boundary. The eight signal-mask backend/cache/pending
variants and eight clone/clone3 TID variants pass. Actual pending bits (thread
and shared) reject before restore; no pending queue is silently discarded.
General signal delivery, altstack, timed/temporary-mask waits, cancellation and
external asynchronous input remain open. This is not complete pthread/Go support.

## Anonymous guard follow-up

C21 now includes private unnamed anonymous PROT_NONE capture and restore. Six
full/pages/verify-pages x normal/tiny-cache regressions cover hidden-byte identity,
persisted reload, mapping recreation, permission-only restore and exact-read
failure without root publication. Guest copies retain native permission checks.
Read errors are never replaced with zeros or live memory. Dispatcher guard
mprotect preflights backing before kernel effects; unsupported file/shared maps
remain rejected. Snapshot layout remains19/20; kernel model isv7.

The pthread smoke now stops at active rseq registration during clone. That existing
whole-region rseq masking/live-byte preservation shortcut still needs a real
per-task kernel ABI contract; guard support does not resolve it. Large guard
reservations currently require full hidden-byte reads and full restore writes.

## rseq unavailable policy (2026-10-16)

C02/C08 no longer rely on whole-region rseq masking or live-byte preservation.
The execution contract exposes rseq as unavailable (ENOSYS), consistently from
exec prelude through runtime. Kernel model v8 records that policy; full/page
layouts remain19/20. Ordinary unregistered memory is preserved byte-for-byte.

RED: a real registration previously allowed capture and snapshot publication.
GREEN: main/worker/spare registrations reject capture, same/different-state
restore and direct carrier/low-level restore APIs without state mutation. Four
full/pages/cache lifecycle variants verify these boundaries and exact archived
rseq-shaped memory. Startup and syscall-policy tests cover valid/invalid/unregister
arguments returning ENOSYS without memory writes or host registration.

This closes the old masking/preservation shortcut, not full rseq semantics.
CPU/node/mm-cid, restartable critical-section abort and signal/preemption outcomes
remain unimplemented. The actual pthread probe's next blocker is set_robust_list;
robust-list ownership and owner-death need a separate generic Linux ABI slice.

## Robust lifecycle, discard and cleanup follow-up (2026-10-16)

C02 now includes logical robust-list registration and ordinary non-PI owner-death;
physical registrations are null. Native oracles and four backend/cache variants
cover pending, broken/cyclic/aliased links, private/shared wake separation, group
exit and subsequent PI rejection without mutation, empty-only bootstrap adoption,
carrier reuse and persisted replay. Single-field archive identity is checked.

C21 adds unnamed private anonymous MADV_DONTNEED. A reproduced incorrect admission
of VFS file mappings is fixed by archived canonical provenance intervals, trimmed
on successful mmap/munmap/brk changes. Native dispatcher and complete capture/
restore tests preserve rejection after branch reload. Mutable-file coherence,
file-backed discard and other advice remain unsupported.

C09 adds the reproduced ptraced-worker shutdown deadlock fix. Dedicated owned host
groups are created under a SIGTERM startup barrier, and cleanup reaps all members
without group-wide signals or waiting on unrelated children. Startup cannot change
PGID/session or fork children. Fifteen native scenarios include a real pthread
TLS/join/robust-mutex/condition program under both snapshot backends, with normal
and SIGTERM cleanup proving all retained TIDs disappear.

Flat21/page22 and kernel modelv9 encode the new semantic fields. These results do
not close C03/C25: current scheduling still does not enumerate every enabled task,
memory-access boundary, wake outcome or weak-memory behavior.

## Generic pending syscall slice (2026-10-16)

C01/C16/C25 now separate syscall admission, blocking, readiness and completion.
Waiting/Retry/Ready plus original scalar arguments and copied normalized deadline
belong to each logical thread. Physically skipped syscall-exit is a carrier stop,
not a guest return; no user instruction runs on the completion edge. Futex queues
and blocked caches are validated against this state, not independent truth.

Pipe/socket reads, pipe capacity waits, recvfrom, accept and epoll can suspend.
Readiness only permits retry; output faults and competing consumption do not lose
payload. EOF after supported normal peer exit is an explicit completion. Finite
futex/epoll/sleep deadlines can be expired by the control API after modeled time
is due, without automatic time advancement or scheduling policy.

ABI2 event phases distinguish unfinished calls, actual returns and nonreturning
exit/crash. CLI and StateView expose pending rather than a fake success. The old
per-futex timeout boolean and ad-hoc blocked CLI rollback are removed.
Flat23/page24 and kernelv10 encode this contract.

Regressions cover real stopped carriers, no-instruction completion, Waiting/Ready
and Retry persistence, two read attempts competing for one payload, EFAULT after
buffer protection changes, explicit deadline expiry, last-thread/group EOF, and
full/pages normal/tiny cache equivalence. A native phase-checking plugin verifies
pthread, pipe and fatal-stop event observation. Driver-created competing reads
are not claimed as exhaustive real thread scheduling.

Still open: automatic deadline/actor/outcome enumeration; pending descriptor
close requires pinned open-file identities (currently explicitly rejected);
partial-progress blocking writes; poll/select/vector I/O; transport backpressure;
TCP reset and fatal-crash resource teardown; independent realtime/monotonic axes;
signals/cancellation and shared-memory instruction boundaries. None is silently
covered by the successful controlled executions above.

## Case-driven admission follow-up (2026-10-16)

The case corpus now tests healthy/seeded-bug C and C++ input protocols plus a
closed-world pthread lost notification. Search-found witnesses are restored and
replayed three times with identical local state identity, in full and pages.
This establishes those concrete counterexamples, not C03/C25 exhaustive coverage.

Runtime probes exposed and drove fixes for argv storage (argc5 null terminator
out of bounds; argc9 rejected), ordinary anonymous zero-length mmap probes, and
poll(timeout0). Native regressions preserve file-backed zero-length EBADF before
EINVAL, explicitly reject huge-page setup, and verify poll invalid/negative FDs,
pipe HUP/ERR, epoll POLLRDNORM and cross-page revents EFAULT without a partial
scalar store. Kernel modelv11 records the expanded contract;
snapshot byte layouts remain23/24.

The six installed toolchains all pass native probe execution and two modeled
state transitions with same-process restore/replay. Only C/C++ currently complete
the checked runtime probe and search; Rust std stops at sigaltstack, gccgo at
sched_getaffinity, and CPython/OpenJDK at readlink. These are explicit next nails,
not supported-language claims. Standard gc Go remains separate from gccgo.

Known search-reporting caveat remains: depth-limited DFS prints 'Complete explore
all' and visited state is shared across CLI searches. The new runner uses fresh
tracers per search and labels bounded results conservatively. Future core search
work should fix reporting/coverage directly rather than treating this runner
boundary as closure of C03/C25.

### Runtime queries: captured paths and CPU affinity

The next case-driven slice adds readlink/readlinkat over captured VFS symlink
bytes, including the executable path chain and /proc/self/exe captured during
initialization. No host pathname lookup occurs on a modeled query. Ordinary
symlink traversal remains explicitly unsupported; readlink and nofollow stat
can inspect final links. Missing/non-directory prefixes must be checked before
lexical normalization. The synthetic proc projection does not claim general
procfs support, and observing /proc/self itself as a link remains unsupported.

sched_getaffinity exposes one fixed virtual CPU, validates logical live task
identity and follows the raw x86-64 mask-copy ABI. No mutable affinity state was
added; sched_setaffinity, CPUID/proc/vDSO topology coherence remain outside scope
(C17). Kernel modelv12 distinguishes these semantics; byte layouts stay23/24.
Executable bytes now enter the captured VFS, increasing state-copy/hash work by
binary size; this is a correctness-first tradeoff, not a performance optimization.

Runtime re-audit after these changes: Python now reaches an explicit failed
probe exit (missing captured standard-library files), Java advances to access21,
and gccgo pages reaches sigaltstack131. Full gccgo still fails allocation under
the audit's2GiB address-space limit; a4GiB diagnostic caught a747,734,229-byte
allocation in packed-save decompression. An intermittent pages allocation failure
also occurred before the capacity fix, but was not reproduced in four bounded
GDB runs. None of these results count as runtime admission.

The audit found and fixed compressed-buffer retention: resize(encoded_size)
kept compressBound(raw_size) capacity while warm-cache accounting charged only
encoded_size. A regression now requires compact retained capacity and exact
byte decompression. This does not bound transient raw capture, queued work and
save-side decompression. Executable capture separately regressed the Redis
SET/GET single-run time from7.691s to26.012s; prioritize immutable VFS payloads
without weakening byte equality or branch isolation.

### Shared VFS payload evidence

The executable-import performance regression is addressed by immutable shared
contents and >=4096-byte StateStore references in production metadata. Mutations
have no writable aliases and detach branches; standalone archives remain inline.
Flat25/pages26 distinguish the wire format. Registration memo is generation-bound
and checks store readiness even on hits; first registration still verifies bytes.
No global intern table or host-file fallback is introduced.

check-vfs-content covers sharing, file write/truncate isolation, registered-key
invalidation, restored-buffer detachment, inline archive without store, nested
scope exception recovery, reference identity independent of object sharing,
zero-cache fresh-process/read-only reopen, new store generation, wrong tag/key/
length, first-registration collision and poisoned-store memo rejection. Native
full/pages repeatedly restore an8192-byte file before modification and verify
its original bytes before replaying the mutation. Existing IO/capture/state-view/
plugin/identity/pending-IO/snapshot/storage gates remain green. C08 remains scoped;
these checks do not establish arbitrary exploration equivalence.

Three fixed-schedule Redis SET/GET runs per mode preserve208 server and58 client
steps: full median26.845→7.553s, pages19.506→1.980s. Median stored bytes fall from
2158.6→599.7MiB/full and1577.6→17.3MiB/pages. Full-file COW on first mutation and
raw snapshot transient-memory peaks remain separate limitations.

### Runtime admission iteration: alternate stacks and rootfs

Per-task virtual sigaltstack now archives sp/size/normalized flags, uses active
RSP for ONSTACK, and matches native commit-before-old-copyout/error behavior.
Clone disables child registration; exit clears it; disabled AUTODISARM from the
trusted prelude is imported. Full/pages×normal/tiny replay, carrier reuse, native
partial fault oracles and malformed capture/restore guards pass. Virtual minimum
2048 is explicit kernel profilev13; layout27/28. Physical carriers stay disabled,
inherited enabled stacks and handler delivery remain unsupported.

FSMap now captures actual root file/directory/link type, hidden names and exact
bytes, rejects missing/unreadable/unsupported inputs and invalid ancestor/root
overlays. F_OK access family uses saved paths and rejects credential-dependent
search; R/W/X and generic symlink following remain unsupported. Runtime workloads
provide explicit Python/JDK roots and input manifests, outside generic product.

Rust std probe completes full/pages. gccgo pages now stops at pselect6(270).
Python rootfs exposed a separate DFS ownership defect: ~30 ancestor frames each
retain independently decompressed identical VFS payloads when the files exceed
hot-cache capacity. GDB confirmed30 byte-identical allocations per several large
files and ~2.085GB in use; this is not merely fragmentation. Hash-only ancestor
residency is the next bounded fix. Java stops at ordinary jvm.cfg symlink traversal;
per user instruction, costly JVM-specific work may be deferred explicitly without
blocking other cases, while keeping its status non-green.

### Bounded DFS resident-state fix

The actual CLI iterative DFS now drops an ancestor's resident sys_state before
pushing its successor, retaining hash/actor/choice bookkeeping. It reloads the
same saved identity only when backtracking needs another expansion. No search
choice, visited rule or SIGINT ordering changes. Deserialize failures now retain
the underlying exception rather than hiding allocation failure as generic corruption.

Regression:44 syscall prefix,4x16MiB immutable files exceeding the32MiB hot cache,
late four-way branching and file mutation, unchanged2GiB limit. Old full/pages
large DFS fail while corresponding BFS passes. New full/pages×small/large×BFS/DFS
all eight runs yield81 unique states,80 actual transition events and four final
branches; file-mutation and terminal edges each replay three times with identical
hash. Existing DFS searched82 vs BFS81 reporting discrepancy remains unchanged;
actual event multisets match. Real Python now advances to getdents64(217) in both
backends without the former allocation failure. C03/C25 remain open.

### Timed pselect and captured path traversal

Zero-descriptor pselect6 uses existing Waiting/Ready/deadline state, including
NULL infinite wait and best-effort timeout copyout; fd multiplexing and temporary
sigmasks remain unsupported. Native oracles verify timeout validation priority,
ignored fdset pointers at nfds0 and readonly timeout behavior. Control-page overlap
is rejected at admission and archive validation. No automatic timer expiry added.

Captured symlink traversal now follows at most40 links, checksNAME_MAX, preserves
relative/dotdot/trailing-slash semantics, canonicalizes open descriptions, and
honors nofollow/exclusive creation. Native stat/open/access/readlink matrices plus
archive/host-unlink/branch tests pass. Kernelv14, unchanged layouts27/28.

Rust now has checked endpoint3x replay. gccgo actual history exposes fatal
nanotime returning zero before both threads wait; the model's zero clock origin
needs an explicit generic profile decision.4GiB full run avoids earlierallocation
failure but hits90s bound, not successful admission. Java reaches dlopen(libjvm)
then exits6 with undefined JNI_CreateJavaVM; zero virtual inode metadata is the
next concrete hypothesis. Neither result is counted green.

### Directory streams, file identity and clock origin

getdents64 now enumerates saved names/types with independent serialized ordinal
FD cookies. Native field ordering, untouched padding and completed-record partial
fault behavior are covered, including21/46 writable-byte boundary successes.
Malformed long/NUL namespace names reject before mutation. stat/lstat/fstat and
dirent share nonzero path-derived inodes and reject namespace-wide collisions.
This models path objects while rename/hardlink remain unsupported; it is not
host-hardlink fidelity. Directory insertion may affect ordinal scan position.

A real two-DSO native regression proved zero inode caused the loader to reuse
an unrelated loaded object; current minimal DSOs load independently. Fixture
omits CRT to isolate identity from SIGBUS-backed EOF-tail support, still rejected.
Linux-ignored MAP_DENYWRITE/EXECUTABLE flags have native and model oracles.

Virtual initial_time_us defaults to1s and is explicitly configurable including0;
its value enters model identity. This fixes gccgo's fatal zero nanotime without
automatic time advance or runtime-private patches. Kernelv15, layout27/28.
24 native scenarios,12case rows, getdents/IO/syscall-policy/runtime-profile gates
pass, with independent review and native RED/GREEN evidence.

Python now reaches rename82 because current minimal stat drops imported mtime,
invalidating bytecode caches whose header timestamps actually match host sources.
Preserving semantic mtime is the next direct input-fidelity fix. gccgo pages
completes+3x replay (235visited); full raw run reaches exit0 but original driver
was lost, so full checked-replay admission remains unproven. History rendering
also rereads large snapshots after exploration and in info, a measured next
control-chain cost. Java advances beyond libjvm identity to missing captured
libstdc++ dependency, still non-green rather than JVM admission.


### Captured mtime and observation cost

Captured files/directories/symlinks/executables now retain precise mtime through
copy/archive/identity/restore and stat/fstat, including successful mutation times.
Native utimensat/lstat regression covers1786363619.987654321; archive rejection
covers invalid nanoseconds. Layout29/30 and kernelv16. Independent review found
no state-closure blocker within this scope. Remaining: native write(EFAULT) can
update mtime while the pre-copy model does not (confirmed via ctypes/native
write with pointer1); input capture does not detect concurrent same-size changes.
These are explicit compatibility gaps, not closed by timestamp preservation.

CLI scalar current-edge observation avoids StateStore reads; optional quiet
search suppresses only automatic history and restores command scope on exceptions.
24 actual full/pages BFS/DFS finite/violation/interrupt searches retain transition
multisets and3x replay; rand has component-level scope verification only. Standard
24 native scenarios and12 case rows remain green. Python advances beyond the
mtime-induced rename to FIOCLEX; full gccgo checked admission is running with
explicit4GiB budget. Neither is counted passed until endpoint replay succeeds.

Java's captured ELF dependency closure advances loading to a genuine unsupported
SIGBUS-backed file-mapping case. Supporting invalid-backed pages requires memory
mapping, snapshot and guest-fault semantics, beyond a flag allowlist. Per user
tradeoff Java remains a conditional, explicitly non-green research target; no
claim of JVM admission or private-heap semantic extraction is made.


### Raft workload oracle schema2

The example plugin now records committed term/type/exact command bytes and a
branch-owned term-to-leader history. Identical index/term with different commands,
including embedded NUL bytes, violates the property; sequential different leaders
in one term also violate it. Monitor records are bounded and strictly validated.
Unobserved compacted prefixes remain explicitly unavailable instead of invented.
The schema version participates in the plugin identity. This is example-side
property extraction, not Raft knowledge in generic state storage or scheduling.

Independent review approved the slice; freshly rebuilt `make NP=2
check-raft-plugin-contract CC=/usr/bin/gcc CXX=/usr/bin/g++` passes both actual
entry layouts. Unit and sanitizer evidence is component-level: full/pages actual
workload exploration and replay are separate pending acceptance. Legacy willemt
main has shallow-copied command payload lifetime errors; the owned bounded driver
must retain payload ownership rather than weaken the stronger oracle.


### Generic close-on-exec ioctls and runtime evidence

FIOCLEX/FIONCLEX now reuse saved per-descriptor F_SETFD state, without guest
argument access or host mutation. ioctl commands normalize to unsigned32,
including high-register-bit aliases. Unknown commands still reject. Native RED
then GREEN covers18 oracle cases and108 modeled dispatch calls; copy/archive
isolation and flags remain checked. Independent review approves. Kernelv17,
unchanged layouts29/30. NP2 IO gate and make check pass; production NP1 rebuilt.

Python actual full/pages admission now passes273visited, public output property,
exit0 and terminal predecessor3x replay: build/cases/run-l3jl43u3/results.json.
gccgo full controlled admission passes237visited, terminal3x and cleanup0 with
explicit4096MiB/600s profile (actual entire driver222s):
build/cases/run-yucgcv9q/results.json. This complements earlier pages evidence.

Separately acquired official SHA-verified isolated gc Go1.23.12. Native passes;
munmap startup boundary correctly rejects because it follows thread creation.
An explicit mmap startup boundary admits control/replay, then actual exploration
rejects MADV_NOHUGEPAGE: build/cases/run-wgjcc09h/results.json. This is not gccgo
admission and is not yet green; no GC/preemption flags were disabled.


### Anonymous huge-page performance hint

Admit only MADV_NOHUGEPAGE alongside DONTNEED through existing native execution
and unchanged range/backing restrictions. Physical THP policy is excluded from
this execution profile. No new saved field or snapshot format is introduced;
kernelv18 identifies the profile. Native/kernel errno matrix and high32 advice
aliases pass; wrapping addresses retain explicit model rejection rather than
claiming native EINVAL. Native-file/shared/named/virtual-file/control ranges and
other advice still reject. Full/pages/verify-pages with ordinary/tiny caches
restore both hinted and recreated-unhinted mappings to equivalent bytes,
protections, provenance and successor identity. Independent review approved the
implementation; its requested unhinted-replay test correction is now green.
NP2 syscall-policy/IO/capture and NP1 mapping gates passed. Actual gc Go advances
to getrlimit97; captured resource-limit query closure is the next bounded task.


### willemt paired case acceptance

Owned three-node driver retains log payloads and initializes entry types. Native
healthy/mutant use an exactly-once copied-source vote-response change. Actual
full/pages DFS discovers the intended denied-vote dual leader at term1 and replays
its predecessor edge3x. Healthy all nodes commit two distinct commands, then a
positive-budget bounded suffix searches42states/27unique in each backend; a
post-commit edge restores3x. Evidence: build/raft-cases/run-i1nlq358/result.json.

Independent review caught/rejected the first healthy audit (budget0) and overly
broad mutant diagnostics. Corrected heartbeat opportunities, positive-budget gate,
exact election receipts and9 negative acceptance tests close those runner defects.
The final-state cutoff flag is only an observation of the final state, never proof
that all branches avoided budget cutoffs. This finite case does not close C03/C25
or establish general Raft/snapshot compaction correctness. No vendor tree changed.


### Native runtime concurrency acceptance

Real Rust std thread/mpsc and CPython Condition/list producer-consumer pairs now
exercise normal operation, omitted task and duplicated task. Twelve actual
full/pages rows pass exact protocol/worker/exit checks and final or violation
edge3x replay, without disabling GC or preemption. Rust visits45/41/41 states;
Python410/390/392. Evidence: build/cases/concurrency-0jmhl7i4/report.json (Rust6),
build/cases/concurrency-nuo4il7p/report.json (Condition6). Optional Python Queue
still reports unsupported SIGBUS-backed mappings and remains non-green.

Independent review approved the public-ABI property and native fixtures, then
caught partial-matrix false green after an exception. Exact complete/unique
matrix acceptance and per-combination failure recording now pass7 negative tests.
Existing12actual rows were revalidated without altering evidence. This is bounded
case capability, not full language/runtime or exhaustive interleaving support.
Go gc/gccgo concurrency and OpenJDK conditional target remain separately pending.


Current immutable22 reruns of the Rust/Condition matrix independently pass all
12 rows: four healthy completions and eight exact omitted/duplicated-output
violations, with 36 raw terminal replays. The six native commands were rerun;
source/binary/plugin/config hashes, 1,409 rootfs input entries and 24 absent owned
PIDs were checked. Rust visits 45/45, 41/40 and 41/41 states (full/pages by variant);
Python visits 410/410, 390/390 and 392/392. Helper provenance identifies the
committed adaptive polling version. Evidence:
build/case-jobs/iteration22-concurrency-audit/results.json. This does not establish
backend exploration equivalence or exhaustive thread scheduling. Current22 Go
concurrency rows are still running and are not counted from provisional stdout.


### RedisLabs library paired case

The independent RedisLabs Raft-library fixture preserves real pre-vote and entry
reference ownership. Its single copied-source change removes only the actual
vote-grant guard. Native and actual NP3 full/pages pairs pass. Healthy all nodes
commit/apply no-op1 and exact normal commands2/3; bounded suffix70visits/45unique
from positive budget1. Mutants find the intended term1 dual leader on an actual
denied vote and reproduce predecessor→successor3x, with exact diagnostic and
prevote/request-term fields required. Evidence:
build/redisraft-library-cases/run-julvrjq0/result.json. Independent review approves;
four permanent negative acceptance tests cover unrelated failures and invalid
roles/terms/receipts. No vendor changes. Both library pairs are implemented;
the real Redis+RedisRaft module remains a separate unfinished target.

Real module prerequisite progress: acquired official RedisRaft ade4aa8e and
unmodified Redis7.4.2 a0a6f23d into ignored owned build trees. The existing local
Redis is a modified static PIE and crashes natively in module pthread_create;
that is not a DetSim failure. Separately built dynamic Redis plus module passes
native three-node INIT/JOIN/SET and GET via legitimate redirects. Model bootstrap
admits all three but first controlled search explicitly rejects prctl157:
PR_CAPBSET_READ; native tracing also identifies worker PR_SET_NAME. Captured
capability-query and per-thread name semantics are the next generic requirement,
not a workload-specific state-extraction patch.


### Captured resource query state

ProcessKernelState now owns all16 x86-64 resource-limit pairs. Bootstrap imports
inside its preserved scratch transaction and publishes after complete success;
archives validate soft<=hard before committing the entire object. getrlimit and
owned-self prlimit64 share saved-table emulation. Removing native prlimit execution
also removes its live PID rewrite and ambient-query dependency. Native ABI
oracles cover every resource, integer aliases, NULL/error priorities and partial
copy prefixes. Changing only a child's native soft limit after capture leaves
modeled answers unchanged. Copy/archive/identity distinguish single-field changes.

Review caught control-page copyout; both API overlap guards now reject without
modifying page/register/model state, while invalid-resource EINVAL and NULL query
rules remain. Independently approved final source.495 NP2 assertions, full/pages
snapshot/state-I/O gates, identity,24 native scenarios,12 case rows, make check and
production build pass. Layout31/32, kernelv19 reject incompatible roots. Actual gc
Go admission/concurrency now rerun against the new immutable binary. This is
query closure only: no claim of restoring/enforcing physical resource limits,
CPU budgets, complete virtual FD capacities or limit mutations.


### Go admission and goroutine protocol matrices

Standard gc1.23.12 now passes admission in full/pages (201visited each), checked
output/exit0 plus control and terminal3x replay. Explicit mmap startup9 ends the
trusted prelude before thread creation.4GiB profile,900s full admission budget;
actual full driver429s. Evidence: build/cases/run-ixays29s/results.json (pages),
build/cases/run-0xfy5b8i/results.json (full). No GC/preemption flags disabled.

Both gc and gccgo goroutine/channel fixtures now have complete healthy/omit/
duplicate full/pages matrices,12actual rows with native/protocol/exit and3x replay.
gc201/199/199; gccgo full234/236/238 and pages236/236/234. Reports respectively
concurrency-dijy2cw7,concurrency-oschmx18,concurrency-nk79ibko,
concurrency-54sbjxge under build/cases. Independent review approved after closing
compiler identity: explicit -compiler overrides oppositeGOFLAGS;13runner tests,
6opposite-flag native builds and stored-binary compiler/hash audit pass. Evidence:
build/case-jobs/go-compiler-audit-7sy2joyc/report.json. This is language/runtime
protocol capability under current actor policy; Go runtime OS-thread count does
not establish goroutine-to-OS-thread assignment or exhaustive scheduling.


Fresh immutable22 runtime admission reruns independently pass all 12 full/pages
rows: C 4 states, C++ 10, Rust 20, Python 274, gc Go 201/201, gccgo 237/235.
The audit checks 24 sessions, 12 control-edge replays, 36 terminal replays,
48 absent owned PIDs, and 1,413 captured-rootfs input entries; all six native
commands were also rerun to exact RUNTIME_OK/exit0. Actual ELF identity separates
gc and gccgo. Evidence: build/case-jobs/iteration22-admission-audit/results.json.
These jobs used the archived pre-adaptive helper. Admission-other lacks an
embedded helper provenance field; its Git revision and clean product diff plus
the archived Git blob support historical attribution; an exact helper hash was
not recorded at launch for that job. Config digests computed by the audit are additional evidence, not original launch-time records. The
gccgo state-count difference is not a claim of backend exploration equivalence.
Current22 Rust/Condition concurrency reruns are independently verified above;
Go concurrency reruns remain in progress. Older Go protocol results must not be
presented as fresh results from this binary.


### EOF fault backing: native feasibility evidence only

Independent native oracle tests/file_eof_native_oracle.cpp compares ordinary file
mappings against empty-memfd tail overlays with requested permissions and closed
FDs.912read/write/execute comparisons per run,1824children all reaped, stable FD
count, exact signal/si_code/relative address and normal-page bytes; half-page tail
zero/COW leaves original file unchanged. Native and UBSan runs pass; current
hostmemfd_noexec=0, zero execute skips. Independent review approves evidence.
Clang analysis has no diagnostic; GCC14 fanalyzer fails with a documented apparent
policy_fd false positive despite immediate close, not mislabeled as clean.

This does not close the framework gap. Approved next-slice contract requires
explicit FileEofFault regions, no fabricated zero payload, both readers unavailable
on faults, target-snapshot restore of genuine fault backing, serialized provenance
and fixed-image mutation guards. Python Queue/Java/module claims remain non-green
until actual program execution, property and replay gates pass.


### Captured capability bounds and logical task names

Closed prctl subset: CAPBSET_READ imports native support count+bounding mask once,
with explicit uncaptured0/0 marker; SET_NAME/GET_NAME use canonical per-task16bytes.
Names enter copy/archive/identity/restore, inherit from logical parent before
vector growth, freeze on exit and survive carrier reuse without physical rename.
Native oracles cover option32/caparg64 and SET15/GET16 fault/partial-copy semantics.

Independent review caught two closure bugs before commit: late prelude native
rename was not imported, and first-capture helper could inject before stop barriers.
Final bootstrap now commits its local name only after the entire transaction;
low-level adoption only reads /proc, never resumes a task. New RED/GREEN wrapper
regression verifies first capture with running worker rejects with zero resume
attempts and unchanged registers/control page. The real startup157 scenario checks
both backends through search, controlled GET/output/exit and3x same hashes.

Independent final review approves; NP2process245/policy134/IO244, thread control/
carrier, four lifecycle combinations,25 native scenarios,12case rows, snapshot/
state-I/O, make check and NP1/NP3 builds pass. Layout33/34, kernelv20. This does
not implement arbitrary prctl, capability mutation or dynamic procfs names.

Real module attempt now searches rather than rejecting its capability query:
three actual Redis+module processes, pages DFSdepth500, about5755unique states
at the180s timeout. No business client or property checkpoint yet, cleanup-9;
recorded owner group has no surviving tasks. Evidence:
build/workloads/module-control-1b3dchvf/result.json. This is execution/search
progress only. Next prioritize a controlled startup prefix and real client, not
unbounded startup interleavings; ftruncate is statically identified at log init.


### Fixed-image mapping origins and log truncation

SYS_ftruncate now implements regular writable-file size/zero-fill/truncation,
offset preservation and modeled mtime with native error-order oracles. Copy/
archive/identity distinguish content and mapped origin changes. Saved canonical
path ranges exactly cover virtual file mapping coverage; clipping and replacement
update both. Positive write/O_TRUNC/ftruncate guards protect mapped inputs through
symlink/pre-opened aliases, retain refs after close, and release only after unmap.
This closes the mutable-backing admission hole for these writes, not full mapping
coherence. Registering with an aliased source-table string is independently safe.

Independent review approves. NP2IO282/policy140 and four capture backend/cache
runs, detailed syntax, identity, NP1snapshot/state-I/O,25native+12case rows,make
check and NP1/NP4 builds pass. Layout35/36/kernel21. Single ftruncate zero extension
above64MiB is explicitly unsupported, with no mutation; already-large inputs can
still grow a little. General sparse-write memory bounds remain open. Allocation
failure during interval-table changes must abort the conversion; no failed
successor is published. Native real module log initialization motivates this gate.

Hand-controlled real module startup now concretely reaches unlink87, whose native
operation is absent dump.rdb.tmp.recv cleanup (ENOENT). Following INIT also needs
atomic closed meta.tmp→meta rename. Next leaf namespace slice must preserve inode
identity, saved allocator and implicit executable/proc references; no fabricated
unlink success or pathname-based inode recomputation after rename.

### Actual RedisRaft module native baseline

The NP4 case owns three dynamic Redis7.4.2 servers and one real RESP client.
Its public-ABI monitor reads saved client output and saved open log files, checks
per-file CRC chains and exact committed command bytes, and retains branch-local
observations. No module globals or VM-private semantic extraction are required.
The same protocol-property approach applies to interpreted runtime cases; debug
symbols are optional for execution and public resource observation. Java remains
an explicitly deferred, non-green target under the user's cost tradeoff.

Native healthy and seeded node3 skipped-apply pairs pass strict six-cell checks:
healthy client/checker0/0; mutant42/1, only node3 alpha absent with all applied and
commit thresholds satisfied. Evidence: build/redisraft-full-cases/run-1di_y4_e.
Independent review verified hashes, real LF command serialization, parser-negative
tests and exact unique output cells. The controlled model attempt in
run-xofb6fg3 still rejects unlink87 before business initialization; all owned
processes were cleaned up. Model full/pages bounded search and threefold replay
remain unfinished. Native success does not close any snapshot or schedule gap.

### Closed leaf namespace lifecycle

unlink/unlinkat and rename/renameat/renameat2 now admit regular/symlink leaves,
including NOREPLACE. Parent walks retain native symlink/error order; the leaf
itself is not followed. Initial path-derived inodes and monotonic guest-created
inodes occupy separate domains. Rename preserves identity, and the saved inode
allocator remains semantic even after its last object is deleted. Archives reject
collisions, invalid counters and noncanonical overrides. Open/mapped objects,
synthetic exe and actual executable names are protected rather than losing refs.

Preallocating namespace/description nodes also fixes an existing O_CREAT defect:
EMFILE or allocation failure previously could leave a newly created file behind.
FD allocation and all fallible preparation now precede the mutation commit;
rename uses preallocated keys and map node handles. Native pointer/flag/dirfd/
slash priority oracles and old-open RED cover both defects. Directory moves,
sticky mutations, root/mount operations, EXCHANGE/WHITEOUT and referenced-object
mutations remain explicitly unsupported; this is not full Linux VFS fidelity.

Independent review approves. NP2IO311/getdents, identity, policy, full/pages
capture with ordinary/tiny caches; NP1 page/state-I/O;26native scenarios;12case
rows;make check and NP1/NP4 production builds pass. The actual-process lifecycle
searches and replays every recorded edge3x in both backends, including rename,
unlink and recreate. Format37/38 and kernel22 reject incompatible stores.
Evidence: /tmp/detsim-iteration12-{wire,io,np1-wire,scenarios,check}.log and
build/cases/run-davl3_p9/results.json. Real module business/search acceptance
remains a separate unfinished goal.

Actual NP4 module run-po77i4vf now reaches all three ready messages, then its
client assertion reveals Redis protected-mode refusal on the modeled non-loopback
network. The isolated native/model case now explicitly shares protected-mode=no;
no target or framework failure is ignored. run-hoqxo7xz completes real INIT,
ftruncate and closed metadata rename, but its main-thread-only pump then stalls
with pending epoll waits. It remains failed, with clean process cleanup.

The case controller now enumerates logical thread slots every round and gives
each an explicit manual step, including real fsync workers. Blocked attempts
record no successor and must preserve the predecessor hash; completed edges
record process/slot/outcome and both hashes. Client assertions are classified at
their origin rather than reported as a later unsupported abort syscall. Seven
controller and three native-acceptance tests pass with independent review.
Fresh native healthy/mutant passes: build/redisraft-full-cases/run-9dhfb55s.
No claim of full-module search/replay or automatic timeout support follows.

The first fair-thread control run-9hxo68ym times out before completing INIT,
with clean shutdown. At one recorded sample1645 of1903 post-startup actor
attempts merely repeated blocked status. Thread listings now expose saved
runnable/waiting/exited state; the controller refreshes every round and skips
only current waiting/exited slots, including newly ready/born slots next round.
Missing or malformed status fails rather than silently dropping actors. This
changes presentation/control cost only, not snapshot identity or atom policy.
Nine controller and three native-acceptance regressions,27actual scenarios
(including full/pages pending/list/restore3x),make check and NP1/NP4 builds pass;
independent review approves. Full module acceptance remains pending.

### Explicit pending timeout transitions

The fair-controller run-t8wxa5r8 completes2295 actual transitions and real INIT
in183s, then has no runnable actor: three main threads wait in1ms epoll, workers
wait in futex and the client waits in read. It fails explicitly with clean
shutdown. A single-thread native nanosleep fixture independently demonstrates
the same missing control path. Neither stall is called a successful search.

`si timeout` now admits a saved finite Waiting actor, advances only its process
clock to max(current,ceil_us(deadline)), expires and directly completes its
pending call. No next user instruction runs. Source-resource preflight is pure;
live and saved readiness share one implementation with explicit FS context.
Ready/Retry, infinite waits and existing I/O readiness reject before restoring
or mutating the machine. Legal deadline kind/clock/result combinations are
validated. Existing capture/predicate/monitor/commit publication owns the result;
plugin event and TSS record the explicit timeout action(-2), original syscall,
actor and result. Strict CLI parsing prevents accidental atoi choice0 aliases.
No new persistent field is needed;kernel23 retains format37/38.

Independent review approves. Seven waiting syscall families pass actual
state_transition tests with full/pages and ordinary/1byte caches,3x persisted
replay, clock carry/KTIME_MAX/monotonicity, readiness/queue/resource rejection
isolation and both state/edge terminal predicates. The tiny-cache test drains
async saves and proves fresh disk reads;1byte capacity bypasses oversized entries
and is not an eviction-count claim. NP2pending/IO/snapshot,identity,policy,plugins,
runtime-profile and full/pages capture gates;NP1page/state-I/O/futex-snapshot;
27native scenarios;12case rows;make check and NP1/NP4 builds pass. Targeted Clang
analysis of pending,transition and IO paths is clean. Logs:
/tmp/detsim-timeout-*-green.log and /tmp/detsim-iteration14-*.log.

This is explicit manual completion, not automatic timer/actor enumeration.
Builtin BFS/DFS/RAND still omit timeout actions (C03). A subsequent raw-clone
regression now verifies worker slot1 timeout in full/pages while main slot0
retains an untimed futex wait: no worker code resumes until ordinary si, and
three restored repetitions preserve both timeout/output hashes and TSS actor2.
Native worker output/exit0 and all28scenarios pass; evidence:
/tmp/detsim-nonmain-timeout.log and /tmp/detsim-iteration14b-scenarios.log.
Full module business/search/replay remains separately unfinished.

### Case-side bounded branch search

The actual-module runner now implements explicit beam-width1 exploration rooted
before the first SET. Every sibling action restores the same source and owns a
private observation copy; deterministic variant-independent guidance retains one
continuation and records pruned frontiers. Its edge/path table is independent of
TSS's first-parent history. Acceptance checks the pre-SET root, connected actual
selected edges, exact business/violation evidence,3x witness replays, unique
requested matrix and per-command/final wall cutoffs. Independent review exposed
and then verified fixes for detached witness paths and late final replays.

16search,15controller and3native acceptance regressions pass. A real two-runnable
actor fixture yields two distinct manual successors in both backends, matching
case enumeration after four alternate restores. Evidence:
build/guided-search-integration/run-_3c1y0k8/result.json. Builtin DFS1 reports only
root plus one successor here, so C03 remains open; this case search is neither
builtin DFS nor an exhaustive general scheduler. Actual module matrix is still
pending. Current experiments exposed busy GET/INFO polling with frozen clocks;
explicit time input during runnable work is the next case-environment requirement.

### Clock choices and bounded vector writes

The module case now uses an explicit public time-choice plugin: hold or advance
10ms on gettimeofday, startup hold, normal business advance. Search executes both
presets and timeout siblings; its retained path favors normal actors in round
robin order. Duplicate non-time successors do not count as branching.38case
component tests and the full/pages finite actor oracle pass. This environment
policy does not close automatic scheduling C03/C25. Actual module run-xed724we
progressed through INIT/JOIN2, then rejected syscall20; it remains non-green.

Kernel-model24 admits bounded writev on fully readable FILE/STDIO and existing
unbounded TCP queues, with one resource commit and no pending vector state.
Low32 count, descriptor/import error priorities and sequential len/base checks
match the native oracle. Independent review found the same-item inaccessible
base/readable negative-length priority bug; a native counterexample and model
RED now protect the corrected EFAULT. Additional native/model RED tests protect
the metadata span/guard preflight and single-iovec clipping before range checks;
multi-iovec raw ranges are checked before clipping. Payload faults, PIPE/UDP, unknown high
addresses, non-routable sockets and >64MiB staging explicitly reject without
resource mutation. Native partial writes/backpressure remain unsupported.

Evidence: tests/writev_native_oracle.cpp strictGCC+UBSan each305cases (610children
reaped, no FD leaks), build/writev-native-oracle/verification.json; check-writev
locks scalar ordering, flattened FILE/STDIO/TCP resource equivalence and3x archive
replay/branch isolation.29native scenarios include writev full/pages3x exact
successor replay and normal exit (/tmp/detsim-writev-final-scenarios.log). NP2IO,
pendingIO/persisted full/pages, policy/profile/capture and NP1page/stateIO gates
pass. Targeted Clang analysis of IO/socket sources reports no diagnostics. These
component and ptrace results do not establish full RedisRaft business/search
completion; the fresh NP4 module run is separate evidence.

### Local-idle case timeout guidance

Actual module prototype run-vwsqchuh reached INIT/JOIN3 and the pre-SET search
root, including real writev1419. It also retained a main epoll deadline1.270s
while that node's clock stayed1.260s for564listings as peers advanced. This is
observed scheduling delay/potential starvation, not proven deadlock or an overdue
wait. The case now permits one timeout on a locally idle process after each
normal round, node round robin then earliest local deadline. Its search guide
rotates normals with finite timeouts on locally idle nodes; every finite sibling
is still executed and checked. Driver cursors remain transient guidance.

43case tests and full/pages finite two-actor manual/enumerator equivalence pass
(build/guided-search-integration/run-692c1gxd/result.json). Actual-module native
and model evidence must include the common case driver source hash and the new
policy; old prototypes do not satisfy that matrix. No core timer delivery or
general fairness guarantee was added; continuously runnable processes can still
postpone their timed workers under this retained-path policy.

### Case-driver transcript polling

Actual sibling exploration rooted before the first SET captured1128task-clock samples
over10s, zero lost; Python accounted for40.21% of sampled CPU, tracer59.52%, target
0.36% (rounding applies). Artifacts: build/case-jobs/iteration17-module-healthy-pages/
search-profile-summary.json and search-dwarf-perf.data. This is a prototype search
sample, not wall-time attribution or a completed business check.

Session.send formerly reread/decoded the complete growing transcript per poll.
It now seeks to the consumed byte offset, preserving locale decoding, malformed
byte replacement, split UTF-8, newline normalization and prompt/EOF/deadline
behavior.16component tests include a measured RED→GREEN: a23byte reply after a
1MiB consumed prefix formerly read3,145,788bytes across3polls, now at most69bytes.
In3trial100reply microbenchmarks, a16MiB prefix costs6.025s before versus0.010s
after (median); reply hashes match. See build/case-session/benchmark.json and
verification.json. This is polling-only improvement, not full exploration speedup.
Real full/pages actor equivalence and the runner's incomplete/missing-tool failure
gates pass; current binary C/C++/pthread12row cases pass with the final reader.

The reply loop now starts at 100µs and doubles its sleep up to the existing 5ms
cap, preserving the same deadline and prompt/EOF rules. The 17 component tests
include backoff order, cap and timeout overshoot. A paired three-trial experiment
issuing 300 read-only scalar queries to one stopped tracee measured median 1.578s
with fixed polling versus 0.216s with adaptive polling; driver CPU was 0.0632s
versus 0.0350s. This measures inspection latency only, not transition or whole-search
throughput. Raw trials: build/case-session-adaptive/run-a3z71zik/results.json
(pre-commit prototype wrapper, explicitly attributed to its archived helper).

Fresh immutable22 NP3 quorum regression with the committed adaptive loop passes
healthy/mutant × full/pages: 444 executed sibling actions and 12 terminal replays.
Per-row action/layer/alias/pruning counters match the earlier fixed-poll run.
That run used iteration19, so this is not a same-binary isolation of polling
effects. Healthy reaches its first NORMAL commit and mutant reaches the specific quorum
violation. Both use bounded beam-1 exploration, not exhaustive schedule coverage.
Evidence: build/polling-quorum-regression/run-2sksjtwh/result.json; independent
raw action/observation/replay audit: build/case-jobs/iteration22-admission-audit/
polling-quorum.json. The 17 session tests and make check pass. Already-running module drivers retain the exact older
helper archived under build/case-jobs/iteration22-source-archive/manifest.json;
its SHA256 is checked against Git 3c72939. Their outcomes are not attributed to
adaptive polling, and no execution equivalence of the two helper versions is
assumed merely to accept historical evidence.

### Compression output allocation

Actual Go gc full capture includes about1196.71MiB per image,1156.98MiB anonymous
guards; these bytes still require exact capture. DWARF sampling separates capture
memset3.77% from IO-worker memset13.33% of CPU samples. The compression workspace
now uses fresh overwrite-only storage for ZSTD_compressBound capacity, then copies
only the successful returned frame into a tight vector. Zstd may use spare output
capacity internally; no published byte depends on preinitialization. Full capture,
encoded-frame validation, duplicate checks and formats remain unchanged.

15poisoned-allocation comparisons verify complete frame equality against an
independent zero-filled Zstd reference, raw roundtrip and tight capacity;3empty
cases make no output allocation. Packed/cache/storage/page/stateIO, full/pages
capture, identity/plugin/policy and29native scenario gates pass. Clang analysis
is clean; NP1/3/4 production builds pass. No kernel-model bump is needed for these
byte-identical codec outputs; each new executable has its own recorded hash.

Three-trial compressor-only medians for1280MiB zeros:1.718→0.574s and2632112→
1316472KiB peak RSS.128MiB random input:0.278→0.289s with similar peak RSS, so no
uniform speedup is claimed. Raw/frame hashes match before/after. Evidence:
build/compression-output-buffer/benchmark.json. Two actual201state Go full runs
now complete business/exit and3x replay (iteration19-admission-gc-full and
iteration19-runtime-gc-healthy-full); DFS took515.36/516.18s versus prior881.14/
878.65s. These initial observations have different concurrent host loads and
include a driver change; they are not a controlled whole-chain speedup estimate.
The prior900s replay deadline failures remain recorded; retries declare1800s.

### Streaming validation of new encoded records

A 10-second DWARF perf sample of a real gc Go healthy full DFS contains 1,586
stacks with no lost samples. Of these, 27.8% pass through full decompression in
encoded packed save: new records allocate a raw image just to validate it, then
publish only the supplied compressed frame. This is a tracer CPU stack share,
not wall-time attribution. Raw data and classification:
build/case-jobs/iteration22-go-full-profile/analysis.json.

New encoded keys now validate the complete single frame through 128 KiB output
scratch, checking exact decoded length, complete input consumption, errors and
no-progress. Duplicate keys retain exact raw-byte collision comparison; public
load/decompress and persisted bytes, formats and identity are unchanged. Only
streaming window-too-large errors fall back to the original complete decoder,
preserving valid advertised windows above the streaming API limit. This is not
a total-memory bound: compressed input copies, Zstd history/context memory and
wide-window fallback allocations remain.

Allocation and wide-window compatibility regressions each failed before their
fix. Final packed/storage/cache/metadata/tool, page-snapshot and state-I/O gates,
29 native ptrace scenarios, eight input and four pthread rows pass. Independent
raw audits verify 48 case replays, plus four quorum rows with 444 executed actions
and 12 terminal replays. NP1/NP3/NP4 production builds, clang analysis and make
check pass. Evidence: build/packed-stream-validation/ and build/packed-stream-audit/.

Three alternating old22/new23 pairs reuse tests/zero_guard_benchmark.py with a
192 MiB private zero guard and the same adaptive driver. Every one of 24 rows
searches 18 states and passes three terminal replays. Median whole-driver seconds
include startup, search, restore, checking, replay and cleanup; compilation/native
oracles are excluded:

| Backend / guard restore | Baseline | Streaming | Change |
| --- | ---: | ---: | ---: |
| full / write | 13.988 | 13.478 | -3.65% |
| full / discard | 14.149 | 12.846 | -9.21% |
| pages / write | 4.287 | 3.586 | -16.34% |
| pages / discard | 3.243 | 3.093 | -4.62% |

Full-mode median GNU time command maximum RSS falls from 630,280 to 433,980 KiB
(write), and 629,752 to 434,008 KiB (discard), about 192 MiB less; page-mode RSS is
nearly unchanged. This is command maximum RSS, not aggregate process memory.
Other module/runtime jobs ran concurrently; paired gains vary substantially.
These are synthetic whole-case measurements, not Redis throughput or a general
speedup guarantee. Raw trials: build/packed-stream-whole-benchmark/run-ldv5lw7m/
results.json; independent audit: build/packed-stream-audit/benchmark-audit.json.

### Offline module evidence validation

Independent critic review reproduced41false-green mutations in the matrix
validator: ignored unavailable/wrong violation verdicts on candidates, equality
of missing hashes, and contradictory cleanup inventories. Runtime generation
already checked these, but submitted final evidence also needs independent
validation. The validator now reconstructs every sibling from its retained
source history and checks its actual verdict/diagnostic/outcome, including pruned
siblings; it binds the terminal edge to witness and3replays. It requires complete
six-source/code/config fingerprints and valid NP4 ownership with explicitly empty
remaining inventory.47component tests pass; independent317negative mutations
have no acceptance escapes. No execution or guidance function changed.

Existing expensive jobs retain their actual older runner fingerprint; final
audit applies the corrected validator and records its hash separately. A live
driver's earlier passed label alone is insufficient. This changes evidence
acceptance, not kernel semantics or any current model trajectory.


Protocol-case evidence (2026-10-16): the opt-in workload-owned RedisLabs Raft
quorum checker detects a committed NORMAL entry present on only one of three
fixed replicas. Native API oracle and full/pages healthy/alternate-library rows
start before NORMAL submission, enumerate six actor/preset siblings per source,
and reproduce the terminal edge three times. This is a pruned beam1 case, not
closure of C03/C25 or general snapshot-state completeness. The full RedisRaft
module pages pair timed out without business acceptance; full pair remains
non-green pending actual results. See [protocol and module evidence](redisraft-benchmark.md).


Log-repair evidence (2026-10-17): scripted three-voter isolation, real term2
re-election and preheal search now exercise previous-term rejection, truncation
and retry in the real RedisLabs library. Healthy full/pages converge through
index3; the owned-copy guard regression reaches exact committed-entry2 conflict.
876 raw sibling transitions and12 terminal replays were independently checked.
This is a case-side pruned beam, not generic scheduler coverage; full module
healthy/mutant full/pages all timed out. C03/C25 remain open. See the linked
[protocol evidence](redisraft-benchmark.md).


Zero anonymous guard restore (2026-10-17): saved/current mapping and target VFS
provenance proofs plus complete saved-byte validation permit MADV_DONTNEED only
for verified-zero private anonymous PROT_NONE regions. Negative/partial failure
falls back to full saved writes; unexpected exceptions retain the existing dirty
contract. Six backend/cache mapping oracles, native regressions and a paired
finite-search benchmark pass; this optimizes restore, not capture, and does not
close C08 or certify physical-memory observations. See [scope and measurements](page-snapshots.md).

### Captured output after descriptor lifetime (2026-10-17)

A runtime checker regression reproduces the actual-module healthy terminal
observation defect: virtual exit closes fd1, but the retained stdout VFS node is
still saved. FILE correctly rejects that closed descriptor; the checker formerly
treated this as unavailable before checking its exit event. New generic read-only
DETSIM_BUFFER_STDIO observes the original captured channel independently of fd
lifetime, while FILE retains open-descriptor semantics. Missing/corrupt channels
remain errors; ABI2 layout, kernel24 and formats37/38 are unchanged.

NP2 state-view fixtures cold-reopen both layouts and cover all three binary
channels, close/exit, fd1 replacement, malformed selectors and missing/type/size
failures. A real ABI2 ptrace scenario fails on old23 and passes on the new NP1
tracer in full/pages: close and exit read exact bytes, closed FILE rejects, and
three restored exit edges reproduce the same hash. All30 native scenarios,
make check, NP1 storage/page/state-I/O, NP2 state-view/plugins, production NP1/NP4
builds and targeted StateView Clang analysis pass. The optional startup diagnostic
reuses the public reader and again completes12 sessions/36 checkpoint replays.

Healthy pages22/full23 were explicitly aborted for this confirmed checker bug;
partial ledgers/stores, cleanup and source versions remain archived/non-green.
This is not evidence that either module reached its terminal business result.
Old mutant/pages continues independently; full module acceptance is still open.
See [current report](technical-report-2026-10-17.md) and
`build/captured-stdio-{observation,regression}/`. C03/C08/C25 remain open.

### Bounded saved-metadata observations (2026-10-17)

Actual module full INIT/JOIN perf has229/1136 tracer stacks in plugin metadata
inspection loading/decompressing a full image; this is CPU sampling, not search
walltime. SnapshotView now reuses its existing4MiB/256entry LRU for independent
serialized metadata slices when a large root cannot remain cached. Misses retain
complete root validation; every tracee constructor independently runs Cereal,
trailing and I/O checks. Memory reads still require the complete reader. Poison,
ready and epoch guards remain; no formats, identity or kernel-model change.

A runtimeRED old constructor reads a>5MiBflatroot twice; GREEN full/pages with
zero/tiny store caches reads its root once. Component tests cover bothcapacity
limits and positives, oversized bypass, sameepoch concurrent misses, independent
branches, metadata-first memory, repeated invalid archives/I/O, poison, reopen
and posteviction missing/corrupt roots. NP1/4production, NP2framework/page,
NP1storage/page/stateIO,30native and12input/pthread rows pass. Readonly NP4
observations consume actual closed module images in both formats without writes.
Paired synthetic full resource-observation wholecase improves23.251→11.136s,
pages2.864→2.839s;18visited and3replays everyrow. These are not Redis speedups.
See [report](technical-report-2026-10-17.md) and build/compact-snapshot-metadata/.
C03/C08/C25 remain open; nextpriority is actual CLI BFS/DFS actor+timeout coverage,
not extending the Python workaround. Existing immutable22/24jobs retain versions.

### Actual native CLI actor/action coverage and receipts (2026-10-17)

Actual BFS/iterativeDFS now enumerate saved logical runnable/Ready/Retry threads
and each admissible finite timeout, install explicit actors after restore, and
discover choices independently for each source/actor/action. Shared timeout
admission reads only saved resource graphs. Normal search completion may report
one scalar SEARCH_EXITED receipt for an actual all-exited transition, including
its incoming edge and unmodified exit codes; it does not restore UI or imply
success/exhaustion. The case verifier strictly loads/replays that exact actor.

Samefixture/observer matched8oldRED proves missing actor/timeout/Ready successors.
Final immutable26b gate28rows observes980native callbacks/76rootactions and228
explicit same-source replays, exact BFS terminal projection sets and cleanup.
22session/protocol tests cover initial(no predecessor) parsing, nonmain replay,
malformed/nonzero/duplicate receipts and failures. Four actual receipt fixtures
add12replays for first-step exit and last-worker exit. Final12input/pthread cases
pass44sessions/48replays;30native scenarios,8DFS/BFSresidency searches, purepolicy,
NP2pending/timeout/IO/framework, productionNP1/4,makecheck and targeted Clang pass.
Sources, rawlogs and version boundaries are retained under build/native-search-*.

The old pipe checker wrongly required every valid exit to have blocked; the new
BFS covers both blocking and producer-first zero-wait success. Per-event validity
checks remain. Large3cycle pthread control is explicitly a bounded functional
path; finite raw-thread fixtures verify native BFS/DFS successor sets separately.

This fixes the actual CLI actor dimension, not all of C03/C25: RAND remains
separate, a frontier left by an abnormal stop is still reported as incomplete,
and all futex/message outcomes plus instruction atoms are not established. The
depth-sensitive DFS and BFS-frontier parts are addressed in the 2026-10-17
section below. Snapshot37/38
and kernel24 remain unchanged. Four existing actual module attempts still use
immutable22/24, and no terminal acceptance is inferred from these new tests.

### Depth-aware DFS expansion and honest completion (2026-10-17)

RED: on a native fixture whose root needs three transitions, `dfs 1` printed
`Complete explore all 3 sys_states (new: 3), total unique: 2` even though the
discovered successor still had legal actions. After `load <root>`, `dfs 4`
printed `new: 1, total unique: 2` and executed no transition: the hash-only
visited set treated the root as fully explored at the smaller budget. A following
`bfs` from the same root loaded only the root, reported `Search coverage
incomplete`, and never expanded the discovered successor. Both full and pages
reproduced this.

GREEN: iterative DFS keeps a process-local map from saved hash to the largest
remaining depth for which every supported actor/action was expanded. Discovery
still feeds `state_set` for unique accounting, but a later command with a larger
remaining depth reopens the identity; an equal or smaller budget reuses the
earlier expansion. A depth-zero frontier with legal actors reports
`Depth limit reached; Searched for ...`, and a model-budget cut reports
`Model budget exhausted; Searched for ...`. BFS records unbounded expansions and
enqueues a known identity whose recorded expansion was bounded; a per-search
queued set avoids duplicate entries and a stale entry that became fully expanded
while the search was running is skipped. A frontier that remains unexpanded still
reports `Search coverage incomplete; Searched for ...`. SIGINT clears discovery
and expansion bookkeeping together. DFS searched/new counters now count the root
once instead of twice. The map is search bookkeeping only: it is not archived,
does not enter state identity, and does not change snapshot or kernel-model
formats.

The case helper now distinguishes complete, depth-limited, model-budget and
unexpanded-frontier results. It still requires a property violation for a seeded
bug, records the bounded reason in the row, and no longer treats a bounded return
as exhaustive. The actual Willemt healthy suffix had in fact exhausted its model
budget while the old runner required `Complete explore all`; it now records
`search_completion=model_budget`.

Evidence: the native reopen regression passes in full/pages (first search reports
the depth bound, a reload with `dfs 4` reaches `SEARCH_EXITED` and more unique
states); a native DFS-then-BFS regression recovers the recorded frontier and
reaches `Complete explore all` with `SEARCH_EXITED`; all32 native scenarios;28
native actor rows (the DFS1 row now asserts the depth-bound message);8
DFS-residency searches;24 history/violation/interrupt searches;2 guided-search
rows;26 session/protocol tests including completion classification and
bounded-replay cases; and `make check` (9 component,14 build-contract and5 layout
tests). Actual NP3 Willemt full/pages healthy/mutant rows pass; the healthy rows
report49 searched states with `search_completion=model_budget`. The RedisLabs
library full/pages rows also pass; their healthy searches report109 states with
the same budget reason. This closes the depth-sensitive DFS bookkeeping defect,
not C03/C25: RAND keeps its old behavior, a
frontier left by an abnormal stop is still reported as incomplete rather than
solved, and futex/message outcomes plus instruction atoms remain open.

### Case-side exploration debt and built-in-first policy (2026-10-17)

User direction: actual exploration must prefer the built-in CLI BFS/DFS/RAND.
Python case drivers may prepare inputs, write configuration, launch the tracer,
read saved state and replay/validate edges, but must not implement a parallel
sibling/beam/layer search. Manual `si` is allowed only for deterministic scenario
setup or a documented unsupported boundary, and is never built-in exploration
coverage. The rule is recorded in `AGENTS.md`, development guidance and the example
index.

Current inventory:

| Entry | Role | Status |
| --- | --- | --- |
| `examples/raft/run.py`, `examples/redisraft/run.py` | 131/33-step controlled prefix, then built-in `dfs <depth> --quiet` | Setup only; suffix is built-in. Do not add a Python search loop. |
| `examples/redisraft/search.py::search_siblings` | beam-1 manual `si` sibling enumeration used by quorum/repair/full-module cases | Case-side search debt; do not extend. Replace when built-in DFS can produce the same property/witness/replay evidence. |
| `examples/redisraft_module/run.py` | manual thread/actor layer controller and beam-1 search for the full module | Debt; full-module results remain non-green/historical. |
| `examples/_support/session.py::search` | launches built-in `bfs`/`dfs`, parses bounds and replays edges | Orchestration only; correct direction. |
| `tests/guided_search_integration.py` | verifies a case enumerator against manual successors | Host regression, not product exploration; never cite as built-in coverage. |

The Raft-library controlled prefix is not a hidden search. Starting from the
mutant cold-start config and running built-in DFS alone at depths3/5/8/10/12
produced 31/106/421/1000/2275 searched states (20/56/201/457/992 unique) in
0.22/0.63/2.37/6.76/14.59s; every run ended with `Depth limit reached` and no
violation. The injected dual-leader scenario needs the controlled 33-step election
prefix; after that root, the case's suffix exploration is already built-in DFS.
Removing the prefix would require a deeper built-in search over startup
interleavings or a generic checkpoint/target predicate; neither is implemented.

Acceptance for retiring a Python search strategy:

1. Run the same healthy/mutant case from a saved root through actual CLI BFS/DFS
   with explicit actor/action choices and built-in completion/bounded reporting.
2. Preserve the exact property diagnostic, witness state, actor/choice and
   threefold predecessor/successor replay in full and pages.
3. Show the built-in run reaches the property without manual sibling selection;
   keep the old case-side result as historical, not as equivalent coverage.
4. For the full Redis module, built-in coverage also requires the relevant
   syscall/actor path to be supported; until then its result stays explicitly
   non-green rather than being promoted by the Python beam.

New cases must not add a Python search loop. If a case cannot yet use built-in
exploration, record the concrete unsupported boundary and removal target here
before adding any workaround.

Performance record rule: a case performance record uses only the built-in
automatic search report after the deterministic prefix. `search_metrics` exposes
engine/completion, searched/new/unique, elapsed_s/states_per_s, save/restore
times, state-store counters and page capture/restore stats. Native build, manual
setup, replay, CLI/driver polling and whole-case wall time are operational context
and must not be mixed into search throughput. End-to-end timings may be reported
for cost awareness but not as a before/after search optimization result.

There is no fixed search-throughput baseline yet. The historical 500+ states/s
numbers came from an old depth8 Raft smoke (2,582 visits/1,347 new) whose checker
and choice plugin binaries are no longer in the workspace; the current depth3
willemt case is small and budget-cut. A real regression claim needs a pinned
workload (config, target/plugin hashes, prefix) run against archived and current
binaries, with only `search_metrics` compared. Same-case A/B with iteration23 NP3
already shows no 500→67 step: 42 states/83.6 full and 42/96.5 pages, versus the
current 49 states/67-88 full and 49/113.6 pages.

### TUI/CLI command parity and blocking display (2026-10-17)

The ncurses TUI had a second stale command table in `monitor.cpp`; new commands
such as `pending` existed only in `commands.cpp`, while readline/batch used the
new table. Both manual paths now call `execute_command_line`/`find_command`, so
TUI help, argument parsing and commands match the CLI. Manual UI startup selects
process 0 so `thread`/`si` work before an explicit `sw 0`.

TUI crash risks were reproduced at a 10x40 PTY: the 11-row status area produced a
negative content height and a zero/null ncurses window. Init and resize now clamp
the effective status/content heights, reject unusable dimensions, check
`newwin`/`wresize`/`mvwin`, guard zero-width rendering, disable mouse reporting on
shutdown, and serialize public UI entry points against shutdown. The status bar
now lists real commands and a live runnable/waiting/ready/retry count.

New CLI surface:

- `pending [PROCESS]` lists each logical thread, its pending syscall/arguments,
  reason/phase, deadline/result, the next legal action, and all runnable actors.
- `inspect [PROCESS]` prints the schema1 `StateView` JSON: per-thread registers,
  pending/deadline, futex queues, signal/robust/clear-tid state, descriptors,
  files, sockets, pipes and virtual mappings. The old fixed 4 KiB `ui_printf`
  truncated this (a real inspect reply was 4,247 bytes); formatting is now
  dynamically sized so inspect/history/long diagnostics are complete.
- `thread` includes the pending call, reason, phase and deadline instead of only
  `state=waiting`.
- TUI status rows 7/8 are a live panel for the selected process: current slot/TID,
  per-thread `run/wait/ready/retry/dead`, pending reason and deadline. It refreshes
  after every command and during automatic-search status polling.
- `plugins` lists loaded module IDs, ABI/monitor schema, time-choice presence and
  the current saved monitor-state keys/sizes; it does not expose opaque monitor
  payload semantics.
- `diff [--brief] [--max-diff=N] H1 H2` compares two saved `StateView` JSON
  identities recursively (processes/registers/threads/files/descriptors/sockets).
  `--brief` compares top-level sections; `-m/--memory-only` is explicitly
  unavailable instead of silently comparing something weaker.
- A blocked `si` now points to `pending`, `thread N` and `si timeout` instead of
  only saying "Cannot step".

Verified blocking behavior with native fixtures:

- futex WAIT: the waiting actor's `si` returns blocked and preserves the exact
  saved state; `pending` shows `reason=futex` and a runnable peer. Running the
  peer's nanosleep (`si timeout`), write and futex WAKE makes the waiter Ready;
  `si 0` then completes the original futex call. New full/pages regression:
  `test_futex_wait_blocks_actor_but_other_thread_can_run`.
- pipe read: the reader enters `read(...) reason=io phase=waiting`; the peer
  thread remains runnable, writes one byte, and the reader's next `si 0` returns
  the byte. A later pthread-join futex is again visible in `pending`.
- oversized pipe write: explicitly fails with "Blocking pipe writes requiring
  partial progress are unsupported"; it is not reported as a pending wait. Fits-in-
  capacity writes remain synchronous. There is no `wake` command; a peer actor or
  explicit `si timeout` is still required.

Evidence: three PTY TUI tests (`make NP=1 check-tui`) cover 24x80, 10x40, resize
to 8x20, `help/pending/inspect/thread/q` and clean exit; 36 native scenarios
(including the new futex UX test and the extended pending listing, which parses
the inspect JSON and checks pending reason/registers); 28 native actor rows;
`make check`; and the production NP1 build. The TUI test checks safety/command
parity, not colors or layout. Raw-memory-only diff remains open (`-m` rejects).
Automatic time advance, wake injection, and blocking vector/partial pipe writes
remain open.


### Native shared-frontier parallel exploration (2026-10-18)

The original split-frontier / copied-store / stdout-union prototype has been
replaced by `core/search`: one authoritative frontier, persistent native workers,
complete snapshot bundles, typed leases/results, bounded reassignment and explicit
completion classes. No 512-branch truncation or independent worker visited sets.
See [the current implementation and evidence](parallel-exploration.md).

Framework-managed target layout leaves host ASLR enabled. Repeated full/pages
adoption covers thread births/exits, dead main/all logical threads, and selected
pthread TLS/signal-mask/altstack/join/robust paths. Fixed-profile state identity
binds its original bootstrap compatibility manifest; a current worker Hello cannot
by itself certify an older snapshot from a different environment.

Integration gates must jointly exercise choices, pending completion, timeout,
thread adoption, property replay and worker loss. Component frontier/bundle tests
alone do not close these gates. Arbitrary cold-root equivalence, relocation of
untyped pointers, direct hardware nondeterminism, remote workers, durable
coordinator restart, and every Linux syscall/thread model remain outside the
current verified contract. Full-closure transport and metadata durability barriers
still require performance measurement; parallelism does not imply speedup.

### Search residency and shared transport verification (2026-10-18)

Follow-up C03 regression reproduced on 2026-10-19: from the same saved counter
state, `dfs 1 --quiet` reports a depth cut; `load <same-root>` followed by the
same command reports `Complete explore all` with zero transitions, although the
unexplored suffix still exists. Both full/pages reproduce this in
`build/search-throughput/repeated-depth-cut-red.json`. The expansion cache stores
remaining depth but not the bounded outcome. A separate source-supported cost is
that a frame with a model-budget-cut successor is never cached as enumerated,
even after processing every actor/choice. These are not closed by the earlier
depth-increase regressions. A fix must preserve cutoff outcomes across cache
hits and propagate them from descendants without publishing partial expansions.

The targeted DFS defect is now fixed with exact `(hash, remaining_depth)`
outcomes and propagated depth/budget cuts. Only a completely enumerated closure
without cuts is reusable at arbitrary depths; partial BFS expansion records are
not such a proof. Budget-zero runnable roots execute no transition, failed roots
stop as violations, and interrupted frames do not publish completion. The five
targeted old-binary scenarios produced 12 semantic failures; the new NP1 gate
passes all 40 scenarios, including those cases, plus DFS residency and actual
parallel actors. NP2 parallel failure/equivalence scenarios and 12 healthy/mutant
C/C++ and pthread case rows with three replays also pass. Evidence is under
`build/search-throughput/coverage-*-gates.log` and `coverage-cases/summary.json`.
This does not close C03 generally: partial BFS-to-BFS queue recovery remains
incomplete. The DFS cache can grow with the number of distinct remaining-depth
queries; it is not an adjacency cache or ancestor/SCC pruning algorithm.

Automatic BFS/DFS and worker expansion now own a bounded resident-memory proof.
Inactive process bytes may be reused; selected regular-file-backed private RO
regions survive user execution only across audited resume/write boundaries.
Registers, thread state and modeled kernel metadata are still captured. Native
syscalls, unknown resumes, mapping changes and intersecting framework writes
invalidate the relevant proof. This assumes no external memory/backing writers
or plugins bypassing the framework's write helpers. It is not kernel dirty-page
tracking. `DETSIM_RESIDENT_MEMORY=off` disables this optimization; `verify` checks
optimized capture/restore against observed bytes.

A reproduced same-hash bug is covered by `check-capture`: after a helper write
and capture, restoring the originally selected hash must restore its bytes even
when the selected UI state has not changed. The fast path now requires the
resident root and brk to match. Explicit CLI `load` also forces observed restore;
`test_same_hash_load_restores_debugger_memory_edit` checks three edits/restores in
both backends. This does not establish arbitrary external-write safety for all
low-level restore callers outside automatic scopes.

Shared transport uses sealed memfd objects and import-before-confirmation.
The first real graph run exposed an XXH64/XXH3 mismatch hidden by synthetic test
keys; transport and fixtures now use the StateStore hash helper. The real
`check-parallel-bundle` gate covers full/pages export, cold/hot transfer,
validated import and fresh-process reads including VFS content. All ten native
parallel scenarios pass, including worker loss, cancellation and violation
replay. Their journals record 610 shared frames and zero raw fallbacks.

Verification also includes normal/tiny-cache capture byte oracles, 37 native
scenarios, thread adoption/lifecycle, parallel actors, pending syscalls, mappings,
XSTATE, page/state I/O, component/build checks and production NP1/2/3 builds.
The fixed Willemt Raft depth-6 workload retains 1084 visits, 201 new states and
1463 transitions; three native searches exceed 500 visits/s. Its projected-edge
multiset matches the baseline, and a separate verify run checks full capture
bytes. This is scoped evidence, not general full-memory graph equivalence.
See [measurements and remaining limits](search-throughput-2026-10-18.md).

### Shared immutable pool and phase-only worker inventory (2026-10-19)

Parallel search now defaults to a private same-host immutable CAS. Workers verify
and publish complete dependencies before atomically publishing Ready; the
coordinator imports metadata references without a per-edge durability barrier.
The strict complete-bundle path remains available with `--storage durable`.
Existing wait/shutdown APIs still synchronize data. The pool uses filesystem-wide
`syncfs` plus ancestor fsync; its writeback-error contract requires Linux >=5.8
and includes unrelated writes/errors on the same backing filesystem.

Evidence: concurrent/corrupt/missing/evicted object and Ready components;
full/pages VFS closure and normal cold reopen after worker-store removal;
15 NP2 native parallel scenarios, including a post-Hello setsid host helper
without parent-death protection that is naturally killed/reaped after worker
loss; 40 NP1 native scenarios, actual parallel actors and thread adoption;
`make check`, pure pool ASan/UBSan and NP1/2/3 production builds.
Bulk-barrier device coverage uses mocked st_dev, not real mount experiments.
Logs are in `build/shared-parallel/{np2-syncfs-gates,np2-phase-inventory-gates,
np1-syncfs-gates,syncfs-make-check,syncfs-asan}.log`; the retained helper evidence
is in `build/shared-parallel/late-helper-evidence/`.

Worker descendant scans now happen at Hello/terminate, outside the scheduling
loop. Subreaper cleanup covers unobserved orphans before parallel returns;
unguarded host helpers may wait until that final cleanup. This does not extend
PDEATHSIG/EXITKILL to arbitrary host helpers if the coordinator itself is killed.
Published files must remain immutable for the private pool lifetime; warm proof
reuse does not detect arbitrary external edits/deletion. Normal close/reopen and
worker loss tests do not establish power-loss or coordinator-restart recovery.

The frozen initial shared implementation completes a bounded 291,332-node,
521,196-edge Raft-library search and local serial node/first-parent coverage
checks, but no same-size worker-count speedup measurement exists yet. Small-graph
8-worker speedup is 4.52x search-only, 3.65x including finalization; near-linear
scaling is not established. See the [scale report](shared-pool-scaling-2026-10-19.md).
None of this closes general snapshot/atom coverage or runtime admission gaps.

Follow-up v3 adds a worker-local SharedPublisher root-proof LRU bound to one
Pool instance. Only successful Ready publication updates it; hits still decode
metadata and derive actors, and poison/miss/eviction retain explicit checks.
Component wrappers verify actual skipped page/blob calls for fresh system keys,
mixed-root equivalence, failed publication/LRU behavior, fresh pool/context and
warm-poison rejection. NP2 15 native scenarios, NP1 actor/adoption gates, strict
v1 bundles, make check and production builds pass. Cold 1/2/4/6/8 pilots retain
the same complete labeled graph. Absolute throughput improves at every count,
but 8-worker whole-chain speedup falls to 3.98x because the 1-worker path benefits
more. This is not near-linear scaling; further work remains. These proof caches
are host optimization state, excluded from semantic identity and persistence.

Follow-up v5 shares complete immutable-root certificates in the pool's typed
Ready namespace, with distinct domain/version/NP/root/full-model bytes. Only
recognized framework bootstrap identities enable it; cap0/free/v1 paths retain
full validation. Successful SystemReady publication or completed import precedes
certificate publication, and local caching follows success. Missing optional
certificates do not break readonly old-pool imports (regression reproduced RED
then GREEN); conflicting existing certificates and I/O/poison still fail. These
files are pool-lifetime operational proofs, not semantic state identity.
Four-slot immutable page-reader FIFO caching preserves ownership, length and
generation/poison checks, with all four pages charged to the view-cache budget.
Alternating-page regression reduces 40 Store loads to 2 and covers eviction,
held spans, cold reopen and malformed lengths. Two interleaved fresh 1/8-worker
comparisons retain complete labeled graphs and zero retries: combined median
8-worker time falls from 8.2549s to 7.0050s (about 15.1%); same-version speedup
is 4.52x, still below the scaling objective. Formal product gates and current
limitations are recorded in the [scale report](shared-pool-scaling-2026-10-19.md).
This does not establish larger-graph scaling or close model correctness gaps.
