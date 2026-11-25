# SPDX-License-Identifier: MIT
"""Test support for `tests/serve`: thin wrapper around `vehicle_sil_control.Serve` adding full-stream
recording and test wait helpers (`events`, `snapshot`, `window`, `wait_for`, `wait_event`, `ready`),
fault-injection helpers (`send_raw`, `signal`, `kill`), `Cold` baseline runner, and Linux `/proc`
resource inspection (standard library only).
"""
from __future__ import annotations

import itertools
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import time
import unittest
from collections.abc import Callable, Iterable, Mapping, Sequence
from typing import Dict, List, Optional

_REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
_PYTHON_DIR = str(_REPO_ROOT / "python")
if _PYTHON_DIR not in sys.path:
    sys.path.insert(0, _PYTHON_DIR)
_COMMON_DIR = str(pathlib.Path(__file__).resolve().parents[1] / "common")
if _COMMON_DIR not in sys.path:
    sys.path.insert(0, _COMMON_DIR)

from timeout_scale import timeout_scale as _timeout_scale  # noqa: E402
from vehicle_sil_control.protocol import (  # noqa: E402
    EVENTS_FORMAT as SERVE_EVENTS_FORMAT,
    EXIT_CONTAMINATED,
    EXIT_IDLE_EOF,
    EXIT_INTERRUPTED,
    EXIT_USAGE,
    HELLO_FORMAT_KEY,
    HELLO_LAUNCHER_KEY,
    REASON_BUSY,
    REASON_CONTAMINATED,
    REASON_NOT_PREPARED,
    REASON_RUN_CHANGED,
    REASON_RUN_REQUIRED,
    RUN_KEY,
    serve_run_payload as run_command,
)
from vehicle_sil_control.serve import Serve as _SdkServe, ServeError  # noqa: E402

SERVE_VERB = "serve"
FAULT_DELAY_ENV = "VEHICLE_SIL_FAULT_CLOSE_DELAY_MS"
FAULT_CLOSE_TIMEOUT_ENV = "VEHICLE_SIL_FAULT_CLOSE_TIMEOUT_MS"
TRACE_ENV = "VEHICLE_SIL_TRACE"
RUN_METADATA = "vehicle-sil-run"

EXE = ".exe" if os.name == "nt" else ""
WINDOWS = os.name == "nt"

try:
    SCALE = _timeout_scale()
except ValueError as e:
    raise SystemExit(str(e)) from None


def scaled(seconds: float) -> float:
    """A limit for waiting for something that is expected to happen, times the factor."""
    return seconds * SCALE


def env_path(name: str) -> pathlib.Path:
    value = os.environ.get(name)
    if not value:
        raise unittest.SkipTest(f"{name} is not set (the test is run by CTest, tests/serve/CMakeLists.txt)")
    return pathlib.Path(value)


def runs(name: str, default: int) -> int:
    """How many Runs a repetition makes (CMake sets the counts; a longer label sets more)."""
    return int(os.environ.get(name) or default)


def fault_delay_built() -> bool:
    return os.environ.get("VEHICLE_SIL_SERVE_FAULT_DELAY", "").upper() in ("1", "ON", "TRUE", "YES")


def asan_built() -> bool:
    return os.environ.get("VEHICLE_SIL_SERVE_ASAN", "").upper() in ("1", "ON", "TRUE", "YES")


def free_port() -> int:
    """A TCP port that nothing listens on now (for a fixed address that the test chooses)."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Scratch:
    """A directory of the test's own (emptied when it is made)."""

    def __init__(self, name: str):
        self.dir = env_path("VEHICLE_SIL_SERVE_SCRATCH") / name
        if self.dir.exists():
            shutil.rmtree(self.dir, ignore_errors=True)
        self.dir.mkdir(parents=True, exist_ok=True)
        self._numbers = itertools.count(1)

    def path(self, name: str) -> pathlib.Path:
        return self.dir / name

    def write(self, name: str, text: str) -> pathlib.Path:
        path = self.dir / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def request(self, name: str, **keys) -> pathlib.Path:
        """A request file (vehicle_sil_run_request 1) with `keys`; a pathlib.Path value is written as a path."""
        body = {"vehicle_sil_run_request": 1}
        body.update({k: (v.as_posix() if isinstance(v, pathlib.PurePath) else v) for k, v in keys.items()})
        return self.write(f"{name}.request.json", json.dumps(body, indent=1) + "\n")

    def timeline(self, name: str) -> pathlib.Path:
        return self.dir / f"{name}-{next(self._numbers):04d}.timeline.json"


def read_json(path: os.PathLike) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# --- Cold: one run --json ---------------------------------------------------------------------------------------


class Cold:
    """One `vehicle_sil_launch run --json` with the same configuration, request and timeline as a serve Run."""

    def __init__(self, returncode: int, result: Optional[dict], timeline: Optional[dict], stderr: str):
        self.returncode = returncode
        self.result = result
        self.timeline = timeline
        self.stderr = stderr


def cold(
    launch: os.PathLike,
    config: os.PathLike,
    request: Optional[os.PathLike] = None,
    timeline: Optional[os.PathLike] = None,
    arguments: Sequence[str] = (),
    env: Optional[Mapping] = None,
    timeout: float = 120.0,
) -> Cold:
    command = [os.fspath(launch), "run", "--json", *arguments]
    if request is not None:
        command += ["--request", os.fspath(request)]
    if timeline is not None:
        command += ["--timeline", os.fspath(timeline)]
    command += ["--", os.fspath(config)]
    done = subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=scaled(timeout),
        env=None if env is None else dict(env),
    )
    try:
        result = json.loads(done.stdout)
    except ValueError:
        result = None
    tl = read_json(timeline) if timeline is not None and os.path.exists(timeline) else None
    return Cold(done.returncode, result, tl, done.stderr.decode("utf-8", "replace"))


# --- Serve test wrapper over vehicle_sil_control.Serve ----------------------------------------------------------


class RunRecord:
    """One serve Run: its ID, its run_start and result lines, the lines of its window and its timeline file."""

    def __init__(self, run: str, start: dict, result_event: dict, window: List[dict], timeline: Optional[dict]):
        self.run = run
        self.start = start
        self.result_event = result_event
        self.result = result_event.get("result") or {}
        self.status = result_event.get("status")
        self.window = window
        self.timeline = timeline

    @property
    def launcher_ends(self) -> bool:
        return bool(self.result_event.get("launcher_ends"))


class Serve(_SdkServe):
    """Thin test wrapper around `vehicle_sil_control.Serve` adding full-stream recording,
    fault-injection (`send_raw`, `signal`), and `scaled()` timeouts."""

    def __init__(
        self,
        launch: os.PathLike,
        config: os.PathLike,
        arguments: Sequence[str] = (),
        *,
        once: bool = False,
        env: Optional[Mapping] = None,
        cwd: Optional[os.PathLike] = None,
        keep_logs: bool = True,
        stdout=subprocess.PIPE,
    ):
        if not WINDOWS:
            for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                if signal.getsignal(sig) == signal.SIG_IGN:
                    signal.signal(sig, signal.SIG_DFL)
        self.events: List[dict] = []
        self._record_keep_logs = keep_logs
        super().__init__(
            launch,
            config,
            once=once,
            arguments=arguments,
            env=env,
            cwd=cwd,
            stdout=stdout,
            stderr=subprocess.PIPE,
            keep_logs=keep_logs,
            on_event=self._record_event,
            start_new_session=True,
            wait_ready=False,
        )
        self._next_id = 1000

    def _record_event(self, event: dict) -> None:
        with self._changed:
            if self._record_keep_logs or event.get("event") != "log":
                self.events.append(event)
            else:
                self.events.append({k: event.get(k) for k in ("event", "seq", "t_ms", "run") if k in event})
            self._changed.notify_all()

    def _tail(self, count: int = 8) -> str:
        with self._changed:
            tail = list(self.events[-count:])
        lines = [json.dumps(e)[:300] for e in tail]
        err = self.stderr_text.strip()[-1500:]
        return (
            "\n--- last events ---\n"
            + "\n".join(lines)
            + ("\n--- stderr ---\n" + err if err else "")
            + f"\n--- exit code: {self.process.poll()}"
        )

    def snapshot(self) -> List[dict]:
        with self._changed:
            return list(self.events)

    def window(self, run_id: str) -> List[dict]:
        with self._changed:
            return [e for e in self.events if e.get("run") == run_id]

    def wait_for(
        self,
        condition: Callable[[dict], bool],
        what: str = "event",
        timeout: float = 60.0,
        after: Optional[Mapping] = None,
    ) -> dict:
        """Wait for the first event after `after` (by `seq`) satisfying `condition`.

        Note: `_record_event` runs pre-dispatch via `on_event`, so callers needing settled
        post-dispatch `Serve`/`RunHandle` state should synchronize via `ready()` or `run.wait()`.
        """
        limit = scaled(timeout)
        deadline = time.monotonic() + limit
        after_seq = -1 if after is None else after["seq"]
        timed_out = False
        with self._changed:
            start = 0
            while True:
                for index in range(start, len(self.events)):
                    e = self.events[index]
                    if e.get("seq", -1) > after_seq and condition(e):
                        return e
                start = len(self.events)
                if self._ended:
                    break
                left = deadline - time.monotonic()
                if left <= 0:
                    timed_out = True
                    break
                self._changed.wait(min(left, 1.0))
        if timed_out:
            raise ServeError(f"no {what} within {limit} s{self._tail()}")
        raise ServeError(f"vehicle_sil_launch serve events ended without {what}{self._tail()}")

    def wait_event(
        self,
        event: str,
        timeout: float = 60.0,
        after: Optional[Mapping] = None,
        **keys,
    ) -> dict:
        return self.wait_for(
            lambda e: e.get("event") == event and all(e.get(k) == v for k, v in keys.items()),
            f"{event} {keys or ''}",
            timeout,
            after,
        )

    def ready(self, timeout: float = 60.0) -> dict:
        """Wait until the launcher has emitted its initial status and has no active run in flight."""
        limit = scaled(timeout)
        deadline = time.monotonic() + limit
        timed_out = False
        with self._changed:
            while True:
                if (
                    self._hello is not None
                    and self._ready
                    and self._active_run is None
                    and self.events
                    and self.events[-1].get("event") != "run_start"
                ):
                    return self.events[-1]
                if self._startup_result is not None or self._ended:
                    break
                left = deadline - time.monotonic()
                if left <= 0:
                    timed_out = True
                    break
                self._changed.wait(min(left, 1.0))
        if timed_out:
            raise ServeError(f"vehicle_sil_launch serve did not become ready within {limit} s{self._tail()}")
        raise ServeError(f"vehicle_sil_launch serve ended before becoming ready{self._tail()}")

    def send(self, command: Mapping) -> None:
        """Fault-injection raw JSON line write to stdin."""
        self.send_raw((json.dumps(command) + "\n").encode())

    def send_raw(self, data: bytes) -> None:
        """Fault-injection raw bytes write to stdin (e.g. batched or malformed commands)."""
        with self._write_lock:
            if self.process.stdin is None or self.process.stdin.closed:
                return
            try:
                self.process.stdin.write(data)
                self.process.stdin.flush()
            except (BrokenPipeError, ValueError, OSError):
                pass

    def run_once(
        self,
        request: Optional[os.PathLike] = None,
        timeline: Optional[os.PathLike] = None,
        timeout: float = 120.0,
        intent: Optional[Mapping] = None,
    ) -> RunRecord:
        """Execute one Run via `vehicle_sil_control.Serve.start()` and `RunHandle.wait()`."""
        self.ready(timeout)
        handle = self.start(
            request=request,
            timeline=timeline,
            intent=intent,
            timeout=scaled(timeout),
        )
        handle.wait(timeout=scaled(timeout))
        res_ev = handle.result_event
        assert res_ev is not None
        tl = read_json(timeline) if timeline is not None and os.path.exists(timeline) else None
        return RunRecord(handle.id, handle.start_event, res_ev, self.window(handle.id), tl)

    def signal(self, number: int) -> None:
        """Send a POSIX signal to the launcher process."""
        os.kill(self.process.pid, number)

    def kill(self) -> None:
        if self.process.poll() is None:
            if WINDOWS:
                self.process.kill()
            else:
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        try:
            self.process.wait(30)
        except subprocess.TimeoutExpired:
            pass

    def wait(self, timeout: float = 60.0) -> int:
        try:
            return super().wait(timeout=scaled(timeout))
        except ServeError:
            self.kill()
            raise ServeError(
                f"the launcher did not end within {scaled(timeout):.0f} s: it was killed{self._tail()}"
            ) from None


# --- Linux: what a process holds --------------------------------------------------------------------------------


def descendants(pid: int) -> List[int]:
    """The processes below `pid` (Linux; [] elsewhere)."""
    proc = pathlib.Path("/proc")
    if not proc.is_dir():
        return []
    task_dir = proc / str(pid) / "task"
    if task_dir.is_dir():
        try:
            out_fast: List[int] = []
            seen = {pid}
            queue = [pid]
            while queue:
                cur = queue.pop(0)
                cur_task = proc / str(cur) / "task"
                for tid_entry in cur_task.iterdir():
                    for token in (tid_entry / "children").read_text().split():
                        child = int(token)
                        if child not in seen:
                            seen.add(child)
                            out_fast.append(child)
                            queue.append(child)
            return out_fast
        except (OSError, ValueError):
            pass
    parent: Dict[int, int] = {}
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
        except OSError:
            continue
        fields = stat[stat.rfind(")") + 2 :].split()
        parent[int(entry.name)] = int(fields[1])
    out: List[int] = []
    wanted = {pid}
    changed = True
    while changed:
        changed = False
        for child, p in parent.items():
            if p in wanted and child not in wanted:
                wanted.add(child)
                out.append(child)
                changed = True
    return out


def _listening_inodes() -> set:
    inodes = set()
    for name in ("tcp", "tcp6"):
        try:
            lines = pathlib.Path(f"/proc/net/{name}").read_text().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) > 9 and fields[3] == "0A":  # LISTEN
                inodes.add(fields[9])
    return inodes


def resources(pid: int, *, check_listen: bool = True) -> Optional[dict]:
    """What the process `pid` holds now (Linux): descriptors, threads, RSS and anonymous memory (KiB),
    child processes, listening TCP sockets."""
    base = pathlib.Path(f"/proc/{pid}")
    if not base.is_dir():
        return None
    try:
        fds = list((base / "fd").iterdir())
        status = (base / "status").read_text()
    except OSError:
        return None
    values = {}
    for line in status.splitlines():
        key, _, rest = line.partition(":")
        if key in ("Threads", "VmRSS", "RssAnon", "VmSize"):
            values[key] = int(rest.split()[0])
    socket_inodes = []
    for fd in fds:
        try:
            target = os.readlink(fd)
        except OSError:
            continue
        if target.startswith("socket:["):
            socket_inodes.append(target[8:-1])
    listen = 0
    if check_listen and socket_inodes:
        listening = _listening_inodes()
        listen = sum(1 for inode in socket_inodes if inode in listening)
    return {
        "fds": len(fds),
        "threads": values.get("Threads"),
        "rss_kb": values.get("VmRSS"),
        "anon_kb": values.get("RssAnon"),
        "vm_kb": values.get("VmSize"),
        "descendants": len(descendants(pid)),
        "listen": listen,
    }


def settled_resources(
    pid: int,
    baseline: Mapping,
    keys: Iterable[str] = ("fds", "threads", "descendants", "listen"),
    timeout: float = 5.0,
    *,
    check_listen: bool = True,
) -> dict:
    """resources(pid), polled briefly until `keys` settle back to `baseline`."""
    active_keys = tuple(k for k in keys if check_listen or k != "listen")
    deadline = time.monotonic() + scaled(timeout)
    while True:
        now = resources(pid, check_listen=check_listen) or {}
        if all(now.get(k) == baseline.get(k) for k in active_keys) or time.monotonic() > deadline:
            return now
        time.sleep(0.05)


def slope(ys: Sequence[float]) -> float:
    """The least-squares slope of ys over 0, 1, 2, ..."""
    n = len(ys)
    if n < 2:
        return 0.0
    mx, my = (n - 1) / 2, sum(ys) / n
    den = sum((x - mx) ** 2 for x in range(n))
    return sum((x - mx) * (y - my) for x, y in enumerate(ys)) / den


def say(text: str) -> None:
    """A line for the test's log (CTest keeps it)."""
    print(text, flush=True)
    sys.stdout.flush()
