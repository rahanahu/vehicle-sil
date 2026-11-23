"""In-process fake LauncherRuntime and TestServer harness for deterministic GUI server tests."""

from __future__ import annotations

import asyncio
import json
from collections.abc import AsyncIterator, Awaitable, Callable, Sequence
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from pathlib import Path

import aiohttp
from aiohttp.test_utils import TestServer
from conftest import CONFIGS, Client, Gui, REPLAY

from vehicle_sil_gui import __version__
from vehicle_sil_gui.launcher import Launcher, REQUIRED_FEATURES
from vehicle_sil_gui.runtime import DEFAULT_ESCALATE_WAIT_S, RunSink, RunSpec
from vehicle_sil_gui.server import GuiServer, Options

DEFAULT_FEATURES = (
    *REQUIRED_FEATURES,
    "timeline",
    "run-stop-at",
    "control-links",
    "control-server",
)


class FakeLauncher(Launcher):
    """Launcher stand-in whose probe() succeeds in-memory without starting a subprocess."""

    def __init__(self, features: Sequence[str] = DEFAULT_FEATURES, path: Path = REPLAY):
        super().__init__(path=path)
        self.usable = True
        self.problems = []
        self.version = {
            "vehicle_sil_launch": 1,
            "vehicle_sil": __version__,
            "formats": {"check": 1, "run": 2, "events": 2, "describe": 1, "test": 1,
                        "timeline": 1, "connections": 1},
            "features": list(features),
        }

    async def probe(self) -> None:
        return


def default_check_result(config: str = "simple.yaml",
                         participants: Sequence[str] = ("Engine",)) -> dict:
    """Minimal valid `check --json` result that allows a run to start."""
    return {
        "vehicle_sil_check": 1,
        "vehicle_sil": __version__,
        "file": config,
        "result": "passed",
        "strict": False,
        "errors": 0,
        "warnings": 0,
        "notes": 0,
        "stop_at_ms": 1000,
        "time_step_ms": 10,
        "start_timeout_ms": 30000,
        "stop_timeout_ms": 10000,
        "registry_uri": None,
        "commands": None,
        "participants": [
            {"name": name, "enabled": True, "manual": False, "variables": []}
            for name in participants
        ],
        "networks": [],
        "findings": [],
    }


class FakeRun:
    """In-memory RunHandle driven directly by tests."""

    def __init__(self, spec: RunSpec, *, pid: int, on_done: Callable[[FakeRun], None]):
        self.spec = spec
        self._pid = pid
        self._on_done = on_done
        self.sink: RunSink | None = None
        self.started = asyncio.Event()
        self.finished = asyncio.Event()
        self.stops: list[int] = []
        self.stop_event = asyncio.Event()
        self.time_commands: list[dict] = []
        self.input_open = True
        self.drain_ok = True
        self.drain_gate: asyncio.Event | None = None
        self.shutdown_requested = asyncio.Event()
        self.escalated = False
        self.killed = False
        self._process_exited = False
        self._seq = 0
        self._t_ms = 0

    @property
    def pid(self) -> int | None:
        return self._pid

    def start(self, sink: RunSink) -> None:
        self.sink = sink
        self.started.set()

    def can_send(self) -> bool:
        return self.input_open

    def send_stop(self, count: int) -> bool:
        if not self.input_open:
            return False
        self.stops.append(count)
        self.stop_event.set()
        return True

    def send_time(self, command: dict) -> bool:
        if not self.input_open:
            return False
        self.time_commands.append(dict(command))
        return True

    async def drain(self, timeout_s: float) -> bool:
        if self.drain_gate is not None:
            try:
                async with asyncio.timeout(timeout_s):
                    await self.drain_gate.wait()
            except TimeoutError:
                return True
        return self.drain_ok

    def emit(self, event: str, **fields) -> dict:
        """Emits one launcher event JSON line to the server sink."""
        assert self.sink is not None
        payload = {"event": event, "seq": self._seq, "t_ms": self._t_ms, **fields}
        self._seq += 1
        self._t_ms += 10
        self.sink.on_stdout_line(json.dumps(payload))
        return payload

    def emit_raw(self, line: str | None) -> None:
        assert self.sink is not None
        self.sink.on_stdout_line(line)

    def emit_stderr(self, line: str) -> None:
        assert self.sink is not None
        self.sink.on_stderr_line(line)

    def emit_problem(self, code: str, message: str, detail: object = None) -> None:
        assert self.sink is not None
        self.sink.on_problem(code, message, detail)

    def exit_process(self, returncode: int | None = 0) -> None:
        assert self.sink is not None
        if not self._process_exited:
            self._process_exited = True
            self.input_open = False
            self.sink.on_process_exited(returncode)

    async def finish(self, *, returncode: int | None = 0, output_cut: bool = False,
                     process_exited: bool = True) -> None:
        """Ends the run cleanly or as a crash, optionally keeping the process alive."""
        if self.finished.is_set():
            return
        assert self.sink is not None
        if process_exited:
            self.exit_process(returncode)
        await self.sink.on_run_finished(output_cut=output_cut)
        await self.sink.on_run_cleanup()
        self.finished.set()
        self._on_done(self)


class FakeRuntime:
    """In-memory LauncherRuntime that records starts, stops, shutdown escalation, and kill_all."""

    def __init__(self, *, escalate_wait_s: float = DEFAULT_ESCALATE_WAIT_S):
        self.runs: list[FakeRun] = []
        self._active: set[FakeRun] = set()
        self._next_pid = 10000
        self.start_gate: asyncio.Event | None = None
        self.start_entered = asyncio.Event()
        self.start_error: BaseException | None = None
        self.shutdown_auto_finish = True
        self.escalate_wait_s = escalate_wait_s
        self.ignore_kill = False
        self.kill_all_called = 0

    async def start_run(self, spec: RunSpec) -> FakeRun:
        self.start_entered.set()
        if self.start_gate is not None:
            await self.start_gate.wait()
        if self.start_error is not None:
            raise self.start_error
        self._next_pid += 1
        run = FakeRun(spec, pid=self._next_pid, on_done=self._active.discard)
        self.runs.append(run)
        self._active.add(run)
        return run

    async def shutdown(self, hurry: asyncio.Event, stop_timeout_s: float | None) -> None:
        await asyncio.gather(*(self._stop_run(run, hurry, stop_timeout_s) for run in list(self._active)))

    async def _stop_run(self, run: FakeRun, hurry: asyncio.Event, stop_timeout_s: float | None) -> None:
        run.shutdown_requested.set()
        run.input_open = False
        if self.shutdown_auto_finish:
            await run.finish(returncode=0)
            return
        if not await _wait_run_or_hurry(run, hurry, stop_timeout_s):
            why = ("shutdown was requested again" if hurry.is_set() or stop_timeout_s is None else
                   f"the launcher did not stop in {stop_timeout_s:g} s after its input closed")
            run.escalated = True
            if run.sink is not None:
                run.sink.on_problem("stop-escalated", f"{why}: the launcher was asked to stop "
                                    "(SIGTERM / Ctrl+Break)", None)
            if not await _wait_run_or_hurry(run, hurry, self.escalate_wait_s) and not self.ignore_kill:
                run.killed = True
                await run.finish(returncode=-9)

    def kill_all(self) -> None:
        self.kill_all_called += 1

    def active_pids(self) -> list[int]:
        return [run.pid for run in list(self._active) if run.pid is not None]


async def _wait_run_or_hurry(run: FakeRun, hurry: asyncio.Event, seconds: float | None) -> bool:
    if run.finished.is_set():
        return True
    hurry_task = asyncio.create_task(hurry.wait())
    done_task = asyncio.create_task(run.finished.wait())
    try:
        await asyncio.wait({hurry_task, done_task}, timeout=seconds, return_when=asyncio.FIRST_COMPLETED)
        return run.finished.is_set()
    finally:
        for task in (hurry_task, done_task):
            if not task.done():
                task.cancel()
        await asyncio.gather(hurry_task, done_task, return_exceptions=True)


@dataclass
class InProcessGui:
    server: GuiServer
    runtime: FakeRuntime
    client: Client
    port: int
    token: str


@asynccontextmanager
async def in_process_gui(
    root: Path = CONFIGS,
    *,
    runtime: FakeRuntime | None = None,
    features: Sequence[str] = DEFAULT_FEATURES,
    check_fn: Callable[[list[str], Path, Path | None, bytes | None], Awaitable[tuple[int, dict]]] | None = None,
    variables: bool | None = None,
    source: Callable[[str, str], object] | None = None,
    stop_timeout_s: float | None = 1.0,
    silent_after_s: float = 5.0,
    token: str = "test-token",
) -> AsyncIterator[InProcessGui]:
    """Starts GuiServer over aiohttp TestServer with an in-memory LauncherRuntime."""
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]

    fake_runtime = runtime or FakeRuntime()
    use_variables = (source is not None) if variables is None else variables
    server = GuiServer(Options(
        root=root,
        launcher=FakeLauncher(features=features),
        port=port,
        token=token,
        stop_timeout_s=stop_timeout_s,
        silent_after_s=silent_after_s,
        variables=use_variables,
        source=source,
        runtime=fake_runtime,
    ))

    async def fake_check(argv: list[str], request: Path, descriptions: Path | None = None,
                         text: bytes | None = None) -> tuple[int, dict]:
        if check_fn is not None:
            return await check_fn(argv, request, descriptions, text)
        return 0, default_check_result(argv[-1])

    server._launcher_check = fake_check  # type: ignore[method-assign]
    test_server = TestServer(server.app(), host="127.0.0.1", port=port)
    await test_server.start_server()
    http = aiohttp.ClientSession()
    gui = Gui(proc=None, port=port, token=token, url=f"http://127.0.0.1:{port}/#token={token}",
              stderr=root / ".unused.err")
    try:
        yield InProcessGui(server=server, runtime=fake_runtime, client=Client(gui, http),
                           port=port, token=token)
    finally:
        await http.close()
        await server.shutdown()
        await test_server.close()
