"""Deterministic tests for subprocess reaping, resident cleanup shielding, and shutdown draining."""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

import pytest

from vehicle_sil_gui import control, process, runtime as runtime_mod
from vehicle_sil_gui.launcher import Launcher
from vehicle_sil_gui.refusal import Refused
from vehicle_sil_gui.runtime import ConfigInspection, HotDecision, RunSpec, ServeRuntime, _Resident


def _sleep_proc_argv() -> list[str]:
    return [sys.executable, "-c", "import time; time.sleep(60)"]


def _transport_closing(proc: asyncio.subprocess.Process) -> bool:
    transport = getattr(proc, "_transport", None)
    assert transport is not None
    return bool(transport.is_closing())


def test_start_once_closing_reaps_killed_process_and_closes_pipes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    req = tmp_path / "req.json"
    req.write_text("{}", encoding="utf-8")

    async def main() -> None:
        rt = ServeRuntime(Launcher(Path("vehicle_sil_launch")), cwd=tmp_path)
        spawned: list[asyncio.subprocess.Process] = []
        orig_start = process.start

        async def start_then_close(_argv, cwd=None, stdin=True):
            proc = await orig_start(_sleep_proc_argv(), cwd=cwd, stdin=stdin)
            spawned.append(proc)
            rt._closing = True
            return proc

        monkeypatch.setattr(process, "start", start_then_close)

        with pytest.raises(Refused) as exc_info:
            await rt._start_once(RunSpec(config="a.yaml", request=req))
        assert exc_info.value.code == "shutting-down"
        assert len(spawned) == 1
        assert spawned[0].returncode is not None
        assert _transport_closing(spawned[0]) is True

    asyncio.run(main())


def test_spawn_resident_closing_and_exception_reap_process_and_close_pipes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    async def main() -> None:
        orig_start = process.start
        orig_creds = control.credentials_file

        # Case A: server starts closing right after process.start() returns in _spawn_resident.
        rt1 = ServeRuntime(Launcher(Path("vehicle_sil_launch")), cwd=tmp_path)
        spawned1: list[asyncio.subprocess.Process] = []
        creds1: list[Path] = []

        def track_creds1(*args, **kwargs):
            path, tok = orig_creds(*args, **kwargs)
            creds1.append(path)
            return path, tok

        async def start_then_close(_argv, cwd=None, stdin=True):
            proc = await orig_start(_sleep_proc_argv(), cwd=cwd, stdin=stdin)
            spawned1.append(proc)
            rt1._closing = True
            return proc

        monkeypatch.setattr(control, "credentials_file", track_creds1)
        monkeypatch.setattr(process, "start", start_then_close)

        with pytest.raises(Refused) as exc_info:
            await rt1._spawn_resident("a.yaml", want_control=True)
        assert exc_info.value.code == "shutting-down"
        assert len(spawned1) == 1
        assert spawned1[0].returncode is not None
        assert _transport_closing(spawned1[0]) is True
        assert len(creds1) == 1 and not creds1[0].exists()

        # Case B: exception raised in _spawn_resident after process.start() before _Resident is created.
        rt2 = ServeRuntime(Launcher(Path("vehicle_sil_launch")), cwd=tmp_path)
        spawned2: list[asyncio.subprocess.Process] = []
        creds2: list[Path] = []

        def track_creds2(*args, **kwargs):
            path, tok = orig_creds(*args, **kwargs)
            creds2.append(path)
            return path, tok

        async def start_normal(_argv, cwd=None, stdin=True):
            proc = await orig_start(_sleep_proc_argv(), cwd=cwd, stdin=stdin)
            spawned2.append(proc)
            return proc

        def boom_resident(*args, **kwargs):
            raise RuntimeError("boom before resident handoff")

        monkeypatch.setattr(control, "credentials_file", track_creds2)
        monkeypatch.setattr(process, "start", start_normal)
        monkeypatch.setattr(runtime_mod, "_Resident", boom_resident)

        with pytest.raises(RuntimeError, match="boom before resident handoff"):
            await rt2._spawn_resident("a.yaml", want_control=True)
        assert len(spawned2) == 1
        assert spawned2[0].returncode is not None
        assert _transport_closing(spawned2[0]) is True
        assert len(creds2) == 1 and not creds2[0].exists()

    asyncio.run(main())


def test_shutdown_waits_for_cancelled_kill_and_close_cleanup_task(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    req = tmp_path / "req.json"
    req.write_text("{}", encoding="utf-8")

    async def main() -> None:
        rt = ServeRuntime(Launcher(Path("vehicle_sil_launch")), cwd=tmp_path)
        spawned: list[asyncio.subprocess.Process] = []
        orig_start = process.start
        orig_wait_bounded = process.wait_bounded
        wait_entered = asyncio.Event()
        wait_release = asyncio.Event()

        async def start_then_close(_argv, cwd=None, stdin=True):
            proc = await orig_start(_sleep_proc_argv(), cwd=cwd, stdin=stdin)
            spawned.append(proc)
            rt._closing = True
            return proc

        async def gated_wait_bounded(proc: asyncio.subprocess.Process, seconds: float) -> bool:
            wait_entered.set()
            await wait_release.wait()
            return await orig_wait_bounded(proc, seconds)

        monkeypatch.setattr(process, "start", start_then_close)
        monkeypatch.setattr(process, "wait_bounded", gated_wait_bounded)

        start_task = asyncio.create_task(rt._start_once(RunSpec(config="a.yaml", request=req)))
        await asyncio.wait_for(wait_entered.wait(), timeout=5.0)

        # Cancel caller mid-reap: the detached cleanup task stays tracked on rt._cleanup_tasks.
        start_task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await start_task
        assert len(rt._cleanup_tasks) == 1

        # shutdown() must not return until the detached reap task completes and closes pipes.
        shutdown_task = asyncio.create_task(rt.shutdown(asyncio.Event(), 1.0))
        await asyncio.sleep(0)
        assert not shutdown_task.done()

        wait_release.set()
        await asyncio.wait_for(shutdown_task, timeout=5.0)
        assert not rt._cleanup_tasks
        assert len(spawned) == 1
        assert spawned[0].returncode is not None
        assert _transport_closing(spawned[0]) is True

    asyncio.run(main())


def test_prepare_fallback_close_resident_survives_cancellation(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    async def main() -> None:
        rt = ServeRuntime(Launcher(Path("vehicle_sil_launch")), cwd=tmp_path, escalate_wait_s=0.1)
        proc = await process.start(_sleep_proc_argv(), cwd=tmp_path)
        resident = _Resident(config="a.yaml", want_control=False, proc=proc, credentials=None, token=None)
        rt._residents.add(resident)
        rt._resident = resident

        async def fake_inspect(*args, **kwargs):
            return ConfigInspection(hot=HotDecision(True)), None

        async def fake_ensure(*args, **kwargs):
            return resident

        async def fake_send_prepare(*args, **kwargs):
            return None  # triggers parsed is None fallback path

        entered = asyncio.Event()
        release = asyncio.Event()
        finished = asyncio.Event()
        orig_close = rt._close_resident

        async def gated_close(res, hurry, stop_timeout_s):
            entered.set()
            await release.wait()
            try:
                await orig_close(res, hurry, stop_timeout_s)
            finally:
                finished.set()

        monkeypatch.setattr(rt, "_inspect_unlocked", fake_inspect)
        monkeypatch.setattr(rt, "_ensure_resident", fake_ensure)
        monkeypatch.setattr(rt, "_send_prepare", fake_send_prepare)
        monkeypatch.setattr(rt, "_close_resident", gated_close)

        task = asyncio.create_task(rt.prepare("a.yaml", want_control=False))
        await asyncio.wait_for(entered.wait(), timeout=5.0)
        assert resident.ending is True
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

        release.set()
        await asyncio.wait_for(finished.wait(), timeout=5.0)
        assert proc.returncode is not None
        assert _transport_closing(proc) is True
        assert rt._resident is None
        assert resident not in rt._residents

    asyncio.run(main())


def test_start_run_fallback_close_resident_survives_cancellation(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    req = tmp_path / "req.json"
    req.write_text("{}", encoding="utf-8")

    async def main() -> None:
        rt = ServeRuntime(Launcher(Path("vehicle_sil_launch")), cwd=tmp_path, escalate_wait_s=0.1)
        proc = await process.start(_sleep_proc_argv(), cwd=tmp_path)
        resident = _Resident(config="a.yaml", want_control=False, proc=proc, credentials=None, token=None)
        rt._residents.add(resident)
        rt._resident = resident

        async def fake_ensure(*args, **kwargs):
            return resident

        async def fake_start_on_resident(*args, **kwargs):
            return None, "refused"

        entered = asyncio.Event()
        release = asyncio.Event()
        finished = asyncio.Event()
        orig_close = rt._close_resident

        async def gated_close(res, hurry, stop_timeout_s):
            entered.set()
            await release.wait()
            try:
                await orig_close(res, hurry, stop_timeout_s)
            finally:
                finished.set()

        monkeypatch.setattr(rt, "_ensure_resident", fake_ensure)
        monkeypatch.setattr(rt, "_start_run_on_resident", fake_start_on_resident)
        monkeypatch.setattr(rt, "_close_resident", gated_close)

        spec = RunSpec(config="a.yaml", request=req, hot_decision=HotDecision(True))
        task = asyncio.create_task(rt.start_run(spec))
        await asyncio.wait_for(entered.wait(), timeout=5.0)
        assert resident.ending is True
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

        release.set()
        await asyncio.wait_for(finished.wait(), timeout=5.0)
        assert proc.returncode is not None
        assert _transport_closing(proc) is True
        assert rt._resident is None
        assert resident not in rt._residents

    asyncio.run(main())
