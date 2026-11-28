# SPDX-License-Identifier: MIT
"""Pure standard-library protocol constants, request-file serialization, and Events v4 NDJSON command
builders shared between `vehicle_sil_control` and `vehicle_sil_gui` (`python -S` safe)."""
from __future__ import annotations

import json
import os
import tempfile
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

EVENTS_FORMAT: int = 4
REQUEST_VERSION: int = 1
REQUEST_VERSION_KEY: str = "vehicle_sil_run_request"

SPEED_MIN: float = 0.000001
SPEED_MAX: float = 1000000.0
STEP_MS_MAX: int = 10 ** 12
TIME_COMMANDS: tuple[str, ...] = ("pause", "resume", "step", "run_until", "speed")
MAX_COMMAND_ID: int = 1 << 53
MAX_ID: int = MAX_COMMAND_ID
MAX_COMMAND_LINE: int = 64 * 1024

NO_CONNECTION: str = "none"

HELLO_FORMAT_KEY: str = "vehicle_sil_events"
HELLO_LAUNCHER_KEY: str = "launcher"
RUN_KEY: str = "run"

EXIT_IDLE_EOF: int = 0
EXIT_USAGE: int = 4
EXIT_CONTAMINATED: int = 6
EXIT_INTERRUPTED: int = 130

REASON_RUN_REQUIRED: str = "run-required"
REASON_RUN_CHANGED: str = "run-changed"
REASON_BUSY: str = "busy"
REASON_LAUNCHER_ENDING: str = "launcher-ending"
REASON_NOT_PREPARED: str = "not-prepared"
REASON_CONTAMINATED: str = "contaminated"
REASON_TRACE_ENABLED: str = "trace-enabled"


class InvalidRequest(ValueError):
    """A run/check/time request body failed schema or range validation."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def validate_name_sequence(seq: Sequence[str], name: str, *, kind: str = "participant names") -> list[str]:
    if isinstance(seq, (str, bytes)):
        raise TypeError(f"{name} must be a sequence of {kind}, not {type(seq).__name__}")
    out: list[str] = []
    for item in seq:
        if not isinstance(item, str) or not item:
            raise ValueError(f"{name} elements must be non-empty strings")
        out.append(item)
    return out


def speed_argument(speed: float | str) -> str:
    """A speed as --speed and the speed command take it: max, or the shortest exact decimal."""
    if speed == "max":
        return "max"
    text = repr(float(speed))
    return text[:-2] if text.endswith(".0") else text


def encode_ndjson(payload: Mapping[str, Any]) -> bytes:
    """Compact ASCII JSON bytes for one NDJSON command (without trailing newline)."""
    text = json.dumps(payload, ensure_ascii=True, allow_nan=False, separators=(",", ":"))
    return text.encode("ascii")


def command_line(command: Mapping[str, Any], *, run_id: str | None = None) -> bytes:
    """The line written to the launcher's standard input for a validated time command."""
    if command["command"] == "speed":
        value = command["value"]
        text = '"max"' if value == "max" else speed_argument(value)
        number = f',"id":{command["id"]}' if "id" in command else ""
        run_part = f',"run":{json.dumps(run_id)}' if run_id is not None else ""
        return f'{{"command":"speed","value":{text}{number}{run_part}}}\n'.encode()
    payload = {**command, "run": run_id} if run_id is not None else dict(command)
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode()


def stop_command_line(
    run_id: str | None = None,
    *,
    force: bool | None = None,
    command_id: int | None = None,
) -> bytes:
    """The line written to the launcher's standard input to ask the current run to stop."""
    if run_id is None and force is None and command_id is None:
        return b'{"command":"stop"}\n'
    payload: dict[str, object] = {"command": "stop"}
    if run_id is not None:
        payload["run"] = run_id
    if force is not None:
        payload["force"] = force
    if command_id is not None:
        payload["id"] = command_id
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode()


def serve_run_payload(
    request: str | os.PathLike | None = None,
    timeline: str | os.PathLike | None = None,
    command_id: int | None = None,
    *,
    intent: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Builds the dict payload for an Events v4 `run` command."""
    payload: dict[str, Any] = {"command": "run"}
    if command_id is not None:
        payload["id"] = command_id
    if request is not None:
        payload["request"] = str(request)
    if intent is not None:
        payload["intent"] = dict(intent)
    if timeline is not None:
        payload["timeline"] = str(timeline)
    return payload


def serve_run_command(
    request: str | os.PathLike | None = None,
    timeline: str | os.PathLike | None = None,
    command_id: int | None = None,
    *,
    intent: Mapping[str, Any] | None = None,
) -> bytes:
    """Builds the NDJSON `run` command written to `vehicle_sil_launch serve` stdin."""
    payload = serve_run_payload(request, timeline, command_id, intent=intent)
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode()


def serve_prepare_command(
    *,
    variant: str | None = None,
    disable: Sequence[str] = (),
    enable: Sequence[str] = (),
    manual: Sequence[str] = (),
    command_id: int | None = None,
) -> bytes:
    """Builds the NDJSON `prepare` command written to `vehicle_sil_launch serve` stdin."""
    payload: dict[str, object] = {"command": "prepare"}
    if variant is not None:
        payload["variant"] = variant
    if disable:
        payload["disable"] = list(disable)
    if enable:
        payload["enable"] = list(enable)
    if manual:
        payload["manual"] = list(manual)
    if command_id is not None:
        payload["id"] = command_id
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode()


def request_text(intent: dict) -> str:
    """The text of the request file for a validated run intent."""
    return json.dumps({REQUEST_VERSION_KEY: 1, **intent}, ensure_ascii=False, indent=2) + "\n"


def write_request_file(intent: dict, *, prefix: str = "vehicle-sil-gui-request-") -> Path:
    """A new request file for this run or check, readable by this user only."""
    try:
        data = request_text(intent).encode("utf-8")
    except UnicodeEncodeError:
        raise InvalidRequest(
            "invalid-field",
            "a text of the request is not valid Unicode (it has a lone surrogate such as \\ud800): "
            "send the text as UTF-8",
        ) from None
    fd, name = tempfile.mkstemp(prefix=prefix, suffix=".json")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
    except BaseException:
        Path(name).unlink(missing_ok=True)
        raise
    return Path(name)
