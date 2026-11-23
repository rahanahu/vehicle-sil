# SPDX-License-Identifier: MIT
"""vehicle_sil_launch serve as a child process, for the tests of tests/serve (standard library only).

- Serve: starts `vehicle_sil_launch serve <file>`, writes commands to its standard input, reads its events on a
  thread of its own, waits for idle, run_start and result, sends signals, and collects the exit code.
- cold(): the same request as one `vehicle_sil_launch run --json` of its own (a new process), for the comparison.
- Scratch: a directory of the test's own with request files, configuration files and behaviour files.
- resources(): what a launcher holds on Linux (descriptors, threads, child processes, listening sockets, RSS).

The names that the launcher's documentation settles (the verb, the hello's format, the exit codes, the reasons of a
refusal, the variable of the delay of the fault injection) are the constants below, one place each.

Environment (CMake sets it, tests/serve/CMakeLists.txt): VEHICLE_SIL_LAUNCH_EXE, VEHICLE_SIL_TEST_ECU,
VEHICLE_SIL_EXAMPLE_DIR (the build's examples/), VEHICLE_SIL_SERVE_SCRATCH, VEHICLE_SIL_SERVE_LIN_MASTER and
VEHICLE_SIL_SERVE_LIN_SLAVE, VEHICLE_SIL_SERVE_RUNS_1 and VEHICLE_SIL_SERVE_RUNS_10 (how many Runs the repetitions
make), VEHICLE_SIL_SERVE_FAULT_DELAY (ON when the launcher was built with the delay of its close),
VEHICLE_SIL_SERVE_ASAN (ON when built with AddressSanitizer), and the factor
of the waits (VEHICLE_SIL_TEST_TIMEOUT_SCALE, else VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT).
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
import threading
import time
import unittest
from typing import Callable, Dict, Iterable, List, Mapping, Optional, Sequence

# --- The names of the contract -------------------------------------------------------------------------------
SERVE_VERB = "serve"
# hello: the key of the format and its value for serve (None: any number; run --events writes 2).
HELLO_FORMAT_KEY = "vehicle_sil_events"
SERVE_EVENTS_FORMAT: Optional[int] = None
HELLO_LAUNCHER_KEY = "launcher"  # the launcher's instance (16 hexadecimal digits)
RUN_KEY = "run"  # the key that names the Run on a line and in a command
EXIT_IDLE_EOF = 0  # the end of standard input while idle
EXIT_INTERRUPTED = 130  # a signal, the end of input during a Run, a closed output
EXIT_USAGE = 4  # a usage error at the start (trace included)
EXIT_CONTAMINATED = 6  # the launcher ended after a Run it could not close (launcher_ends)
REASON_RUN_REQUIRED = "run-required"
REASON_RUN_CHANGED = "run-changed"
REASON_BUSY = "busy"
# The fault injection of a test build: how long the launcher sleeps before it closes its own SIL Kit connections.
FAULT_DELAY_ENV = "VEHICLE_SIL_FAULT_CLOSE_DELAY_MS"
TRACE_ENV = "VEHICLE_SIL_TRACE"
# The gRPC metadata of the Run (also in vehicle_sil_control).
RUN_METADATA = "vehicle-sil-run"
# ---------------------------------------------------------------------------------------------------------------

EXE = ".exe" if os.name == "nt" else ""
WINDOWS = os.name == "nt"


def _scale() -> float:
    text = os.environ.get("VEHICLE_SIL_TEST_TIMEOUT_SCALE") or os.environ.get("VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT") or "1"
    try:
        value = float(text)
    except ValueError:
        value = -1.0
    if not value > 0 or not all(c.isdigit() or c == "." for c in text) or text.count(".") > 1:
        raise SystemExit(f"VEHICLE_SIL_TEST_TIMEOUT_SCALE={text!r} is not a number greater than 0 in digits "
                         "(such as 3 or 2.5): set it to such a factor, or unset it")
    return value


SCALE = _scale()


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


class ServeError(AssertionError):
    """The launcher did not do what the test waited for (the message says what it wrote)."""


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


def cold(launch: os.PathLike, config: os.PathLike, request: Optional[os.PathLike] = None,
         timeline: Optional[os.PathLike] = None, arguments: Sequence[str] = (), env: Optional[Mapping] = None,
         timeout: float = 120.0) -> Cold:
    command = [os.fspath(launch), "run", "--json", *arguments]
    if request is not None:
        command += ["--request", os.fspath(request)]
    if timeline is not None:
        command += ["--timeline", os.fspath(timeline)]
    command += ["--", os.fspath(config)]
    done = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, timeout=scaled(timeout),
                          env=None if env is None else dict(env))
    try:
        result = json.loads(done.stdout)
    except ValueError:
        result = None
    tl = read_json(timeline) if timeline is not None and os.path.exists(timeline) else None
    return Cold(done.returncode, result, tl, done.stderr.decode("utf-8", "replace"))


# --- serve ------------------------------------------------------------------------------------------------------

class Serve:
    """`vehicle_sil_launch serve [arguments] <config>` as a child process (in a process group of its own, so that a
    signal the test sends reaches the launcher only, as Ctrl+C in its terminal would).

    `events` keeps every line of its standard output (log lines too, unless keep_logs=False); `bad_lines` the lines
    that are no JSON. `stderr_text` its standard error. A last line without a line end is left out (the launcher
    was killed)."""

    _ids = itertools.count(1000)

    def __init__(self, launch: os.PathLike, config: os.PathLike, arguments: Sequence[str] = (), *,
                 env: Optional[Mapping] = None, cwd: Optional[os.PathLike] = None, keep_logs: bool = True,
                 stdout=subprocess.PIPE):
        self.command = [os.fspath(launch), SERVE_VERB, *arguments, "--", os.fspath(config)]
        self.events: List[dict] = []
        self.bad_lines: List[bytes] = []
        self.stderr_text = ""
        self._keep_logs = keep_logs
        self._changed = threading.Condition()
        self._ended = False
        options: Dict = {}
        if WINDOWS:
            options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            options["start_new_session"] = True
        self.process = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=stdout, stderr=subprocess.PIPE,
                                        cwd=None if cwd is None else os.fspath(cwd),
                                        env=None if env is None else dict(env), **options)
        self._reader = None
        if stdout == subprocess.PIPE:
            self._reader = threading.Thread(target=self._read, name="serve events", daemon=True)
            self._reader.start()
        else:
            self._ended = True
        self._stderr_reader = threading.Thread(target=self._read_stderr, name="serve stderr", daemon=True)
        self._stderr_reader.start()

    # The reading threads.
    def _read(self) -> None:
        try:
            for line in self.process.stdout:
                if not line.endswith(b"\n"):
                    break
                try:
                    event = json.loads(line)
                except ValueError:
                    with self._changed:
                        self.bad_lines.append(line)
                    continue
                with self._changed:
                    if self._keep_logs or event.get("event") != "log":
                        self.events.append(event)
                    else:
                        # The stream checks need every seq: keep a log line's skeleton.
                        self.events.append({k: event.get(k) for k in ("event", "seq", "t_ms", RUN_KEY) if k in event})
                    self._changed.notify_all()
        except (OSError, ValueError):
            pass
        finally:
            with self._changed:
                self._ended = True
                self._changed.notify_all()

    def _read_stderr(self) -> None:
        try:
            for line in self.process.stderr:
                with self._changed:
                    self.stderr_text += line.decode("utf-8", "replace")
        except (OSError, ValueError):
            pass

    # Waiting for lines.
    def wait_for(self, condition: Callable[[dict], bool], what: str, timeout: float = 60.0,
                 after: Optional[Mapping] = None) -> dict:
        """The first line after `after` (by seq) for which condition() holds. ServeError if the stream ends or the
        (scaled) time is over first."""
        deadline = time.monotonic() + scaled(timeout)
        after_seq = -1 if after is None else after["seq"]
        with self._changed:
            start = 0
            while True:
                for index in range(start, len(self.events)):
                    e = self.events[index]
                    if e.get("seq", -1) > after_seq and condition(e):
                        return e
                start = len(self.events)
                if self._ended:
                    raise ServeError(f"the launcher's events ended without {what}{self._tail()}")
                left = deadline - time.monotonic()
                if left <= 0:
                    raise ServeError(f"no {what} within {scaled(timeout):.0f} s{self._tail()}")
                self._changed.wait(min(left, 1.0))

    def wait_event(self, event: str, timeout: float = 60.0, after: Optional[Mapping] = None, **keys) -> dict:
        return self.wait_for(lambda e: e.get("event") == event and all(e.get(k) == v for k, v in keys.items()),
                             f"{event} {keys or ''}", timeout, after)

    def _tail(self, count: int = 8) -> str:
        lines = [json.dumps(e)[:300] for e in self.events[-count:]]
        err = self.stderr_text.strip()[-1500:]
        return ("\n--- last events ---\n" + "\n".join(lines) + ("\n--- stderr ---\n" + err if err else "")
                + f"\n--- exit code: {self.process.poll()}")

    @property
    def hello(self) -> dict:
        return self.wait_event("hello", timeout=30)

    @property
    def instance(self) -> str:
        return self.hello[HELLO_LAUNCHER_KEY]

    def wait_idle(self, after: Optional[Mapping] = None, timeout: float = 60.0) -> dict:
        return self.wait_event("idle", timeout, after)

    def snapshot(self) -> List[dict]:
        with self._changed:
            return list(self.events)

    def last(self) -> Optional[dict]:
        with self._changed:
            return self.events[-1] if self.events else None

    # Writing commands.
    def send(self, command: Mapping) -> None:
        self.send_raw((json.dumps(command) + "\n").encode())

    def send_raw(self, data: bytes) -> None:
        """Bytes on the launcher's standard input in one write (several commands in one write, a part of one)."""
        try:
            self.process.stdin.write(data)
            self.process.stdin.flush()
        except (BrokenPipeError, ValueError, OSError):
            pass

    def run(self, request: Optional[os.PathLike] = None, timeline: Optional[os.PathLike] = None,
            id: Optional[int] = None) -> int:
        """Sends a run command; returns its id (the run_start carries it)."""
        id = next(self._ids) if id is None else id
        self.send(run_command(request, timeline, id))
        return id

    def run_started(self, id: int, after: Optional[Mapping] = None, timeout: float = 60.0) -> dict:
        """The run_start of the run command `id`; ServeError if the launcher refused it."""
        e = self.wait_for(lambda e: e.get("id") == id and (e.get("event") == "run_start" or
                                                           (e.get("event") == "command" and e.get("accepted") is False)),
                          f"run_start of the run command {id}", timeout, after)
        if e["event"] != "run_start":
            raise ServeError(f"the run command {id} was refused: {json.dumps(e)}")
        return e

    def result_of(self, run: str, timeout: float = 120.0) -> dict:
        return self.wait_for(lambda e: e.get("event") == "result" and e.get(RUN_KEY) == run,
                             f"the result of Run {run}", timeout)

    def run_once(self, request: Optional[os.PathLike] = None, timeline: Optional[os.PathLike] = None,
                 timeout: float = 120.0, idle: bool = True) -> "RunRecord":
        """One Run from idle to its result, and to the next idle unless the launcher ends (idle=False: does not wait
        for that idle)."""
        before = self.ready(timeout)
        id = self.run(request, timeline)
        start = self.run_started(id, after=before, timeout=timeout)
        run = start[RUN_KEY]
        result = self.result_of(run, timeout)
        if idle and not result.get("launcher_ends"):
            self.wait_idle(after=result, timeout=timeout)
        tl = read_json(timeline) if timeline is not None and os.path.exists(timeline) else None
        return RunRecord(run, id, start, result, self.window(run), tl)

    def ready(self, timeout: float = 60.0) -> dict:
        """The idle that follows the last Run (waits for it): the launcher takes a run command now."""
        with self._changed:
            boundary = None
            for e in reversed(self.events):
                if e.get("event") == "idle" and boundary is None:
                    return e
                if e.get("event") in ("run_start", "result"):
                    boundary = e
                    break
        return self.wait_idle(after=boundary, timeout=timeout)

    def window(self, run: str) -> List[dict]:
        with self._changed:
            return [e for e in self.events if e.get(RUN_KEY) == run]

    # Ending.
    def close_input(self) -> None:
        try:
            self.process.stdin.close()
        except OSError:
            pass

    def signal(self, number: int) -> None:
        """Sends a signal to the launcher alone (POSIX)."""
        os.kill(self.process.pid, number)

    def wait(self, timeout: float = 60.0) -> int:
        """Waits for the launcher to end; its exit code. ServeError (after killing it) if it does not end in time."""
        try:
            self.process.wait(scaled(timeout))
        except subprocess.TimeoutExpired:
            self.kill()
            raise ServeError(f"the launcher did not end within {scaled(timeout):.0f} s: it was killed{self._tail()}") from None
        if self._reader is not None:
            self._reader.join(scaled(10))
        self._stderr_reader.join(scaled(10))
        return self.process.returncode

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

    def close(self) -> Optional[int]:
        """Ends the launcher if it still runs (the end of its input, then a kill) and closes the pipes."""
        if self.process.poll() is None:
            self.close_input()
            try:
                self.process.wait(scaled(60))
            except subprocess.TimeoutExpired:
                self.kill()
        if self._reader is not None:
            self._reader.join(10)
            if not self._reader.is_alive() and self.process.stdout:
                self.process.stdout.close()
        self._stderr_reader.join(10)
        if not self._stderr_reader.is_alive() and self.process.stderr:
            self.process.stderr.close()
        if self.process.stdin:
            try:
                self.process.stdin.close()
            except OSError:
                pass
        return self.process.returncode

    def __enter__(self) -> "Serve":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def run_command(request: Optional[os.PathLike] = None, timeline: Optional[os.PathLike] = None,
                id: Optional[int] = None) -> dict:
    command: dict = {"command": "run"}
    if id is not None:
        command["id"] = id
    if request is not None:
        command["request"] = pathlib.Path(request).as_posix()
    if timeline is not None:
        command["timeline"] = pathlib.Path(timeline).as_posix()
    return command


class RunRecord:
    """One serve Run: its ID, the id of its run command, its run_start and result lines, the lines of its window and
    its timeline file (None if it has none)."""

    def __init__(self, run: str, id: int, start: dict, result_event: dict, window: List[dict], timeline: Optional[dict]):
        self.run = run
        self.id = id
        self.start = start
        self.result_event = result_event
        self.result = result_event.get("result") or {}
        self.window = window
        self.timeline = timeline

    @property
    def launcher_ends(self) -> bool:
        return bool(self.result_event.get("launcher_ends"))


# --- Linux: what a process holds --------------------------------------------------------------------------------

def descendants(pid: int) -> List[int]:
    """The processes below `pid` (Linux; [] elsewhere)."""
    proc = pathlib.Path("/proc")
    if not proc.is_dir():
        return []
    parent: Dict[int, int] = {}
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
        except OSError:
            continue
        # pid (comm) state ppid ...: comm can hold spaces and parentheses.
        fields = stat[stat.rfind(")") + 2:].split()
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


def resources(pid: int) -> Optional[dict]:
    """What the process `pid` holds now (Linux): descriptors, threads, RSS and anonymous memory (KiB), the processes
    below it, the TCP sockets it listens on. None elsewhere, or if the process has gone."""
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
    listening = _listening_inodes()
    listen = 0
    for fd in fds:
        try:
            target = os.readlink(fd)
        except OSError:
            continue
        if target.startswith("socket:[") and target[8:-1] in listening:
            listen += 1
    return {"fds": len(fds), "threads": values.get("Threads"), "rss_kb": values.get("VmRSS"),
            "anon_kb": values.get("RssAnon"), "vm_kb": values.get("VmSize"), "descendants": len(descendants(pid)),
            "listen": listen}


def settled_resources(pid: int, baseline: Mapping, keys: Iterable[str] = ("fds", "threads", "descendants", "listen"),
                      timeout: float = 5.0) -> dict:
    """resources(pid), taken again for a short while until `keys` are back at `baseline` (a thread or a descriptor of
    the last Run may still be on its way out right after idle). The last one taken."""
    deadline = time.monotonic() + scaled(timeout)
    while True:
        now = resources(pid) or {}
        if all(now.get(k) == baseline.get(k) for k in keys) or time.monotonic() > deadline:
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
