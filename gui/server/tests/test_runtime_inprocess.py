"""In-process GuiServer lifecycle tests using aiohttp TestServer and FakeRuntime (zero OS subprocesses)."""

from __future__ import annotations

import asyncio
from pathlib import Path

import pytest
from conftest import CONFIGS, is_ended, is_synced, is_variables
from fake_runtime import FakeRun, FakeRuntime, default_check_result, in_process_gui

from vehicle_sil_gui.refusal import Refused
from vehicle_sil_gui.source import PeerInfo, Sink, SnapshotInfo, TimeInfo


def _simple_config(tmp_path: Path) -> Path:
    root = tmp_path / "root"
    root.mkdir()
    (root / "simple.yaml").write_text("vehicle_sil_simulation: 2\n", encoding="utf-8")
    return root


def _is_problem(code: str):
    return lambda m: m.get("type") == "error" and m.get("code") == code


def test_full_run_lifecycle_and_request_cleanup(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    async def main() -> None:
        async with in_process_gui(root) as app:
            sock = await app.client.ws()
            await sock.until(is_synced)
            status, body = await app.client.post("api/run", {"file": "simple.yaml"})
            assert status == 200 and body["session"]["state"] == "running"
            run = app.runtime.runs[0]
            assert run.started.is_set()
            request_file = run.spec.request
            assert request_file.is_file()

            run.emit("hello", vehicle_sil_events=2, pid=run.pid, file="simple.yaml")
            run.emit("phase", phase="running")
            run.emit("time", sim_ms=500, actual_speed=1.0, late_steps=0, max_late_ms=0, slipped_ms=0, late_ms=0)
            run.emit("state", rev=1, paused=False, running=True, paused_at_ms=None, speed="max", allowed={})
            run.emit("result", result={"vehicle_sil_run": 2, "result": "passed", "end": "completed", "ok": True})
            await run.finish(returncode=0)

            messages = await sock.until(is_ended)
            ended = messages[-1]["session"]
            assert ended["state"] == "ended"
            assert ended["result"]["result"] == "passed"
            assert ended["launcher"]["exit_code"] == 0
            assert not request_file.exists()
            await sock.close()

    asyncio.run(main())


def test_stop_and_force_stop_and_idempotency(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    async def main() -> None:
        async with in_process_gui(root) as app:
            status, _ = await app.client.post("api/run", {"file": "simple.yaml"})
            assert status == 200
            run1 = app.runtime.runs[0]

            s1, _ = await app.client.post("api/stop", {})
            s2, _ = await app.client.post("api/stop", {})  # duplicate non-force stop sends nothing
            s3, _ = await app.client.post("api/stop", {"force": True})
            s4, _ = await app.client.post("api/stop", {"force": True})  # already forced
            assert (s1, s2, s3, s4) == (200, 200, 200, 200)
            assert run1.stops == [1, 1]
            await run1.finish(returncode=0)

            # A fresh run stopped immediately with force=True receives 2 stop commands at once.
            status, _ = await app.client.post("api/run", {"file": "simple.yaml"})
            assert status == 200
            run2 = app.runtime.runs[1]
            status, _ = await app.client.post("api/stop", {"force": True})
            assert status == 200 and run2.stops == [2]
            await run2.finish(returncode=0)

    asyncio.run(main())


def test_stop_not_delivered_when_input_closed(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    async def main() -> None:
        async with in_process_gui(root) as app:
            sock = await app.client.ws()
            await sock.until(is_synced)
            status, _ = await app.client.post("api/run", {"file": "simple.yaml"})
            assert status == 200
            run = app.runtime.runs[0]
            run.input_open = False

            status, body = await app.client.post("api/stop", {})
            assert status == 200 and body["session"]["stop"] == "requested"
            await sock.until(_is_problem("stop-not-delivered"))
            await run.finish(returncode=1)
            await sock.close()

    asyncio.run(main())


def test_stop_while_starting_delivers_before_first_event_and_stop_session_rejects(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    async def main() -> None:
        runtime = FakeRuntime()
        runtime.start_gate = asyncio.Event()
        async with in_process_gui(root, runtime=runtime) as app:
            start_task = asyncio.create_task(app.client.post("api/run", {"file": "simple.yaml"}))
            await runtime.start_entered.wait()

            # api/stop-session during startup is rejected with 409 session-changed and writes no stop.
            st_targeted, err_targeted = await app.client.post(
                "api/stop-session", {"session": 1, "instance": app.server.instance},
            )
            assert st_targeted == 409 and err_targeted["error"]["code"] == "session-changed"

            # Plain api/stop during startup queues stop on start.
            st_plain, plain_body = await app.client.post("api/stop", {})
            assert st_plain == 200 and plain_body == {"session": None}

            runtime.start_gate.set()
            st_run, run_body = await start_task
            assert st_run == 200 and run_body["session"]["stop"] == "requested"
            run = runtime.runs[0]
            assert run.stops == [1]

            # Wrong session / wrong instance on api/stop-session is rejected after start too.
            st_wrong_sid, err_wrong_sid = await app.client.post(
                "api/stop-session", {"session": 999, "instance": app.server.instance},
            )
            st_wrong_inst, err_wrong_inst = await app.client.post(
                "api/stop-session", {"session": run_body["session"]["id"], "instance": "wrong"},
            )
            assert st_wrong_sid == 409 and err_wrong_sid["error"]["code"] == "session-changed"
            assert st_wrong_inst == 409 and err_wrong_inst["error"]["code"] == "session-changed"
            assert run.stops == [1]

            await run.finish(returncode=0)

    asyncio.run(main())


def test_time_commands_order_and_input_errors(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    async def main() -> None:
        async with in_process_gui(root) as app:
            # No run active -> 409 not-running.
            st, err = await app.client.post("api/time", {"command": "pause"})
            assert st == 409 and err["error"]["code"] == "not-running"

            st, run_body = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            sid = run_body["session"]["id"]
            run = app.runtime.runs[0]

            cmds = [
                {"command": "pause", "id": 1},
                {"command": "step", "ms": 20, "id": 2},
                {"command": "resume", "id": 3},
                {"command": "speed", "value": 2.0, "id": 4},
                {"command": "run_until", "at_ms": 500, "id": 5},
            ]
            for c in cmds:
                st, _ = await app.client.post("api/time", c)
                assert st == 200
            assert run.time_commands == cmds

            # Targeted time command for wrong session/instance -> 409 session-changed.
            st, err = await app.client.post("api/time", {
                "command": "pause", "session": sid + 1, "instance": app.server.instance,
            })
            assert st == 409 and err["error"]["code"] == "session-changed"

            # Drain failure -> 409 not-running.
            run.drain_ok = False
            st, err = await app.client.post("api/time", {"command": "pause"})
            assert st == 409 and err["error"]["code"] == "not-running"

            # Closed input -> 409 not-running.
            run.drain_ok = True
            run.input_open = False
            st, err = await app.client.post("api/time", {"command": "pause"})
            assert st == 409 and err["error"]["code"] == "not-running"

            await run.finish(returncode=0)

    asyncio.run(main())


def test_concurrent_run_conflicts_and_start_failure_cleanup(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    async def main() -> None:
        runtime = FakeRuntime()
        runtime.start_gate = asyncio.Event()
        async with in_process_gui(root, runtime=runtime) as app:
            # First run blocked inside start_run -> second run gets 409 starting.
            t1 = asyncio.create_task(app.client.post("api/run", {"file": "simple.yaml"}))
            await runtime.start_entered.wait()
            st_dup, err_dup = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st_dup == 409 and err_dup["error"]["code"] == "starting"

            # Fail start_run with Refused(502, "launcher-start") -> server recovers cleanly for next run.
            runtime.start_error = Refused(502, "launcher-start", "cannot start fake launcher")
            runtime.start_gate.set()
            st_fail, err_fail = await t1
            assert st_fail == 502 and err_fail["error"]["code"] == "launcher-start"
            assert not runtime.runs

            # Next start succeeds -> another run while active gets 409 already-running.
            runtime.start_error = None
            runtime.start_gate = None
            st_ok, _ = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st_ok == 200
            st_busy, err_busy = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st_busy == 409 and err_busy["error"]["code"] == "already-running"
            await runtime.runs[0].finish(returncode=0)

    asyncio.run(main())


def test_line_too_long_stderr_hiding_and_crash_problems(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    async def main() -> None:
        async with in_process_gui(root) as app:
            sock = await app.client.ws()
            await sock.until(is_synced)
            status, _ = await app.client.post("api/run", {"file": "simple.yaml", "stop_at_ms": 2500})
            assert status == 200
            run = app.runtime.runs[0]

            run.emit_raw(None)  # triggers line-too-long + preflight_timing fallback before `check` event
            msg = (await sock.until(_is_problem("line-too-long")))[-1]
            assert msg["code"] == "line-too-long"
            run.emit_stderr(f"failed near {run.spec.request}, line 7")
            await run.finish(returncode=2, output_cut=True)

            await sock.until(is_ended)
            _, sess = await app.client.get("api/session")
            session = sess["session"]
            codes = [p["code"] for p in session["problems"]]
            assert codes == ["line-too-long", "launcher-stderr", "output-left-open", "no-result"]
            stderr_problem = next(p for p in session["problems"] if p["code"] == "launcher-stderr")
            assert str(run.spec.request) not in stderr_problem["message"]
            assert "the request" in stderr_problem["message"]
            assert session["stop_at_ms"] == 2500
            await sock.close()

    asyncio.run(main())


class _FakeSource:
    def __init__(self) -> None:
        self.closed = False
        self._sink: Sink | None = None

    def start(self, sink: Sink) -> None:
        self._sink = sink
        sink.on_peers([PeerInfo(name="Engine", incarnation="1", connected=True, variables=[
            {"name": "speed", "type": "real", "role": "output", "unit": "rpm",
             "min": None, "max": None, "step": None, "description": "", "enum": [], "availability": None},
        ])], [])
        sink.on_time(TimeInfo(now_ns=0, paused=False, running=True, ended=False))
        sink.on_synced()

    async def read_peers(self, timeout_s: float) -> list[PeerInfo]:
        return []

    async def read_snapshot(self, peers: list[str], timeout_s: float) -> SnapshotInfo:
        return SnapshotInfo(TimeInfo(0, False, True, False), {p: None for p in peers}, False)

    async def read_operation(self, seq: int, timeout_s: float):
        return None

    async def write(self, writes, at_ns, late, timeout_s: float) -> int:
        return 1

    async def close(self) -> None:
        self.closed = True


def test_run_finish_without_process_exit_closes_variables(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)
    sources: list[_FakeSource] = []

    def make_source(address: str, token: str) -> _FakeSource:
        src = _FakeSource()
        sources.append(src)
        return src

    async def main() -> None:
        async with in_process_gui(root, source=make_source) as app:
            sock = await app.client.ws()
            await sock.until(is_synced)
            status, _ = await app.client.post("api/run", {"file": "simple.yaml"})
            assert status == 200
            run = app.runtime.runs[0]
            creds = run.spec.credentials
            assert creds is not None and creds.is_file()

            run.emit("control_server", address="127.0.0.1:50051", tls=False)
            await sock.until(is_variables("ready"))
            assert not creds.exists()
            assert len(sources) == 1 and not sources[0].closed

            # Ending the run window without exiting the process closes Variables and ends Session.
            await run.finish(process_exited=False)
            await sock.until(is_variables("ended"))
            await sock.until(is_ended)
            assert sources[0].closed
            assert app.server.session is not None and app.server.session.state == "ended"
            await sock.close()

    asyncio.run(main())


def test_shutdown_hurry_escalation_and_kill_all(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    async def main() -> None:
        # 1) shutdown while a start_run is in flight waits for start_run then stops it, while rejecting new starts.
        rt1 = FakeRuntime()
        rt1.start_gate = asyncio.Event()
        async with in_process_gui(root, runtime=rt1, stop_timeout_s=0.2) as app:
            start_task = asyncio.create_task(app.client.post("api/run", {"file": "simple.yaml"}))
            await rt1.start_entered.wait()
            shutdown_task = asyncio.create_task(app.server.shutdown())
            await asyncio.sleep(0.02)
            rt1.start_gate.set()
            st, body = await start_task
            await shutdown_task
            assert st == 503 and body["error"]["code"] == "shutting-down"
            assert len(rt1.runs) == 1 and rt1.runs[0].shutdown_requested.is_set()
            assert rt1.runs[0].finished.is_set()

        # 2) unresponsive run escalates on second shutdown() (`hurry`) and kill_all delegates to runtime.
        rt2 = FakeRuntime(escalate_wait_s=0.05)
        rt2.shutdown_auto_finish = False
        async with in_process_gui(root, runtime=rt2, stop_timeout_s=5.0) as app:
            sock = await app.client.ws()
            await sock.until(is_synced)
            st, _ = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            run = rt2.runs[0]

            first = asyncio.create_task(app.server.shutdown())
            await run.shutdown_requested.wait()
            assert not run.escalated
            await app.server.shutdown()  # sets hurry
            await first
            assert run.escalated and run.killed and run.finished.is_set()
            app.server.kill_all()
            assert rt2.kill_all_called == 1
            await sock.close()

    asyncio.run(main())


class _GatedFakeRun(FakeRun):
    """FakeRun that uses GuiServer's _PreStartGate, with a hook run inside start(sink)."""

    waits_for_check = True

    def __init__(self, spec, *, pid: int, on_done, on_start):
        super().__init__(spec, pid=pid, on_done=on_done)
        self._on_start = on_start

    def start(self, sink) -> None:
        super().start(sink)
        self._on_start(self)


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


def test_prestart_gate_buffered_exit_does_not_push_intermediate_allowed(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    def on_start(run: _GatedFakeRun) -> None:
        run.emit("hello", vehicle_sil_events=2, pid=run.pid, file="simple.yaml")
        run.emit("check", check=default_check_result("simple.yaml"))
        run.emit("log", source="Dashboard", line="before crash")
        run.emit_stderr("dying on purpose")
        # Launcher exits while _PreStartGate is still buffering, before on_run_finished runs.
        run.exit_process(70)

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        async with in_process_gui(root, runtime=runtime) as app:
            sock = await app.client.ws()
            await sock.until(is_synced)

            st, body = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            assert body["session"]["state"] == "running"
            assert body["session"]["allowed"]["rev"] == 3

            await runtime.runs[0].finish(returncode=70, process_exited=False)
            await sock.until(is_ended)

            allowed_frames = [m for m in sock.received if m["type"] == "allowed"]
            assert [(m["allowed"]["rev"], m["allowed"]["start"]["reason"]) for m in allowed_frames] == [
                (2, "starting"),
            ]
            sessions = [m["session"] for m in sock.received if m["type"] == "session"]
            assert [(s["state"], s["allowed"]["rev"]) for s in sessions] == [("running", 3), ("ended", 5)]
            assert sessions[-1]["allowed"]["start"]["ok"] is True
            assert sessions[-1]["allowed"]["stop"]["reason"] == "not-running"
            assert sessions[-1]["allowed"]["force"]["reason"] == "not-running"
            await sock.close()

    asyncio.run(main())


def test_prestart_gate_buffered_finish_preserves_ended_allowed_and_allows_next_run(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    def on_start(run: _GatedFakeRun) -> None:
        async def drive() -> None:
            run.emit("hello", vehicle_sil_events=2, pid=run.pid, file="simple.yaml")
            run.emit("check", check=default_check_result("simple.yaml"))
            run.emit_stderr("dying immediately")
            run.exit_process(70)
            # While gate.live is False, on_run_finished and on_run_cleanup buffer without yielding,
            # so finish + cleanup land in the gate buffer before _start() wakes from gate.decision.wait().
            await run.finish(returncode=70, process_exited=False)

        asyncio.create_task(drive())

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        async with in_process_gui(root, runtime=runtime) as app:
            sock = await app.client.ws()
            await sock.until(is_synced)

            st, body = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            assert body["session"]["state"] == "running"

            await sock.until(is_ended)

            # Start a second run and wait for it to end as a deterministic WS FIFO fence: any
            # trailing frame from run 1 would arrive before run 2's frames without needing a sleep.
            st2, body2 = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st2 == 200 and body2["session"]["id"] == 2
            await sock.until(lambda m: m.get("type") == "session" and m["session"]["id"] == 2
                             and m["session"]["state"] == "ended")

            allowed_frames = [m for m in sock.received if m["type"] == "allowed"]
            assert [(m["allowed"]["rev"], m["allowed"]["start"]["reason"]) for m in allowed_frames] == [
                (2, "starting"),
                (6, "starting"),
            ]
            sessions = [m["session"] for m in sock.received if m["type"] == "session"]
            assert [(s["id"], s["state"], s["allowed"]["rev"]) for s in sessions] == [
                (1, "running", 3),
                (1, "ended", 5),
                (2, "running", 7),
                (2, "ended", 9),
            ]
            for ended_session in (sessions[1], sessions[3]):
                ended_allowed = ended_session["allowed"]
                assert ended_allowed["start"]["ok"] is True
                assert ended_allowed["stop"]["reason"] == "not-running"
                assert ended_allowed["force"]["reason"] == "not-running"
            await sock.close()

    asyncio.run(main())


def test_shutdown_waits_for_prestart_gate_buffered_finish_and_cleanup(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    def on_start(run: _GatedFakeRun) -> None:
        async def drive() -> None:
            run.emit("hello", vehicle_sil_events=2, pid=run.pid, file="simple.yaml")
            run.emit("check", check=default_check_result("simple.yaml"))
            run.exit_process(70)
            await run.finish(returncode=70, process_exited=False)

        asyncio.create_task(drive())

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        async with in_process_gui(root, runtime=runtime) as app:
            entered = asyncio.Event()
            release = asyncio.Event()
            orig_close_variables = app.server._close_variables

            async def gated_close_variables(run) -> None:
                entered.set()
                await release.wait()
                await orig_close_variables(run)

            app.server._close_variables = gated_close_variables  # type: ignore[method-assign]

            start_task = asyncio.create_task(app.client.post("api/run", {"file": "simple.yaml"}))
            await entered.wait()

            # The handle already finished into gate.buffer, so runtime.active_pids() is empty;
            # shutdown() must still wait via _started until post_run() finishes draining and cleanup.
            assert runtime.active_pids() == []
            assert not app.server._started.is_set()
            shutdown_task = asyncio.create_task(app.server.shutdown())
            for _ in range(5):
                await asyncio.sleep(0)
            assert not shutdown_task.done()
            assert not app.server.hub.closed

            release.set()
            st, body = await start_task
            await shutdown_task
            assert st == 503 and body["error"]["code"] == "shutting-down"
            assert app.server._started.is_set()
            assert app.server._starts_in_flight == 0
            assert not app.server._runs

    asyncio.run(main())


def test_prestart_gate_buffers_and_replays_parsed_events(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    def on_start(run: _GatedFakeRun) -> None:
        run.emit_event("hello", vehicle_sil_events=2, pid=run.pid, file="simple.yaml")
        run.emit_event("check", check=default_check_result("simple.yaml"))
        run.emit_event("log", source="Engine", line="via event")

    async def main() -> None:
        runtime = _GatedFakeRuntime(on_start)
        async with in_process_gui(root, runtime=runtime) as app:
            sock = await app.client.ws()
            await sock.until(is_synced)
            st, body = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st == 200 and body["session"]["state"] == "running"
            run = runtime.runs[0]
            run.emit_event("result", result={"vehicle_sil_run": 2, "result": "passed", "end": "completed", "ok": True})
            await run.finish(returncode=0)
            msgs = await sock.until(is_ended)
            assert any(m["type"] == "event" and m["event"]["event"] == "log" and m["event"]["line"] == "via event"
                       for m in msgs)
            assert msgs[-1]["session"]["result"]["result"] == "passed"
            await sock.close()

    asyncio.run(main())
