# SPDX-License-Identifier: MIT
"""Pure standard-library protocol constants, run-request validation, and Events v4 NDJSON command
builders shared between `vehicle_sil_control` and `vehicle_sil_gui` (`python -S` safe)."""
from __future__ import annotations

import json
import math
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

INTENT_FIELDS: tuple[str, ...] = (
    "scenario",
    "variant",
    "params",
    "disable",
    "enable",
    "manual",
    "strict",
    "connect",
    "time_policy",
)
REQUEST_FIELDS: tuple[str, ...] = (
    "file",
    "$schema",
    REQUEST_VERSION_KEY,
    *INTENT_FIELDS,
    "speed",
    "start_paused",
    "stop_at_ms",
    "late_tolerance_ms",
)
TEXT_FIELDS: tuple[str, ...] = ("file", "$schema", REQUEST_VERSION_KEY, "variant", "strict", "text")
DESCRIBE_FIELDS: tuple[str, ...] = ("file", "variant")
CONNECTIONS_FIELDS: tuple[str, ...] = ("file", "network")
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


def validate_name_sequence(seq: Sequence[str], name: str) -> list[str]:
    if isinstance(seq, (str, bytes)):
        raise TypeError(f"{name} must be a sequence of participant names, not {type(seq).__name__}")
    out: list[str] = []
    for item in seq:
        if not isinstance(item, str) or not item:
            raise ValueError(f"{name} elements must be non-empty strings")
        out.append(item)
    return out


def check_fields(endpoint: str, body: dict, allowed: tuple[str, ...]) -> None:
    """Refuses a body with a field the endpoint does not take, a field that is null (except file),
    or a request format that is not 1."""
    unknown = sorted(k for k in body if k not in allowed)
    if unknown:
        raise InvalidRequest(
            "invalid-field",
            f"{endpoint} takes no {', '.join(repr(k) for k in unknown)}: "
            f"the fields it takes are {', '.join(allowed)}",
        )
    nulls = sorted(k for k, v in body.items() if v is None and k != "file")
    if nulls:
        raise InvalidRequest(
            "invalid-field",
            f"{endpoint} takes no null: leave out {', '.join(repr(k) for k in nulls)} to choose nothing",
        )
    if "$schema" in body and not isinstance(body["$schema"], str):
        raise InvalidRequest("invalid-field", "$schema must be a text (the path or URL of the schema), or left out")
    version = body.get(REQUEST_VERSION_KEY)
    if REQUEST_VERSION_KEY in body and (isinstance(version, bool) or version != 1):
        raise InvalidRequest(
            "invalid-field",
            f"{REQUEST_VERSION_KEY} must be 1 (the format of the request file) or left out",
        )


def check_variant(variant: object) -> str | None:
    if variant is None:
        return None
    if not isinstance(variant, str) or not variant:
        raise InvalidRequest(
            "invalid-field",
            "variant must be the name of a variant of the file (a non-empty string), or left out",
        )
    return variant


def check_params(params: object) -> dict:
    if params is None:
        return {}
    if not isinstance(params, dict):
        raise InvalidRequest(
            "invalid-field",
            'params must be an object of parameter name to value, e.g. {"idle_rpm": 650}',
        )
    for name, value in params.items():
        ok = isinstance(value, (str, bool, int)) or (isinstance(value, float) and math.isfinite(value))
        if not ok:
            raise InvalidRequest("invalid-field", f"params {name!r} must be a string, a number or true or false")
    return dict(params)


def check_strict(strict: object) -> bool:
    if strict is None:
        return False
    if not isinstance(strict, bool):
        raise InvalidRequest("invalid-field", "strict must be true or false")
    return strict


def check_connect(connect: object) -> dict[str, str]:
    if connect is None:
        return {}
    if not isinstance(connect, dict) or not all(
        isinstance(n, str) and n and isinstance(t, str) for n, t in connect.items()
    ):
        raise InvalidRequest(
            "invalid-field",
            'connect must be an object of network name to target, e.g. '
            '{"ETH_EXT": "enp3s0"}: a target that POST api/connections lists, or '
            '"none" for SIL Kit only',
        )
    return dict(connect)


def check_time_policy(policy: object) -> str | None:
    if policy is None:
        return None
    if not isinstance(policy, str) or not policy:
        raise InvalidRequest(
            "invalid-field",
            'time_policy must be the name of a time policy that the launcher takes (a string, e.g. "free"), or left out',
        )
    return policy


def check_network(network: object) -> str:
    if not isinstance(network, str) or not network:
        raise InvalidRequest(
            "invalid-field",
            'network must be the name of a network of the file (a non-empty string), e.g. '
            '{"file": "simulation.yaml", "network": "ETH_EXT"}',
        )
    return network


def check_run_millis(body: dict, key: str, minimum: int) -> int | None:
    if key not in body:
        return None
    value = body[key]
    if type(value) is not int or not minimum <= value <= STEP_MS_MAX:
        raise InvalidRequest("invalid-field", f"{key} must be an integer from {minimum} to {STEP_MS_MAX}")
    return value


def check_speed(speed: object) -> float | str | None:
    if speed is None or speed == "max":
        return speed
    if isinstance(speed, bool) or not isinstance(speed, (int, float)) or not SPEED_MIN <= speed <= SPEED_MAX:
        raise InvalidRequest(
            "invalid-field",
            f'speed must be "max" or a number from {SPEED_MIN:f} to {SPEED_MAX:.0f} (1 is real time)',
        )
    return float(speed)


def check_disable(disable: object, known: list[str]) -> list[str]:
    return _check_names("disable", disable, known)


def check_manual(manual: object, known: list[str]) -> list[str]:
    return _check_names("manual", manual, known)


def check_enable(enable: object) -> list[str]:
    return _check_names("enable", enable, None)


def check_name_list(key: str, value: object) -> list[str]:
    return _check_names(key, value, None)


def _check_names(key: str, value: object, known: list[str] | None) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(isinstance(n, str) and n for n in value):
        raise InvalidRequest("invalid-field", f"{key} must be a list of participant names")
    unknown = [n for n in value if known is not None and n not in known]
    if unknown:
        raise InvalidRequest(
            "unknown-participant",
            f"no participant named {', '.join(repr(n) for n in unknown)} in this file: the "
            f"participants are {', '.join(known or []) or 'none'}",
        )
    if len(set(value)) != len(value):
        raise InvalidRequest("invalid-field", f"{key} names a participant twice")
    return list(value)


def speed_argument(speed: float | str) -> str:
    """A speed as --speed and the speed command take it: max, or the shortest exact decimal."""
    if speed == "max":
        return "max"
    text = repr(float(speed))
    return text[:-2] if text.endswith(".0") else text


def check_time_command(body: dict) -> dict:
    name = body.get("command")
    if name not in TIME_COMMANDS:
        raise InvalidRequest(
            "invalid-field",
            f"command must be one of {', '.join(TIME_COMMANDS)} (to stop, use POST api/stop)",
        )
    allowed = {"command", "id", *({"step": ("ms",), "run_until": ("at_ms",), "speed": ("value",)}.get(name, ()))}
    extra = sorted(k for k in body if k not in allowed)
    if extra:
        raise InvalidRequest("invalid-field", f"{name} takes no {', '.join(extra)}: send {_time_example(name)}")
    request = {}
    if "id" in body:
        number = body["id"]
        if isinstance(number, bool) or not isinstance(number, int) or not 0 <= number <= MAX_ID:
            raise InvalidRequest(
                "invalid-field",
                f"id must be a whole number from 0 to {MAX_ID} (the launcher's answer carries it back): "
                f"leave it out, or send {_time_example(name, 7)}",
            )
        request["id"] = number
    if name == "step":
        ms = body.get("ms")
        if isinstance(ms, bool) or not isinstance(ms, int) or not 1 <= ms <= STEP_MS_MAX:
            raise InvalidRequest(
                "invalid-field",
                f"step needs ms, a whole number of milliseconds of virtual time from 1 to {STEP_MS_MAX}: "
                f"send {_time_example(name)}",
            )
        return {"command": name, "ms": ms, **request}
    if name == "run_until":
        at_ms = body.get("at_ms")
        if isinstance(at_ms, bool) or not isinstance(at_ms, int) or not 0 <= at_ms <= STEP_MS_MAX:
            raise InvalidRequest(
                "invalid-field",
                f"run_until needs at_ms, the virtual time to stand still at, a whole number of milliseconds "
                f"from 0 to {STEP_MS_MAX}: send {_time_example(name)}",
            )
        return {"command": name, "at_ms": at_ms, **request}
    if name == "speed":
        value = body.get("value")
        if value is None:
            raise InvalidRequest("invalid-field", f"speed needs value: send {_time_example(name)}")
        return {"command": name, "value": "max" if value == "max" else check_speed(value), **request}
    return {"command": name, **request}


def _time_example(name: str, id: int | None = None) -> str:
    text = {
        "step": '{"command":"step","ms":100}',
        "run_until": '{"command":"run_until","at_ms":5000}',
        "speed": '{"command":"speed","value":1}',
    }.get(name, f'{{"command":"{name}"}}')
    return text if id is None else f'{text[:-1]},"id":{id}}}'


def encode_ndjson(payload: Mapping[str, Any]) -> bytes:
    """Compact ASCII JSON bytes for one NDJSON command (without trailing newline)."""
    text = json.dumps(payload, ensure_ascii=True, allow_nan=False, separators=(",", ":"))
    return text.encode("ascii")


def command_line(command: Mapping[str, Any], *, run_id: str | None = None) -> bytes:
    """The line written to the launcher's standard input for a command that check_time_command accepted."""
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


def serve_status_command(command_id: int | None = None) -> bytes:
    """Builds the NDJSON `status` command written to `vehicle_sil_launch serve` stdin."""
    payload: dict[str, object] = {"command": "status"}
    if command_id is not None:
        payload["id"] = command_id
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode()


def request_text(intent: dict) -> str:
    """The text of the request file for an intent that the check_* functions accepted."""
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
