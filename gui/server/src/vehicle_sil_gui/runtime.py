"""Minimal launcher supervision boundary for starting, controlling, and shutting down runs."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

from . import process
from .launcher import Launcher, command_line, run_command, with_participant_timeouts
from .refusal import Refused

STDERR_MAX_LINE = 64 * 1024
DEFAULT_MAX_LINE = 16 * 1024 * 1024
DEFAULT_ESCALATE_WAIT_S = 5.0
DEFAULT_FINISH_TIMEOUT_S = process.OUTPUT_GRACE_S + 1 + 3.0


@dataclass(frozen=True)
class RunSpec:
    """Parameters needed to start one run in a runtime."""

    config: str
    request: Path
    credentials: Path | None = None
    timeline: Path | None = None


class RunSink(Protocol):
    """Normalized run lifecycle callbacks delivered by a runtime to the GUI server."""

    def on_stdout_line(self, line: str | None) -> None: ...

    def on_stderr_line(self, line: str) -> None: ...

    def on_problem(self, code: str, message: str, detail: object = None) -> None: ...

    def on_process_exited(self, returncode: int | None) -> None: ...

    async def on_run_finished(self, *, output_cut: bool = False) -> None: ...

    async def on_run_cleanup(self) -> None: ...


class RunHandle(Protocol):
    """Control handle for one started run."""

    @property
    def pid(self) -> int | None: ...

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
