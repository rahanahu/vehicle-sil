"""Minimal launcher supervision boundary for starting, controlling, and shutting down runs."""

from __future__ import annotations

import asyncio
import contextlib
import copy
from collections.abc import Callable
from dataclasses import dataclass, field
import json
import logging
import os
from pathlib import Path
from typing import Protocol

from . import control, process
from .launcher import (
    FEATURE_CHECK_FILE_ONLY,
    FEATURE_SERVE,
    FEATURE_SERVE_PREPARE,
    Launcher,
    command_line,
    file_only_check_command,
    run_command,
    serve_command,
    serve_prepare_command,
    serve_run_command,
    serve_status_command,
    stop_command_line,
    with_participant_timeouts,
)
from .refusal import Refused
from .session import _finite, _no_constant

log = logging.getLogger("vehicle_sil_gui")

STDERR_MAX_LINE = 64 * 1024
DEFAULT_MAX_LINE = 16 * 1024 * 1024
DEFAULT_CHECK_TIMEOUT_S = 120.0
DEFAULT_ESCALATE_WAIT_S = 5.0
DEFAULT_FINISH_TIMEOUT_S = process.OUTPUT_GRACE_S + 1 + 3.0
IDLE_WAIT_S = 5.0
RESIDENT_READY_TIMEOUT_S = 10.0
RUN_START_TIMEOUT_S = 10.0
_FILE_ONLY_EXIT_CODES = frozenset({0, 1, 3, 4})


@dataclass(frozen=True)
class HotDecision:
    """Whether a run or configuration is eligible for persistent serve execution."""

    hot: bool
    reason: str | None = None
    message: str | None = None


@dataclass(frozen=True)
class ConfigInspection:
    """Metadata and hot-eligibility returned by C++ `check --file-only --json` or `serve prepare`."""

    hot: HotDecision
    participants: list[str] | None = None
    networks: list[str] | None = None
    file_sha256: str | None = None
    result: str | None = None


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
    hot_decision: HotDecision | None = None


class RunSink(Protocol):
    """Normalized run lifecycle callbacks delivered by a runtime to the GUI server."""

    def on_stdout_line(self, line: str | None) -> None: ...

    def on_stdout_event(self, event: dict, size: int) -> None: ...

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

    def send_stop(self, count: int, *, force: bool | None = None) -> bool: ...

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

    def execution_status(self) -> dict | None: ...

    async def prepare(self, config: str, *, variant: str | None = None, disable: tuple[str, ...] = (),
                      enable: tuple[str, ...] = (), manual: tuple[str, ...] = (),
                      want_control: bool = True) -> dict: ...

    async def inspect_config(self, config: str, *, variant: str | None = None, disable: tuple[str, ...] = (),
                             enable: tuple[str, ...] = (), manual: tuple[str, ...] = (),
                             want_control: bool = True, need_metadata: bool = True,
                             allow_prepare_on_resident: bool = True) -> ConfigInspection: ...

    async def release_config(self, config: str) -> None: ...


_UNSUPPORTED_MSG = "this launcher was built without serve mode: running each simulation in its own launcher"


def _pre_launch_hot_guard(launcher: Launcher, *, manual: tuple[str, ...] = ()) -> HotDecision | None:
    """Synchronous pre-launch Cold guard evaluated before spawning `serve` or `check --file-only`."""
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
    return None


def parse_file_only_check(doc: object) -> ConfigInspection:
    """Parses `vehicle_sil_launch check --file-only --json` stdout into fail-closed metadata and HotDecision."""
    if (
        not isinstance(doc, dict)
        or not isinstance(doc.get("result"), str)
        or not isinstance(doc.get("participants"), list)
        or not isinstance(doc.get("hot_eligibility"), dict)
    ):
        raise Refused(502, "launcher-output", "check --file-only --json did not write a valid check object")
    result = doc["result"]
    he = doc["hot_eligibility"]
    hot = HotDecision(
        bool(he.get("eligible")),
        he.get("reason") if isinstance(he.get("reason"), str) else None,
        he.get("message") if isinstance(he.get("message"), str) else None,
    )
    if result in ("usage", "unreadable"):
        participants: list[str] | None = None
    else:
        participants = [
            p["name"] for p in doc["participants"] if isinstance(p, dict) and isinstance(p.get("name"), str)
        ]
    if result != "passed" or not isinstance(doc.get("networks"), list):
        networks: list[str] | None = None
    else:
        networks = [
            n["name"] for n in doc["networks"] if isinstance(n, dict) and isinstance(n.get("name"), str)
        ]
    raw_sha = doc.get("file_sha256")
    file_sha256 = raw_sha if isinstance(raw_sha, str) and len(raw_sha) == 64 else None
    return ConfigInspection(
        hot=hot,
        participants=participants,
        networks=networks,
        file_sha256=file_sha256,
        result=result,
    )


def parse_prepare_event(ev: dict) -> ConfigInspection | None:
    """Parses one `serve` `prepare` event into fail-closed metadata and HotDecision, or None if refused."""
    if ev.get("accepted") is not True:
        return None
    status = ev.get("status")
    elig = status.get("eligibility") if isinstance(status, dict) else None
    meta = ev.get("metadata")
    if not isinstance(elig, dict) or not isinstance(meta, dict):
        return None
    reason = elig.get("reason") if isinstance(elig.get("reason"), str) else None
    message = elig.get("message") if isinstance(elig.get("message"), str) else None
    hot = HotDecision(bool(elig.get("eligible")), reason, message)
    findings = ev.get("findings")
    has_errors = isinstance(findings, list) and any(
        isinstance(f, dict) and f.get("severity") == "error" for f in findings
    )
    if reason in ("usage", "config-unreadable") or not isinstance(meta.get("participants"), list):
        participants: list[str] | None = None
        networks: list[str] | None = None
    else:
        participants = [p for p in meta["participants"] if isinstance(p, str)]
        if has_errors or not isinstance(meta.get("networks"), list):
            networks = None
        else:
            networks = [n for n in meta["networks"] if isinstance(n, str)]
    raw_sha = meta.get("file_sha256")
    file_sha256 = raw_sha if isinstance(raw_sha, str) and len(raw_sha) == 64 else None
    result = (
        "usage" if reason == "usage"
        else "unreadable" if reason == "config-unreadable"
        else "failed" if has_errors
        else "passed"
    )
    return ConfigInspection(
        hot=hot,
        participants=participants,
        networks=networks,
        file_sha256=file_sha256,
        result=result,
    )


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

    def send_stop(self, count: int, *, force: bool | None = None) -> bool:
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

    def execution_status(self) -> dict | None:
        return None

    async def prepare(self, config: str, *, variant: str | None = None, disable: tuple[str, ...] = (),
                      enable: tuple[str, ...] = (), manual: tuple[str, ...] = (),
                      want_control: bool = True) -> dict:
        return self.status()

    async def inspect_config(self, config: str, *, variant: str | None = None, disable: tuple[str, ...] = (),
                             enable: tuple[str, ...] = (), manual: tuple[str, ...] = (),
                             want_control: bool = True, need_metadata: bool = True,
                             allow_prepare_on_resident: bool = True) -> ConfigInspection:
        return ConfigInspection(hot=HotDecision(False, "launcher-unsupported", _UNSUPPORTED_MSG))

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

    def send_stop(self, count: int, *, force: bool | None = None) -> bool:
        if not self.can_send() or count <= 0:
            return False
        assert self._resident.proc.stdin is not None and self._run_id is not None
        if force is not None and self._resident.supports_stop_force:
            line = (
                stop_command_line(self._run_id, force=True)
                if force
                else stop_command_line(self._run_id, force=False) * count
            )
        else:
            line = stop_command_line(self._run_id) * count
        try:
            self._resident.proc.stdin.write(line)
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
                if kind == "event":
                    sink.on_stdout_event(item[1], item[2])
                elif kind == "stdout":
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


_EXECUTION_STATUS_KEYS = (
    "instance",
    "status_rev",
    "launcher_state",
    "config",
    "eligibility",
    "launcher_allowed",
    "active_run_id",
    "last_run_id",
    "run_state",
)


def _extract_execution_status(raw: dict) -> dict:
    return {k: copy.deepcopy(raw[k]) for k in _EXECUTION_STATUS_KEYS if k in raw}


def project_runtime_status(
    *,
    has_serve: bool,
    cold_fallback: tuple[str | None, str | None, str | None] | None = None,
    resetting: bool = False,
    resetting_config: str | None = None,
    spawning_config: str | None = None,
    preparing_config: str | None = None,
    resident_config: str | None = None,
    resident_alive: bool = False,
    resident_ending: bool = False,
    current_run_active: bool = False,
    execution_status: dict | None = None,
) -> dict:
    """Projects Protocol v3 RuntimeStatus from C++ ExecutionStatus plus GUI-local transition/fallback state."""
    if not has_serve:
        cfg = cold_fallback[0] if cold_fallback is not None else None
        return {
            "mode": "cold",
            "state": "cold",
            "config": cfg,
            "reason": "launcher-unsupported",
            "message": _UNSUPPORTED_MSG,
        }
    if resetting:
        return {
            "mode": "hot",
            "state": "resetting",
            "config": resetting_config,
            "reason": None,
            "message": None,
        }
    if spawning_config is not None or preparing_config is not None:
        return {
            "mode": "hot",
            "state": "preparing",
            "config": spawning_config if spawning_config is not None else preparing_config,
            "reason": None,
            "message": None,
        }
    if resident_alive and (not resident_ending or current_run_active):
        if isinstance(execution_status, dict):
            lstate = execution_status.get("launcher_state")
            if "config" in execution_status:
                raw_cfg = execution_status.get("config")
                cfg = raw_cfg if isinstance(raw_cfg, str) and raw_cfg else None
            else:
                cfg = resident_config
            elig = execution_status.get("eligibility")
            elig_dict = elig if isinstance(elig, dict) else {}
            allowed_map = execution_status.get("launcher_allowed")
            run_allowed = allowed_map.get("run") if isinstance(allowed_map, dict) else None
            run_ok = run_allowed.get("ok") if isinstance(run_allowed, dict) else None

            if lstate == "preparing":
                return {"mode": "hot", "state": "preparing", "config": cfg, "reason": None, "message": None}
            if lstate == "running":
                return {"mode": "hot", "state": "running", "config": cfg, "reason": None, "message": None}
            if lstate == "ready":
                return {"mode": "hot", "state": "ready", "config": cfg, "reason": None, "message": None}
            if lstate == "idle":
                elig_reason = elig_dict.get("reason")
                has_reason = isinstance(elig_reason, str) and bool(elig_reason)
                if elig_dict.get("eligible") is False and (
                    has_reason or (bool(cfg) and run_ok is False)
                ):
                    reason = elig_reason if has_reason else None
                    raw_msg = elig_dict.get("message")
                    message = raw_msg if isinstance(raw_msg, str) and raw_msg else None
                    return {"mode": "cold", "state": "cold", "config": cfg, "reason": reason, "message": message}
                if cfg and run_ok is not False:
                    return {"mode": "hot", "state": "ready", "config": cfg, "reason": None, "message": None}
                return {"mode": "hot", "state": "idle", "config": None, "reason": None, "message": None}
            if lstate in ("ending", "contaminated"):
                return {"mode": "hot", "state": "idle", "config": None, "reason": None, "message": None}
            return {"mode": "hot", "state": "idle", "config": None, "reason": None, "message": None}
        if resident_config:
            return {"mode": "hot", "state": "ready", "config": resident_config, "reason": None, "message": None}
        return {"mode": "hot", "state": "idle", "config": None, "reason": None, "message": None}
    if cold_fallback is not None:
        cfg, reason, message = cold_fallback
        return {
            "mode": "cold",
            "state": "cold",
            "config": cfg,
            "reason": reason,
            "message": message,
        }
    return {
        "mode": "hot",
        "state": "idle",
        "config": None,
        "reason": None,
        "message": None,
    }


@dataclass(eq=False)
class _Resident:
    config: str
    want_control: bool
    proc: asyncio.subprocess.Process
    credentials: Path | None
    token: str | None
    supports_stop_force: bool = False
    hello: dict | None = None
    hello_size: int = 0
    idle: asyncio.Event = field(default_factory=asyncio.Event)
    ready_or_ended: asyncio.Event = field(default_factory=asyncio.Event)
    startup_error: str | None = None
    exited: bool = False
    ending: bool = False
    exit_code: int | None = None
    pending_run: ServeRunHandle | None = None
    pending_prepare: tuple[int | None, asyncio.Future[dict]] | None = None
    pending_status: tuple[int | None, asyncio.Future[dict]] | None = None
    execution_status: dict | None = None
    status_rev: int = -1
    current_run: ServeRunHandle | None = None
    line_seq: int = 0
    run_t0: int = 0
    command_seq: int = 0
    follow_task: asyncio.Task | None = None

    def next_command_id(self) -> int:
        self.command_seq += 1
        return self.command_seq


def _initial_execution_status(resident: _Resident) -> dict:
    instance = (resident.hello or {}).get("launcher")
    return {
        "instance": instance if isinstance(instance, str) else None,
        "status_rev": 0,
        "launcher_state": "idle",
        "config": resident.config,
        "eligibility": {"eligible": True, "reason": None, "message": None},
        "launcher_allowed": {
            "prepare": {"ok": True, "reason": None, "message": None},
            "run": {"ok": True, "reason": None, "message": None},
        },
        "active_run_id": None,
        "last_run_id": None,
        "run_state": None,
    }


class ServeRuntime:
    """Supervises one persistent `vehicle_sil_launch serve` resident plus Cold fallback."""

    def __init__(
        self,
        launcher: Launcher,
        *,
        cwd: Path,
        participant_start_timeout_ms: int = 30000,
        participant_stop_timeout_ms: int = 10000,
        check_timeout_s: float = DEFAULT_CHECK_TIMEOUT_S,
        max_line: int = DEFAULT_MAX_LINE,
        escalate_wait_s: float = DEFAULT_ESCALATE_WAIT_S,
        finish_timeout_s: float = DEFAULT_FINISH_TIMEOUT_S,
        on_status: Callable[[dict], None] | None = None,
        track: set[asyncio.subprocess.Process] | None = None,
    ):
        self._launcher = launcher
        self._cwd = cwd
        self._participant_start_timeout_ms = participant_start_timeout_ms
        self._participant_stop_timeout_ms = participant_stop_timeout_ms
        self._check_timeout_s = check_timeout_s
        self._max_line = max_line
        self._escalate_wait_s = escalate_wait_s
        self._finish_timeout_s = finish_timeout_s
        self._on_status = on_status
        self._track = track
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
        self._cold_fallback: tuple[str | None, str | None, str | None] | None = None
        self._spawning_config: str | None = None
        self._preparing_config: str | None = None
        self._resetting = False
        self._resetting_config: str | None = None
        self._last_status: dict = self._projected_status()

    @property
    def resident_pid(self) -> int | None:
        if self._resident is not None and not self._resident.exited and self._resident.proc.returncode is None:
            return self._resident.proc.pid
        return None

    def execution_status(self) -> dict | None:
        resident = self._resident
        if (
            resident is not None
            and not resident.exited
            and resident.proc.returncode is None
            and (not resident.ending or resident.current_run is not None)
            and resident.execution_status is not None
        ):
            return copy.deepcopy(resident.execution_status)
        return None

    def _projected_status(self) -> dict:
        resident = self._resident
        resident_alive = resident is not None and not resident.exited and resident.proc.returncode is None
        return project_runtime_status(
            has_serve=FEATURE_SERVE in self._launcher.features,
            cold_fallback=self._cold_fallback,
            resetting=self._resetting,
            resetting_config=self._resetting_config,
            spawning_config=self._spawning_config,
            preparing_config=self._preparing_config,
            resident_config=resident.config if resident is not None else None,
            resident_alive=resident_alive,
            resident_ending=resident.ending if resident is not None else False,
            current_run_active=resident.current_run is not None if resident is not None else False,
            execution_status=resident.execution_status if resident is not None else None,
        )

    def _sync_status(self, *, notify: bool = True) -> dict:
        new_status = self._projected_status()
        changed = new_status != self._last_status
        self._last_status = new_status
        if changed and notify and self._on_status is not None:
            self._on_status(dict(new_status))
        return dict(new_status)

    def _schedule_sync_status(self) -> None:
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            self._sync_status()
            return
        loop.call_soon(self._sync_status_if_open)

    def _sync_status_if_open(self) -> None:
        if not self._closing:
            self._sync_status()

    def sync_launcher(self) -> None:
        has_serve = FEATURE_SERVE in self._launcher.features
        if has_serve and self._cold_fallback is not None and self._cold_fallback[1] == "launcher-unsupported":
            self._cold_fallback = None
        self._sync_status(notify=False)

    def status(self) -> dict:
        return self._projected_status()

    def _apply_execution_status(self, resident: _Resident, raw: dict) -> bool:
        if not isinstance(raw, dict) or not isinstance(raw.get("launcher_state"), str):
            return False
        expected_instance = (resident.hello or {}).get("launcher")
        incoming_instance = raw.get("instance")
        if isinstance(expected_instance, str) and expected_instance:
            if not isinstance(incoming_instance, str) or incoming_instance != expected_instance:
                return False
        rev = raw.get("status_rev")
        if type(rev) is not int or rev < resident.status_rev:
            return False
        if (current := resident.current_run) is not None and current.run_id is not None:
            lstate = raw.get("launcher_state")
            if lstate != "running" and raw.get("last_run_id") != current.run_id:
                return False
            active_id = raw.get("active_run_id")
            if isinstance(active_id, str) and active_id and active_id != current.run_id:
                return False
        resident.status_rev = rev
        resident.execution_status = _extract_execution_status(raw)
        return True

    async def _send_status(self, resident: _Resident) -> dict | None:
        stdin = resident.proc.stdin
        if stdin is None or stdin.is_closing() or resident.proc.returncode is not None or resident.ending:
            return None
        cmd_id = resident.next_command_id()
        fut: asyncio.Future[dict] = asyncio.get_running_loop().create_future()
        resident.pending_status = (cmd_id, fut)
        try:
            stdin.write(serve_status_command(command_id=cmd_id))
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            resident.pending_status = None
            return None
        try:
            async with asyncio.timeout(self._check_timeout_s):
                return await fut
        except (TimeoutError, RuntimeError):
            return None
        finally:
            if resident.pending_status is not None and resident.pending_status[0] == cmd_id:
                resident.pending_status = None

    async def query_status(self) -> dict | None:
        if self._closing or FEATURE_SERVE_PREPARE not in self._launcher.features:
            return self.execution_status()
        resident = self._resident
        if resident is None or resident.exited or resident.ending or resident.proc.returncode is not None:
            return None
        await self._send_status(resident)
        return self.execution_status()

    async def _reset_resident(self, resident: _Resident, config: str | None) -> None:
        self._resetting = True
        self._resetting_config = config
        if not self._closing:
            self._sync_status()
        try:
            await asyncio.shield(self._close_resident(resident, asyncio.Event(), self._escalate_wait_s))
        finally:
            self._resetting = False
            self._resetting_config = None
            if not self._closing:
                self._sync_status()

    async def _run_file_only_check(
        self,
        config: str,
        *,
        variant: str | None = None,
        disable: tuple[str, ...] = (),
        enable: tuple[str, ...] = (),
        manual: tuple[str, ...] = (),
    ) -> ConfigInspection:
        argv = file_only_check_command(
            self._launcher, config, variant=variant, disable=disable, enable=enable, manual=manual,
        )
        try:
            code, out, err = await process.run_to_end(
                argv, cwd=self._cwd, timeout_s=self._check_timeout_s, track=self._track,
            )
        except TimeoutError:
            raise Refused(504, "check-timeout", f"the check did not end in {self._check_timeout_s:g} s (it was "
                                                "stopped): an ECU's --describe may hang") from None
        except OSError as e:
            raise Refused(502, "launcher-start", f"cannot start {self._launcher.path}: {e}") from None
        if self._closing:
            raise Refused(503, "shutting-down", "the GUI server is stopping")
        if code not in _FILE_ONLY_EXIT_CODES:
            tail = err.decode("utf-8", "replace").strip().splitlines()[-10:]
            raise Refused(502, "launcher-output", f"check --file-only --json exited with code {code}", tail)
        try:
            doc = json.loads(out, parse_constant=_no_constant, parse_float=_finite)
        except (ValueError, RecursionError):
            doc = None
        if not isinstance(doc, dict):
            tail = err.decode("utf-8", "replace").strip().splitlines()[-10:]
            raise Refused(502, "launcher-output", f"check --file-only --json exited with code {code} and did not "
                                                  "write a valid JSON object", tail)
        return parse_file_only_check(doc)

    async def _send_prepare(
        self,
        resident: _Resident,
        *,
        variant: str | None = None,
        disable: tuple[str, ...] = (),
        enable: tuple[str, ...] = (),
        manual: tuple[str, ...] = (),
    ) -> dict | None:
        stdin = resident.proc.stdin
        if stdin is None or stdin.is_closing() or resident.proc.returncode is not None or resident.ending:
            return None
        cmd_id = resident.next_command_id()
        fut: asyncio.Future[dict] = asyncio.get_running_loop().create_future()
        resident.pending_prepare = (cmd_id, fut)
        try:
            stdin.write(serve_prepare_command(
                variant=variant, disable=disable, enable=enable, manual=manual, command_id=cmd_id,
            ))
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            resident.pending_prepare = None
            return None
        try:
            async with asyncio.timeout(self._check_timeout_s):
                return await fut
        except (TimeoutError, RuntimeError):
            return None
        finally:
            if resident.pending_prepare is not None and resident.pending_prepare[0] == cmd_id:
                resident.pending_prepare = None

    async def _inspect_unlocked(
        self,
        config: str,
        *,
        variant: str | None = None,
        disable: tuple[str, ...] = (),
        enable: tuple[str, ...] = (),
        manual: tuple[str, ...] = (),
        want_control: bool = True,
        need_metadata: bool = True,
        allow_prepare_on_resident: bool = True,
    ) -> tuple[ConfigInspection, _Resident | None]:
        guard = _pre_launch_hot_guard(self._launcher, manual=manual)
        if guard is not None:
            if not need_metadata or FEATURE_CHECK_FILE_ONLY not in self._launcher.features:
                return ConfigInspection(hot=guard), None
            checked = await self._run_file_only_check(
                config, variant=variant, disable=disable, enable=enable, manual=manual,
            )
            return ConfigInspection(
                hot=guard,
                participants=checked.participants,
                networks=checked.networks,
                file_sha256=checked.file_sha256,
                result=checked.result,
            ), None

        resident = self._resident
        if (
            allow_prepare_on_resident
            and FEATURE_SERVE_PREPARE in self._launcher.features
            and resident is not None
            and not resident.exited
            and not resident.ending
            and resident.proc.returncode is None
            and resident.config == config
            and resident.want_control == want_control
            and resident.idle.is_set()
        ):
            prev_preparing = self._preparing_config
            self._preparing_config = config
            self._sync_status()
            try:
                ev = await self._send_prepare(
                    resident, variant=variant, disable=disable, enable=enable, manual=manual,
                )
            except BaseException:
                self._preparing_config = prev_preparing
                await asyncio.shield(self._close_resident(resident, asyncio.Event(), self._escalate_wait_s))
                raise
            finally:
                self._preparing_config = prev_preparing
                if not self._closing:
                    self._sync_status()
            if self._closing:
                raise Refused(503, "shutting-down", "the GUI server is stopping")
            if ev is not None:
                parsed = parse_prepare_event(ev)
                if parsed is not None:
                    return parsed, resident
            await self._reset_resident(resident, config)
            if self._closing:
                raise Refused(503, "shutting-down", "the GUI server is stopping")

        if FEATURE_CHECK_FILE_ONLY in self._launcher.features:
            return await self._run_file_only_check(
                config, variant=variant, disable=disable, enable=enable, manual=manual,
            ), None
        return ConfigInspection(hot=HotDecision(True)), None

    async def inspect_config(
        self,
        config: str,
        *,
        variant: str | None = None,
        disable: tuple[str, ...] = (),
        enable: tuple[str, ...] = (),
        manual: tuple[str, ...] = (),
        want_control: bool = True,
        need_metadata: bool = True,
        allow_prepare_on_resident: bool = True,
    ) -> ConfigInspection:
        if self._closing:
            raise Refused(503, "shutting-down", "the GUI server is stopping")
        async with self._lock:
            if self._closing:
                raise Refused(503, "shutting-down", "the GUI server is stopping")
            inspection, _ = await self._inspect_unlocked(
                config,
                variant=variant,
                disable=disable,
                enable=enable,
                manual=manual,
                want_control=want_control,
                need_metadata=need_metadata,
                allow_prepare_on_resident=allow_prepare_on_resident,
            )
            return inspection

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

            inspected_resident = self._resident
            inspection, prepared_resident = await self._inspect_unlocked(
                config,
                variant=variant,
                disable=disable,
                enable=enable,
                manual=manual,
                want_control=want_control,
                need_metadata=False,
            )
            decision = inspection.hot
            if not decision.hot:
                status_cfg = None if decision.reason == "launcher-unsupported" else config
                self._cold_fallback = (status_cfg, decision.reason, decision.message)
                if self._resident is not None:
                    await self._reset_resident(self._resident, status_cfg)
                return self._sync_status()

            already_prepared_hint = (
                prepared_resident is not None
                and prepared_resident is inspected_resident
                and prepared_resident is self._resident
            )
            self._cold_fallback = None
            if not already_prepared_hint:
                self._preparing_config = config
                self._sync_status()
            try:
                resident = await self._ensure_resident(config, want_control=want_control)
                if resident is None:
                    self._preparing_config = None
                    return self._sync_status()
                already_prepared = (
                    prepared_resident is not None
                    and resident is inspected_resident
                    and resident is prepared_resident
                )
                if FEATURE_SERVE_PREPARE in self._launcher.features and not already_prepared:
                    try:
                        ev = await self._send_prepare(
                            resident, variant=variant, disable=disable, enable=enable, manual=manual,
                        )
                    except BaseException:
                        self._preparing_config = None
                        await asyncio.shield(self._close_resident(resident, asyncio.Event(), self._escalate_wait_s))
                        raise
                    if self._closing:
                        self._preparing_config = None
                        await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                        raise Refused(503, "shutting-down", "the GUI server is stopping")
                    parsed = parse_prepare_event(ev) if ev is not None else None
                    if parsed is None:
                        self._preparing_config = None
                        self._cold_fallback = (
                            config,
                            "resident-failed",
                            "the persistent launcher could not prepare the configuration",
                        )
                        await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                        return self._sync_status()
                    if not parsed.hot.hot:
                        self._preparing_config = None
                        self._cold_fallback = (config, parsed.hot.reason, parsed.hot.message)
                        await self._reset_resident(resident, config)
                        return self._sync_status()
            finally:
                self._preparing_config = None
                if not self._closing:
                    self._sync_status()
            return self.status()

    async def release_config(self, config: str) -> None:
        async with self._lock:
            if self._resident is None or self._resident.config != config:
                return
            if self._resident.current_run is not None or self._resident.pending_run is not None:
                self._resident.ending = True
                return
            self._cold_fallback = None
            await self._reset_resident(self._resident, None)

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
            want_control = spec.credentials is not None
            if spec.hot_decision is not None:
                decision = spec.hot_decision
            else:
                inspection, _ = await self._inspect_unlocked(
                    spec.config,
                    variant=spec.variant,
                    disable=spec.disable,
                    enable=spec.enable,
                    manual=spec.manual,
                    want_control=want_control,
                    need_metadata=False,
                )
                decision = inspection.hot
            if not decision.hot:
                status_cfg = None if decision.reason == "launcher-unsupported" else spec.config
                self._cold_fallback = (status_cfg, decision.reason, decision.message)
                if self._resident is not None:
                    await self._reset_resident(self._resident, status_cfg)
                self._sync_status()
                return await self._start_cold(spec)

            self._cold_fallback = None
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
                self._cold_fallback = (
                    spec.config,
                    "resident-failed",
                    "the persistent launcher closed unexpectedly before starting the run",
                )
                await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                self._sync_status()
                return await self._start_cold(spec)

            try:
                async with asyncio.timeout(RUN_START_TIMEOUT_S):
                    await handle._started_event.wait()
            except TimeoutError:
                resident.pending_run = None
                self._cold_fallback = (
                    spec.config,
                    "resident-failed",
                    "the persistent launcher did not answer run_start in time",
                )
                await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                self._sync_status()
                return await self._start_cold(spec)
            except BaseException:
                if resident.pending_run is handle:
                    resident.pending_run = None
                await asyncio.shield(self._close_resident(resident, asyncio.Event(), self._escalate_wait_s))
                if not self._closing:
                    self._sync_status()
                raise

            if self._closing:
                await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                raise Refused(503, "shutting-down", "the GUI server is stopping")

            if handle._run_id is None:
                resident.pending_run = None
                reason_msg = handle._startup_refusal or resident.startup_error or "the persistent launcher could not start the run"
                self._cold_fallback = (spec.config, "resident-failed", reason_msg)
                await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                self._sync_status()
                return await self._start_cold(spec)

            self._sync_status()
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
            await self._reset_resident(resident, config)
        elif resident is not None:
            await self._reset_resident(resident, config)

        return await self._spawn_resident(config, want_control=want_control)

    async def _spawn_resident(self, config: str, *, want_control: bool) -> _Resident | None:
        self._spawning_config = config
        self._sync_status()
        credentials: Path | None = None
        token: str | None = None
        proc: asyncio.subprocess.Process | None = None
        resident: _Resident | None = None
        try:
            if want_control:
                making = asyncio.ensure_future(asyncio.to_thread(control.credentials_file))
                try:
                    credentials, token = await asyncio.shield(making)
                except asyncio.CancelledError:
                    making.add_done_callback(
                        lambda f: not f.cancelled() and f.exception() is None and control.remove(f.result()[0])
                    )
                    raise
                except OSError as e:
                    self._cold_fallback = (
                        config,
                        "resident-failed",
                        f"cannot create serve credentials ({e}): falling back to cold run",
                    )
                    return None
                if self._closing:
                    control.remove(credentials)
                    credentials = None
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
                credentials = None
                self._cold_fallback = (
                    config,
                    "resident-failed",
                    f"cannot start {self._launcher.path} serve: {e}",
                )
                return None
            if self._closing:
                process.kill_tree(proc)
                process.close_pipes(proc)
                control.remove(credentials)
                credentials = None
                proc = None
                raise Refused(503, "shutting-down", "the GUI server is stopping")

            self.resident_starts += 1
            resident = _Resident(
                config=config,
                want_control=want_control,
                proc=proc,
                credentials=credentials,
                token=token,
                supports_stop_force=FEATURE_SERVE_PREPARE in self._launcher.features,
            )
            credentials = None
            proc = None
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
                resident = None
                raise Refused(503, "shutting-down", "the GUI server is stopping")

            if resident.exited or resident.ending or resident.startup_error is not None or not resident.idle.is_set():
                msg = resident.startup_error or "the persistent launcher failed during startup"
                await self._close_resident(resident, asyncio.Event(), self._escalate_wait_s)
                resident = None
                self._cold_fallback = (config, "resident-failed", msg)
                return None

            ready_resident = resident
            resident = None
            return ready_resident
        except BaseException:
            if resident is not None:
                await asyncio.shield(self._close_resident(resident, asyncio.Event(), self._escalate_wait_s))
            elif proc is not None:
                process.kill_tree(proc)
                process.close_pipes(proc)
            if credentials is not None:
                control.remove(credentials)
            raise
        finally:
            self._spawning_config = None
            if not self._closing:
                self._sync_status()

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
            if (prep := resident.pending_prepare) is not None:
                resident.pending_prepare = None
                _, fut = prep
                if not fut.done():
                    fut.set_exception(RuntimeError("the persistent launcher exited during prepare"))
            if (stat := resident.pending_status) is not None:
                resident.pending_status = None
                _, fut = stat
                if not fut.done():
                    fut.set_exception(RuntimeError("the persistent launcher exited during status"))
            if not self._closing and self._resident is resident:
                self._resident = None
                self._sync_status()
        finally:
            reader.cancel()
            await asyncio.gather(reader, return_exceptions=True)
            process.close_pipes(proc)
            control.remove(resident.credentials)
            resident.credentials = None
            self._residents.discard(resident)
            if self._resident is resident:
                self._resident = None
                if not self._closing:
                    self._sync_status()

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
        size = len(line.encode("utf-8", "surrogatepass"))
        try:
            ev = json.loads(line, parse_constant=_no_constant, parse_float=_finite)
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
            resident.hello_size = size
            version = ev.get("vehicle_sil_events")
            if version != 3:
                resident.startup_error = f"serve wrote events format {version!r}, expected 3"
                resident.ending = True
                resident.ready_or_ended.set()
            elif resident.execution_status is None:
                resident.execution_status = _initial_execution_status(resident)
                resident.status_rev = max(resident.status_rev, 0)
            return

        if kind == "idle":
            if current is None and not resident.ending:
                if resident.execution_status is None:
                    resident.execution_status = _initial_execution_status(resident)
                    resident.status_rev = max(resident.status_rev, 0)
                else:
                    st = resident.execution_status
                    if st.get("launcher_state") == "running":
                        elig = st.get("eligibility")
                        elig_ok = elig.get("eligible", True) if isinstance(elig, dict) else True
                        st["launcher_state"] = "ready" if elig_ok and st.get("config") else "idle"
                    st["active_run_id"] = None
                    st["run_state"] = None
                    elig = st.get("eligibility")
                    elig_reason = elig.get("reason") if isinstance(elig, dict) else None
                    elig_blocked = (
                        isinstance(elig, dict)
                        and elig.get("eligible") is False
                        and isinstance(elig_reason, str)
                        and bool(elig_reason)
                    )
                    allowed = st.setdefault("launcher_allowed", {})
                    if isinstance(allowed, dict):
                        allowed["prepare"] = {"ok": True, "reason": None, "message": None}
                        if not elig_blocked and st.get("config"):
                            allowed["run"] = {"ok": True, "reason": None, "message": None}
                resident.idle.set()
                resident.ready_or_ended.set()
                if self._resident is resident:
                    self._schedule_sync_status()
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
                if resident.execution_status is None:
                    resident.execution_status = _initial_execution_status(resident)
                    resident.status_rev = max(resident.status_rev, 0)
                resident.status_rev = max(resident.status_rev + 1, 1)
                st = resident.execution_status
                st["status_rev"] = resident.status_rev
                st["launcher_state"] = "running"
                st["active_run_id"] = run_id
                st["run_state"] = None
                allowed = st.setdefault("launcher_allowed", {})
                if isinstance(allowed, dict):
                    busy = {"ok": False, "reason": "busy", "message": "a run is under way"}
                    allowed["prepare"] = dict(busy)
                    allowed["run"] = dict(busy)
                t_val = ev.get("t_ms")
                resident.run_t0 = t_val if type(t_val) is int else 0
                resident.line_seq = 1
                pending._run_id = run_id
                hello_base = {
                    k: v for k, v in (resident.hello or {}).items()
                    if k not in ("event", "seq", "t_ms")
                }
                hello_event = {
                    **hello_base,
                    "event": "hello",
                    "seq": 0,
                    "t_ms": 0,
                    "vehicle_sil_events": 2,
                }
                pending.enqueue(("event", hello_event, resident.hello_size))
                pending._started_event.set()
                if self._resident is resident:
                    self._schedule_sync_status()
            return

        ev_run = ev.get("run")
        if kind == "prepare" and ev_run is None:
            if isinstance(ev.get("status"), dict):
                if not self._apply_execution_status(resident, ev["status"]):
                    return
                if self._resident is resident:
                    self._schedule_sync_status()
            if (prep := resident.pending_prepare) is not None:
                req_id, fut = prep
                if req_id is None or ev.get("id") == req_id:
                    resident.pending_prepare = None
                    if not fut.done():
                        fut.set_result(ev)
            return

        if kind == "status" and ev_run is None:
            if not self._apply_execution_status(resident, ev):
                return
            if self._resident is resident:
                self._schedule_sync_status()
            if (stat := resident.pending_status) is not None:
                req_id, fut = stat
                if req_id is None or ev.get("id") == req_id:
                    resident.pending_status = None
                    if not fut.done():
                        fut.set_result(_extract_execution_status(ev))
            return

        if current is not None and ev_run == current.run_id:
            if kind == "state" and resident.execution_status is not None:
                rs = {k: v for k, v in ev.items() if k not in ("event", "seq", "t_ms", "run")}
                cur_rs = resident.execution_status.get("run_state")
                new_rev = rs.get("rev")
                cur_rev = cur_rs.get("rev") if isinstance(cur_rs, dict) else None
                if type(new_rev) is int and (type(cur_rev) is not int or new_rev >= cur_rev):
                    resident.execution_status["run_state"] = rs
            norm = {k: v for k, v in ev.items() if k not in ("run", "launcher_ends")}
            norm["seq"] = resident.line_seq
            resident.line_seq += 1
            t_val = ev.get("t_ms")
            if type(t_val) is int:
                norm["t_ms"] = max(0, t_val - resident.run_t0)
            current.enqueue(("event", norm, size))
            if kind == "result":
                launcher_ends = ev.get("launcher_ends") is True
                resident.current_run = None
                self.runs_completed += 1
                if launcher_ends:
                    resident.ending = True
                resident.status_rev = max(resident.status_rev + 1, 1)
                if resident.execution_status is not None:
                    st = resident.execution_status
                    st["status_rev"] = resident.status_rev
                    st["last_run_id"] = ev_run
                    st["active_run_id"] = None
                    st["run_state"] = None
                    if launcher_ends:
                        if st.get("launcher_state") != "contaminated":
                            st["launcher_state"] = "ending"
                        allowed = st.setdefault("launcher_allowed", {})
                        if isinstance(allowed, dict):
                            ending = {"ok": False, "reason": "launcher-ending", "message": "the launcher is ending"}
                            allowed["prepare"] = dict(ending)
                            allowed["run"] = dict(ending)
                    else:
                        elig = st.get("eligibility")
                        elig_ok = elig.get("eligible", True) if isinstance(elig, dict) else True
                        elig_reason = elig.get("reason") if isinstance(elig, dict) else None
                        elig_blocked = (
                            isinstance(elig, dict)
                            and elig.get("eligible") is False
                            and isinstance(elig_reason, str)
                            and bool(elig_reason)
                        )
                        st["launcher_state"] = "ready" if elig_ok and st.get("config") else "idle"
                        allowed = st.setdefault("launcher_allowed", {})
                        if isinstance(allowed, dict):
                            allowed["prepare"] = {"ok": True, "reason": None, "message": None}
                            if not elig_blocked and st.get("config"):
                                allowed["run"] = {"ok": True, "reason": None, "message": None}
                if self._resident is resident:
                    self._schedule_sync_status()
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
                current.enqueue(("event", norm, size))
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
            if (prep := resident.pending_prepare) is not None:
                resident.pending_prepare = None
                _, fut = prep
                if not fut.done():
                    fut.set_exception(RuntimeError(resident.startup_error))
            if (stat := resident.pending_status) is not None:
                resident.pending_status = None
                _, fut = stat
                if not fut.done():
                    fut.set_exception(RuntimeError(resident.startup_error))
            return

        if kind == "heartbeat" and ev_run is None:
            return

    def _on_hot_run_finished(self, handle: ServeRunHandle) -> None:
        if self._closing:
            return
        self._sync_status()

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
