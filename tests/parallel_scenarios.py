#!/usr/bin/env python3
"""NP=2 native shared-frontier integration; Python only launches and checks receipts."""
import argparse
import ctypes
from collections import Counter
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import socket
import struct
import select
import subprocess
import sys
import tempfile
import time
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples"))
from _support.session import Session, ENV, limits
TRACER = ROOT / "tracer"
OUTPUT = None


def receipts(text):
    return [json.loads(line) for line in re.findall(r"PARALLEL_RESULT (\{[^\n]+\})", text)]


class ParallelScenarios(unittest.TestCase):
    def setUp(self):
        if OUTPUT is None:
            self.temp = tempfile.TemporaryDirectory(prefix="ds-parallel-")
            self.addCleanup(self.temp.cleanup)
            self.root = Path(self.temp.name)
        else:
            self.root = Path(tempfile.mkdtemp(prefix="ds-parallel-", dir=OUTPUT))
        source = self.root / "probe.c"
        source.write_text('#include <unistd.h>\nvolatile int counter=0;\n'
                          'int main(void) { for (int i=0;i<2;++i)\n'
                          'if ((++counter,write(1,"p",1))!=1) return 1; return 0; }\n')
        subprocess.run(shlex.split(os.environ.get("CC", "/usr/bin/gcc")) +
                       ["-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie", source,
                        "-o", self.root / "probe"], check=True)

    def session(self, backend, fixed=True, assertions=(), plugins=()):
        binary = str(self.root / "probe")
        session = Session(TRACER, [binary], list(plugins), self.root / (backend + str(fixed)), backend,
                          240, tracees=[[binary], [binary]],
                          runtime_profile={"fixed_layout": fixed}, assertions=assertions)
        self.addCleanup(session.close)
        session.send()
        session.send("sw 0")
        return session

    def result(self, session, command):
        reply = session.send(command)
        rows = receipts(reply)
        self.assertEqual(len(rows), 1, reply)
        return rows[0]

    def journal(self, result):
        directory = Path(result["artifacts"])
        files = list(directory.glob("*.jsonl"))
        self.assertEqual(len(files), 1, list(directory.iterdir()))
        return [json.loads(line) for line in files[0].read_text().splitlines()]

    @staticmethod
    def event(row):
        return row.get("event", row.get("type"))

    def graph(self, result):
        rows = self.journal(result)
        edges = {tuple(row[key] for key in ("source", "target", "process", "thread", "choice", "action"))
                 for row in rows if self.event(row) == "edge"}
        self.assertEqual(len(edges), result["edges"], result)
        self.assertTrue(edges, rows)
        return edges

    def matrix(self, backend):
        session = self.session(backend)
        original = session.current_hash()
        reference = None
        results = []
        for strategy in ("dfs", "bfs"):
            for workers in (1, 2, 4):
                result = self.result(session, f"parallel {workers} 6 --strategy {strategy}")
                # Each of two independent processes performs write, write,
                # exit: the reference graph is the 4x4 progress grid.
                self.assertEqual(result["status"], "complete", result)
                self.assertEqual((result["nodes"], result["edges"]), (16, 24), result)
                self.assertEqual(result["retries"], 0, result)
                graph = self.graph(result)
                nodes = [row for row in self.journal(result) if self.event(row) == "node"]
                self.assertEqual(Counter(row["depth"] for row in nodes),
                                 Counter({0: 1, 1: 2, 2: 3, 3: 4, 4: 3, 5: 2, 6: 1}))
                self.assertEqual(Counter(row["phase"] for row in nodes),
                                 Counter({"expanded": 15, "terminal": 1}))
                if reference is None:
                    reference = graph
                else:
                    self.assertEqual(graph, reference, result)
                self.assertEqual(session.current_hash(), original, "coordinator cursor changed")
                results.append(result)
        (self.root / "results.json").write_text(json.dumps(results, indent=2))
        # A depth-zero probe must not execute a guest transition.
        zero = self.result(session, "parallel 2 0 --strategy bfs")
        self.assertEqual(zero["status"], "bounded", zero)
        self.assertEqual(zero["edges"], 0, zero)
        self.assertEqual(zero["nodes"], 1, zero)

    def test_full_graph_equivalence(self):
        self.matrix("full")

    def test_pages_graph_equivalence(self):
        self.matrix("pages")

    def storage_equivalence(self, backend):
        session = self.session(backend)
        seed = session.current_hash()
        reference = None
        results = []
        # Same seed/session intentionally shares warm data. This is a correctness
        # comparison, never a storage-mode performance measurement.
        for strategy in ("dfs", "bfs"):
            for storage in ("durable", "shared"):
                result = self.result(session, f"parallel 2 6 --strategy {strategy} --storage {storage}")
                self.assertEqual(result["status"], "complete", result)
                self.assertEqual(result["storage"], storage, result)
                self.assertEqual((result["nodes"], result["edges"]), (16, 24), result)
                self.assertEqual(result["retries"], 0, result)
                rows = self.journal(result)
                edges = {tuple(row[key] for key in
                               ("source", "target", "process", "thread", "choice", "action",
                                "choice_count", "result_code", "violation"))
                         for row in rows if self.event(row) == "edge"}
                nodes = {(row["state"], row["depth"], row["phase"])
                         for row in rows if self.event(row) == "node"}
                self.assertEqual(len(edges), 24)
                self.assertEqual(len(nodes), 16)
                if reference is None:
                    reference = edges, nodes
                else:
                    self.assertEqual((edges, nodes), reference, result)
                local = [row for row in rows if self.event(row) == "done" and row.get("local")]
                self.assertEqual(result["local_completions"], len(local), result)
                if storage == "shared":
                    self.assertGreater(result["local_completions"], 0, result)
                else:
                    self.assertEqual(result["local_completions"], 0, result)
                self.assertEqual(session.current_hash(), seed)
                results.append(result)
        (self.root / "storage-equivalence.json").write_text(json.dumps({
            "scope": "same-session warm correctness only; not timing evidence", "results": results}, indent=2))

    def test_full_shared_durable_equivalence(self):
        self.storage_equivalence("full")

    def test_pages_shared_durable_equivalence(self):
        self.storage_equivalence("pages")

    def shared_cold_replay(self, backend):
        session = self.session(backend, assertions=["tracee0(counter < 2)"])
        result = self.result(session, "parallel 2 6 --strategy bfs --storage shared")
        self.assertEqual(result["storage"], "shared", result)
        self.assertEqual(result["status"], "violation", result)
        self.assertTrue(result["property_violation"], result)
        witness = result["violation_state"]
        incoming = [row for row in self.journal(result)
                    if self.event(row) == "edge" and row["target"] == witness and row["violation"]]
        self.assertTrue(incoming, result)
        edge = incoming[0]
        self.assertEqual(session.close(timeout=60), 0, "normal parent shutdown must complete")
        pool = session.directory / "memory/shared-objects"
        self.assertTrue(pool.is_dir(), "parent shared pool must survive normal shutdown")
        removed = []
        for directory in session.directory.glob(".parallel-*/worker-*"):
            if directory.is_dir():
                removed.append(str(directory))
                shutil.rmtree(directory)
        self.assertTrue(removed, "must remove actual worker private stores before cold reopen")
        self.assertTrue((session.directory / "sstate/packed.dat").is_file())
        step = "si timeout" if edge["action"] == "timeout" else "si " + str(max(0, edge["choice"]))
        commands = [f"load --adopt-threads {edge['source']:016x}", f"sw {edge['process']}",
                    f"thread {edge['thread']}", step, "info current",
                    "parallel 1 0 --storage shared", "q"]
        # Reuse the existing case directory/config without Session.mkdir; the
        # process has no parent/worker memory caches. This tests normal close,
        # not coordinator mid-append death or power-loss recovery.
        process = subprocess.Popen(
            [str(TRACER), "-n", "-c", str(session.directory / "config.json")],
            cwd=session.directory, env=os.environ | ENV | {"DETSIM_SNAPSHOT_BACKEND": backend},
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, preexec_fn=limits, start_new_session=True)
        transcript = ""
        try:
            transcript, _ = process.communicate("\n".join(commands) + "\n", timeout=90)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            transcript, _ = process.communicate(timeout=10)
            self.fail("fresh cold replay timed out: " + transcript[-2000:])
        finally:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.communicate(timeout=10)
            (self.root / "cold-replay.log").write_text(transcript)
            (self.root / "cold-replay.json").write_text(json.dumps({
                "scope": "normal close; worker private directories removed; parent pool retained",
                "result": result, "incoming": edge, "removed": removed, "commands": commands,
                "reopen_exit": process.returncode}, indent=2))
        self.assertEqual(process.returncode, 0, transcript)
        self.assertIn(f"Loaded state {edge['source']:016x}", transcript)
        self.assertIn("Reach illegal state", transcript)
        edges = re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})", transcript)
        self.assertTrue(edges, transcript)
        self.assertEqual(edges[-1][1], f"{witness:016x}", transcript)
        rows = receipts(transcript)
        self.assertEqual(len(rows), 1, transcript)
        self.assertEqual(rows[0]["status"], "violation", rows)
        self.assertEqual(rows[0]["storage"], "shared", rows)
        self.assertTrue(rows[0]["property_violation"], rows)
        self.assertEqual(rows[0]["violation_state"], witness, rows)
        self.assertEqual(rows[0]["edges"], 0, rows)

    def test_full_shared_violation_survives_worker_store_removal(self):
        self.shared_cold_replay("full")

    def test_pages_shared_violation_survives_worker_store_removal(self):
        self.shared_cold_replay("pages")

    def injected_failure(self, session, command, trigger="lease", interrupt=False):
        old = set(session.directory.glob(".parallel-*"))
        stopped = threading.Event()
        victims, errors = [], []

        def inject():
            try:
                deadline = time.monotonic() + 30
                while not stopped.is_set() and time.monotonic() < deadline:
                    for directory in set(session.directory.glob(".parallel-*")) - old:
                        path = directory / "journal.jsonl"
                        if not path.exists():
                            continue
                        rows = []
                        for line in path.read_text().splitlines():
                            try:
                                rows.append(json.loads(line))
                            except json.JSONDecodeError:
                                pass  # concurrent append; retry the completed record
                        spawns = {row["worker"]: row["pid"] for row in rows
                                  if self.event(row) == "worker_spawn"}
                        for row in rows:
                            if self.event(row) != trigger or row["worker"] not in spawns:
                                continue
                            pid = session.proc.pid if interrupt else spawns[row["worker"]]
                            try:
                                fd = os.pidfd_open(pid)
                            except ProcessLookupError:
                                continue
                            try:
                                signal.pidfd_send_signal(fd, signal.SIGINT if interrupt else signal.SIGKILL)
                                victims.append(pid)
                                return
                            finally:
                                os.close(fd)
                    time.sleep(.001)
            except Exception as error:
                errors.append(error)

        killer = threading.Thread(target=inject)
        killer.start()
        try:
            result = self.result(session, command)
        finally:
            stopped.set()
            killer.join(timeout=5)
        self.assertFalse(killer.is_alive())
        self.assertFalse(errors, errors)
        self.assertEqual(len(victims), 1, "failed to exercise actual worker death")
        for pid in victims:
            self.assertEqual(Path(f"/proc/{pid}").exists(), interrupt, "incorrect process lifetime after injection")
        return result

    def test_worker_loss_requeues_without_losing_edges(self):
        session = self.session("pages")
        baseline = self.result(session, "parallel 1 6 --strategy bfs")
        self.assertIn(baseline["status"], ("complete", "bounded"), baseline)
        expected = self.graph(baseline)
        for trigger in ("lease", "edge"):
            result = self.injected_failure(session, "parallel 2 6 --strategy bfs --retries 2", trigger)
            self.assertEqual(result["status"], baseline["status"], result)
            self.assertGreaterEqual(result["retries"], 1, result)
            self.assertEqual(self.graph(result), expected)

    def test_worker_loss_reaps_helper_created_after_hello(self):
        # A transition callback cannot run during check_initial/pre-Hello boot.
        # Its mutable flag is host-only fault injection, never semantic state.
        endpoint = "detsim-helper-" + self.root.name[:48] + "-" + str(os.getpid())
        source = self.root / "late_helper.cpp"
        source.write_text(r'''#include "api/plugin.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstddef>
namespace {
bool worker_process() {
    const int fd=open("/proc/self/cmdline",O_RDONLY|O_CLOEXEC);
    if(fd<0)return false;
    char bytes[8192];const auto count=read(fd,bytes,sizeof(bytes));close(fd);
    if(count<=0)return false;
    for(size_t pos=0;pos<size_t(count);){
        const size_t len=strnlen(bytes+pos,size_t(count)-pos);
        if(len==sizeof("--worker-fd")-1&&!memcmp(bytes+pos,"--worker-fd",len))return true;
        pos+=len+1;
    }
    return false;
}
detsim_verdict inject(const detsim_host_v2*,const detsim_state_view*,
                     const detsim_state_view*,const detsim_event_v2*,detsim_diagnostic) {
    static bool fired=false;
    if(fired||!worker_process())return DETSIM_PASS;
    fired=true;
    const long descriptor_limit=sysconf(_SC_OPEN_MAX);
    if(descriptor_limit<=0)return DETSIM_ERROR;
    static constexpr char endpoint[]="\0" __ENDPOINT__;
    sockaddr_un address{};address.sun_family=AF_UNIX;
    static_assert(sizeof(endpoint)-1<=sizeof(address.sun_path));
    memcpy(address.sun_path,endpoint,sizeof(endpoint)-1);
    const pid_t child=fork();
    if(child<0)return DETSIM_ERROR;
    if(!child){
        if(setsid()<0||prctl(PR_SET_PDEATHSIG,0))_exit(91);
        // Drop protocol sockets, packed-store locks, and every other inherited
        // fd before opening our independent test rendezvous connection.
#ifdef SYS_close_range
        if(syscall(SYS_close_range,0U,~0U,0)<0)
#endif
            for(long fd=0;fd<descriptor_limit;++fd)close(int(fd));
        const int control=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(control<0||connect(control,reinterpret_cast<sockaddr*>(&address),
            socklen_t(offsetof(sockaddr_un,sun_path)+sizeof(endpoint)-1)))_exit(92);
        const int32_t identity[2]={int32_t(getppid()),int32_t(getpid())};
        const auto* next=reinterpret_cast<const char*>(identity);size_t left=sizeof(identity);
        while(left){
            const auto n=write(control,next,left);
            if(n<0&&errno==EINTR)continue;
            if(n<=0)_exit(93);
            next+=n;left-=size_t(n);
        }
        for(;;)pause();
    }
    // Remain inside the callback until the controller has the helper pidfd and
    // kills us. No observer cadence or guessed delay is involved.
    for(;;)pause();
}
const detsim_plugin_v2 descriptor{DETSIM_PLUGIN_ABI_V2,sizeof(detsim_plugin_v2),
    "test.late-host-helper",1,nullptr,inject,nullptr,nullptr,nullptr,nullptr};
}
extern "C" const detsim_plugin_v2* detsim_plugin_entry_v2(){return &descriptor;}
'''.replace("__ENDPOINT__", json.dumps(endpoint)))
        plugin = self.root / "late_helper.so"
        subprocess.run(shlex.split(os.environ.get("CXX", "/usr/bin/g++")) +
                       ["-std=gnu++20", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "src"), str(source), "-o", str(plugin)], check=True)
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        session, collector = None, None
        observed, errors = {}, []
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.settimeout(15)
        listener.bind("\0" + endpoint)
        listener.listen(1)
        try:
            session = self.session("pages", plugins=[plugin])
            original = session.current_hash()
            children = Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children")
            original_tracees = set(children.read_text().split())
            self.assertEqual(len(original_tracees), 2)

            def kill_after_handshake():
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(15)
                        data = b""
                        while len(data) < 8:
                            block = connection.recv(8-len(data))
                            if not block:
                                raise AssertionError("helper disconnected before its identity handshake")
                            data += block
                        worker, helper = struct.unpack("=ii", data)
                        observed.update(worker=worker, helper=helper, helper_fd=os.pidfd_open(helper))
                        if os.getsid(helper) != helper or os.getpgid(helper) != helper:
                            raise AssertionError("helper did not escape the worker process group")
                        descriptors = sorted(path.name for path in Path(f"/proc/{helper}/fd").iterdir())
                        if descriptors != ["0"]:
                            raise AssertionError(f"helper retained inherited descriptors: {descriptors}")
                        with Path(f"/proc/{helper}/status").open() as status:
                            parent = re.search(r"(?m)^PPid:\s+(\d+)", status.read())
                        if not parent or int(parent[1]) != worker:
                            raise AssertionError("helper was not a live child of the blocked worker")
                        worker_fd = os.pidfd_open(worker)
                        try:
                            worker_status = Path(f"/proc/{worker}/status").read_text()
                            worker_parent = re.search(r"(?m)^PPid:\s+(\d+)", worker_status)
                            worker_args = Path(f"/proc/{worker}/cmdline").read_bytes().split(b"\0")
                            if (worker in (os.getpid(), session.proc.pid) or not worker_parent or
                                    int(worker_parent[1]) != session.proc.pid or b"--worker-fd" not in worker_args):
                                raise AssertionError("handshake did not identify an owned worker")
                            signal.pidfd_send_signal(worker_fd, signal.SIGKILL)
                        finally:
                            os.close(worker_fd)
                except Exception as error:
                    errors.append(error)

            collector = threading.Thread(target=kill_after_handshake, daemon=True)
            collector.start()
            result = self.result(session, "parallel 1 6 --retries 0")
            collector.join(timeout=20)
            self.assertFalse(collector.is_alive(), "helper handshake controller did not finish")
            self.assertFalse(errors, errors)
            self.assertIn("helper_fd", observed, "first worker transition never created the helper")
            self.assertEqual(result["status"], "failed", result)
            self.assertIn("retry", result["error"].lower(), result)
            self.assertNotIn("deadline", result["error"].lower(), result)
            spawns = [row["pid"] for row in self.journal(result) if self.event(row) == "worker_spawn"]
            self.assertIn(observed["worker"], spawns)
            poller = select.poll()
            poller.register(observed["helper_fd"], select.POLLIN)
            naturally_exited = bool(poller.poll(0))
            naturally_reaped = not Path(f"/proc/{observed['helper']}").exists()
            (self.root / "late-helper.json").write_text(json.dumps({
                "worker": observed["worker"], "helper": observed["helper"],
                "naturally_exited": naturally_exited, "naturally_reaped": naturally_reaped,
                "result": result}, indent=2))
            self.assertTrue(naturally_exited and naturally_reaped,
                            "late setsid helper survived worker loss or remained unreaped")
            self.assertEqual(set(children.read_text().split()), original_tracees)
            self.assertEqual(session.current_hash(), original)
            session.send("sw 0")
            reply = session.send("si")
            self.assertNotIn("DetSim state/execution error", reply)
            self.assertNotEqual(session.current_hash(), original, "original tracees could not resume")
        finally:
            # Only after the natural-reaping verdict above may cleanup signal a
            # surviving helper. Python's subreaper scope makes failed-test cleanup
            # reapable after the tracer exits; it cannot create a passing verdict.
            listener.close()
            if collector is not None:
                collector.join(timeout=20)
            helper_fd = observed.get("helper_fd")
            if helper_fd is not None:
                try:
                    signal.pidfd_send_signal(helper_fd, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if session is not None:
                session.close()
            if helper_fd is not None:
                try:
                    os.waitid(os.P_PIDFD, helper_fd, os.WEXITED)
                except ChildProcessError:
                    pass
                finally:
                    os.close(helper_fd)
            self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)

    def test_retry_exhaustion_is_failed(self):
        session = self.session("full")
        result = self.injected_failure(session, "parallel 2 6 --retries 0")
        self.assertEqual(result["status"], "failed", result)
        self.assertIn("retry", result["error"].lower(), result)

    def test_startup_deadline_cleans_partial_workers(self):
        session = self.session("pages")
        children = Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children")
        baseline = set(children.read_text().split())
        result = self.result(session, "parallel 2 6 --timeout-ms 1 --retries 0")
        self.assertEqual(result["status"], "failed", result)
        self.assertIn("deadline", result["error"].lower(), result)
        self.assertEqual(set(children.read_text().split()), baseline)

    def test_interrupt_stops_search_and_keeps_cli_usable(self):
        session = self.session("pages")
        source = session.current_hash()
        result = self.injected_failure(session, "parallel 2 6", interrupt=True)
        self.assertEqual(result["status"], "failed", result)
        self.assertIn("interrupt", result["error"].lower(), result)
        self.assertEqual(session.current_hash(), source)
        for row in self.journal(result):
            if self.event(row) == "worker_spawn":
                self.assertFalse(Path(f"/proc/{row['pid']}").exists(), "cancelled worker survived")

    def test_requires_configured_layout(self):
        session = self.session("full", fixed=False)
        result = self.result(session, "parallel 2 1")
        self.assertEqual(result["status"], "failed", result)
        self.assertIn("layout", result["error"].lower(), result)

    def test_violation_has_replayable_incoming_edge(self):
        session = self.session("pages", assertions=["tracee0(counter < 2)"])
        result = self.result(session, "parallel 2 6 --strategy bfs")
        self.assertEqual(result["status"], "violation", result)
        witness = result["violation_state"]
        incoming = [row for row in self.journal(result)
                    if self.event(row) == "edge" and row["target"] == witness and row["violation"]]
        self.assertTrue(incoming, result)
        edge = incoming[0]
        records = self.journal(result)
        self.assertTrue(result["property_violation"])
        self.assertFalse(any(row["event"] == "done" and row["lease"] == edge["lease"] for row in records),
                         "validated counterexample must not wait for Done")
        source_nodes = [row for row in records if row["event"] == "node" and row["state"] == edge["source"]]
        self.assertEqual(len(source_nodes), 1)
        self.assertEqual(source_nodes[0]["phase"], "pending")
        for _ in range(3):
            reply = session.send(f"load --adopt-threads {edge['source']:016x}")
            self.assertIn("Loaded state", reply)
            session.send(f"sw {edge['process']}")
            session.send(f"thread {edge['thread']}")
            reply = session.send("si timeout" if edge["action"] == "timeout" else
                                 "si " + str(max(0, edge["choice"])))
            self.assertIn("Reach illegal state", reply)
            self.assertEqual(session.current_hash(), f"{witness:016x}")
        already_failed = self.result(session, "parallel 2 0")
        self.assertEqual(already_failed["status"], "violation", already_failed)
        self.assertEqual(already_failed["violation_state"], witness)
        self.assertEqual(already_failed["edges"], 0)

    def test_saved_origin_rejects_changed_environment(self):
        seed = self.session("pages")
        source = seed.current_hash()
        self.assertEqual(seed.close(), 0)
        directory = self.root / "different-environment"
        directory.mkdir()
        for name in ("memory", "sstate"):
            shutil.copytree(seed.directory / name, directory / name)
        (directory / "memory/.packed.lock").unlink(missing_ok=True)
        result = subprocess.run(
            [str(TRACER), "-n", "-c", str(seed.directory / "config.json"),
             "--target-cwd", str(seed.directory)],
            input=f"info current\nload --adopt-threads {source}\ninfo current\nq\n", cwd=directory,
            env=os.environ | ENV | {"DETSIM_SNAPSHOT_BACKEND": "pages", "DETSIM_ORIGIN_TEST": "changed"},
            preexec_fn=limits, start_new_session=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=30)
        (directory / "transcript.log").write_text(result.stdout)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("Loaded state " + source, result.stdout)
        self.assertIn("Load rejected: snapshot execution model differs", result.stdout)
        current = re.findall(r"Current state ([0-9a-f]{16}) ", result.stdout)
        self.assertEqual(len(current), 2, result.stdout)
        self.assertEqual(current[0], current[1])

    def test_coordinator_death_kills_owned_workers_and_tracees(self):
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        owned = {}
        session = None
        try:
            session = self.session("pages")
            session.proc.stdin.write(b"parallel 2 100 --timeout-ms 30000\n")
            session.proc.stdin.flush()
            deadline = time.monotonic() + 10
            stopped = None
            workers = set()
            while time.monotonic() < deadline:
                for path in session.directory.glob(".parallel-*/journal.jsonl"):
                    rows = []
                    for line in path.read_text().splitlines():
                        try:
                            rows.append(json.loads(line))
                        except json.JSONDecodeError:
                            pass
                    spawned = {r["worker"]: r["pid"] for r in rows if r["event"] == "worker_spawn"}
                    workers.update(spawned.values())
                    if stopped is None:
                        leases = [r for r in rows if r["event"] == "lease"]
                        if leases:
                            stopped = spawned[leases[0]["worker"]]
                            fd = os.pidfd_open(stopped)
                            try:
                                signal.pidfd_send_signal(fd, signal.SIGSTOP)
                            finally:
                                os.close(fd)
                # Wait for both workers' tracees to exist. A leased worker has
                # already installed its parent-death guard; STOP before Hello
                # would instead test an artificial bootstrap scheduling race.
                children = {}
                for parent in workers | {session.proc.pid}:
                    path = Path(f"/proc/{parent}/task/{parent}/children")
                    if path.exists():
                        children[parent] = {int(pid) for pid in path.read_text().split()}
                if stopped and len(workers) == 2 and all(len(children.get(pid, ())) >= 2 for pid in workers):
                    for pid in workers | set().union(*children.values()):
                        owned[pid] = os.pidfd_open(pid)
                    break
                time.sleep(.001)
            self.assertTrue(stopped and len(owned) >= 8, "did not capture live worker/tracee tree")
            session.proc.kill()
            session.proc.wait(timeout=5)
            deadline = time.monotonic() + 10
            remaining = set(owned)
            while remaining and time.monotonic() < deadline:
                for pid in tuple(remaining):
                    try:
                        result = os.waitid(os.P_PIDFD, owned[pid], os.WEXITED | os.WNOHANG)
                        if result is not None:
                            remaining.remove(pid)
                    except ChildProcessError:
                        if not Path(f"/proc/{pid}").exists():
                            remaining.remove(pid)
                time.sleep(.002)
            (self.root / "parent-death.json").write_text(json.dumps({
                "coordinator": session.proc.pid, "owned": sorted(owned), "remaining": sorted(remaining)}, indent=2))
            self.assertFalse(remaining, "parent-death guards left owned processes alive")
        finally:
            # Cleanup only after recording/checking the natural parent-death
            # result; these signals cannot manufacture a passing verdict.
            if session is not None and session.proc.poll() is None:
                session.proc.kill()
                session.proc.wait()
            for pid, fd in owned.items():
                try:
                    signal.pidfd_send_signal(fd, signal.SIGKILL)
                    os.waitid(os.P_PIDFD, fd, os.WEXITED)
                except (ProcessLookupError, ChildProcessError):
                    pass
                finally:
                    os.close(fd)
            self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)


def main():
    global TRACER, OUTPUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, default=TRACER)
    parser.add_argument("--output-dir", type=Path)
    args, tests = parser.parse_known_args()
    TRACER = args.tracer.resolve()
    if args.output_dir:
        OUTPUT = args.output_dir.resolve()
        OUTPUT.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[sys.argv[0], *tests], verbosity=2)


if __name__ == "__main__":
    main()
