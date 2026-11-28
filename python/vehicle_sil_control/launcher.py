# SPDX-License-Identifier: MIT
"""Thin 1-run helper over `vehicle_sil_launch serve --once` with its control server (standard library only).

    with Launch("build/.../vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch:
        with Control(launch.address) as c:
            ...
    print(launch.result["result"])

The launcher writes its Events v4 stream on standard output (`serve --once`); the event
`"control_server"` says where the server listens. See docs/control-api.md.
"""
from __future__ import annotations

import json
import os
import shlex
import subprocess
import threading
import time
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple, Union

from .protocol import validate_name_sequence

_MAX_FINDING_DETAIL_CHARS = 500


class LaunchError(Exception):
    """The launcher did not get as far as asked (the message says what it wrote)."""


def control_address(events: Iterable[Union[str, bytes, Mapping]]) -> str:
    """The address of the first "control_server" event among `events` (NDJSON lines, or
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


def _format_finding(finding: Mapping) -> str:
    msg = str(finding.get("message") or "")
    raw_detail = finding.get("detail")
    if isinstance(raw_detail, str):
        detail = raw_detail.strip()
        if detail:
            if len(detail) > _MAX_FINDING_DETAIL_CHARS:
                head_len = _MAX_FINDING_DETAIL_CHARS * 2 // 5
                tail_len = _MAX_FINDING_DETAIL_CHARS - head_len
                detail = f"{detail[:head_len]} ... {detail[-tail_len:]}"
            return f"{msg} ({detail})" if msg else detail
    return msg


def _why(result_event: Mapping) -> str:
    result = result_event.get("result") or {}
    findings = list(result.get("findings") or []) + list((result.get("check") or {}).get("findings") or [])
    errors = [text for f in findings if f.get("severity") == "error" for text in (_format_finding(f),) if text]
    return f"result {result.get('result')!r}" + (": " + "; ".join(errors) if errors else "")


def _participant_pids(events: Iterable[Mapping[str, Any]]) -> Dict[str, int]:
    """Participant name -> PID from Events v4 `participant`/`started` events seen so far."""
    pids: Dict[str, int] = {}
    for e in events:
        if e.get("event") == "participant" and e.get("state") == "started":
            name = e.get("name")
            pid = e.get("pid")
            if isinstance(name, str) and isinstance(pid, int) and not isinstance(pid, bool):
                pids[name] = pid
    return pids


_HOST_VALUE_OPTIONS = frozenset({
    "--control-credentials",
    "--control-tls-cert",
    "--control-tls-key",
    "--control-audit-log",
    "--log-dir",
    "--timeout-ms",
    "--describe-timeout-ms",
    "--participant-start-timeout-ms",
    "--participant-stop-timeout-ms",
    "--events-stall-ms",
})


def _split_once_arguments(
    arguments: Sequence[str],
    *,
    start_paused: bool,
    case: Optional[str] = None,
    params: Optional[Mapping[str, Union[bool, int, float, str]]] = None,
    after_scenario: Optional[str] = None,
    record: Sequence[str] = (),
) -> Tuple[List[str], Dict[str, Any]]:
    """Splits legacy single-run CLI `arguments` into `serve --once` host CLI flags and the initial
    stdin `{"command": "run", ...}` payload."""
    host_args: List[str] = []
    intent: Dict[str, Any] = {}
    run_cmd: Dict[str, Any] = {"command": "run"}
    if start_paused:
        intent["start_paused"] = True
    if case is not None:
        if not isinstance(case, str):
            raise TypeError("case must be a str")
        intent["case"] = case
    if params is not None:
        if not isinstance(params, Mapping):
            raise TypeError("params must be a mapping")
        intent["params"] = dict(params)
    if after_scenario is not None:
        if not isinstance(after_scenario, str):
            raise TypeError("after_scenario must be a str")
        intent["after_scenario"] = after_scenario
    if record:
        record_list = list(dict.fromkeys(validate_name_sequence(record, "record", kind="signal names")))
        if record_list:
            intent["record"] = record_list

    i = 0
    n = len(arguments)
    while i < n:
        arg = arguments[i]
        if arg in _HOST_VALUE_OPTIONS and i + 1 < n:
            host_args.extend((arg, arguments[i + 1]))
            i += 2
        elif arg == "--start-paused":
            intent["start_paused"] = True
            i += 1
        elif arg == "--strict":
            intent["strict"] = True
            i += 1
        elif arg == "--stop-at-ms" and i + 1 < n:
            intent["stop_at_ms"] = int(arguments[i + 1])
            i += 2
        elif arg == "--late-tolerance-ms" and i + 1 < n:
            intent["late_tolerance_ms"] = int(arguments[i + 1])
            i += 2
        elif arg == "--speed" and i + 1 < n:
            val = arguments[i + 1]
            intent["speed"] = "max" if val == "max" else float(val)
            i += 2
        elif arg == "--scenario" and i + 1 < n:
            intent["scenario"] = arguments[i + 1]
            i += 2
        elif arg == "--case" and i + 1 < n:
            val = arguments[i + 1]
            if "case" in intent:
                raise ValueError(f"case is given more than once ({intent['case']!r} and {val!r})")
            intent["case"] = val
            i += 2
        elif arg == "--after-scenario" and i + 1 < n:
            val = arguments[i + 1]
            if "after_scenario" in intent:
                raise ValueError(
                    f"after_scenario is given more than once ({intent['after_scenario']!r} and {val!r})"
                )
            intent["after_scenario"] = val
            i += 2
        elif arg == "--variant" and i + 1 < n:
            intent["variant"] = arguments[i + 1]
            i += 2
        elif arg == "--time-policy" and i + 1 < n:
            intent["time_policy"] = arguments[i + 1]
            i += 2
        elif arg in ("--disable", "--enable", "--manual", "--record") and i + 1 < n:
            key = arg[2:]
            val = arguments[i + 1]
            if key == "record" and not val:
                raise ValueError("record elements must be non-empty strings")
            entries = intent.setdefault(key, [])
            if key != "record" or val not in entries:
                entries.append(val)
            i += 2
        elif arg == "--param" and i + 1 < n:
            raw = arguments[i + 1]
            k, sep, v = raw.partition("=")
            if not sep:
                raise ValueError(f"--param {raw!r}: write --param <name>=<value>")
            cli_params = intent.setdefault("cli_params", {})
            if k in cli_params:
                raise ValueError(
                    f"--param {k} is given twice ({cli_params[k]} and {v}): give each parameter one value"
                )
            cli_params[k] = v
            i += 2
        elif arg == "--connect" and i + 1 < n:
            net, _, target = arguments[i + 1].partition("=")
            intent.setdefault("connect", {})[net] = target
            i += 2
        elif arg == "--request" and i + 1 < n:
            run_cmd["request"] = arguments[i + 1]
            i += 2
        elif arg == "--timeline" and i + 1 < n:
            run_cmd["timeline"] = arguments[i + 1]
            i += 2
        else:
            host_args.append(arg)
            i += 1

    if "request" in run_cmd and intent:
        raise ValueError(
            "cannot combine --request with inline run options "
            f"({', '.join(sorted(intent))}): put them in the request file or omit --request"
        )
    if intent:
        run_cmd["intent"] = intent
    return host_args, run_cmd


class Launch:
    """`vehicle_sil_launch serve --once --control <address> <config>` as a 1-run child process.
    Its Events v4 stream is read on a background thread (`events`, `result`)."""

    def __init__(
        self,
        launcher: Union[str, os.PathLike],
        config: Union[str, os.PathLike],
        *,
        address: str = "0",
        start_paused: bool = False,
        case: Optional[str] = None,
        params: Optional[Mapping[str, Union[bool, int, float, str]]] = None,
        after_scenario: Optional[str] = None,
        record: Sequence[str] = (),
        arguments: Sequence[str] = (),
        cwd: Optional[str] = None,
        stderr=None,
        keep_logs: bool = True,
    ):
        host_args, run_cmd = _split_once_arguments(
            arguments,
            start_paused=start_paused,
            case=case,
            params=params,
            after_scenario=after_scenario,
            record=record,
        )
        command = [
            os.fspath(launcher),
            "serve",
            "--once",
            "--control",
            address,
            *host_args,
            "--",
            os.fspath(config),
        ]
        self.command: List[str] = command
        self.events: List[dict] = []
        self.result: Optional[dict] = None
        self.bad_lines: List[bytes] = []
        self.stderr_text = ""
        self._keep_logs = keep_logs
        self._changed = threading.Condition()
        self._ended = False
        self._process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=stderr,
            cwd=cwd,
        )
        self._stderr_reader = None
        if stderr == subprocess.PIPE:
            self._stderr_reader = threading.Thread(
                target=self._read_stderr, name="vehicle_sil_launch stderr", daemon=True
            )
            self._stderr_reader.start()
        self._reader = threading.Thread(target=self._read, name="vehicle_sil_launch events", daemon=True)
        self._reader.start()
        self.send(run_cmd)

    def _read(self) -> None:
        try:
            for line in self._process.stdout:
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
                    if event.get("event") == "result":
                        self.result = event["result"]
                    self._changed.notify_all()
        except (OSError, ValueError):
            pass
        finally:
            if self._stderr_reader is not None:
                self._stderr_reader.join(0.2)
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
                    if (
                        e.get("seq", -1) > after_seq
                        and e.get("event") == event
                        and all(e.get(k) == v for k, v in keys.items())
                    ):
                        return e
                if self.result is not None or self._ended:
                    if self.result is not None:
                        why = _why({"result": self.result})
                    else:
                        code = self._process.poll()
                        code_part = f"exit code {code}" if code is not None else "exit code unknown"
                        stderr_tail = self.stderr_text.strip()
                        detail = f": {stderr_tail[-400:]}" if stderr_tail else ""
                        why = f"its events ended ({code_part}; command: {shlex.join(self.command)}){detail}"
                    raise LaunchError(f"vehicle_sil_launch wrote no {event} event {keys or ''}: {why}")
                left = deadline - time.monotonic()
                if left <= 0:
                    raise LaunchError(f"no {event} event {keys or ''} within {timeout} s")
                self._changed.wait(left)

    @property
    def pid(self) -> int:
        """The child launcher process PID."""
        return self._process.pid

    @property
    def participant_pids(self) -> Dict[str, int]:
        """Mapping of participant name -> PID from Events v4 `participant`/`started` events seen so far."""
        with self._changed:
            return _participant_pids(self.events)

    @property
    def address(self) -> str:
        """Where the control server listens (waits for the event "control_server")."""
        return self.wait_for_event("control_server")["address"]

    def send(self, command: Mapping) -> None:
        """One command on the launcher's standard input ({"command": "stop"}, ...)."""
        try:
            self._process.stdin.write((json.dumps(command, separators=(",", ":")) + "\n").encode())
            self._process.stdin.flush()
        except (BrokenPipeError, ValueError, OSError):
            pass

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
            stderr_tail = self.stderr_text.strip()
            detail = f": {stderr_tail[-400:]}" if stderr_tail else ""
            raise LaunchError(
                f"vehicle_sil_launch ended with code {self._process.returncode} and wrote no result"
                f" (command: {shlex.join(self.command)}){detail}"
            )
        return self.result

    @property
    def returncode(self) -> Optional[int]:
        return self._process.poll()

    def __enter__(self) -> "Launch":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        """Stops the launcher if it still runs and closes the pipes."""
        if self._process.poll() is None:
            self.stop()
        try:
            self._process.stdin.close()
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
