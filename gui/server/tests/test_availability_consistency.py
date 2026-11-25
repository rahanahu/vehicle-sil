"""Deterministic tests for stop, force, and can_send availability consistency."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from types import SimpleNamespace

from conftest import is_ended, is_synced
from fake_runtime import FakeRuntime, in_process_gui

from vehicle_sil_gui.launcher import Launcher
from vehicle_sil_gui.runtime import ServeRunHandle, ServeRuntime, _Resident


def _simple_config(tmp_path: Path) -> Path:
    root = tmp_path / "root"
    root.mkdir()
    (root / "simple.yaml").write_text("vehicle_sil_simulation: 2\n", encoding="utf-8")
    return root


def test_starting_reports_starting_for_stop_and_force_consistent_with_stop_session(tmp_path: Path) -> None:
    root = _simple_config(tmp_path)

    async def main() -> None:
        runtime = FakeRuntime()
        runtime.start_gate = asyncio.Event()
        async with in_process_gui(root, runtime=runtime) as app:
            sock = await app.client.ws()
            await sock.until(is_synced)

            start_task = asyncio.create_task(app.client.post("api/run", {"file": "simple.yaml"}))
            await asyncio.wait_for(runtime.start_entered.wait(), timeout=5.0)

            pushed = await sock.until(
                lambda m: m.get("type") == "allowed" and m["allowed"]["start"]["reason"] == "starting",
            )
            ws_allowed = pushed[-1]["allowed"]
            assert ws_allowed["start"]["ok"] is False and ws_allowed["start"]["reason"] == "starting"
            assert ws_allowed["stop"]["ok"] is False and ws_allowed["stop"]["reason"] == "starting"
            assert ws_allowed["force"]["ok"] is False and ws_allowed["force"]["reason"] == "starting"

            st_sess, sess = await app.client.get("api/session")
            assert st_sess == 200
            assert sess["allowed"] == ws_allowed

            # POST api/stop-session in this same state refuses with 409 session-changed.
            st_stop, err_stop = await app.client.post(
                "api/stop-session",
                {"run_id": f"{app.server.instance}-1", "instance": app.server.instance, "force": False},
            )
            assert st_stop == 409 and err_stop["error"]["code"] == "session-changed"

            runtime.start_gate.set()
            st_run, _ = await start_task
            assert st_run == 200

            st_sess2, sess2 = await app.client.get("api/session")
            assert st_sess2 == 200
            assert sess2["allowed"]["start"]["reason"] == "already-running"
            assert sess2["allowed"]["stop"]["ok"] is True
            assert sess2["allowed"]["force"]["ok"] is True

            await runtime.runs[0].finish(returncode=0)
            await sock.until(is_ended)
            await sock.close()

    asyncio.run(main())


def test_cold_mode_projects_external_stop_skips_redundant_writes_and_preserves_post_result_stop(
    tmp_path: Path,
) -> None:
    root = _simple_config(tmp_path)

    async def main() -> None:
        async with in_process_gui(root) as app:
            sock = await app.client.ws()
            await sock.until(is_synced)

            st, started = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st == 200
            target = {"run_id": started["run_id"], "instance": started["instance"]}
            run = app.runtime.runs[0]
            assert app.server.session is not None and app.server.session.launcher_mode == "cold"

            run.emit("run_start", run="r1")
            run.emit("phase", phase="running")
            run.emit(
                "state",
                rev=1,
                phase="running",
                paused=False,
                running=True,
                speed="max",
                stop={"requests": 0, "by": None},
                allowed={},
            )
            _, sess0 = await app.client.get("api/session")
            assert sess0["allowed"]["stop"]["ok"] is True
            assert sess0["allowed"]["force"]["ok"] is True

            # External graceful stop (e.g. Control Server / signal) -> allowed.stop becomes already-stopping and pushes;
            # a graceful POST api/stop-session writes nothing to stdin, while force=True still writes a forced stop.
            run.emit(
                "state",
                rev=2,
                phase="stopping",
                paused=False,
                running=True,
                speed="max",
                stop={"requests": 1, "by": "control_server"},
                allowed={},
            )
            pushed1 = await sock.until(
                lambda m: m.get("type") == "allowed" and m["allowed"]["stop"]["reason"] == "already-stopping",
            )
            assert pushed1[-1]["allowed"]["force"]["ok"] is True
            _, sess1 = await app.client.get("api/session")
            assert sess1["allowed"]["stop"]["reason"] == "already-stopping"
            assert sess1["allowed"]["force"]["ok"] is True

            st_graceful, _ = await app.client.post("api/stop-session", {**target, "force": False})
            assert st_graceful == 200
            assert run.stops == []

            # External forced stop (requests == 2) -> allowed.force becomes already-forced and pushes;
            # a forced POST api/stop-session writes nothing to stdin either.
            run.emit(
                "state",
                rev=3,
                phase="stopping",
                paused=False,
                running=True,
                speed="max",
                stop={"requests": 2, "by": "signal"},
                allowed={},
            )
            pushed2 = await sock.until(
                lambda m: m.get("type") == "allowed" and m["allowed"]["force"]["reason"] == "already-forced",
            )
            assert pushed2[-1]["allowed"]["stop"]["reason"] == "already-stopping"
            st_forced, _ = await app.client.post("api/stop-session", {**target, "force": True})
            assert st_forced == 200
            assert run.stops == []
            await run.finish(returncode=0)
            await sock.until(is_ended)

            # Second run: external graceful stop (requests == 1) still allows escalating via force=True.
            st_esc, started_esc = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st_esc == 200
            run_esc = app.runtime.runs[1]
            run_esc.emit("run_start", run="r-esc")
            run_esc.emit(
                "state",
                rev=1,
                phase="stopping",
                paused=False,
                running=True,
                speed="max",
                stop={"requests": 1, "by": "control_server"},
                allowed={},
            )
            st_esc_force, _ = await app.client.post(
                "api/stop-session",
                {"run_id": started_esc["run_id"], "instance": started_esc["instance"], "force": True},
            )
            assert st_esc_force == 200
            assert run_esc.stops == [2]
            await run_esc.finish(returncode=0)
            await sock.until(
                lambda m: m.get("type") == "session"
                and m["session"]["id"] == started_esc["run_id"]
                and m["session"]["state"] == "ended",
            )

            # Third run: Cold launcher emits result (requests == 0) but stays running -> still stoppable & forceable.
            st2, started2 = await app.client.post("api/run", {"file": "simple.yaml"})
            assert st2 == 200
            run2 = app.runtime.runs[2]
            run2.emit("run_start", run="r2")
            run2.emit(
                "state",
                rev=1,
                phase="ended",
                paused=False,
                running=False,
                speed="max",
                stop={"requests": 0, "by": None},
                allowed={},
            )
            run2.emit("result", result={"vehicle_sil_run": 2, "result": "passed", "end": "completed", "ok": True})
            _, sess_post = await app.client.get("api/session")
            assert sess_post["allowed"]["stop"]["ok"] is True
            assert sess_post["allowed"]["force"]["ok"] is True

            st_stop, _ = await app.client.post(
                "api/stop-session", {"run_id": started2["run_id"], "instance": started2["instance"], "force": False},
            )
            assert st_stop == 200
            assert run2.stops == [1]
            _, sess_after_stop = await app.client.get("api/session")
            assert sess_after_stop["allowed"]["stop"]["reason"] == "already-stopping"
            assert sess_after_stop["allowed"]["force"]["ok"] is True

            await run2.finish(returncode=0)
            await sock.until(
                lambda m: m.get("type") == "session"
                and m["session"]["id"] == started2["run_id"]
                and m["session"]["state"] == "ended",
            )
            await sock.close()

    asyncio.run(main())


def test_hot_can_send_turns_false_immediately_on_result_while_cold_stays_true(tmp_path: Path) -> None:
    class _FakeStdin:
        def __init__(self) -> None:
            self.writes: list[bytes] = []

        def is_closing(self) -> bool:
            return False

        def write(self, data: bytes) -> None:
            self.writes.append(data)

    async def main() -> None:
        rt = ServeRuntime(Launcher(Path("vehicle_sil_launch")), cwd=tmp_path)

        # Hot resident: result clears resident.current_run immediately, so can_send() turns False BEFORE _drain() runs.
        hot_stdin = _FakeStdin()
        hot_proc = SimpleNamespace(returncode=None, pid=1111, stdin=hot_stdin, stdout=None, stderr=None)
        hot_res = _Resident(
            config="a.yaml", want_control=False, proc=hot_proc, credentials=None, token=None, once=False,  # type: ignore[arg-type]
        )
        hot_res.hello = {"event": "hello", "vehicle_sil_events": 4, "launcher": "inst-hot"}
        hot_handle = ServeRunHandle(hot_res, command_id=1, on_finished=lambda _: None)
        hot_handle._run_id = "run-hot-1"
        hot_res.current_run = hot_handle

        assert hot_handle.can_send() is True
        result_line = json.dumps({
            "event": "result",
            "seq": 5,
            "t_ms": 100,
            "run": "run-hot-1",
            "result": {"vehicle_sil_run": 2, "result": "passed", "ok": True},
            "status": {
                "instance": "inst-hot",
                "status_rev": 2,
                "launcher_state": "ready",
                "config": "a.yaml",
                "eligibility": {"eligible": True},
                "launcher_allowed": {"run": {"ok": True}},
                "active_run_id": None,
                "last_run_id": "run-hot-1",
                "run_state": None,
            },
            "launcher_ends": False,
        })
        rt._handle_stdout_line(hot_res, result_line)
        # _drain() has NOT run yet (hot_handle._ended is still False), yet can_send() is immediately False.
        assert hot_handle._ended is False
        assert hot_res.current_run is None
        assert hot_handle.can_send() is False
        assert hot_handle.send_time({"command": "pause"}) is False
        assert hot_handle.send_stop(1, force=False) is False
        assert hot_stdin.writes == []

        # Cold single-run resident (once=True): keeps current_run after result until process exit, so Stop still works.
        cold_stdin = _FakeStdin()
        cold_proc = SimpleNamespace(returncode=None, pid=2222, stdin=cold_stdin, stdout=None, stderr=None)
        cold_res = _Resident(
            config="a.yaml", want_control=False, proc=cold_proc, credentials=None, token=None, once=True,  # type: ignore[arg-type]
        )
        cold_res.hello = {"event": "hello", "vehicle_sil_events": 4, "launcher": "inst-cold"}
        cold_handle = ServeRunHandle(cold_res, command_id=1, on_finished=lambda _: None)
        cold_handle._run_id = "run-cold-1"
        cold_res.current_run = cold_handle

        cold_result_line = json.dumps({
            "event": "result",
            "seq": 5,
            "t_ms": 100,
            "run": "run-cold-1",
            "result": {"vehicle_sil_run": 2, "result": "passed", "ok": True},
            "launcher_ends": True,
        })
        rt._handle_stdout_line(cold_res, cold_result_line)
        assert cold_res.current_run is cold_handle
        assert cold_handle.can_send() is True
        assert cold_handle.send_stop(1, force=False) is True
        assert len(cold_stdin.writes) == 1

    asyncio.run(main())
