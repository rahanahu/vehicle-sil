"""Minimal launcher supervision boundary for starting, controlling, and shutting down runs."""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Callable
from dataclasses import dataclass, field
import json
import logging
import os
from pathlib import Path
from typing import Protocol
import yaml

from . import control, process
from .launcher import (
    FEATURE_SERVE,
    Launcher,
    command_line,
    run_command,
    serve_command,
    serve_run_command,
    stop_command_line,
    with_participant_timeouts,
)
from .refusal import Refused

log = logging.getLogger("vehicle_sil_gui")

STDERR_MAX_LINE = 64 * 1024
DEFAULT_MAX_LINE = 16 * 1024 * 1024
DEFAULT_ESCALATE_WAIT_S = 5.0
DEFAULT_FINISH_TIMEOUT_S = process.OUTPUT_GRACE_S + 1 + 3.0
IDLE_WAIT_S = 5.0
RESIDENT_READY_TIMEOUT_S = 10.0
RUN_START_TIMEOUT_S = 10.0
_YAML_MAX_BYTES = 1024 * 1024


@dataclass(frozen=True)
class RunSpec:
    """Parameters needed to start one run in a runtime."""

    config: str
    request: Path
    credentials: Path | None = None
    timeline: Path | None = None
    variant: str | None = None
    disable: tuple[str, ...] = ()
    enable: tuple[str, ...] = ()
    manual: tuple[str, ...] = ()


@dataclass(frozen=True)
class HotDecision:
    """Whether a run or configuration is eligible for persistent serve execution."""

    hot: bool
    reason: str | None = None
    message: str | None = None


class RunSink(Protocol):
    """Normalized run lifecycle callbacks delivered by a runtime to the GUI server."""

    def on_stdout_line(self, line: str | None) -> None: ...

    def on_stderr_line(self, line: str) -> None: ...

    def on_problem(self, code: str, message: str, detail: object = None) -> None: ...

    def on_process_exited(self, returncode: int | None) -> None: ...

    async def on_run_finished(self, *, output_cut: bool = False, launcher_alive: bool = False) -> None: ...

    async def on_run_cleanup(self) -> None: ...


class RunHandle(Protocol):
    """Control handle for one started run."""

    @property
    def pid(self) -> int | None: ...

    @property
    def run_id(self) -> str | None: ...

    @property
    def mode(self) -> str: ...

    @property
    def control_token(self) -> str | None: ...

    def start(self, sink: RunSink) -> None: ...

    def can_send(self) -> bool: ...

    def send_stop(self, count: int) -> bool: ...

    def send_time(self, command: dict) -> bool: ...

    async def drain(self, timeout_s: float) -> bool: ...


class LauncherRuntime(Protocol):
    """Runtime boundary supervising run processes on behalf of GuiServer."""

    async def start_run(self, spec: RunSpec) -> RunHandle: ...

    async def shutdown(self, hurry: asyncio.Event, stop_timeout_s: float | None) -> None: ...

    def kill_all(self) -> None: ...

    def active_pids(self) -> list[int]: ...

    def sync_launcher(self) -> None: ...

    def status(self) -> dict: ...

    async def prepare(self, config: str, *, variant: str | None = None, disable: tuple[str, ...] = (),
                      enable: tuple[str, ...] = (), manual: tuple[str, ...] = (),
                      want_control: bool = True) -> dict: ...

    async def release_config(self, config: str) -> None: ...


_UNSUPPORTED_MSG = "this launcher was built without serve mode: running each simulation in its own launcher"


def check_hot_eligibility(
    launcher: Launcher,
    root: Path,
    config: str,
    *,
    variant: str | None = None,
    disable: tuple[str, ...] = (),
    enable: tuple[str, ...] = (),
    manual: tuple[str, ...] = (),
) -> HotDecision:
    """Classifies whether a configuration run can reuse a persistent `serve` launcher."""
    if FEATURE_SERVE not in launcher.features:
        return HotDecision(
            False,
            "launcher-unsupported",
            _UNSUPPORTED_MSG,
        )
    if os.environ.get("VEHICLE_SIL_TRACE"):
        return HotDecision(
            False,
            "trace-enabled",
            "VEHICLE_SIL_TRACE is set: persistent serve refuses tracing so trace buffers do not accumulate",
        )
    if manual:
        return HotDecision(
            False,
            "manual-participants",
            "manual participants were requested: running each simulation in its own launcher",
        )
    blocker = _yaml_hot_blocker(root / config, variant=variant, disable=disable, enable=enable, manual=manual)
    if blocker is not None:
        return blocker
    return HotDecision(True)


def _yaml_hot_blocker(
    path: Path,
    *,
    variant: str | None,
    disable: tuple[str, ...],
    enable: tuple[str, ...],
    manual: tuple[str, ...],
) -> HotDecision | None:
    """Checks YAML config for fixed registry_uri or enabled manual participants."""
    try:
        with path.open("rb") as f:
            raw = f.read(_YAML_MAX_BYTES + 1)
        if len(raw) > _YAML_MAX_BYTES:
            return None
        doc = yaml.safe_load(raw.decode("utf-8"))
    except (OSError, UnicodeDecodeError, yaml.YAMLError):
        return None
    if not isinstance(doc, dict):
        return None
    registry_uri = doc.get("registry_uri")
    if isinstance(registry_uri, str) and registry_uri.strip():
        return HotDecision(
            False,
            "fixed-registry",
            "the configuration sets a fixed registry_uri: running each simulation in its own launcher",
        )
    variant_participants: dict[str, dict] = {}
    if variant is not None and isinstance(doc.get("variants"), dict):
        var_cfg = doc["variants"].get(variant)
        if isinstance(var_cfg, dict) and isinstance(var_cfg.get("participants"), dict):
            variant_participants = {
                k: v for k, v in var_cfg["participants"].items() if isinstance(k, str) and isinstance(v, dict)
            }
    disabled_set = set(disable)
    enabled_set = set(enable)
    manual_set = set(manual)
    raw_participants = doc.get("participants")
    items: list[tuple[str, dict]] = []
    if isinstance(raw_participants, list):
        for entry in raw_participants:
            if isinstance(entry, dict) and isinstance(entry.get("name"), str):
                items.append((entry["name"], entry))
    elif isinstance(raw_participants, dict):
        for name, entry in raw_participants.items():
            if isinstance(name, str) and isinstance(entry, dict):
                items.append((name, entry))
    for name, entry in items:
        override = variant_participants.get(name, {})
        enabled = entry.get("enabled", True)
        if isinstance(override.get("enabled"), bool):
            enabled = override["enabled"]
        if name in disabled_set:
            enabled = False
        elif name in enabled_set:
            enabled = True
        required = entry.get("required", True)
        if isinstance(override.get("required"), bool):
            required = override["required"]
        is_manual = entry.get("manual", False) is True
        if isinstance(override.get("manual"), bool):
            is_manual = override["manual"]
        if name in manual_set:
            is_manual = True
        if enabled is True and required is not False and is_manual:
            return HotDecision(
                False,
                "manual-participants",
                f"participant '{name}' is manual: running each simulation in its own launcher",
            )
    return None


class OneShotRunHandle:
    """RunHandle backed by a single `vehicle_sil_launch run` child process."""

    def __init__(self, proc: asyncio.subprocess.Process, *, max_line: int,
                 on_done: Callable[[OneShotRunHandle], None]):
        self._proc = proc
        self._max_line = max_line
        self._on_done = on_done
        self._sink: RunSink | None = None
        self._task: asyncio.Task | None = None

    @property
    def pid(self) -> int | None:
        return self._proc.pid

    @property
    def run_id(self) -> str | None:
        return None

    @property
    def mode(self) -> str:
        return "cold"

    @property
    def control_token(self) -> str | None:
        return None

    def start(self, sink: RunSink) -> None:
        self._sink = sink
        self._task = asyncio.create_task(self._follow(sink))

    def can_send(self) -> bool:
        return self._proc.stdin is not None and not self._proc.stdin.is_closing()

    def send_stop(self, count: int) -> bool:
        if not self.can_send():
            return False
        try:
            self._proc.stdin.write(b'{"command":"stop"}\n' * count)
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            return False
        return True

    def send_time(self, command: dict) -> bool:
        if not self.can_send():
            return False
        try:
            self._proc.stdin.write(command_line(command))
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            return False
        return True

    def abort(self) -> None:
        process.kill_tree(self._proc)

    async def drain(self, timeout_s: float) -> bool:
        if self._proc.stdin is None:
            return False
        try:
            async with asyncio.timeout(timeout_s):
                await self._proc.stdin.drain()
        except TimeoutError:
            return True
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            return False
        return True

    async def _follow(self, sink: RunSink) -> None:
        """Reads the launcher's output until exit (not pipe EOF), then finishes after grace."""
        reader = asyncio.create_task(self._read_output(sink))
        try:
            code = await process.wait_exit(self._proc)
            process.kill_tree(self._proc)  # what the launcher left in its group (POSIX)
            sink.on_process_exited(code)
            done, _ = await asyncio.wait({reader}, timeout=process.OUTPUT_GRACE_S)
            reader.cancel()
            await asyncio.gather(reader, return_exceptions=True)
            process.close_pipes(self._proc)
            await sink.on_run_finished(output_cut=not done)
        finally:
            reader.cancel()
            await asyncio.gather(reader, return_exceptions=True)
            process.close_pipes(self._proc)
            await sink.on_run_cleanup()
            self._on_done(self)

    async def _read_output(self, sink: RunSink) -> None:
        assert self._proc.stdout is not None and self._proc.stderr is not None

        async def stdout() -> None:
            async for line in process.lines(self._proc.stdout, self._max_line):
                sink.on_stdout_line(line)

        async def stderr() -> None:
            async for line in process.lines(self._proc.stderr, STDERR_MAX_LINE):
                if line:
                    sink.on_stderr_line(line)

        await asyncio.gather(stdout(), stderr(), return_exceptions=True)


class OneShotRuntime:
    """Supervises one child process per run (`vehicle_sil_launch run --events --stdin-commands`)."""

    def __init__(self, launcher: Launcher, *, cwd: Path, participant_start_timeout_ms: int = 30000,
                 participant_stop_timeout_ms: int = 10000, max_line: int = DEFAULT_MAX_LINE,
                 escalate_wait_s: float = DEFAULT_ESCALATE_WAIT_S,
                 finish_timeout_s: float = DEFAULT_FINISH_TIMEOUT_S):
        self._launcher = launcher
        self._cwd = cwd
        self._participant_start_timeout_ms = participant_start_timeout_ms
        self._participant_stop_timeout_ms = participant_stop_timeout_ms
        self._max_line = max_line
        self._escalate_wait_s = escalate_wait_s
        self._finish_timeout_s = finish_timeout_s
        self._active: set[OneShotRunHandle] = set()

    async def start_run(self, spec: RunSpec) -> OneShotRunHandle:
        argv = with_participant_timeouts(
            run_command(self._launcher, spec.config, spec.request, spec.credentials, spec.timeline),
            self._participant_start_timeout_ms,
            self._participant_stop_timeout_ms,
        )
        try:
            proc = await process.start(argv, cwd=self._cwd)
        except OSError as e:
            raise Refused(502, "launcher-start", f"cannot start {self._launcher.path}: {e}") from None
        handle = OneShotRunHandle(proc, max_line=self._max_line, on_done=self._active.discard)
        self._active.add(handle)
        return handle

    async def shutdown(self, hurry: asyncio.Event, stop_timeout_s: float | None) -> None:
        await asyncio.gather(*(self._stop_handle(handle, hurry, stop_timeout_s) for handle in list(self._active)))

    async def _stop_handle(self, handle: OneShotRunHandle, hurry: asyncio.Event,
                           stop_timeout_s: float | None) -> None:
        proc = handle._proc
        if proc.returncode is None:
            if proc.stdin is not None:
                proc.stdin.close()
            if not await process.wait_exit_or_hurry(proc, hurry, stop_timeout_s):
                why = ("shutdown was requested again" if hurry.is_set() or stop_timeout_s is None else
                       f"the launcher did not stop in {stop_timeout_s:g} s after its input closed")
                if handle._sink is not None:
                    handle._sink.on_problem("stop-escalated", f"{why}: the launcher was asked to stop "
                                            "(SIGTERM / Ctrl+Break)", None)
                process.ask_to_stop(proc)
                if not await process.wait_exit_or_hurry(proc, hurry, self._escalate_wait_s):
                    process.kill_tree(proc)
                    await process.wait_bounded(proc, process.KILL_WAIT_S)
        if handle._task is not None:
            await asyncio.wait({handle._task}, timeout=self._finish_timeout_s)
        else:
            process.close_pipes(proc)
            if proc.returncode is not None:
                self._active.discard(handle)

    def kill_all(self) -> None:
        for handle in list(self._active):
            process.kill_tree(handle._proc)

    def active_pids(self) -> list[int]:
        return [handle.pid for handle in list(self._active) if handle.pid is not None]

    def sync_launcher(self) -> None:
        return

    def status(self) -> dict:
        return {
            "mode": "cold",
            "state": "running" if self._active else "cold",
            "config": None,
            "reason": "one-shot-runtime",
            "message": None,
        }

    async def prepare(self, config: str, *, variant: str | None = None, disable: tuple[str, ...] = (),
                      enable: tuple[str, ...] = (), manual: tuple[str, ...] = (),
                      want_control: bool = True) -> dict:
        return self.status()

    async def release_config(self, config: str) -> None:
        return


class ServeRunHandle:
    """RunHandle multiplexed onto one persistent `vehicle_sil_launch serve` process."""

    def __init__(self, resident: _Resident, *, command_id: int,
                 on_finished: Callable[[ServeRunHandle], None]):
        self._resident = resident
        self._command_id = command_id
        self._on_finished = on_finished
        self._run_id: str | None = None
        self._started_event = asyncio.Event()
        self._startup_refusal: str | None = None
        self._queue: asyncio.Queue[tuple] = asyncio.Queue()
        self._sink: RunSink | None = None
        self._task: asyncio.Task | None = None
        self._drain_done = asyncio.Event()
        self._ended = False

    @property
    def pid(self) -> int | None:
        return self._resident.proc.pid

    @property
    def run_id(self) -> str | None:
        return self._run_id

    @property
    def mode(self) -> str:
        return "hot"

    @property
    def control_token(self) -> str | None:
        return self._resident.token

    def start(self, sink: RunSink) -> None:
        self._sink = sink
        self._task = asyncio.create_task(self._drain(sink))

    def can_send(self) -> bool:
        proc = self._resident.proc
        return (
            not self._ended
            and self._run_id is not None
            and proc.returncode is None
            and proc.stdin is not None
            and not proc.stdin.is_closing()
        )

    def send_stop(self, count: int) -> bool:
        if not self.can_send():
            return False
        assert self._resident.proc.stdin is not None and self._run_id is not None
        try:
            self._resident.proc.stdin.write(stop_command_line(self._run_id) * count)
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            return False
        return True

    def send_time(self, command: dict) -> bool:
        if not self.can_send():
            return False
        assert self._resident.proc.stdin is not None and self._run_id is not None
        try:
            self._resident.proc.stdin.write(command_line(command, run_id=self._run_id))
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            return False
        return True

    def abort(self) -> None:
        self._resident.ending = True
        process.kill_tree(self._resident.proc)

    async def drain(self, timeout_s: float) -> bool:
        stdin = self._resident.proc.stdin
        if stdin is None:
            return False
        try:
            async with asyncio.timeout(timeout_s):
                await stdin.drain()
        except TimeoutError:
            return True
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            return False
        return True

    def enqueue(self, item: tuple) -> None:
        self._queue.put_nowait(item)

    async def _drain(self, sink: RunSink) -> None:
        try:
            while True:
                item = await self._queue.get()
                kind = item[0]
                if kind == "stdout":
                    sink.on_stdout_line(item[1])
                elif kind == "stderr":
                    sink.on_stderr_line(item[1])
                elif kind == "problem":
                    sink.on_problem(item[1], item[2], item[3])
                elif kind == "exit":
                    sink.on_process_exited(item[1])
                elif kind == "finish":
                    self._ended = True
                    alive_now = bool(item[2]) and not self._resident.exited and self._resident.proc.returncode is None
                    await sink.on_run_finished(output_cut=item[1], launcher_alive=alive_now)
                    break
        finally:
            self._ended = True
            await sink.on_run_cleanup()
            self._on_finished(self)
            self._drain_done.set()


@dataclass(eq=False)
class _Resident:
    config: str
    want_control: bool
    proc: asyncio.subprocess.Process
    credentials: Path | None
    token: str | None
    hello: dict | None = None
    idle: asyncio.Event = field(default_factory=asyncio.Event)
    ready_or_ended: asyncio.Event = field(default_factory=asyncio.Event)
    startup_error: str | None = None
    exited: bool = False
    ending: bool = False
    exit_code: int | None = None
    pending_run: ServeRunHandle | None = None
    current_run: ServeRunHandle | None = None
    line_seq: int = 0
    run_t0: int = 0
    command_seq: int = 0
    follow_task: asyncio.Task | None = None

    def next_command_id(self) -> int:
        self.command_seq += 1
        return self.command_seq


class ServeRuntime:
    """Supervises one persistent `vehicle_sil_launch serve` resident plus Cold fallback."""

    def __init__(
        self,
        launcher: Launcher,
        *,
        cwd: Path,
        participant_start_timeout_ms: int = 30000,
        participant_stop_timeout_ms: int = 10000,
        max_line: int = DEFAULT_MAX_LINE,
        escalate_wait_s: float = DEFAULT_ESCALATE_WAIT_S,
        finish_timeout_s: float = DEFAULT_FINISH_TIMEOUT_S,
        on_status: Callable[[dict], None] | None = None,
    ):
        self._launcher = launcher
        self._cwd = cwd
        self._participant_start_timeout_ms = participant_start_timeout_ms
        self._participant_stop_timeout_ms = participant_stop_timeout_ms
        self._max_line = max_line
        self._escalate_wait_s = escalate_wait_s
        self._finish_timeout_s = finish_timeout_s
        self._on_status = on_status
        self._cold = OneShotRuntime(
            launcher,
            cwd=cwd,
            participant_start_timeout_ms=participant_start_timeout_ms,
            participant_stop_timeout_ms=participant_stop_timeout_ms,
            max_line=max_line,
            escalate_wait_s=escalate_wait_s,
            finish_timeout_s=finish_timeout_s,
        )
        self._lock = asyncio.Lock()
        self._residents: set[_Resident] = set()
        self._resident: _Resident | None = None
        self._closing = False
        self.resident_starts = 0
        self.runs_completed = 0
        has_serve = FEATURE_SERVE in launcher.features
        self._status: dict = {
            "mode": "hot" if has_serve else "cold",
            "state": "idle" if has_serve else "cold",
            "config": None,
            "reason": None if has_serve else "launcher-unsupported",
            "message": None if has_serve else _UNSUPPORTED_MSG,
        }

    @property
    def resident_pid(self) -> int | None:
        if self._resident is not None and not self._resident.exited and self._resident.proc.returncode is None:
            return self._resident.proc.pid
        return None

    def sync_launcher(self) -> None:
        has_serve = FEATURE_SERVE in self._launcher.features
        if not has_serve and self._status["config"] is None and self._resident is None:
            self._status = {
                "mode": "cold",
                "state": "cold",
                "config": None,
                "reason": "launcher-unsupported",
                "message": _UNSUPPORTED_MSG,
            }
        elif has_serve and self._status["reason"] == "launcher-unsupported" and self._resident is None:
            self._status = {
                "mode": "hot",
                "state": "idle",
                "config": None,
                "reason": None,
                "message": None,
            }

    def status(self) -> dict:
        return dict(self._status)

    def _set_status(self, mode: str, state: str, config: str | None,
                    reason: str | None = None, message: str | None = None, *, notify: bool = True) -> dict:
        new_status = {
            "mode": mode,
            "state": state,
            "config": config,
            "reason": reason,
            "message": message,
        }
        changed = new_status != self._status
        self._status = new_status
        if changed and notify and self._on_status is not None:
            self._on_status(dict(new_status))
        return dict(new_status)

    async def prepare(
        self,
        config: str,
        *,
        variant: str | None = None,
        disable: tuple[str, ...] = (),
        enable: tuple[str, ...] = (),
        manual: tuple[str, ...] = (),
        want_control: bool = True,
    ) -> dict:
        if self._closing:
            raise Refused(503, "shutting-down", "the GUI server is stopping")
        async with self._lock:
            if self._closing:
                raise Refused(503, "shutting-down", "the GUI server is stopping")
            if self._resident is not None and (self._resident.current_run is not None or self._resident.pending_run is not None):
                raise Refused(409, "already-running", "a simulation is running: stop it before preparing another")
            if self._cold.active_pids():
                raise Refused(409, "already-running", "a simulation is running: stop it before preparing another")
            decision = check_hot_eligibility(
                self._launcher, self._cwd, config,
                variant=variant, disable=disable, enable=enable, manual=manual,
            )
            if not decision.hot:
                if self._resident is not None:
                    self._set_status("hot", "resetting", None if decision.reason == "launcher-unsupported" else config)
                    await self._close_resident(self._resident, asyncio.Event(), self._escalate_wait_s)
                status_cfg = None if decision.reason == "launcher-unsupported" else config
                return self._set_status("cold", "cold", status_cfg, decision.reason, decision.message)
            resident = await self._ensure_resident(config, want_control=want_control)
            if resident is None:
                return self.status()
            return self._set_status("hot", "ready", config)

    async def release_config(self, config: str) -> None:
        async with self._lock:
            if self._resident is None or self._resident.config != config:
                return
            if self._resident.current_run is not None or self._resident.pending_run is not None:
                self._resident.ending = True
                return
            self._set_status("hot", "resetting", None)
            await self._close_resident(self._resident, asyncio.Event(), self._escalate_wait_s)
            if not self._closing:
                has_serve = FEATURE_SERVE in self._launcher.features
                self._set_status(
                    "hot" if has_serve else "cold",
                    "idle" if has_serve else "cold",
                    None,
                    None if has_serve else "launcher-unsupported",
                )

    async def _start_cold(self, spec: RunSpec) -> RunHandle:
        if self._closing:
            raise Refused(503, "shutting-down", "the GUI server is stopping")
        return await self._cold.start_run(spec)

    async def start_run(self, spec: RunSpec) -> RunHandle:
        if self._closing:
            raise Refused(503, "shutting-down", "the GUI server is stopping")
        async with self._lock:
            if self._closing:
                raise Refused(503, "shutting-down", "the GUI server is stopping")
            decision = check_hot_eligibility(
                self._launcher, self._cwd, spec.config,
                variant=spec.variant, disable=spec.disable, enable=spec.enable, manual=spec.manual,
            )
            want_control = spec.credentials is not None
            if not decision.hot:
                if self._resident is not None:
                    self._set_status("hot", "resetting", None if decision.reason == "launcher-unsupported" else spec.config)
                    await self._close_resident(self._resident, asyncio.Event(), self._escalate_wait_s)
                status_cfg = None if decision.reason == "launcher-unsupported" else spec.config
                self._set_status("cold", "cold", status_cfg, decision.reason, decision.message)
                return await self._start_cold(spec)

            resident = await self._ensure_resident(spec.config, want_control=want_control)
            if self._closing:
                raise Refused(503, "shutting-down", "the GUI server is stopping")
            if resident is None:
                return await self._start_cold(spec)

            handle = ServeRunHandle(
                resident,
                command_id=resident.next_command_id(),
                on_finished=self._on_hot_run_finished,
            )
            resident.pending_run = handle
            resident.idle.clear()
            stdin = resident.proc.stdin
            wrote = False
            if stdin is not None and not stdin.is_closing() and resident.proc.returncode is None and not resident.ending:
                try:
                    stdin.write(serve_run_command(spec.request, spec.timeline, handle._command_id))
                    wrote = True
                except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
                    wrote = False
            if not wrote:
                resident.pending_run = None
                self._set_status("cold", "cold", spec.config, "resident-failed",
                                 "the persistent launcher closed unexpectedly before starting the run")
                await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                return await self._start_cold(spec)

            try:
                async with asyncio.timeout(RUN_START_TIMEOUT_S):
                    await handle._started_event.wait()
            except TimeoutError:
                resident.pending_run = None
                self._set_status("cold", "cold", spec.config, "resident-failed",
                                 "the persistent launcher did not answer run_start in time")
                await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                return await self._start_cold(spec)

            if self._closing:
                await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                raise Refused(503, "shutting-down", "the GUI server is stopping")

            if handle._run_id is None:
                resident.pending_run = None
                reason_msg = handle._startup_refusal or resident.startup_error or "the persistent launcher could not start the run"
                self._set_status("cold", "cold", spec.config, "resident-failed", reason_msg)
                await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                return await self._start_cold(spec)

            self._set_status("hot", "running", spec.config)
            return handle

    async def _ensure_resident(self, config: str, *, want_control: bool) -> _Resident | None:
        resident = self._resident
        if (
            resident is not None
            and not resident.exited
            and not resident.ending
            and resident.proc.returncode is None
            and resident.config == config
            and resident.want_control == want_control
        ):
            if resident.idle.is_set():
                return resident
            try:
                async with asyncio.timeout(IDLE_WAIT_S):
                    await resident.idle.wait()
                if not resident.exited and not resident.ending and resident.proc.returncode is None:
                    return resident
            except TimeoutError:
                pass
            self._set_status("hot", "resetting", config)
            await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
        elif resident is not None:
            self._set_status("hot", "resetting", config)
            await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)

        self._set_status("hot", "preparing", config)
        return await self._spawn_resident(config, want_control=want_control)

    async def _spawn_resident(self, config: str, *, want_control: bool) -> _Resident | None:
        credentials: Path | None = None
        token: str | None = None
        if want_control:
            try:
                credentials, token = await asyncio.to_thread(control.credentials_file)
            except OSError as e:
                self._set_status("cold", "cold", config, "resident-failed",
                                 f"cannot create serve credentials ({e}): falling back to cold run")
                return None
            if self._closing:
                control.remove(credentials)
                raise Refused(503, "shutting-down", "the GUI server is stopping")

        argv = with_participant_timeouts(
            serve_command(self._launcher, config, credentials),
            self._participant_start_timeout_ms,
            self._participant_stop_timeout_ms,
        )
        try:
            proc = await process.start(argv, cwd=self._cwd)
        except OSError as e:
            control.remove(credentials)
            self._set_status("cold", "cold", config, "resident-failed",
                             f"cannot start {self._launcher.path} serve: {e}")
            return None
        if self._closing:
            process.kill_tree(proc)
            process.close_pipes(proc)
            control.remove(credentials)
            raise Refused(503, "shutting-down", "the GUI server is stopping")

        self.resident_starts += 1
        resident = _Resident(
            config=config,
            want_control=want_control,
            proc=proc,
            credentials=credentials,
            token=token,
        )
        self._residents.add(resident)
        self._resident = resident
        resident.follow_task = asyncio.create_task(self._follow_resident(resident))

        try:
            async with asyncio.timeout(RESIDENT_READY_TIMEOUT_S):
                await resident.ready_or_ended.wait()
        except TimeoutError:
            resident.startup_error = "the persistent launcher did not become idle in time"

        if self._closing:
            await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
            raise Refused(503, "shutting-down", "the GUI server is stopping")

        if resident.exited or resident.ending or resident.startup_error is not None or not resident.idle.is_set():
            msg = resident.startup_error or "the persistent launcher failed during startup"
            await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
            self._set_status("cold", "cold", config, "resident-failed", msg)
            return None

        return resident

    async def _follow_resident(self, resident: _Resident) -> None:
        proc = resident.proc
        reader = asyncio.create_task(self._read_resident(resident))
        try:
            code = await process.wait_exit(proc)
            process.kill_tree(proc)
            done, _ = await asyncio.wait({reader}, timeout=process.OUTPUT_GRACE_S)
            reader.cancel()
            await asyncio.gather(reader, return_exceptions=True)
            process.close_pipes(proc)
            resident.exited = True
            resident.exit_code = code
            resident.idle.clear()
            resident.ready_or_ended.set()
            if (current := resident.current_run) is not None:
                resident.current_run = None
                current.enqueue(("exit", code))
                current.enqueue(("finish", not done, False))
            if (pending := resident.pending_run) is not None:
                resident.pending_run = None
                pending._started_event.set()
            if not self._closing and self._resident is resident:
                self._resident = None
                if current is None and self._status.get("state") in ("ready", "preparing", "running"):
                    self._set_status("hot", "idle", None)
        finally:
            reader.cancel()
            await asyncio.gather(reader, return_exceptions=True)
            process.close_pipes(proc)
            control.remove(resident.credentials)
            resident.credentials = None
            self._residents.discard(resident)
            if self._resident is resident:
                self._resident = None

    async def _read_resident(self, resident: _Resident) -> None:
        assert resident.proc.stdout is not None and resident.proc.stderr is not None

        async def stdout() -> None:
            async for line in process.lines(resident.proc.stdout, self._max_line):
                self._handle_stdout_line(resident, line)

        async def stderr() -> None:
            async for line in process.lines(resident.proc.stderr, STDERR_MAX_LINE):
                if line and (current := resident.current_run) is not None:
                    current.enqueue(("stderr", line))

        await asyncio.gather(stdout(), stderr(), return_exceptions=True)

    def _handle_stdout_line(self, resident: _Resident, line: str | None) -> None:
        current = resident.current_run
        if line is None:
            if current is not None:
                current.enqueue(("stdout", None))
            return
        if not line.strip():
            return
        try:
            ev = json.loads(line)
        except (ValueError, RecursionError):
            if current is not None:
                current.enqueue(("stdout", line))
            return
        if not isinstance(ev, dict) or not isinstance(ev.get("event"), str):
            if current is not None:
                current.enqueue(("stdout", line))
            return

        kind = ev["event"]
        if kind == "hello" and resident.hello is None:
            resident.hello = ev
            version = ev.get("vehicle_sil_events")
            if version != 3:
                resident.startup_error = f"serve wrote events format {version!r}, expected 3"
                resident.ending = True
                resident.ready_or_ended.set()
            return

        if kind == "idle":
            if current is None and not resident.ending:
                resident.idle.set()
                resident.ready_or_ended.set()
            return

        if kind == "run_start":
            run_id = ev.get("run")
            pending = resident.pending_run
            if (
                pending is not None
                and isinstance(run_id, str)
                and run_id
                and ev.get("id") in (None, pending._command_id)
            ):
                resident.pending_run = None
                resident.current_run = pending
                resident.idle.clear()
                t_val = ev.get("t_ms")
                resident.run_t0 = t_val if type(t_val) is int else 0
                resident.line_seq = 1
                pending._run_id = run_id
                hello_base = {
                    k: v for k, v in (resident.hello or {}).items()
                    if k not in ("event", "seq", "t_ms")
                }
                hello_line = json.dumps(
                    {
                        **hello_base,
                        "event": "hello",
                        "seq": 0,
                        "t_ms": 0,
                        "vehicle_sil_events": 2,
                    },
                    ensure_ascii=False,
                    separators=(",", ":"),
                )
                pending.enqueue(("stdout", hello_line))
                pending._started_event.set()
            return

        ev_run = ev.get("run")
        if current is not None and ev_run == current.run_id:
            norm = {k: v for k, v in ev.items() if k not in ("run", "launcher_ends")}
            norm["seq"] = resident.line_seq
            resident.line_seq += 1
            t_val = ev.get("t_ms")
            if type(t_val) is int:
                norm["t_ms"] = max(0, t_val - resident.run_t0)
            current.enqueue(("stdout", json.dumps(norm, ensure_ascii=False, separators=(",", ":"))))
            if kind == "result":
                launcher_ends = ev.get("launcher_ends") is True
                resident.current_run = None
                self.runs_completed += 1
                if launcher_ends:
                    resident.ending = True
                current.enqueue(("finish", False, not launcher_ends))
            return

        if kind == "command" and ev_run is None:
            asked = ev.get("asked_run")
            if current is not None and asked == current.run_id:
                norm = {k: v for k, v in ev.items() if k != "asked_run"}
                norm["seq"] = resident.line_seq
                resident.line_seq += 1
                t_val = ev.get("t_ms")
                if type(t_val) is int:
                    norm["t_ms"] = max(0, t_val - resident.run_t0)
                current.enqueue(("stdout", json.dumps(norm, ensure_ascii=False, separators=(",", ":"))))
            elif (
                (pending := resident.pending_run) is not None
                and ev.get("id") == pending._command_id
                and ev.get("accepted") is False
            ):
                pending._startup_refusal = (
                    ev.get("message") if isinstance(ev.get("message"), str) else ev.get("reason")
                )
                pending._started_event.set()
            return

        if kind == "result" and ev_run is None:
            res_obj = ev.get("result")
            msg = None
            if isinstance(res_obj, dict):
                findings = res_obj.get("findings")
                if isinstance(findings, list) and findings and isinstance(findings[0], dict):
                    msg = findings[0].get("message")
            resident.startup_error = msg if isinstance(msg, str) else "serve refused startup"
            resident.ending = True
            resident.ready_or_ended.set()
            if (pending := resident.pending_run) is not None:
                pending._started_event.set()
            return

        if kind == "heartbeat" and ev_run is None:
            return

    def _on_hot_run_finished(self, handle: ServeRunHandle) -> None:
        resident = handle._resident
        if self._closing:
            return
        if self._resident is resident:
            if not resident.exited and not resident.ending and resident.proc.returncode is None:
                self._set_status("hot", "ready", resident.config)
            else:
                has_serve = FEATURE_SERVE in self._launcher.features
                self._set_status(
                    "hot" if has_serve else "cold",
                    "idle" if has_serve else "cold",
                    None,
                    None if has_serve else "launcher-unsupported",
                )
        elif self._resident is None:
            has_serve = FEATURE_SERVE in self._launcher.features
            if self._status.get("mode") != "cold":
                self._set_status(
                    "hot" if has_serve else "cold",
                    "idle" if has_serve else "cold",
                    None,
                    None if has_serve else "launcher-unsupported",
                )

    async def _close_resident(self, resident: _Resident, hurry: asyncio.Event,
                              stop_timeout_s: float | None) -> None:
        proc = resident.proc
        resident.ending = True
        active_handle = resident.current_run
        if proc.returncode is None:
            if proc.stdin is not None and not proc.stdin.is_closing():
                with contextlib.suppress(BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
                    proc.stdin.close()
            if not await process.wait_exit_or_hurry(proc, hurry, stop_timeout_s):
                why = ("shutdown was requested again" if hurry.is_set() or stop_timeout_s is None else
                       f"the launcher did not stop in {stop_timeout_s:g} s after its input closed")
                active_handle = active_handle or resident.current_run
                if active_handle is not None:
                    active_handle.enqueue((
                        "problem",
                        "stop-escalated",
                        f"{why}: the launcher was asked to stop (SIGTERM / Ctrl+Break)",
                        None,
                    ))
                process.ask_to_stop(proc)
                if not await process.wait_exit_or_hurry(proc, hurry, self._escalate_wait_s):
                    process.kill_tree(proc)
                    await process.wait_bounded(proc, process.KILL_WAIT_S)
        if resident.follow_task is not None:
            await asyncio.wait({resident.follow_task}, timeout=self._finish_timeout_s)
        else:
            process.close_pipes(proc)
            control.remove(resident.credentials)
            resident.credentials = None
            self._residents.discard(resident)
        active_handle = active_handle or resident.current_run
        if active_handle is not None and active_handle._task is not None:
            await asyncio.wait({active_handle._task}, timeout=self._finish_timeout_s)
        if self._resident is resident:
            self._resident = None

    async def shutdown(self, hurry: asyncio.Event, stop_timeout_s: float | None) -> None:
        self._closing = True
        await asyncio.gather(
            *(self._close_resident(r, hurry, stop_timeout_s) for r in list(self._residents)),
            self._cold.shutdown(hurry, stop_timeout_s),
        )

    def kill_all(self) -> None:
        for resident in list(self._residents):
            process.kill_tree(resident.proc)
        self._cold.kill_all()

    def active_pids(self) -> list[int]:
        pids = [r.proc.pid for r in list(self._residents) if r.proc.pid is not None]
        pids.extend(self._cold.active_pids())
        return pids
