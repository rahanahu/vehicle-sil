"""Deterministic tests for bounded pre-start buffering in _PreStartGate."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path

from aiohttp.test_utils import TestServer
import aiohttp
from conftest import Client, Gui, free_port, is_ended, is_synced
from fake_runtime import FakeLauncher, FakeRun, FakeRuntime, default_check_result

from vehicle_sil_gui.server import GuiServer, Options, _PreStartGate
from vehicle_sil_gui.session import Limits, Session


def _simple_config(tmp_path: Path) -> Path:
    root = tmp_path / "root"
    root.mkdir()
    (root / "simple.yaml").write_text("vehicle_sil_simulation: 2\n", encoding="utf-8")
    return root


def _event_size(ev: dict) -> int:
    return len(json.dumps(ev, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))


class _GatedFakeRun(FakeRun):
    waits_for_check = True

    def __init__(self, spec, *, pid: int, on_done, on_start):
        super().__init__(spec, pid=pid, on_done=on_done)
        self._on_start = on_start

    def start(self, sink) -> None:
        super().start(sink)
        self._on_start(self, sink)


class _GatedFakeRuntime(FakeRuntime):
    def __init__(self, on_start):
        super().__init__()
        self._on_start = on_start

    async def start_run(self, spec):
        self.start_entered.set()
        self._next_pid += 1
        run = _GatedFakeRun(spec, pid=self._next_pid, on_done=self._active.discard, on_start=self._on_start)
        self.runs.append(run)
        self._active.add(run)
        return run


async def _start_gui(root: Path, runtime: FakeRuntime, limits: Limits):
    port = free_port()
    token = "test-token"
    server = GuiServer(Options(
        root=root,
        launcher=FakeLauncher(),
        port=port,
        token=token,
        variables=False,
        limits=limits,
        runtime=runtime,
    ))

    async def fake_check(argv, request, descriptions=None, text=None):
        return 0, default_check_result(argv[-1])

    server._launcher_check = fake_check  # type: ignore[method-assign]
    test_server = TestServer(server.app(), host="127.0.0.1", port=port)
    await test_server.start_server()
    http = aiohttp.ClientSession()
    gui = Gui(proc=None, port=port, token=token, url=f"http://127.0.0.1:{port}/#token={token}",
              stderr=root / ".unused.err")
    return server, test_server, http, Client(gui, http)


def test_prestart_gate_bounds_logs_bytes_sources_markers_stderr_and_bad_stdout() -> None:
    limits = Limits(
        log_lines_per_source=10,
        log_bytes_per_source=2000,
        log_bytes=3000,
        log_line_bytes=512,
        log_sources=2,
        stderr_first=3,
        stderr_last=4,
        problems=5,
    )
    gate = _PreStartGate("simple.yaml", None, limits=limits)

    seq = 0

    def push(ev: dict) -> None:
        nonlocal seq
        ev = {"seq": seq, "t_ms": seq, **ev}
        seq += 1
        gate.add_event(ev, _event_size(ev))

    push({"event": "run_start", "run": "r1"})
    # Feed 200 logs across 4 sources (S0, S1 get their own buckets; S2, S3 fold into "*").
    for i in range(200):
        src = f"S{i % 4}"
        push({"event": "log", "run": "r1", "source": src, "line": f"line-{i:04d}-" + ("x" * 40)})
        assert gate.buffered_log_count <= limits.log_lines_per_source * (limits.log_sources + 1)
        assert gate.buffered_log_bytes <= limits.log_bytes

    # Oversized single log line is dropped immediately without evicting retained lines.
    big = {"event": "log", "run": "r1", "source": "S0", "line": "Z" * 1000}
    push(big)
    assert gate.buffered_log_bytes <= limits.log_bytes

    # Flood log_dropped markers well past limit: memory stays bounded, counts fold into dropped_logs.
    for _ in range(50):
        push({"event": "log_dropped", "run": "r1", "count": 3, "sources": {"S0": 3}})
    assert len(gate._markers) <= limits.log_lines_per_source

    # Flood heartbeats: only the latest is kept.
    for _ in range(100):
        push({"event": "heartbeat", "run": "r1"})
    assert gate._heartbeat is not None

    # Flood stderr lines: only first 3 + last 4 retained, middle counted in stderr_skipped.
    for i in range(50):
        gate.add_stderr(f"err-{i}")
    assert len(gate._stderr_first) == 3
    assert len(gate._stderr_last) == 4
    assert gate.stderr_skipped == 50 - 3 - 4

    # Flood non-JSON stdout lines: bounded by limits.problems, rest counted in omitted_problems.
    for i in range(40):
        gate.add_stdout(f"garbage-{i}")
    assert gate.omitted_problems == 40 - limits.problems

    # Critical check + finding + result + exit + finish are never evicted.
    push({"event": "finding", "run": "r1", "severity": "warning", "message": "important warning"})
    push({"event": "check", "run": "r1", "check": default_check_result("simple.yaml")})
    push({"event": "result", "run": "r1", "result": {"vehicle_sil_run": 2, "result": "passed", "ok": True}})
    gate.add_exit(0)
    gate.add_finish(False, False)

    assert gate.decision.is_set()
    assert gate.passed_check is not None
    assert gate.failed_check is None

    drained = gate.drain()
    # Buffers are freed upon drain.
    assert gate.buffered_log_count == 0
    assert gate.buffered_log_bytes == 0

    event_kinds = [item[1]["event"] for item in drained if item[0] == "event"]
    assert "run_start" in event_kinds
    assert "finding" in event_kinds
    assert "check" in event_kinds
    assert "result" in event_kinds
    assert event_kinds.count("heartbeat") == 1
    assert any(item == ("exit", 0) for item in drained)
    assert any(item == ("finish", False, False) for item in drained)
    assert any(item == ("stderr_skipped", 43) for item in drained)
    stderr_lines = [item[1] for item in drained if item[0] == "stderr"]
    assert stderr_lines == ["err-0", "err-1", "err-2", "err-46", "err-47", "err-48", "err-49"]
    assert sum(gate.dropped_logs.values()) > 0


def test_heavy_prestart_logs_bound_memory_and_preserve_check_diagnostics_and_end(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)
    limits = Limits(
        log_lines_per_source=25,
        log_bytes_per_source=64 * 1024,
        log_bytes=128 * 1024,
        stderr_first=2,
        stderr_last=3,
    )
    peak_logs = 0
    peak_bytes = 0
    total_engine_logs = 300
    total_stderr = 30

    def on_start(run: _GatedFakeRun, sink) -> None:
        async def drive() -> None:
            nonlocal peak_logs, peak_bytes
            gate = sink._gate
            assert gate is not None
            run.emit("run_start", run="r-1")
            run.emit("phase", phase="checking")
            for i in range(total_engine_logs):
                run.emit("log", source="Engine", line=f"prestart-{i:04d}")
                peak_logs = max(peak_logs, gate.buffered_log_count)
                peak_bytes = max(peak_bytes, gate.buffered_log_bytes)
            for i in range(total_stderr):
                run.emit_stderr(f"stderr-line-{i:02d}")
            run.emit("finding", severity="warning", message="prestart diagnostic finding")
            run.emit("check", check=default_check_result("simple.yaml"))
            run.emit("result", result={"vehicle_sil_run": 2, "result": "passed", "end": "completed", "ok": True})
            run.exit_process(0)
            await run.finish(returncode=0, process_exited=False)

        asyncio.create_task(drive())

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        server, test_server, http, client = await _start_gui(root, runtime, limits)
        try:
            sock = await client.ws()
            await sock.until(is_synced)

            status, body = await client.post("api/run", {"file": "simple.yaml"})
            assert status == 200
            run_id = body["run_id"]

            await sock.until(is_ended)

            # Retention in _PreStartGate never exceeded Limits.
            assert peak_logs == limits.log_lines_per_source
            assert peak_bytes <= limits.log_bytes

            # Live WS client received accurate dropped notice, retained tail logs, check, finding, result, end.
            dropped_msgs = [m for m in sock.received if m.get("type") == "dropped"]
            assert len(dropped_msgs) == 1
            dropped_sseq = dropped_msgs[0]["stream_seq"]
            assert isinstance(dropped_sseq, int)
            assert dropped_msgs[0] == {
                "type": "dropped",
                "stream_seq": dropped_sseq,
                "run_id": run_id,
                "reason": "not-kept",
                "log": {"Engine": total_engine_logs - limits.log_lines_per_source},
                "at_least": [],
                "events": 0,
            }
            log_lines = [
                m["event"]["line"]
                for m in sock.received
                if m.get("type") == "event" and m["event"].get("event") == "log"
            ]
            assert len(log_lines) == limits.log_lines_per_source
            assert log_lines[0] == f"prestart-{total_engine_logs - limits.log_lines_per_source:04d}"
            assert log_lines[-1] == f"prestart-{total_engine_logs - 1:04d}"

            # No false seq-gap errors.
            errors = [m for m in sock.received if m.get("type") == "error"]
            assert not any(e["code"] == "seq-gap" for e in errors)

            # Stderr first, skipped notice, and last diagnostic lines all arrived intact.
            stderr_errors = [e for e in errors if e["code"] == "launcher-stderr"]
            assert len(stderr_errors) == 2 + 1 + 3
            assert stderr_errors[0]["message"].endswith("stderr-line-00")
            assert stderr_errors[1]["message"].endswith("stderr-line-01")
            assert stderr_errors[2]["detail"] == {"skipped": total_stderr - 2 - 3}
            assert [e["message"].rsplit(": ", 1)[-1] for e in stderr_errors[3:]] == [
                "stderr-line-27", "stderr-line-28", "stderr-line-29",
            ]

            # Summary has check, finding, result, ended state, and exact log kept/dropped counts.
            st_sess, sess_body = await client.get("api/session")
            assert st_sess == 200
            summary = sess_body["session"]
            assert summary["state"] == "ended"
            assert summary["check"]["result"] == "passed"
            assert summary["result"]["result"] == "passed"
            assert any(f["message"] == "prestart diagnostic finding" for f in summary["findings"])
            assert summary["log"]["Engine"] == {
                "kept": limits.log_lines_per_source,
                "dropped": total_engine_logs - limits.log_lines_per_source,
            }

            # Reconnect before the dropped notice sees it; reconnect at/after it gets no duplicate or spurious at_least.
            before_drop = server.snapshot(run_id, dropped_sseq - 1)
            assert any(
                m.get("type") == "dropped"
                and m["reason"] == "not-kept"
                and m["log"] == {"Engine": total_engine_logs - limits.log_lines_per_source}
                and m["at_least"] == []
                for m in before_drop
            )
            at_drop = server.snapshot(run_id, dropped_sseq)
            assert not any(m.get("type") == "dropped" for m in at_drop)
            last_sseq = max(m["stream_seq"] for m in sock.received if "stream_seq" in m)
            fresh = server.snapshot(run_id, None)
            assert any(
                m.get("type") == "dropped"
                and m["reason"] == "not-kept"
                and m["log"] == {"Engine": total_engine_logs - limits.log_lines_per_source}
                and m["at_least"] == []
                for m in fresh
            )
            caught_up = server.snapshot(run_id, last_sseq)
            assert not any(m.get("type") == "dropped" for m in caught_up)

            await sock.close()
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_heavy_prestart_logs_preserve_failed_check_findings_and_allow_next_run(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)
    limits = Limits(log_lines_per_source=15, log_bytes_per_source=16 * 1024, log_bytes=32 * 1024)
    attempt = 0
    peak_logs = 0

    def on_start(run: _GatedFakeRun, sink) -> None:
        nonlocal attempt
        attempt += 1
        cur = attempt

        async def drive() -> None:
            nonlocal peak_logs
            gate = sink._gate
            assert gate is not None
            run.emit("run_start", run=f"r-{cur}")
            for i in range(250):
                run.emit("log", source="ECU", line=f"spam-{i}")
                peak_logs = max(peak_logs, gate.buffered_log_count)
            if cur == 1:
                run.emit("check", check={
                    "result": "failed",
                    "errors": 1,
                    "findings": [{
                        "severity": "error",
                        "code": "bad-participant",
                        "file": "simple.yaml",
                        "line": 3,
                        "participant": "ECU",
                        "network": None,
                        "message": "ECU failed --describe",
                    }],
                    "participants": [],
                })
                await run.finish(returncode=1)
            else:
                run.emit("check", check=default_check_result("simple.yaml"))

        asyncio.create_task(drive())

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        server, test_server, http, client = await _start_gui(root, runtime, limits)
        try:
            st1, body1 = await client.post("api/run", {"file": "simple.yaml"})
            assert st1 == 422
            assert body1["error"]["code"] == "check-failed"
            assert body1["error"]["detail"]["findings"][0]["code"] == "bad-participant"
            assert body1["error"]["detail"]["findings"][0]["message"] == "ECU failed --describe"
            assert peak_logs == limits.log_lines_per_source

            # Subsequent start succeeds cleanly after failed pre-start flood.
            st2, body2 = await client.post("api/run", {"file": "simple.yaml"})
            assert st2 == 200
            assert body2["run_id"] == f"{server.instance}-1"
            await runtime.runs[1].finish(returncode=0)
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_heavy_prestart_logs_preserve_crash_diagnostics_and_genuine_seq_gap(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)
    limits = Limits(log_lines_per_source=10, stderr_first=2, stderr_last=2)

    def on_start(run: _GatedFakeRun, sink) -> None:
        async def drive() -> None:
            run.emit("run_start", run="r-crash")
            for i in range(100):
                run.emit("log", source="Engine", line=f"before-gap-{i}")
            # Inject a genuine launcher sequence gap (as delivered by ServeRuntime on resident seq jump).
            run._seq += 3
            run.emit_problem(
                "seq-gap",
                "events are missing: seq 101 to 103 never came from the launcher",
                {"expected": 101, "got": 104},
            )
            for i in range(50):
                run.emit("log", source="Engine", line=f"after-gap-{i}")
            for i in range(20):
                run.emit_stderr(f"crash-diag-{i:02d}")
            # Launcher crashes without emitting check or result.
            run.exit_process(42)
            await run.finish(returncode=42, process_exited=False)

        asyncio.create_task(drive())

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        server, test_server, http, client = await _start_gui(root, runtime, limits)
        try:
            sock = await client.ws()
            await sock.until(is_synced)

            st, _ = await client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            await sock.until(is_ended)

            errors = [m for m in sock.received if m.get("type") == "error"]
            seq_gaps = [e for e in errors if e["code"] == "seq-gap"]
            assert len(seq_gaps) == 1
            assert seq_gaps[0]["detail"] == {"expected": 101, "got": 104}
            assert any(e["code"] == "no-result" and e["detail"] == {"exit_code": 42} for e in errors)

            stderr_errors = [e for e in errors if e["code"] == "launcher-stderr"]
            assert stderr_errors[2]["detail"] == {"skipped": 20 - 2 - 2}
            assert [e["message"].rsplit(": ", 1)[-1] for e in stderr_errors[3:]] == [
                "crash-diag-18", "crash-diag-19",
            ]

            _, sess = await client.get("api/session")
            assert sess["session"]["state"] == "ended"
            assert sess["session"]["launcher"]["exit_code"] == 42
            assert sess["session"]["log"]["Engine"] == {"kept": 10, "dropped": 140}
            await sock.close()
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_note_prestart_dropped_logs_skips_stream_seq_when_counts_non_positive() -> None:
    s = Session("1", "simple.yaml", [], None, False)
    calls = 0

    def next_seq() -> int:
        nonlocal calls
        calls += 1
        return calls

    s.next_stream_seq = next_seq
    assert s.note_prestart_dropped_logs({"Engine": 0}) == []
    assert calls == 0


def test_duplicate_seq_after_dropped_prestart_log_emits_seq_order(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)
    limits = Limits(log_lines_per_source=5, log_line_bytes=120, problems=10)

    def on_start(run: _GatedFakeRun, sink) -> None:
        async def drive() -> None:
            run.emit("run_start", run="r-dup")
            # Oversized log at seq=1 is dropped immediately by _PreStartGate.
            oversized = {"event": "log", "seq": 1, "t_ms": 10, "run": "r-dup", "source": "Engine", "line": "X" * 300}
            sink.on_stdout_event(oversized, _event_size(oversized))
            # Duplicate seq=1 arriving right after the dropped log must produce seq-order, not be accepted.
            dup = {"event": "log", "seq": 1, "t_ms": 20, "run": "r-dup", "source": "Engine", "line": "DUPLICATE_SEQ_1"}
            sink.on_stdout_event(dup, _event_size(dup))
            run._seq = 2
            run.emit("check", check=default_check_result("simple.yaml"))
            run.emit("result", result={"vehicle_sil_run": 2, "result": "passed", "ok": True})
            run.exit_process(0)
            await run.finish(returncode=0, process_exited=False)

        asyncio.create_task(drive())

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        server, test_server, http, client = await _start_gui(root, runtime, limits)
        try:
            sock = await client.ws()
            await sock.until(is_synced)
            st, _ = await client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            await sock.until(is_ended)

            errors = [m for m in sock.received if m.get("type") == "error"]
            assert [e["code"] for e in errors] == ["seq-order"]
            log_lines = [
                m["event"]["line"]
                for m in sock.received
                if m.get("type") == "event" and m["event"].get("event") == "log"
            ]
            assert "DUPLICATE_SEQ_1" not in log_lines
            _, sess = await client.get("api/session")
            assert sess["session"]["log"]["Engine"] == {"kept": 0, "dropped": 1}
            await sock.close()
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_buffered_finish_after_dropped_trailing_log_broadcasts_current_last_seq(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)
    limits = Limits(log_lines_per_source=5, log_line_bytes=120)

    def on_start(run: _GatedFakeRun, sink) -> None:
        async def drive() -> None:
            run.emit("run_start", run="r-tail")  # seq 0
            run.emit("phase", phase="checking")  # seq 1
            run.emit("check", check=default_check_result("simple.yaml"))  # seq 2
            # Trailing oversized log at seq=3 is dropped while finish is also buffered in _PreStartGate.
            oversized = {"event": "log", "seq": 3, "t_ms": 40, "run": "r-tail", "source": "Engine", "line": "Y" * 300}
            sink.on_stdout_event(oversized, _event_size(oversized))
            run.exit_process(1)
            await run.finish(returncode=1, process_exited=False)

        asyncio.create_task(drive())

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        server, test_server, http, client = await _start_gui(root, runtime, limits)
        try:
            sock = await client.ws()
            await sock.until(is_synced)
            st, _ = await client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            msgs = await sock.until(is_ended)
            ended_broadcast = msgs[-1]["session"]
            _, sess = await client.get("api/session")
            assert ended_broadcast["last_seq"] == 3
            assert sess["session"]["last_seq"] == 3
            await sock.close()
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())
