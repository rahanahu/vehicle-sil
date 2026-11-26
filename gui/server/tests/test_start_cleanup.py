"""Deterministic pre-handoff resource cleanup tests for RunManager._start()."""

from __future__ import annotations

import asyncio
import json
import threading
from pathlib import Path

import aiohttp
import pytest
from aiohttp.test_utils import make_mocked_request
from fake_runtime import FakeRun, FakeRuntime, in_process_gui

from vehicle_sil_gui import config_service as config_service_mod, control


def _config_with_scenario(tmp_path: Path) -> Path:
    root = tmp_path / "root"
    (root / "scenarios").mkdir(parents=True)
    (root / "simple.yaml").write_text("vehicle_sil_simulation: 2\n", encoding="utf-8")
    (root / "scenarios" / "smoke.yaml").write_text(
        "vehicle_sil_scenario: 1\nname: smoke\nsteps: []\n",
        encoding="utf-8",
    )
    return root


class _DummySource:
    def start(self, sink) -> None:
        return

    async def close(self) -> None:
        return


def _dummy_source(_address: str, _token: str) -> _DummySource:
    return _DummySource()


def _post_run_request(body: dict) -> aiohttp.web.BaseRequest:
    raw = json.dumps(body).encode("utf-8")
    payload = aiohttp.StreamReader(aiohttp.base_protocol.BaseProtocol(asyncio.get_running_loop()), 2**16, loop=asyncio.get_running_loop())
    payload.feed_data(raw)
    payload.feed_eof()
    return make_mocked_request(
        "POST",
        "/api/run",
        headers={"Content-Type": "application/json", "Content-Length": str(len(raw))},
        payload=payload,
    )


def _track_credentials(monkeypatch: pytest.MonkeyPatch, created: list[Path]) -> None:
    orig = control.credentials_file

    def tracked(*args, **kwargs):
        path, token = orig(*args, **kwargs)
        created.append(path)
        return path, token

    monkeypatch.setattr(control, "credentials_file", tracked)


def test_cancel_during_timeline_prep_cleans_credentials_and_allows_next_start(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    root = _config_with_scenario(tmp_path)
    created_creds: list[Path] = []
    _track_credentials(monkeypatch, created_creds)

    async def main() -> None:
        async with in_process_gui(root, source=_dummy_source) as app:
            loop = asyncio.get_running_loop()
            entered = asyncio.Event()
            release = threading.Event()
            worker_done = asyncio.Event()
            orig_new_file = app.server.runs.timelines.new_file
            calls = 0

            def gated_new_file() -> Path:
                nonlocal calls
                calls += 1
                if calls == 1:
                    loop.call_soon_threadsafe(entered.set)
                    release.wait(timeout=5.0)
                    try:
                        return orig_new_file()
                    finally:
                        loop.call_soon_threadsafe(worker_done.set)
                return orig_new_file()

            monkeypatch.setattr(app.server.runs.timelines, "new_file", gated_new_file)

            body = {"file": "simple.yaml", "scenario": "scenarios/smoke.yaml"}
            start_task = asyncio.create_task(app.server.post_run(_post_run_request(body)))
            await asyncio.wait_for(entered.wait(), timeout=5.0)

            assert len(created_creds) == 1
            assert created_creds[0].is_file()
            assert app.server.runs._starting is True
            assert app.server.runs._starts_in_flight == 1
            assert not app.server.runs._started.is_set()

            start_task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await start_task

            release.set()
            await asyncio.wait_for(worker_done.wait(), timeout=5.0)

            assert not created_creds[0].exists()
            assert not app.server.runs._starting
            assert app.server.runs._starts_in_flight == 0
            assert app.server.runs._started.is_set()
            assert not app.server.runs._runs
            assert app.server.runs.allowed()["start"]["ok"] is True

            # Subsequent Start through HTTP succeeds cleanly.
            status, started = await app.client.post("api/run", body)
            assert status == 200
            assert started["run_id"] == f"{app.server.instance}-1"
            assert len(created_creds) == 2
            await app.runtime.runs[0].finish(returncode=0)
            assert not created_creds[1].exists()

    asyncio.run(main())


def test_exception_during_timeline_prep_cleans_credentials_and_allows_next_start(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    root = _config_with_scenario(tmp_path)
    created_creds: list[Path] = []
    _track_credentials(monkeypatch, created_creds)

    async def main() -> None:
        async with in_process_gui(root, source=_dummy_source) as app:
            orig_new_file = app.server.runs.timelines.new_file
            calls = 0

            def failing_once() -> Path:
                nonlocal calls
                calls += 1
                if calls == 1:
                    raise OSError("disk full")
                return orig_new_file()

            monkeypatch.setattr(app.server.runs.timelines, "new_file", failing_once)

            body = {"file": "simple.yaml", "scenario": "scenarios/smoke.yaml"}
            status, err = await app.client.post("api/run", body)
            assert status == 500
            assert err["error"]["code"] == "internal"
            assert len(created_creds) == 1
            assert not created_creds[0].exists()
            assert not app.server.runs._starting
            assert app.server.runs._starts_in_flight == 0
            assert not app.server.runs._runs
            assert app.server.runs.allowed()["start"]["ok"] is True

            # Subsequent Start succeeds cleanly.
            status, started = await app.client.post("api/run", body)
            assert status == 200
            assert started["run_id"] == f"{app.server.instance}-1"
            assert len(created_creds) == 2
            await app.runtime.runs[0].finish(returncode=0)
            assert not created_creds[1].exists()

    asyncio.run(main())


def test_cancel_during_credentials_creation_cleans_file_and_allows_next_start(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    root = _config_with_scenario(tmp_path)
    orig_creds = control.credentials_file
    orig_remove = control.remove
    created_creds: list[Path] = []

    async def main() -> None:
        async with in_process_gui(root, source=_dummy_source) as app:
            loop = asyncio.get_running_loop()
            entered = asyncio.Event()
            release = threading.Event()
            first_removed = asyncio.Event()
            calls = 0

            def gated_creds(*args, **kwargs):
                nonlocal calls
                calls += 1
                path, token = orig_creds(*args, **kwargs)
                created_creds.append(path)
                if calls == 1:
                    loop.call_soon_threadsafe(entered.set)
                    release.wait(timeout=5.0)
                return path, token

            def tracked_remove(path: Path | None) -> bool:
                result = orig_remove(path)
                if created_creds and path == created_creds[0]:
                    first_removed.set()
                return result

            monkeypatch.setattr(control, "credentials_file", gated_creds)
            monkeypatch.setattr(control, "remove", tracked_remove)

            body = {"file": "simple.yaml", "scenario": "scenarios/smoke.yaml"}
            start_task = asyncio.create_task(app.server.post_run(_post_run_request(body)))
            await asyncio.wait_for(entered.wait(), timeout=5.0)
            assert len(created_creds) == 1
            assert created_creds[0].is_file()
            assert app.server.runs._starting is True

            start_task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await start_task

            release.set()
            await asyncio.wait_for(first_removed.wait(), timeout=5.0)

            assert not created_creds[0].exists()
            assert not app.server.runs._starting
            assert app.server.runs._starts_in_flight == 0
            assert app.server.runs._started.is_set()
            assert app.server.runs.allowed()["start"]["ok"] is True

            status, started = await app.client.post("api/run", body)
            assert status == 200
            assert started["run_id"] == f"{app.server.instance}-1"
            assert len(created_creds) == 2
            await app.runtime.runs[0].finish(returncode=0)
            assert not created_creds[1].exists()

    asyncio.run(main())


def test_cancel_and_exception_during_start_run_clean_all_prehandoff_files(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    root = _config_with_scenario(tmp_path)
    created_creds: list[Path] = []
    created_requests: list[Path] = []
    _track_credentials(monkeypatch, created_creds)

    orig_write_request = config_service_mod.write_request_file

    def tracked_request(intent: dict) -> Path:
        path = orig_write_request(intent)
        created_requests.append(path)
        return path

    monkeypatch.setattr(config_service_mod, "write_request_file", tracked_request)

    async def main() -> None:
        runtime = FakeRuntime()
        runtime.start_gate = asyncio.Event()
        async with in_process_gui(root, runtime=runtime, source=_dummy_source) as app:
            body = {"file": "simple.yaml", "scenario": "scenarios/smoke.yaml"}

            # 1) Cancellation while awaiting runtime.start_run()
            start_task = asyncio.create_task(app.server.post_run(_post_run_request(body)))
            await asyncio.wait_for(runtime.start_entered.wait(), timeout=5.0)
            assert len(created_creds) == 1 and created_creds[0].is_file()
            assert len(created_requests) == 1 and created_requests[0].is_file()
            assert app.server.runs._starting is True

            start_task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await start_task

            assert not created_creds[0].exists()
            assert not created_requests[0].exists()
            assert not app.server.runs._starting
            assert app.server.runs._starts_in_flight == 0
            assert app.server.runs._started.is_set()
            assert not app.server.runs._runs

            # 2) Ordinary exception raised by runtime.start_run()
            runtime.start_gate = None
            runtime.start_error = RuntimeError("boom in start_run")
            status, err = await app.client.post("api/run", body)
            assert status == 500
            assert err["error"]["code"] == "internal"
            assert len(created_creds) == 2 and not created_creds[1].exists()
            assert len(created_requests) == 2 and not created_requests[1].exists()
            assert not app.server.runs._starting
            assert not app.server.runs._runs

            # 3) Subsequent Start succeeds cleanly.
            runtime.start_error = None
            status, started = await app.client.post("api/run", body)
            assert status == 200
            assert started["run_id"] == f"{app.server.instance}-1"
            assert len(created_creds) == 3
            assert len(created_requests) == 3
            await runtime.runs[0].finish(returncode=0)
            assert not created_creds[2].exists()
            assert not created_requests[2].exists()

    asyncio.run(main())


def test_exception_in_handle_start_cleans_prehandoff_resources_and_stops_handle(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    root = _config_with_scenario(tmp_path)
    created_creds: list[Path] = []
    created_requests: list[Path] = []
    _track_credentials(monkeypatch, created_creds)

    orig_write_request = config_service_mod.write_request_file

    def tracked_request(intent: dict) -> Path:
        path = orig_write_request(intent)
        created_requests.append(path)
        return path

    monkeypatch.setattr(config_service_mod, "write_request_file", tracked_request)

    class _FailOnceStartRun(FakeRun):
        def __init__(self, spec, *, pid: int, on_done, fail_start: bool):
            super().__init__(spec, pid=pid, on_done=on_done)
            self._fail_start = fail_start
            self.aborted = False

        def abort(self) -> None:
            self.aborted = True

        def start(self, sink) -> None:
            if self._fail_start:
                raise RuntimeError("sink handoff failed")
            super().start(sink)

    class _FailOnceRuntime(FakeRuntime):
        def __init__(self) -> None:
            super().__init__()
            self._calls = 0

        async def start_run(self, spec):
            self._calls += 1
            self._next_pid += 1
            run = _FailOnceStartRun(
                spec, pid=self._next_pid, on_done=self._active.discard, fail_start=(self._calls == 1),
            )
            self.runs.append(run)
            if self._calls != 1:
                self._active.add(run)
            return run

    async def main() -> None:
        runtime = _FailOnceRuntime()
        async with in_process_gui(root, runtime=runtime, source=_dummy_source) as app:
            body = {"file": "simple.yaml", "scenario": "scenarios/smoke.yaml"}

            status, err = await app.client.post("api/run", body)
            assert status == 500
            assert err["error"]["code"] == "internal"
            failed_run = runtime.runs[0]
            assert failed_run.stops == ["force"]
            assert failed_run.aborted is True
            assert len(created_creds) == 1 and not created_creds[0].exists()
            assert len(created_requests) == 1 and not created_requests[0].exists()
            assert not app.server.runs._runs
            assert not app.server.runs._starting

            status, started = await app.client.post("api/run", body)
            assert status == 200
            assert started["run_id"] == f"{app.server.instance}-1"
            await runtime.runs[1].finish(returncode=0)
            assert not created_creds[1].exists()
            assert not created_requests[1].exists()

    asyncio.run(main())


def test_cancel_before_timelines_lock_then_shutdown_leaks_no_temp_dir(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    root = _config_with_scenario(tmp_path)
    created_creds: list[Path] = []
    _track_credentials(monkeypatch, created_creds)

    async def main() -> None:
        async with in_process_gui(root, source=_dummy_source) as app:
            loop = asyncio.get_running_loop()
            entered = asyncio.Event()
            release = threading.Event()
            worker_done = asyncio.Event()
            worker_errors: list[BaseException] = []
            orig_new_file = app.server.runs.timelines.new_file

            def gated_new_file() -> Path:
                loop.call_soon_threadsafe(entered.set)
                release.wait(timeout=5.0)
                try:
                    return orig_new_file()
                except BaseException as exc:
                    worker_errors.append(exc)
                    raise
                finally:
                    loop.call_soon_threadsafe(worker_done.set)

            monkeypatch.setattr(app.server.runs.timelines, "new_file", gated_new_file)

            body = {"file": "simple.yaml", "scenario": "scenarios/smoke.yaml"}
            start_task = asyncio.create_task(app.server.post_run(_post_run_request(body)))
            await asyncio.wait_for(entered.wait(), timeout=5.0)
            assert len(created_creds) == 1 and created_creds[0].is_file()

            # Cancel before the worker thread acquires Timelines._lock, then shut the server down.
            start_task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await start_task

            assert not created_creds[0].exists()
            await app.server.shutdown()
            assert app.server.runs.timelines.directory is None

            # Wake the late worker thread after remove_all() has run: it must refuse to create a temp dir.
            release.set()
            await asyncio.wait_for(worker_done.wait(), timeout=5.0)
            assert app.server.runs.timelines.directory is None
            assert len(worker_errors) == 1 and isinstance(worker_errors[0], RuntimeError)

    asyncio.run(main())
