# SPDX-License-Identifier: MIT
"""`vehicle_sil_launch serve` as a resident child process speaking Events v4 (standard library only).

    with Serve("build/.../vehicle_sil_launch", "simulation.yaml", control=True) as runner:
        prep = runner.prepare()
        with runner.start(start_paused=True) as run:
            with run.control() as c:   # bound to run.id
                c.sim.wait_paused()
                c.sim.resume()
            print(run.wait()["result"])
        with runner.start(scenario="scenarios/smoke.yaml") as run2:
            print(run2.wait()["result"])

`Serve` and `RunHandle` use only the standard library; gRPC is imported lazily when
`RunHandle.control()` is called. See docs/control-api.md.
"""
from __future__ import annotations

import collections
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Callable, Mapping, Sequence
from typing import TYPE_CHECKING, Any, Dict, List, Optional, Union

from .launcher import LaunchError, _why
from .protocol import (
    MAX_COMMAND_ID,
    MAX_COMMAND_LINE,
    REASON_TRACE_ENABLED,
    REQUEST_VERSION,
    encode_ndjson,
    validate_name_sequence,
)

if TYPE_CHECKING:
    from .client import Control

_MAX_BAD_LINES = 64
_MAX_STDERR_CHARS = 64 * 1024
_STATUS_ENVELOPE_KEYS = frozenset(("event", "seq", "t_ms", "id"))


class _PendingCommand:
    __slots__ = ("kind", "reply", "refusal")

    def __init__(self, kind: str) -> None:
        self.kind = kind
        self.reply: Any = None
        self.refusal: Optional[dict] = None


class ServeError(LaunchError):
    """The resident launcher (`vehicle_sil_launch serve`) failed, timed out, or refused a command."""

    def __init__(self, message: str, *, reason: Optional[str] = None) -> None:
        super().__init__(message)
        self.reason = reason


def _encode_ndjson(payload: Mapping[str, Any]) -> bytes:
    try:
        return encode_ndjson(payload)
    except (TypeError, ValueError) as exc:
        raise ServeError(f"invalid command payload: {exc}") from exc


def _startup_failure_reason(result_event: Mapping[str, Any]) -> Optional[str]:
    res = result_event.get("result") or {}
    findings = list(res.get("findings") or []) + list((res.get("check") or {}).get("findings") or [])
    for finding in findings:
        if finding.get("code") == REASON_TRACE_ENABLED:
            return REASON_TRACE_ENABLED
    reason = res.get("result")
    return reason if isinstance(reason, str) else None


class RunHandle:
    """One run inside a resident `Serve` launcher (`run_id` bound)."""

    def __init__(self, serve: "Serve", run_id: str, run_start_event: dict) -> None:
        self._serve = serve
        self._id = run_id
        self.start_event: dict = run_start_event
        self._events: List[dict] = [run_start_event]
        self._result_event: Optional[dict] = None
        self.result: Optional[dict] = None
        self.status: Optional[dict] = None
        self.launcher_ends: Optional[bool] = None

    @property
    def id(self) -> str:
        return self._id

    @property
    def run_id(self) -> str:
        return self._id

    @property
    def result_event(self) -> Optional[dict]:
        with self._serve._changed:
            return self._result_event

    @property
    def events(self) -> List[dict]:
        with self._serve._changed:
            return list(self._events)

    def _append_event(self, event: dict, keep_logs: bool) -> None:
        if keep_logs or event.get("event") != "log":
            self._events.append(event)

    def wait_for_event(self, event: str, timeout: float = 60.0, after: Optional[Mapping] = None, **keys) -> dict:
        """The first event of this run named `event` whose keys have the values in `keys`."""
        deadline = time.monotonic() + timeout
        after_seq = -1 if after is None else after["seq"]
        with self._serve._changed:
            while True:
                for e in self._events:
                    if (
                        e.get("seq", -1) > after_seq
                        and e.get("event") == event
                        and all(e.get(k) == v for k, v in keys.items())
                    ):
                        return e
                if self._result_event is not None or self._serve._ended:
                    why = _why(self._result_event) if self._result_event is not None else "its events ended"
                    raise ServeError(f"run {self._id} wrote no {event} event {keys or ''}: {why}")
                self._serve._check_not_reader_thread(f"RunHandle.wait_for_event({event!r})")
                left = deadline - time.monotonic()
                if left <= 0:
                    raise ServeError(f"run {self._id}: no {event} event {keys or ''} within {timeout} s")
                self._serve._changed.wait(left)

    @property
    def address(self) -> str:
        """Where this run's Control Server listens (waits for the `control_server` event)."""
        return self.wait_for_event("control_server")["address"]

    def control(self, **options: Any) -> "Control":
        """Connect a gRPC `Control` client bound to this run (`run=self.id`)."""
        if "run" in options:
            raise TypeError("control() binds to this run itself: do not pass run=...")
        addr = self.address
        from .client import Control

        return Control(addr, run=self._id, **options)

    def send(self, command: Mapping[str, Any]) -> None:
        """Send one stdin command targeted at this run."""
        if not isinstance(command, Mapping):
            raise TypeError("command must be a mapping")
        if "run" in command and command["run"] != self._id:
            raise ValueError(f"command names run {command['run']!r}, expected {self._id!r}")
        payload = {**command, "run": self._id}
        line_bytes = _encode_ndjson(payload)
        if len(line_bytes) > MAX_COMMAND_LINE:
            raise ServeError("command exceeds 64 KiB line limit", reason="invalid-command")
        self._serve._write_line(line_bytes)

    def stop(self, force: Optional[bool] = None, *, id: Optional[int] = None) -> None:
        """Stop this run (`force=False` graceful, `force=True` immediate, `None` default graceful)."""
        if force is not None and not isinstance(force, bool):
            raise TypeError("force must be True, False, or None")
        with self._serve._changed:
            if self._result_event is not None or self._serve._ended:
                return
        payload: Dict[str, Any] = {"command": "stop", "run": self._id}
        if force is not None:
            payload["force"] = force
        if id is not None:
            payload["id"] = id
        line_bytes = _encode_ndjson(payload)
        try:
            self._serve._write_line(line_bytes)
        except ServeError:
            pass

    def wait(self, timeout: float = 120.0) -> dict:
        """Wait for this run's Events v4 `result` event (which embeds settled `ExecutionStatus`)."""
        deadline = time.monotonic() + timeout
        with self._serve._changed:
            while self._result_event is None and not self._serve._ended:
                self._serve._check_not_reader_thread("RunHandle.wait() before result is processed")
                left = deadline - time.monotonic()
                if left <= 0:
                    raise ServeError(f"run {self._id} did not finish within {timeout} s")
                self._serve._changed.wait(left)
            if self._result_event is None:
                raise ServeError(f"run {self._id} ended without writing a result")
            return self._result_event["result"]

    def __enter__(self) -> "RunHandle":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        with self._serve._changed:
            need_stop = self._result_event is None and not self._serve._ended
        if need_stop:
            self.stop(force=False)
            if exc_type is not None:
                try:
                    self.wait()
                except ServeError:
                    pass
            else:
                self.wait()


class Serve:
    """`vehicle_sil_launch serve [--once] <config>` as a resident child process (standard library only)."""

    def __init__(
        self,
        launcher: Union[str, os.PathLike],
        config: Union[str, os.PathLike],
        *,
        once: bool = False,
        control: bool = False,
        address: str = "0",
        credentials: Optional[Union[str, os.PathLike]] = None,
        tls_cert: Optional[Union[str, os.PathLike]] = None,
        tls_key: Optional[Union[str, os.PathLike]] = None,
        audit_log: Optional[Union[str, os.PathLike]] = None,
        log_dir: Optional[Union[str, os.PathLike]] = None,
        timeout_ms: Optional[int] = None,
        describe_timeout_ms: Optional[int] = None,
        participant_start_timeout_ms: Optional[int] = None,
        participant_stop_timeout_ms: Optional[int] = None,
        events_stall_ms: Optional[int] = None,
        arguments: Sequence[str] = (),
        cwd: Optional[Union[str, os.PathLike]] = None,
        env: Optional[Mapping[str, str]] = None,
        stdout: Optional[Any] = subprocess.PIPE,
        stderr: Optional[Any] = None,
        keep_logs: bool = True,
        on_event: Optional[Callable[[dict], None]] = None,
        start_new_session: bool = False,
        wait_ready: bool = True,
        startup_timeout: float = 30.0,
    ) -> None:
        if not control:
            if any(x is not None for x in (credentials, tls_cert, tls_key, audit_log)):
                raise ValueError("credentials, tls_cert, tls_key, and audit_log require control=True")
            if address != "0":
                raise ValueError("address requires control=True")
        if isinstance(arguments, (str, bytes)):
            raise TypeError("arguments must be a sequence of strings, not str/bytes")
        if on_event is not None and not callable(on_event):
            raise TypeError("on_event must be callable or None")
        for opt_name, opt_val in (
            ("timeout_ms", timeout_ms),
            ("describe_timeout_ms", describe_timeout_ms),
            ("participant_start_timeout_ms", participant_start_timeout_ms),
            ("participant_stop_timeout_ms", participant_stop_timeout_ms),
            ("events_stall_ms", events_stall_ms),
        ):
            if opt_val is not None and (isinstance(opt_val, bool) or not isinstance(opt_val, int)):
                raise TypeError(f"{opt_name} must be an int")

        command: List[str] = [os.fspath(launcher), "serve"]
        if once:
            command.append("--once")
        if control:
            command += ["--control", address]
            if credentials is not None:
                command += ["--control-credentials", os.fspath(credentials)]
            if tls_cert is not None:
                command += ["--control-tls-cert", os.fspath(tls_cert)]
            if tls_key is not None:
                command += ["--control-tls-key", os.fspath(tls_key)]
            if audit_log is not None:
                command += ["--control-audit-log", os.fspath(audit_log)]
        if log_dir is not None:
            command += ["--log-dir", os.fspath(log_dir)]
        if timeout_ms is not None:
            command += ["--timeout-ms", str(timeout_ms)]
        if describe_timeout_ms is not None:
            command += ["--describe-timeout-ms", str(describe_timeout_ms)]
        if participant_start_timeout_ms is not None:
            command += ["--participant-start-timeout-ms", str(participant_start_timeout_ms)]
        if participant_stop_timeout_ms is not None:
            command += ["--participant-stop-timeout-ms", str(participant_stop_timeout_ms)]
        if events_stall_ms is not None:
            command += ["--events-stall-ms", str(events_stall_ms)]
        command += list(arguments) + ["--", os.fspath(config)]

        self.command: List[str] = command
        self._bad_lines: collections.deque[bytes] = collections.deque(maxlen=_MAX_BAD_LINES)
        self._keep_logs = keep_logs
        self._on_event = on_event
        self._changed = threading.Condition()
        self._write_lock = threading.Lock()
        self._next_id = 1
        self._hello: Optional[dict] = None
        self._ready = False
        self._latest_status: Optional[dict] = None
        self._active_run: Optional[RunHandle] = None
        self._ended = False
        self._closed = False
        self._startup_result: Optional[dict] = None
        self._pending: Dict[int, _PendingCommand] = {}
        self._temp_files: set[str] = set()
        self._stderr_chunks: collections.deque[str] = collections.deque()
        self._stderr_len = 0

        popen_kwargs: Dict[str, Any] = {}
        if start_new_session:
            if os.name == "nt":
                popen_kwargs["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
            else:
                popen_kwargs["start_new_session"] = True

        try:
            self._process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=stdout,
                stderr=stderr,
                cwd=os.fspath(cwd) if cwd is not None else None,
                env=None if env is None else dict(env),
                **popen_kwargs,
            )
        except OSError as exc:
            raise ServeError(f"cannot start {os.fspath(launcher)}: {exc}") from exc

        self._reader: Optional[threading.Thread] = None
        if stdout == subprocess.PIPE:
            self._reader = threading.Thread(target=self._read, name="vehicle_sil_launch serve events", daemon=True)
            self._reader.start()
        else:
            self._ended = True
        self._stderr_reader: Optional[threading.Thread] = None
        if stderr == subprocess.PIPE:
            self._stderr_reader = threading.Thread(
                target=self._read_stderr, name="vehicle_sil_launch serve stderr", daemon=True
            )
            self._stderr_reader.start()

        if not wait_ready:
            return

        deadline = time.monotonic() + startup_timeout
        timed_out = False
        with self._changed:
            while True:
                if self._hello is not None and self._ready:
                    return
                if self._startup_result is not None or self._ended:
                    break
                left = deadline - time.monotonic()
                if left <= 0:
                    timed_out = True
                    break
                self._changed.wait(left)

        self.close(timeout=10.0)
        with self._changed:
            startup_result = self._startup_result
            ended = self._ended
            stderr_tail = "".join(self._stderr_chunks).strip()
        if startup_result is not None:
            reason = _startup_failure_reason(startup_result)
            raise ServeError(f"vehicle_sil_launch serve failed at startup: {_why(startup_result)}", reason=reason)
        if timed_out:
            raise ServeError(f"vehicle_sil_launch serve did not become ready within {startup_timeout} s")
        code = self._process.returncode
        detail = f": {stderr_tail[-400:]}" if stderr_tail else ""
        if ended:
            raise ServeError(f"vehicle_sil_launch serve ended during startup (exit code {code}){detail}")
        raise ServeError(f"vehicle_sil_launch serve failed to start (exit code {code}){detail}")

    def _dispatch_event(self, event: dict) -> None:
        if self._on_event is not None:
            try:
                self._on_event(dict(event))
            except Exception:
                sys.excepthook(*sys.exc_info())
        with self._changed:
            etype = event.get("event")
            run_id = event.get("run")
            if etype == "hello":
                self._hello = event
            elif etype == "status":
                self._ready = True
                self._latest_status = {k: v for k, v in event.items() if k not in _STATUS_ENVELOPE_KEYS}
                cmd_id = event.get("id")
                if isinstance(cmd_id, int) and not isinstance(cmd_id, bool):
                    slot = self._pending.get(cmd_id)
                    if slot is not None and slot.kind == "status":
                        slot.reply = event
                    slot = None
            elif etype == "prepare":
                self._ready = True
                if isinstance(event.get("status"), dict):
                    self._latest_status = event["status"]
                cmd_id = event.get("id")
                if isinstance(cmd_id, int) and not isinstance(cmd_id, bool):
                    slot = self._pending.get(cmd_id)
                    if slot is not None and slot.kind == "prepare":
                        slot.reply = event
                    slot = None
            elif etype == "run_start" and run_id:
                handle = RunHandle(self, run_id, event)
                self._active_run = handle
                cmd_id = event.get("id")
                if isinstance(cmd_id, int) and not isinstance(cmd_id, bool):
                    slot = self._pending.get(cmd_id)
                    if slot is not None and slot.kind == "run":
                        slot.reply = handle
                    slot = None
                handle = None
            elif etype == "command" and run_id is None and event.get("accepted") is False:
                cmd_id = event.get("id")
                if (
                    isinstance(cmd_id, int)
                    and not isinstance(cmd_id, bool)
                    and event.get("command") in ("run", "prepare", "status", None)
                ):
                    slot = self._pending.get(cmd_id)
                    if slot is not None and (
                        event.get("command") is None or event.get("command") == slot.kind
                    ):
                        slot.refusal = event
                    slot = None
            elif etype == "result" and run_id is None:
                self._startup_result = event
            if (
                run_id is not None
                and self._active_run is not None
                and self._active_run._id == run_id
            ):
                handle = self._active_run
                if etype != "run_start":
                    handle._append_event(event, self._keep_logs)
                if etype == "result":
                    handle._result_event = event
                    handle.result = event.get("result")
                    handle.status = event.get("status")
                    handle.launcher_ends = event.get("launcher_ends")
                    if isinstance(handle.status, dict):
                        self._latest_status = handle.status
                    self._active_run = None
                handle = None
            self._changed.notify_all()

    def _read(self) -> None:
        try:
            assert self._process.stdout is not None
            for line in self._process.stdout:
                if not line.endswith(b"\n"):
                    line = b""
                    break
                try:
                    event = json.loads(line)
                except ValueError:
                    with self._changed:
                        self._bad_lines.append(line)
                    line = b""
                    continue
                if not isinstance(event, dict):
                    with self._changed:
                        self._bad_lines.append(line)
                    event = None
                    line = b""
                    continue
                line = b""
                self._dispatch_event(event)
                event = None
        except (OSError, ValueError):
            pass
        finally:
            with self._changed:
                self._active_run = None
                self._ended = True
                self._changed.notify_all()

    def _read_stderr(self) -> None:
        try:
            assert self._process.stderr is not None
            for line in self._process.stderr:
                text = line.decode("utf-8", "replace")
                line = b""
                with self._changed:
                    if len(text) >= _MAX_STDERR_CHARS:
                        self._stderr_chunks.clear()
                        self._stderr_chunks.append(text[-_MAX_STDERR_CHARS:])
                        self._stderr_len = _MAX_STDERR_CHARS
                    else:
                        self._stderr_chunks.append(text)
                        self._stderr_len += len(text)
                        while self._stderr_len > _MAX_STDERR_CHARS and self._stderr_chunks:
                            overflow = self._stderr_len - _MAX_STDERR_CHARS
                            head = self._stderr_chunks[0]
                            if len(head) <= overflow:
                                self._stderr_chunks.popleft()
                                self._stderr_len -= len(head)
                            else:
                                self._stderr_chunks[0] = head[overflow:]
                                self._stderr_len -= overflow
                text = ""
        except (OSError, ValueError):
            pass

    def _check_not_reader_thread(self, action: str) -> None:
        if self._reader is not None and threading.current_thread() is self._reader:
            raise ServeError(f"cannot call {action} from inside on_event")

    @property
    def process(self) -> subprocess.Popen:
        return self._process

    @property
    def bad_lines(self) -> List[bytes]:
        with self._changed:
            return list(self._bad_lines)

    def _wait_hello(self, timeout: float = 30.0) -> dict:
        deadline = time.monotonic() + timeout
        with self._changed:
            while self._hello is None:
                if self._ended:
                    raise ServeError("vehicle_sil_launch serve events ended without hello")
                self._check_not_reader_thread("hello/instance before hello is processed")
                left = deadline - time.monotonic()
                if left <= 0:
                    raise ServeError(f"no hello within {timeout} s")
                self._changed.wait(left)
            return dict(self._hello)

    @property
    def hello(self) -> dict:
        return self._wait_hello(timeout=30.0)

    @property
    def instance(self) -> str:
        hello = self._wait_hello(timeout=30.0)
        if not hello or "launcher" not in hello:
            raise ServeError("hello event has no launcher instance ID")
        return hello["launcher"]

    @property
    def latest_status(self) -> Optional[dict]:
        with self._changed:
            return dict(self._latest_status) if self._latest_status is not None else None

    @property
    def pid(self) -> int:
        return self._process.pid

    @property
    def returncode(self) -> Optional[int]:
        return self._process.poll()

    @property
    def stderr_text(self) -> str:
        with self._changed:
            return "".join(self._stderr_chunks)

    def _next_command_id(self) -> int:
        if self._closed or self._ended:
            raise ServeError("vehicle_sil_launch serve has ended")
        for _ in range(len(self._pending) + 1):
            cmd_id = self._next_id
            self._next_id = 1 if cmd_id >= MAX_COMMAND_ID else cmd_id + 1
            if cmd_id not in self._pending:
                return cmd_id
        raise ServeError("no available command id")

    def _register_pending(self, cmd_id: int, kind: str) -> _PendingCommand:
        if isinstance(cmd_id, bool) or not isinstance(cmd_id, int):
            raise TypeError("id must be an int")
        with self._changed:
            if cmd_id in self._pending:
                raise ServeError(f"command id {cmd_id} is already pending", reason="invalid-command")
            slot = _PendingCommand(kind)
            self._pending[cmd_id] = slot
            return slot

    def _write_line(self, line_bytes: bytes) -> None:
        wire = line_bytes + b"\n"
        with self._write_lock:
            if self._process.stdin is None or self._process.stdin.closed:
                raise ServeError("vehicle_sil_launch serve stdin is closed")
            try:
                self._process.stdin.write(wire)
                self._process.stdin.flush()
            except (BrokenPipeError, ValueError, OSError) as exc:
                raise ServeError(f"cannot write command to vehicle_sil_launch serve: {exc}") from exc

    def _unlink_temp(self, path: Optional[str]) -> None:
        if path is None:
            return
        with self._changed:
            self._temp_files.discard(path)
        try:
            os.unlink(path)
        except OSError:
            pass

    def prepare(
        self,
        *,
        variant: Optional[str] = None,
        disable: Sequence[str] = (),
        enable: Sequence[str] = (),
        manual: Sequence[str] = (),
        id: Optional[int] = None,
        timeout: float = 30.0,
    ) -> dict:
        """Prepare the configuration (`prepare` command) and return the `prepare` reply event."""
        self._check_not_reader_thread("prepare()")
        if variant is not None and (not isinstance(variant, str) or not variant):
            raise ValueError("variant must be a non-empty string")
        disable_list = validate_name_sequence(disable, "disable")
        enable_list = validate_name_sequence(enable, "enable")
        manual_list = validate_name_sequence(manual, "manual")

        with self._changed:
            cmd_id = self._next_command_id() if id is None else id

        payload: Dict[str, Any] = {"command": "prepare", "id": cmd_id}
        if variant is not None:
            payload["variant"] = variant
        if disable_list:
            payload["disable"] = disable_list
        if enable_list:
            payload["enable"] = enable_list
        if manual_list:
            payload["manual"] = manual_list
        line_bytes = _encode_ndjson(payload)
        if len(line_bytes) > MAX_COMMAND_LINE:
            raise ServeError("prepare command exceeds 64 KiB line limit", reason="invalid-command")

        slot = self._register_pending(cmd_id, "prepare")
        try:
            self._write_line(line_bytes)
            deadline = time.monotonic() + timeout
            with self._changed:
                while True:
                    if slot.reply is not None:
                        return slot.reply
                    if slot.refusal is not None:
                        ev = slot.refusal
                        raise ServeError(ev.get("message") or "prepare refused", reason=ev.get("reason"))
                    if self._ended:
                        raise ServeError("vehicle_sil_launch serve ended before answering prepare")
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise ServeError(f"prepare did not answer within {timeout} s")
                    self._changed.wait(left)
        finally:
            with self._changed:
                if self._pending.get(cmd_id) is slot:
                    self._pending.pop(cmd_id, None)

    def status(self, timeout: float = 30.0, *, id: Optional[int] = None) -> dict:
        """Query the current `ExecutionStatus` (`status` command) and return the `status` event."""
        self._check_not_reader_thread("status()")
        with self._changed:
            cmd_id = self._next_command_id() if id is None else id

        line_bytes = _encode_ndjson({"command": "status", "id": cmd_id})
        slot = self._register_pending(cmd_id, "status")
        try:
            self._write_line(line_bytes)
            deadline = time.monotonic() + timeout
            with self._changed:
                while True:
                    if slot.reply is not None:
                        return slot.reply
                    if slot.refusal is not None:
                        ev = slot.refusal
                        raise ServeError(ev.get("message") or "status refused", reason=ev.get("reason"))
                    if self._ended:
                        raise ServeError("vehicle_sil_launch serve ended before answering status")
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise ServeError(f"status did not answer within {timeout} s")
                    self._changed.wait(left)
        finally:
            with self._changed:
                if self._pending.get(cmd_id) is slot:
                    self._pending.pop(cmd_id, None)

    def start(
        self,
        *,
        request: Optional[Union[str, os.PathLike]] = None,
        intent: Optional[Mapping[str, Any]] = None,
        variant: Optional[str] = None,
        params: Optional[Mapping[str, Union[bool, int, float, str]]] = None,
        scenario: Optional[Union[str, os.PathLike]] = None,
        disable: Sequence[str] = (),
        enable: Sequence[str] = (),
        manual: Sequence[str] = (),
        connect: Optional[Mapping[str, str]] = None,
        time_policy: Optional[str] = None,
        speed: Optional[Union[float, int, str]] = None,
        start_paused: Optional[bool] = None,
        strict: Optional[bool] = None,
        stop_at_ms: Optional[int] = None,
        late_tolerance_ms: Optional[int] = None,
        timeline: Optional[Union[str, os.PathLike]] = None,
        id: Optional[int] = None,
        timeout: float = 60.0,
    ) -> RunHandle:
        """Start one simulation run (`run` command) and return its `RunHandle`."""
        self._check_not_reader_thread("start()")
        disable_list = validate_name_sequence(disable, "disable")
        enable_list = validate_name_sequence(enable, "enable")
        manual_list = validate_name_sequence(manual, "manual")

        has_request = request is not None
        has_intent = intent is not None
        has_kwargs = (
            any(
                v is not None
                for v in (
                    variant,
                    params,
                    scenario,
                    connect,
                    time_policy,
                    speed,
                    start_paused,
                    strict,
                    stop_at_ms,
                    late_tolerance_ms,
                )
            )
            or bool(disable_list or enable_list or manual_list)
        )
        if int(has_request) + int(has_intent) + int(has_kwargs) > 1:
            raise ValueError("pass at most one of request, intent, or intent keyword arguments (variant, scenario, ...)")

        intent_dict: Optional[Dict[str, Any]] = None
        if has_intent:
            if not isinstance(intent, Mapping):
                raise TypeError("intent must be a mapping")
            intent_dict = dict(intent)
        elif has_kwargs:
            intent_dict = {}
            if variant is not None:
                intent_dict["variant"] = variant
            if scenario is not None:
                intent_dict["scenario"] = os.fspath(scenario)
            if params is not None:
                if not isinstance(params, Mapping):
                    raise TypeError("params must be a mapping")
                intent_dict["params"] = dict(params)
            if disable_list:
                intent_dict["disable"] = disable_list
            if enable_list:
                intent_dict["enable"] = enable_list
            if manual_list:
                intent_dict["manual"] = manual_list
            if connect is not None:
                if not isinstance(connect, Mapping):
                    raise TypeError("connect must be a mapping")
                intent_dict["connect"] = dict(connect)
            if time_policy is not None:
                intent_dict["time_policy"] = time_policy
            if speed is not None:
                if isinstance(speed, bool):
                    raise TypeError("speed cannot be a bool")
                intent_dict["speed"] = speed
            if start_paused is not None:
                if not isinstance(start_paused, bool):
                    raise TypeError("start_paused must be a bool")
                intent_dict["start_paused"] = start_paused
            if strict is not None:
                if not isinstance(strict, bool):
                    raise TypeError("strict must be a bool")
                intent_dict["strict"] = strict
            if stop_at_ms is not None:
                if isinstance(stop_at_ms, bool) or not isinstance(stop_at_ms, int):
                    raise TypeError("stop_at_ms must be an int")
                intent_dict["stop_at_ms"] = stop_at_ms
            if late_tolerance_ms is not None:
                if isinstance(late_tolerance_ms, bool) or not isinstance(late_tolerance_ms, int):
                    raise TypeError("late_tolerance_ms must be an int")
                intent_dict["late_tolerance_ms"] = late_tolerance_ms

        with self._changed:
            cmd_id = self._next_command_id() if id is None else id

        base_payload: Dict[str, Any] = {"command": "run", "id": cmd_id}
        if timeline is not None:
            base_payload["timeline"] = os.fspath(timeline)

        temp_request_path: Optional[str] = None
        if has_request:
            line_bytes = _encode_ndjson({**base_payload, "request": os.fspath(request)})
            if len(line_bytes) > MAX_COMMAND_LINE:
                raise ServeError("run command exceeds 64 KiB line limit", reason="invalid-command")
        elif intent_dict is not None:
            line_bytes = _encode_ndjson({**base_payload, "intent": intent_dict})
            if len(line_bytes) > MAX_COMMAND_LINE:
                if "vehicle_sil_run_request" in intent_dict:
                    file_payload = dict(intent_dict)
                else:
                    file_payload = {"vehicle_sil_run_request": REQUEST_VERSION, **intent_dict}
                file_bytes = _encode_ndjson(file_payload)
                fd, temp_request_path = tempfile.mkstemp(prefix="vehicle-sil-serve-request-", suffix=".json")
                try:
                    with os.fdopen(fd, "wb") as handle:
                        handle.write(file_bytes)
                except BaseException:
                    self._unlink_temp(temp_request_path)
                    raise
                with self._changed:
                    self._temp_files.add(temp_request_path)
                line_bytes = _encode_ndjson({**base_payload, "request": temp_request_path})
                if len(line_bytes) > MAX_COMMAND_LINE:
                    self._unlink_temp(temp_request_path)
                    raise ServeError("run command exceeds 64 KiB line limit", reason="invalid-command")
        else:
            line_bytes = _encode_ndjson(base_payload)
            if len(line_bytes) > MAX_COMMAND_LINE:
                raise ServeError("run command exceeds 64 KiB line limit", reason="invalid-command")

        try:
            slot = self._register_pending(cmd_id, "run")
        except BaseException:
            self._unlink_temp(temp_request_path)
            raise

        temp_to_delete: Optional[str] = None
        try:
            try:
                self._write_line(line_bytes)
            except BaseException:
                temp_to_delete = temp_request_path
                temp_request_path = None
                raise

            deadline = time.monotonic() + timeout
            with self._changed:
                while True:
                    if slot.reply is not None:
                        run_handle = slot.reply
                        temp_to_delete = temp_request_path
                        temp_request_path = None
                        break
                    if slot.refusal is not None:
                        ev = slot.refusal
                        temp_to_delete = temp_request_path
                        temp_request_path = None
                        raise ServeError(
                            ev.get("message") or f"run refused ({ev.get('reason')})",
                            reason=ev.get("reason"),
                        )
                    if self._ended:
                        temp_to_delete = temp_request_path
                        temp_request_path = None
                        raise ServeError("vehicle_sil_launch serve ended before starting the run")
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise ServeError(f"run did not start within {timeout} s")
                    self._changed.wait(left)
        finally:
            with self._changed:
                if self._pending.get(cmd_id) is slot:
                    self._pending.pop(cmd_id, None)
            self._unlink_temp(temp_to_delete)

        return run_handle

    def close_input(self) -> None:
        """Close stdin (EOF shutdown) without waiting for the child process."""
        with self._write_lock:
            if self._process.stdin is not None and not self._process.stdin.closed:
                try:
                    self._process.stdin.close()
                except OSError:
                    pass

    def wait(self, timeout: float = 60.0) -> int:
        """Wait for the launcher child process to exit and return its exit code."""
        self._check_not_reader_thread("wait()")
        try:
            self._process.wait(timeout)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait()
            raise ServeError(f"vehicle_sil_launch serve did not end within {timeout} s: it was killed") from None
        if self._reader is not None:
            self._reader.join(10.0)
        if self._stderr_reader is not None:
            self._stderr_reader.join(10.0)
        assert self._process.returncode is not None
        return self._process.returncode

    def __enter__(self) -> "Serve":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self, timeout: float = 60.0) -> Optional[int]:
        """Close stdin (EOF shutdown), wait for the child process, join reader threads, and clean temp files."""
        self._check_not_reader_thread("close()")
        with self._changed:
            if self._closed:
                return self._process.poll()
            self._closed = True

        self.close_input()

        try:
            self._process.wait(timeout)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait()

        if self._reader is not None:
            self._reader.join(10.0)
            if not self._reader.is_alive() and self._process.stdout is not None:
                try:
                    self._process.stdout.close()
                except OSError:
                    pass
        if self._stderr_reader is not None:
            self._stderr_reader.join(10.0)
            if not self._stderr_reader.is_alive() and self._process.stderr is not None:
                try:
                    self._process.stderr.close()
                except OSError:
                    pass

        with self._changed:
            paths = list(self._temp_files)
            self._temp_files.clear()
        for path in paths:
            try:
                os.unlink(path)
            except OSError:
                pass

        return self._process.returncode
