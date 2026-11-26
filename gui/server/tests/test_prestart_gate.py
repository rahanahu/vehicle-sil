"""Deterministic tests for bounded pre-start staging in Session."""

from __future__ import annotations

import asyncio
import json
import threading
from pathlib import Path

import aiohttp
import pytest
from aiohttp.test_utils import TestServer
from conftest import Client, Gui, free_port, is_ended, is_synced
from fake_runtime import FakeLauncher, FakeRun, FakeRuntime, default_check_result

from vehicle_sil_gui import process
from vehicle_sil_gui.hub import Hub
from vehicle_sil_gui.server import GuiServer, Options
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


def test_staged_session_bounds_logs_bytes_sources_markers_stderr_and_bad_stdout() -> None:
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
    s = Session("1", "simple.yaml", [], None, False, limits=limits, staged=True)
    seq_calls = 0

    def next_seq() -> int:
        nonlocal seq_calls
        seq_calls += 1
        return seq_calls

    s.next_stream_seq = next_seq

    seq = 0

    def push(ev: dict) -> None:
        nonlocal seq
        ev = {"seq": seq, "t_ms": seq, **ev}
        seq += 1
        assert s.feed_event(ev, _event_size(ev)) == []

    push({"event": "run_start", "run": "r1"})
    # Feed 200 logs across 4 sources (S0, S1 get their own buckets; S2, S3 fold into "*").
    for i in range(200):
        src = f"S{i % 4}"
        push({"event": "log", "run": "r1", "source": src, "line": f"line-{i:04d}-" + ("x" * 40)})
        assert sum(len(log.lines) for log in s._logs.values()) <= limits.log_lines_per_source * (limits.log_sources + 1)
        assert s._log_bytes <= limits.log_bytes

    # Oversized single log line is dropped immediately without evicting retained lines.
    big = {"event": "log", "run": "r1", "source": "S0", "line": "Z" * 1000}
    push(big)
    assert s._log_bytes <= limits.log_bytes

    st = s._staging
    assert st is not None
    # Flood log_dropped markers well past limit: memory stays bounded, counts fold into prestart_dropped.
    for _ in range(50):
        push({"event": "log_dropped", "run": "r1", "count": 3, "sources": {"S0": 3}})
    assert len(st.markers) <= limits.log_lines_per_source

    # Flood heartbeats: only the latest is kept.
    for _ in range(100):
        push({"event": "heartbeat", "run": "r1"})
    assert st.heartbeat is not None

    # Flood stderr lines: only first 3 staged + last 4 retained in _stderr_last.
    for i in range(50):
        assert s.stderr_line(f"err-{i}") == []
    assert len(s._stderr_last) == 4
    assert s._stderr_lines == 50

    # Flood non-JSON stdout lines: bounded by limits.problems, rest counted in omitted_problems.
    for i in range(40):
        assert s.bad_line(f"garbage-{i}") == []
    assert st.omitted_problems == 40 - limits.problems

    # Critical check + finding + result + exit + finish are never evicted.
    push({"event": "finding", "run": "r1", "severity": "warning", "message": "important warning"})
    push({"event": "check", "run": "r1", "check": default_check_result("simple.yaml")})
    push({"event": "result", "run": "r1", "result": {"vehicle_sil_run": 2, "result": "passed", "ok": True}})
    s.stage_exit(0)
    s.stage_finish(False, False)

    assert st.decision is True
    assert st.passed_check is not None
    assert st.failed_check is None
    # Staging consumed zero global stream_seq.
    assert seq_calls == 0

    dropped_before = {src: log.dropped + log.prestart_dropped for src, log in s._logs.items()}
    assert sum(dropped_before.values()) > 0

    batch = s.take_staged()
    assert batch is not None
    # Buffers are freed upon take_staged, and pre-event summary is completely clean.
    assert not s.staged
    assert s._staging is None
    assert sum(len(log.lines) for log in s._logs.values()) == 0
    assert s._log_bytes == 0
    clean = s.summary(variables=False)
    assert clean["log"] == {}
    assert clean["check"] is None
    assert clean["result"] is None
    assert clean["launcher_state"] is None
    assert clean["problems"] == []

    drained = batch.merged_items()
    event_kinds = [item[1]["event"] for item in drained if item[0] == "event"]
    assert "run_start" in event_kinds
    assert "finding" in event_kinds
    assert "check" in event_kinds
    assert "result" in event_kinds
    assert event_kinds.count("heartbeat") == 1
    assert any(item == ("exit", 0) for item in drained)
    assert any(item == ("finish", False, False) for item in drained)
    stderr_first_lines = [item[1] for item in drained if item[0] == "stderr_first"]
    assert stderr_first_lines == ["err-0", "err-1", "err-2"]
    assert list(s._stderr_last) == ["err-46", "err-47", "err-48", "err-49"]


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
            session = sink._run.session
            assert session.staged
            run.emit("run_start", run="r-1")
            run.emit("phase", phase="checking")
            for i in range(total_engine_logs):
                run.emit("log", source="Engine", line=f"prestart-{i:04d}")
                peak_logs = max(peak_logs, sum(len(log.lines) for log in session._logs.values()))
                peak_bytes = max(peak_bytes, session._log_bytes)
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

            # Retention in staged Session never exceeded Limits.
            assert peak_logs == limits.log_lines_per_source
            assert peak_bytes <= limits.log_bytes

            # Live WS client received accurate dropped notice WITHOUT stream_seq (Protocol v4).
            dropped_msgs = [m for m in sock.received if m.get("type") == "dropped"]
            assert len(dropped_msgs) == 1
            assert "stream_seq" not in dropped_msgs[0]
            assert dropped_msgs[0] == {
                "type": "dropped",
                "run_id": run_id,
                "reason": "not-kept",
                "log": {"Engine": total_engine_logs - limits.log_lines_per_source},
                "at_least": [],
                "events": 0,
            }
            vars_msg = next(m for m in sock.received if m.get("type") == "variables" and m.get("run_id") == run_id)
            vars_sseq = vars_msg["stream_seq"]
            first_event = next(m for m in sock.received if m.get("type") == "event" and m.get("run_id") == run_id)
            assert first_event["stream_seq"] == vars_sseq + 1

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

            # Reconnect at vars_sseq sees the prestart drop (at-least-once); at first_flushed_sseq gets no duplicate.
            at_vars = server.snapshot(run_id, vars_sseq)
            assert any(
                m.get("type") == "dropped"
                and "stream_seq" not in m
                and m["reason"] == "not-kept"
                and m["log"] == {"Engine": total_engine_logs - limits.log_lines_per_source}
                and m["at_least"] == []
                for m in at_vars
            )
            at_first = server.snapshot(run_id, first_event["stream_seq"])
            assert not any(m.get("type") == "dropped" for m in at_first)
            last_sseq = max(m["stream_seq"] for m in sock.received if "stream_seq" in m)
            fresh = server.snapshot(run_id, None)
            assert any(
                m.get("type") == "dropped"
                and "stream_seq" not in m
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
            session = sink._run.session
            assert session.staged
            run.emit("run_start", run=f"r-{cur}")
            for i in range(250):
                run.emit("log", source="ECU", line=f"spam-{i}")
                peak_logs = max(peak_logs, sum(len(log.lines) for log in session._logs.values()))
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
            assert server.current is None

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


def test_hub_broadcast_never_stamps_stream_seq_on_dropped() -> None:
    calls = 0

    def next_seq() -> int:
        nonlocal calls
        calls += 1
        return calls

    sent: list[dict] = []

    async def send(text: str) -> None:
        sent.append(json.loads(text))

    hub = Hub(lambda _r, _a: [], next_stream_seq=next_seq)
    client = hub.client(send)
    hub.clients.add(client)

    msg = {"type": "dropped", "run_id": "1", "reason": "not-kept", "log": {"Engine": 4}, "at_least": [], "events": 0}
    assert hub.broadcast([msg]) is True
    assert "stream_seq" not in msg
    assert calls == 0
    assert len(client.items) == 1
    assert "stream_seq" not in json.loads(client.items[0].text)


def test_zero_flushed_event_prestart_drop_watermark(tmp_path: Path) -> None:
    """When zero staged events survive (only a dropped oversized log before pre-check crash),
    prestart_seq = S_vars + 1 aligns cleanly with ended()'s first post-variables frame."""
    root = _simple_config(tmp_path)
    limits = Limits(log_lines_per_source=5, log_line_bytes=120)

    def on_start(run: _GatedFakeRun, sink) -> None:
        async def drive() -> None:
            oversized = {"event": "log", "seq": 0, "t_ms": 1, "run": "r-zero", "source": "Engine", "line": "X" * 300}
            sink.on_stdout_event(oversized, _event_size(oversized))
            run.exit_process(9)
            await run.finish(returncode=9, process_exited=False)

        asyncio.create_task(drive())

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        server, test_server, http, client = await _start_gui(root, runtime, limits)
        try:
            sock = await client.ws()
            await sock.until(is_synced)
            st, body = await client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            run_id = body["run_id"]
            await sock.until(is_ended)

            dropped_msgs = [m for m in sock.received if m.get("type") == "dropped"]
            assert len(dropped_msgs) == 1
            assert "stream_seq" not in dropped_msgs[0]
            assert dropped_msgs[0]["log"] == {"Engine": 1}

            vars_msg = next(m for m in sock.received if m.get("type") == "variables" and m.get("run_id") == run_id)
            vars_sseq = vars_msg["stream_seq"]
            # Zero event messages were flushed; the first post-variables frame is no-result at vars_sseq + 1.
            assert not any(m.get("type") == "event" and m.get("run_id") == run_id for m in sock.received)
            no_result = next(m for m in sock.received if m.get("type") == "error" and m["code"] == "no-result")
            assert no_result["stream_seq"] == vars_sseq + 1

            at_vars = server.snapshot(run_id, vars_sseq)
            assert any(m.get("type") == "dropped" and m["log"] == {"Engine": 1} for m in at_vars)
            at_post = server.snapshot(run_id, vars_sseq + 1)
            assert not any(m.get("type") == "dropped" for m in at_post)

            await sock.close()
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_zero_flushed_frame_commit_without_finish_clamps_prestart_seq_to_stream_head(tmp_path: Path) -> None:
    """When zero stamped frames follow `variables` (no finish/no-result either), prestart_seq must clamp
    to S_vars <= server._stream_seq so reconnecting at synced.stream_seq does not replay `dropped`."""
    root = _simple_config(tmp_path)
    limits = Limits(log_lines_per_source=5, log_line_bytes=120, problems=0)

    def on_start(run: _GatedFakeRun, sink) -> None:
        async def drive() -> None:
            oversized = {"event": "log", "seq": 0, "t_ms": 1, "run": "r-head", "source": "Engine", "line": "X" * 300}
            sink.on_stdout_event(oversized, _event_size(oversized))
            # Check passes decision, but invalid t_ms yields a bad-event omitted by limits.problems=0 -> 0 flushed frames.
            bad_check = {"event": "check", "seq": 1, "t_ms": -1, "run": "r-head", "check": default_check_result("simple.yaml")}
            sink.on_stdout_event(bad_check, _event_size(bad_check))

        asyncio.create_task(drive())

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        server, test_server, http, client = await _start_gui(root, runtime, limits)
        try:
            st, body = await client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            run_id = body["run_id"]

            head = server._stream_seq
            assert server.current is not None
            assert server.current.session._logs["Engine"].prestart_seq <= head

            # Fresh client sees dropped once and synced at server head.
            fresh = server.snapshot(run_id, None)
            assert sum(1 for m in fresh if m.get("type") == "dropped") == 1
            synced_seq = fresh[-1]["stream_seq"]
            assert synced_seq == head

            # Reconnecting at server head (synced.stream_seq) must NOT replay dropped again.
            at_head = server.snapshot(run_id, synced_seq)
            assert not any(m.get("type") == "dropped" for m in at_head)

            # Reconnecting before S_vars (e.g. at S_sess = head - 1) still catches up with dropped.
            before_vars = server.snapshot(run_id, head - 1)
            assert any(m.get("type") == "dropped" and m["log"] == {"Engine": 1} for m in before_vars)

            await runtime.runs[0].finish(returncode=0)
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_zero_prestart_drops_preserves_live_log_source_bucketing_when_marker_precedes_log(tmp_path: Path) -> None:
    """When prestart_counts is empty, commit must not pre-populate source_order buckets ahead of markers."""
    root = _simple_config(tmp_path)
    limits = Limits(log_sources=2, log_lines_per_source=10)

    # Compare live Session vs staged commit on identical event sequence.
    live = Session("live-1", "simple.yaml", [], None, False, limits=limits)
    events = [
        {"event": "run_start", "seq": 0, "t_ms": 0, "run": "r-b"},
        {"event": "log", "seq": 1, "t_ms": 1, "run": "r-b", "source": "S0", "line": "a"},
        {"event": "log_dropped", "seq": 2, "t_ms": 2, "run": "r-b", "count": 5, "sources": {"S1": 5}},
        {"event": "log", "seq": 3, "t_ms": 3, "run": "r-b", "source": "S2", "line": "b"},
        {"event": "check", "seq": 4, "t_ms": 4, "run": "r-b", "check": default_check_result("simple.yaml")},
    ]
    for ev in events:
        live.feed_event(ev, _event_size(ev))

    def on_start(run: _GatedFakeRun, sink) -> None:
        async def drive() -> None:
            for ev in events:
                sink.on_stdout_event(ev, _event_size(ev))
            run.exit_process(0)
            await run.finish(returncode=0, process_exited=False)

        asyncio.create_task(drive())

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        server, test_server, http, client = await _start_gui(root, runtime, limits)
        try:
            st, _ = await client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            _, sess = await client.get("api/session")
            assert sess["session"]["log"] == live.summary()["log"]
            assert sess["session"]["log"] == {
                "S0": {"kept": 1, "dropped": 0},
                "S1": {"kept": 0, "dropped": 5},
                "*": {"kept": 1, "dropped": 0},
            }
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_oversized_and_multi_source_staged_markers_fold_once_without_double_reporting(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)
    limits = Limits(log_lines_per_source=2, log_line_bytes=120, log_bytes_per_source=4096, log_bytes=8192)

    def on_start(run: _GatedFakeRun, sink) -> None:
        async def drive() -> None:
            run.emit("run_start", run="r-mark")
            # 1) Oversized marker (> log_line_bytes): must fold once into prestart_dropped (never lost or double-counted).
            oversized_marker = {
                "event": "log_dropped", "seq": 1, "t_ms": 1, "run": "r-mark",
                "count": 7, "sources": {"S0": 3, "S1": 4}, "pad": "P" * 200,
            }
            sink.on_stdout_event(oversized_marker, _event_size(oversized_marker))
            # 2) Three normal multi-source markers with log_lines_per_source=2:
            #    m1 is evicted from the single staging marker deque (folds into prestart_dropped for BOTH S0 and S1),
            #    while m2 and m3 survive and are emitted on commit (counted in launcher_dropped).
            run._seq = 2
            run.emit("log_dropped", count=8, sources={"S0": 3, "S1": 5})  # evicted
            run.emit("log_dropped", count=3, sources={"S0": 1, "S1": 2})  # kept
            run.emit("log_dropped", count=5, sources={"S0": 2, "S1": 3})  # kept
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

            dropped_msgs = [m for m in sock.received if m.get("type") == "dropped"]
            assert len(dropped_msgs) == 1
            assert "stream_seq" not in dropped_msgs[0]
            # Oversized (S0:3, S1:4) + evicted m1 (S0:3, S1:5) -> exact prestart dropped.
            assert dropped_msgs[0]["log"] == {"S0": 6, "S1": 9}

            # Only the 2 surviving markers (m2, m3) were emitted as log_dropped events.
            emitted_markers = [
                m["event"]["sources"]
                for m in sock.received
                if m.get("type") == "event" and m["event"].get("event") == "log_dropped"
            ]
            assert emitted_markers == [{"S0": 1, "S1": 2}, {"S0": 2, "S1": 3}]

            # Summary dropped totals = prestart_dropped + launcher_dropped (zero loss, zero double-count).
            _, sess = await client.get("api/session")
            assert sess["session"]["log"]["S0"] == {"kept": 0, "dropped": 6 + 1 + 2}
            assert sess["session"]["log"]["S1"] == {"kept": 0, "dropped": 9 + 2 + 3}

            await sock.close()
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_step1_initial_session_clean_and_one_by_one_state_and_bad_event_isolation(tmp_path: Path) -> None:
    """Step 1 initial session uses clean pre-event state/allowed; staged events dispatch one-by-one,
    interleaving `_push_allowed()` at staged `state` events and isolating per-event `bad-event` errors."""
    root = _simple_config(tmp_path)
    limits = Limits()

    def on_start(run: _GatedFakeRun, sink) -> None:
        async def drive() -> None:
            run.emit("run_start", run="r-step1")
            run.emit("check", check=default_check_result("simple.yaml"))
            # Two staged state events in sequence: each must interleave its own _push_allowed() against
            # its own launcher_state (stop.requests=1 -> already-stopping, then stop.requests=2 -> already-forced).
            run.emit(
                "state",
                rev=1,
                allowed={"pause": {"ok": False, "reason": "ending", "message": "ending"}},
                stop={"requests": 1},
            )
            # Staged event that fails JSON serialization (NaN): must isolate to one bad-event error.
            bad_nan = {"event": "custom_bad", "seq": run._seq, "t_ms": 10, "run": "r-step1", "bad": float("nan")}
            run._seq += 1
            sink.on_stdout_event(bad_nan, 80)
            run.emit(
                "state",
                rev=2,
                allowed={"pause": {"ok": False, "reason": "ending", "message": "ending"}},
                stop={"requests": 2},
            )
            # Subsequent staged event must still dispatch cleanly after the isolated bad-event.
            run.emit("log", source="Engine", line="after-bad-nan")
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
            st, body = await client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            run_id = body["run_id"]
            await sock.until(is_ended)

            run_frames = [
                m for m in sock.received
                if m.get("run_id") == run_id
                or (m.get("type") == "session" and m["session"]["id"] == run_id)
                or (m.get("type") == "allowed" and m["allowed"]["start"]["reason"] == "already-running")
            ]

            # Step 1 initial session frame must be completely clean pre-event state & allowed.
            initial_session = next(m["session"] for m in sock.received if m.get("type") == "session" and m["session"]["id"] == run_id)
            assert initial_session["state"] == "running"
            assert initial_session["launcher"]["running"] is True
            assert initial_session["launcher_state"] is None
            assert initial_session["result"] is None
            assert initial_session["check"] is None
            assert initial_session["log"] == {}
            assert initial_session["allowed"]["start"]["reason"] == "already-running"
            assert initial_session["allowed"]["stop"]["ok"] is True

            # Verify exact interleaving order:
            # state(rev=1) -> allowed(already-stopping) -> bad-event -> state(rev=2) -> allowed(already-forced) -> log.
            idx_state1 = next(
                i for i, m in enumerate(run_frames)
                if m.get("type") == "event" and m["event"].get("event") == "state" and m["event"]["rev"] == 1
            )
            idx_allowed1 = next(
                i for i, m in enumerate(run_frames)
                if m.get("type") == "allowed" and m["allowed"]["stop"]["reason"] == "already-stopping"
                and m["allowed"]["force"]["ok"] is True
            )
            idx_bad = next(
                i for i, m in enumerate(run_frames)
                if m.get("type") == "error" and m.get("code") == "bad-event"
            )
            idx_state2 = next(
                i for i, m in enumerate(run_frames)
                if m.get("type") == "event" and m["event"].get("event") == "state" and m["event"]["rev"] == 2
            )
            idx_allowed2 = next(
                i for i, m in enumerate(run_frames)
                if m.get("type") == "allowed" and m["allowed"]["force"]["reason"] == "already-forced"
            )
            idx_log = next(
                i for i, m in enumerate(run_frames)
                if m.get("type") == "event" and m["event"].get("event") == "log" and m["event"]["line"] == "after-bad-nan"
            )
            assert idx_state1 < idx_allowed1 < idx_bad < idx_state2 < idx_allowed2 < idx_log

            # Monotonic stream_seq across all stamped frames of the run.
            seqs = [m["stream_seq"] for m in run_frames if "stream_seq" in m]
            assert seqs == sorted(seqs) and len(seqs) == len(set(seqs))

            await sock.close()
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_duplicate_seq_after_dropped_prestart_log_emits_seq_order(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)
    limits = Limits(log_lines_per_source=5, log_line_bytes=120, problems=10)

    def on_start(run: _GatedFakeRun, sink) -> None:
        async def drive() -> None:
            run.emit("run_start", run="r-dup")
            # Oversized log at seq=1 is dropped immediately while staged.
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
            # Trailing oversized log at seq=3 is dropped while finish is also staged.
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


def test_run_thread_reclaimable_and_race_first_semantics() -> None:
    async def main() -> None:
        loop = asyncio.get_running_loop()
        reclaimed: list[str] = []
        entered = asyncio.Event()
        release = threading.Event()
        reclaimed_ev = asyncio.Event()

        def slow_produce() -> str:
            loop.call_soon_threadsafe(entered.set)
            release.wait(timeout=5.0)
            return "late-resource"

        def cleanup(val: str) -> None:
            reclaimed.append(val)
            reclaimed_ev.set()

        task = asyncio.create_task(process.run_thread_reclaimable(slow_produce, cleanup))
        await asyncio.wait_for(entered.wait(), timeout=5.0)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        release.set()
        await asyncio.wait_for(reclaimed_ev.wait(), timeout=5.0)
        assert reclaimed == ["late-resource"]

        # Late worker exception after cancellation must be retrieved without leaking warnings.
        unhandled: list[dict] = []
        orig_handler = loop.get_exception_handler()
        loop.set_exception_handler(lambda _l, ctx: unhandled.append(ctx))
        try:
            err_entered = asyncio.Event()
            err_release = threading.Event()
            err_done = asyncio.Event()

            def slow_fail() -> str:
                loop.call_soon_threadsafe(err_entered.set)
                err_release.wait(timeout=5.0)
                try:
                    raise RuntimeError("late worker failure")
                finally:
                    loop.call_soon_threadsafe(err_done.set)

            t_err = asyncio.create_task(process.run_thread_reclaimable(slow_fail, reclaimed.append))
            await asyncio.wait_for(err_entered.wait(), timeout=5.0)
            t_err.cancel()
            with pytest.raises(asyncio.CancelledError):
                await t_err
            err_release.set()
            await asyncio.wait_for(err_done.wait(), timeout=5.0)
            await asyncio.sleep(0)
            assert reclaimed == ["late-resource"]
            assert unhandled == []
        finally:
            loop.set_exception_handler(orig_handler)

        # race_first: returns winner index/value and cancels loser.
        loser_cancelled = asyncio.Event()

        async def fast() -> str:
            return "win"

        async def slow() -> str:
            try:
                await asyncio.sleep(10)
                return "lose"
            except asyncio.CancelledError:
                loser_cancelled.set()
                raise

        assert await process.race_first(fast(), slow()) == (0, "win")
        assert loser_cancelled.is_set()

        # race_first: empty call or timeout returns None and cancels all tasks.
        assert await process.race_first() is None
        assert await process.race_first(asyncio.sleep(10), asyncio.sleep(10), timeout=0.01) is None

        # race_first: propagates a single exception (not ExceptionGroup) by default, swallows when propagate=False.
        async def boom() -> None:
            raise ValueError("single-boom")

        with pytest.raises(ValueError, match="single-boom"):
            await process.race_first(boom(), asyncio.sleep(10))
        assert await process.race_first(boom(), asyncio.sleep(10), propagate=False) == (0, None)

    asyncio.run(main())
