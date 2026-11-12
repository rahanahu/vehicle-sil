# SPDX-License-Identifier: MIT
"""vehicle_sil_launch run with its control server, from Python (standard library only).

    with Launch("build/.../vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch:
        with Control(launch.address) as c:
            ...
    print(launch.result["result"])

The launcher writes its events on its standard output (run --events); the event "control_server"
says where the server listens. See docs/control-api.md.
"""
from __future__ import annotations

import json
import os
import subprocess
import threading
import time
from typing import Iterable, List, Mapping, Optional, Sequence, Union


class LaunchError(Exception):
    """The launcher did not get as far as asked (the message says what it wrote)."""


def control_address(events: Iterable[Union[str, bytes, Mapping]]) -> str:
    """The address of the first "control_server" event among `events` (lines of run --events, or
    events already read as dicts). LaunchError if the result comes, or the events end, first."""
    for event in events:
        if not isinstance(event, Mapping):
            text = event.decode("utf-8", "replace") if isinstance(event, bytes) else event
            if not text.strip():
                continue
            event = json.loads(text)
        if event.get("event") == "control_server":
            return event["address"]
        if event.get("event") == "result":
            raise LaunchError(f"the run ended before its control server started: {_why(event)}")
    raise LaunchError("the events ended before the control server started")


def _why(result_event: Mapping) -> str:
    result = result_event.get("result") or {}
    findings = list(result.get("findings") or []) + list((result.get("check") or {}).get("findings") or [])
    errors = [f["message"] for f in findings if f.get("severity") == "error"]
    return f"result {result.get('result')!r}" + (": " + "; ".join(errors) if errors else "")


class Launch:
    """`vehicle_sil_launch run --events --stdin-commands --control <address> <config>` as a child
    process. Its events are read on a thread of their own (`events`, `result`).

    `address`: where the server listens ("0": 127.0.0.1, any free port). `start_paused`: the
    simulation stands still before its first step until a resume, step or run_until (a write
    made then takes effect at 0). `arguments`: more arguments of run (--speed, --disable, ...).
    `stderr`: where the launcher's own failures go (None: this process's standard error). A token
    is never an argument: give --control-credentials <file> in `arguments`.

    `stderr=subprocess.PIPE` is read on a thread of its own into `stderr_text` (the launcher writes
    there only its own failures with --events). `events` keeps every event of the run, the "log"
    events too (`keep_logs=False` leaves those out, for a long run with much output). A line of the
    output that is no JSON is kept in `bad_lines` and skipped."""

    def __init__(self, launcher: Union[str, os.PathLike], config: Union[str, os.PathLike], *, address: str = "0",
                 start_paused: bool = False, arguments: Sequence[str] = (), cwd: Optional[str] = None, stderr=None,
                 keep_logs: bool = True):
        command = [os.fspath(launcher), "run", "--events", "--stdin-commands", "--control", address]
        if start_paused:
            command.append("--start-paused")
        command += list(arguments) + ["--", os.fspath(config)]
        self.events: List[dict] = []
        self.result: Optional[dict] = None
        self.bad_lines: List[bytes] = []
        self.stderr_text = ""
        self._keep_logs = keep_logs
        self._changed = threading.Condition()
        self._ended = False
        self._process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr, cwd=cwd)
        self._reader = threading.Thread(target=self._read, name="vehicle_sil_launch events", daemon=True)
        self._reader.start()
        self._stderr_reader = None
        if stderr == subprocess.PIPE:
            self._stderr_reader = threading.Thread(target=self._read_stderr, name="vehicle_sil_launch stderr", daemon=True)
            self._stderr_reader.start()

    def _read(self) -> None:
        try:
            for line in self._process.stdout:
                if not line.endswith(b"\n"):
                    break  # a line cut short: the launcher was ended
                try:
                    event = json.loads(line)
                except ValueError:
                    with self._changed:
                        self.bad_lines.append(line)
                    continue
                with self._changed:
                    if self._keep_logs or event.get("event") != "log":
                        self.events.append(event)
                    if event.get("event") == "result":
                        self.result = event["result"]
                    self._changed.notify_all()
        except (OSError, ValueError):
            pass  # the pipe was closed under the reader
        finally:
            with self._changed:
                self._ended = True
                self._changed.notify_all()

    def _read_stderr(self) -> None:
        try:
            for line in self._process.stderr:
                self.stderr_text += line.decode("utf-8", "replace")
        except (OSError, ValueError):
            pass

    def wait_for_event(self, event: str, timeout: float = 60.0, after: Optional[Mapping] = None, **keys) -> dict:
        """The first event named `event` whose keys have the values in `keys` (waits for it); with
        `after` (an event), the first one written after it (by "seq"). LaunchError if the events
        end first or the time is over."""
        deadline = time.monotonic() + timeout
        after_seq = -1 if after is None else after["seq"]
        with self._changed:
            while True:
                for e in self.events:
                    if (e.get("seq", -1) > after_seq and e.get("event") == event
                            and all(e.get(k) == v for k, v in keys.items())):
                        return e
                if self.result is not None or self._ended:
                    why = _why({"result": self.result}) if self.result is not None else "its events ended"
                    raise LaunchError(f"vehicle_sil_launch wrote no {event} event {keys or ''}: {why}")
                left = deadline - time.monotonic()
                if left <= 0:
                    raise LaunchError(f"no {event} event {keys or ''} within {timeout} s")
                self._changed.wait(left)

    @property
    def address(self) -> str:
        """Where the control server listens (waits for the event "control_server")."""
        return self.wait_for_event("control_server")["address"]

    def send(self, command: Mapping) -> None:
        """One command on the launcher's standard input ({"command": "stop"}, ...)."""
        try:
            self._process.stdin.write((json.dumps(command) + "\n").encode())
            self._process.stdin.flush()
        except (BrokenPipeError, ValueError, OSError):
            pass  # it has ended, or is ending

    def stop(self) -> None:
        """Stops the run in order (as Ctrl+C)."""
        self.send({"command": "stop"})

    def wait(self, timeout: float = 120.0) -> dict:
        """Waits for the launcher to end; its result (the result event's "result"). LaunchError if
        it does not end in time (it is then ended) or wrote no result."""
        try:
            self._process.wait(timeout)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait()
            self._reader.join(10)
            raise LaunchError(f"vehicle_sil_launch did not end within {timeout} s: it was killed") from None
        self._reader.join(10)
        if self._stderr_reader is not None:
            self._stderr_reader.join(10)
        if self.result is None:
            raise LaunchError(f"vehicle_sil_launch ended with code {self._process.returncode} and wrote no result")
        return self.result

    @property
    def returncode(self) -> Optional[int]:
        return self._process.poll()

    def __enter__(self) -> "Launch":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        """Stops the launcher if it still runs (a stop, the end of its input, at most 60 s, then
        it is killed) and closes the pipes. Called by `with`; safe to call more than once. After
        Ctrl+C in a terminal the launcher got the interrupt too, so this stop is its second request:
        it ends the participants without waiting for them."""
        if self._process.poll() is None:
            self.stop()
        try:
            self._process.stdin.close()  # the end of input stops it as well
        except OSError:
            pass
        try:
            self._process.wait(60)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait()
        self._reader.join(10)
        if not self._reader.is_alive():
            self._process.stdout.close()
        if self._stderr_reader is not None:
            self._stderr_reader.join(10)
            if not self._stderr_reader.is_alive():
                self._process.stderr.close()
