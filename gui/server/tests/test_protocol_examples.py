"""gui/protocol/examples: real conversations with the server and the stand-in launcher, compared with the files.
Of each shape one or two are kept as files (and those that the docs and the other tests read); the others are
checked by what they show (SHOWN).

The values that change from run to run (process IDs, times, the instance, the paths of this computer, counts
that depend on timing) are replaced with fixed ones before the comparison. UPDATE_PROTOCOL_EXAMPLES=1 writes the
files instead. Every file in the directory must be one that these tests write (EXAMPLES below).
"""

from __future__ import annotations

import asyncio
from functools import lru_cache
import json
import os
import re
import shutil
import signal
import sys
from pathlib import Path

import jsonschema
import pytest
import yaml
from referencing import Registry
from referencing.jsonschema import DRAFT202012
from conftest import (CONFIGS, CONTROL_FEATURES, REPLAY, REPOSITORY, Client, gui_server, is_ended, is_state, is_synced, manual_pages,
                      run_async, until_async)
from fake_runtime import _validate_run_state, _validate_status_members, validate_events_v4_event
from vehicle_sil_gui import __version__
from vehicle_sil_gui.contracts import ErrorCode, HTTP_ERROR_CODES
from vehicle_sil_gui.launcher import REQUIRED_FEATURES
from vehicle_sil_gui.openapi import (ERROR_DESCRIPTIONS, TARGETS as OPENAPI_TARGETS,
                                     check_all as check_openapi_drift)
from vehicle_sil_gui.server import route_specs

PROTOCOL_DIR = Path(__file__).resolve().parents[2] / "protocol"
DIRECTORY = PROTOCOL_DIR / "examples"
OPENAPI_PATH = PROTOCOL_DIR / "openapi.yaml"
SCHEMAS_DIR = PROTOCOL_DIR / "schemas"
WS_COMMAND_SCHEMA_PATH = SCHEMAS_DIR / "ws-command-event.schema.json"
WS_SERVER_SCHEMA_PATH = SCHEMAS_DIR / "ws-server.schema.json"
WS_CLIENT_SCHEMA_PATH = SCHEMAS_DIR / "ws-client.schema.json"
CLI_JSON_SCHEMA_PATH = SCHEMAS_DIR / "cli-json.schema.json"
TIMELINE_SCHEMA_PATH = REPOSITORY / "schema" / "timeline.schema.json"
UPDATE = os.environ.get("UPDATE_PROTOCOL_EXAMPLES") == "1"
POSIX = sys.platform != "win32"
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+(?:-(?:rc|beta|alpha)\.\d+)?$")

EXAMPLES = {
    "app", "configs", "prepare", "timeline", "session-none", "session-running", "session-ended", "check-passed", "check-disabled",
    "check-failed", "error-check-failed", "run", "stop", "stop-force", "stop-ended",
    "error-unauthorized", "error-forbidden-host", "error-forbidden-origin", "error-cors-not-allowed",
    "error-not-found", "error-method-not-allowed", "error-too-large", "error-unsupported-media-type",
    "error-invalid-json", "error-invalid-field", "error-invalid-path", "error-outside-root",
    "error-not-a-configuration", "error-unknown-participant", "error-unsupported", "error-file-not-found",
    "error-already-running", "error-check-busy", "error-check-timeout", "error-launcher-output",
    "error-launcher-unusable", "error-launcher-start", "error-shutting-down", "error-too-many-connections",
    "error-not-running", "time", "ws-time", "ws-late", "ws-control-trouble",
    "ws-pass", "ws-log-dropped", "ws-participant-crash", "ws-launcher-crash", "ws-bad-lines", "ws-bad-types",
    "ws-orphan", "ws-stop", "ws-force-stop", "ws-replay", "ws-resume", "ws-dropped", "ws-slow-client",
    "ws-resync", "ws-unauthorized", "ws-large-fields", "ws-loose-fields", "ws-kept-limit",
    "write", "ws-variables", "ws-variables-failed", "error-unknown-variable", "error-not-writable", "error-session-changed",
    "error-variables-not-ready", "error-invalid-watch", "dashboard", "dashboard-errors", "dashboard-none",
    "scenarios", "check-scenario", "error-scenario-not-listed", "ws-scenario", "recording", "error-scenario-running",
    "check-manual", "ws-manual", "ws-allowed", "connections", "check-connect", "run-connect",
    "error-target-not-listed", "error-unknown-network", "ws-availability",
    "config", "check-text", "describe", "error-text-too-large", "error-config-too-large", "error-config-unreadable",
    "save-config", "error-commands-changed", "error-config-changed", "error-save-disabled",
}


# Of these, the files are kept for one or two of each shape and for those that the docs, the client library's
# tests and the screen's tests read. The others are of a shape a kept file has: they are compared by what they
# show (an error's status and code; how a conversation ends), not kept as files.


def last_session(example: dict) -> dict:
    return [m["session"] for m in example["server"] if m["type"] == "session"][-1]


def ended(result: str, interrupted_by=None, problems: dict | None = None, states: list | None = None):
    def check(example: dict) -> None:
        s = last_session(example)
        assert (s["state"], s["result"]["result"], s["result"]["interrupted_by"]) == ("ended", result, interrupted_by)
        assert {p["code"] for p in s["problems"]} == set(problems or {})
        for code, n in (problems or {}).items():
            assert sum(p["code"] == code for p in s["problems"]) >= n
            assert sum(m["type"] == "error" and m["code"] == code for m in example["server"]) >= n
        if states is not None:
            assert [q["state"] for q in s["participants"]] == states
    return check


def resumed(example: dict) -> None:
    kinds = [m["type"] for m in example["server"]]
    assert kinds[0] == "hello" and kinds[-1] == "synced" and set(kinds[1:-1]) == {"event"}
    after = example["client"][0]["after_stream_seq"]
    assert all(m["stream_seq"] > after for m in example["server"][1:-1])


SHOWN = {
    "session-none": lambda e: (e["status"], e["response"]["session"], e["response"]["allowed"]["start"]) ==
    (200, None, {"ok": True, "reason": None, "message": None}),
    "dashboard-none": lambda e: e["status"] == 200 and e["response"]["found"] is False
    and e["response"]["dashboard"] is None,
    "ws-bad-types": ended("passed", problems={"bad-event": 9}),
    "ws-orphan": ended("passed", problems={"output-left-open": 1}),
    "ws-stop": ended("stopped", "stop command", states=["ended", "ended", "ended"]),
    "ws-force-stop": ended("stopped", "stop command", states=["ended", "ended", "killed"]),
    "ws-resume": resumed,
    # an error: its HTTP status (also as the table of errors in gui/protocol/README.md says)
    "error-unauthorized": 401, "error-forbidden-host": 403, "error-forbidden-origin": 403,
    "error-cors-not-allowed": 403, "error-not-found": 404, "error-method-not-allowed": 405, "error-too-large": 413,
    "error-unsupported-media-type": 415, "error-invalid-json": 400, "error-invalid-field": 400,
    "error-invalid-path": 400, "error-outside-root": 400, "error-not-a-configuration": 400,
    "error-unknown-participant": 400, "error-unsupported": 400, "error-file-not-found": 404, "error-check-busy": 503,
    "error-check-timeout": 504, "error-launcher-output": 502, "error-launcher-unusable": 503,
    "error-launcher-start": 502, "error-shutting-down": 503, "error-too-many-connections": 503,
    "error-not-running": 409, "error-unknown-variable": 400, "error-variables-not-ready": 409,
    "error-unknown-network": 400, "error-text-too-large": 413, "error-config-too-large": 413,
    "error-config-changed": 409, "error-save-disabled": 403,
}


# The table of errors in gui/protocol/README.md and in its Japanese page: page -> {code: HTTP status}
ERROR_TABLES = {page.relative_to(REPOSITORY).as_posix():
                {code: int(status) for status, code in re.findall(r"^\| (\d{3}) \| `([a-z-]+)` \|",
                                                                  page.read_text(encoding="utf-8"), re.MULTILINE)}
                for page in manual_pages("gui/protocol/README.md")}


def shown(name: str, example: dict) -> None:
    """What an example that is not kept as a file shows: an error's status and code (its name after error-), and
    a message; for the others, what SHOWN says."""
    check = SHOWN[name]
    if isinstance(check, int):
        error = example["response"]["error"]
        code = name.removeprefix("error-")
        assert (example["status"], error["code"]) == (check, code), example
        assert isinstance(error["message"], str) and error["message"], example
        for page, table in ERROR_TABLES.items():
            assert table.get(code) == check, f"{page} says {code} is {table.get(code)}"
        return
    assert check(example) is not False, f"{name}: {json.dumps(example)[:2000]}"


@pytest.fixture(autouse=True)
def no_heartbeat(monkeypatch) -> None:
    """The examples are compared as they are: a heartbeat comes when the machine is slow, so the stand-in writes none,
    and while it waits for a command it does not run on (no time events that depend on timing)."""
    monkeypatch.setenv("REPLAY_HEARTBEAT_MS", "0")
    monkeypatch.setenv("REPLAY_RUN_ON", "0")
    monkeypatch.setenv("REPLAY_NO_SLEEPERS", "1")


class Normalizer:
    def __init__(self, gui, paths: dict[str, str] | None = None):
        self.pids: dict[int, int] = {}
        self.paths = {str(REPLAY): "/path/to/vehicle_sil_launch", str(CONFIGS): "/path/to/root",
                      f"127.0.0.1:{gui.port}": "127.0.0.1:8765", **(paths or {})}
        self.token = gui.token

    def __call__(self, value, key=None):
        if isinstance(value, dict):
            assert "snapshot_rev" not in value, value
            assert "problem_seq" not in value, value
            assert "event_seq" not in value, value
            out = {k: self(v, k) for k, v in value.items()}
            if "vehicle_sil_gui" in out and isinstance(out.get("version"), str):
                assert out["vehicle_sil_gui"] == 4, out
                assert out["version"] == __version__, out
                out["version"] = "<version>"
            if "vehicle_sil" in out and isinstance(out["vehicle_sil"], str):
                if "vehicle_sil_launch" in out:
                    assert out["vehicle_sil"] == __version__, out
                else:
                    assert SEMVER_RE.fullmatch(out["vehicle_sil"]), out["vehicle_sil"]
                out["vehicle_sil"] = "<version>"
            if isinstance(out.get("seq"), int) and "t_ms" in out:
                out["t_ms"] = out["seq"] * 10
            if isinstance(out.get("last_seq"), int) and "last_t_ms" in out:
                out["last_t_ms"] = out["last_seq"] * 10
            # How fast it went in real time (a time event, the result's timing)
            if "actual_speed" in out:
                out["actual_speed"] = None if out["actual_speed"] is None else 1.5
            if isinstance(out.get("wall_ms"), int):
                out["wall_ms"] = 1000
            if isinstance(out.get("paused_ms"), int) and out["paused_ms"] > 0:
                out["paused_ms"] = 500
            return out
        if isinstance(value, list):
            return [self(v) for v in value]
        if key == "pid" and isinstance(value, int):
            return self.pids.setdefault(value, 10001 + len(self.pids))
        if key == "instance" and isinstance(value, str):
            return "0123456789abcdef"
        if key in ("run_id", "id") and isinstance(value, str):
            m = re.fullmatch(r"[0-9a-f]{16}-(\d+)", value)
            if m:
                return m.group(1)
        if key in ("started_at", "ended_at") and isinstance(value, str):
            return "2026-01-01T00:00:00.000Z" if key == "started_at" else "2026-01-01T00:00:01.000Z"
        if key == "url" and isinstance(value, str) and value.startswith("panels/"):
            return re.sub(r"^panels/[^/]+/", "panels/<key>/", value)  # the server's panel key
        if key == "modified" and isinstance(value, str):
            return "2026-01-01T00:00:00Z"
        if isinstance(value, str):
            for real, fixed in self.paths.items():
                value = value.replace(real, fixed)
            value = value.replace(f"pid {self.pid_in_log(value)}", "pid 10099") if "pid " in value else value
            value = re.sub(r"\b[0-9a-f]{16}-(\d+)\b", r"\1", value)
            value = re.sub(r'"t_ms":\s*\d+', '"t_ms":0', value)  # a raw line in a problem's detail
            value = re.sub(r"127\.0\.0\.1:(?!8765\b)\d+", "127.0.0.1:8510", value)  # a run's control server
            # The file a scenario run's timeline is written to (in the server's temporary directory)
            value = re.sub(r"\S*vehicle-sil-gui-timelines-[^/\\]+[/\\][0-9a-f]+\.timeline\.json",
                           "/path/to/vehicle-sil-gui-timelines/run.timeline.json", value)
            assert self.token not in value, "the token must not appear in any response"
        return value

    @staticmethod
    def pid_in_log(text: str) -> str:
        import re
        m = re.search(r"pid (\d+)", text)
        return m.group(1) if m else "-"


_STREAM_SEQ_TYPES = frozenset({
    "hello", "synced", "session", "event", "allowed", "runtime",
    "variables", "operation", "availability",
})

_NO_STREAM_SEQ_TYPES = frozenset({"values", "dropped", "resync"})

_EXACT_HTTP_EXAMPLES = frozenset({
    "configs", "prepare", "recording", "run", "run-connect", "stop", "stop-force", "stop-ended",
    "time", "timeline", "write", "config", "save-config", "scenarios", "dashboard", "dashboard-errors",
    "error-already-running", "error-commands-changed", "error-config-unreadable",
    "error-not-writable", "error-scenario-not-listed", "error-session-changed",
    "error-target-not-listed",
})

_EXACT_STRIP_STREAM_SEQ_WS = frozenset({
    "error-invalid-watch", "ws-availability", "ws-resync", "ws-slow-client",
    "ws-unauthorized", "ws-variables-failed",
})


@lru_cache(maxsize=1)
def _openapi_document() -> dict:
    return yaml.safe_load(OPENAPI_PATH.read_text(encoding="utf-8"))


@lru_cache(maxsize=1)
def _contract_validators() -> dict[str, jsonschema.Draft202012Validator]:
    openapi_doc = _openapi_document()
    uri = "urn:vehicle-sil:gui:openapi"
    registry = Registry().with_resource(uri, DRAFT202012.create_resource(openapi_doc))
    validators: dict[str, jsonschema.Draft202012Validator] = {
        "TimeCommandRequest": jsonschema.Draft202012Validator(
            {"$ref": f"{uri}#/components/schemas/TimeCommandRequest"}, registry=registry
        ),
        "TimeCommandAck": jsonschema.Draft202012Validator(
            {"$ref": f"{uri}#/components/schemas/TimeCommandAck"}, registry=registry
        ),
        "ErrorResponse": jsonschema.Draft202012Validator(
            {"$ref": f"{uri}#/components/schemas/ErrorResponse"}, registry=registry
        ),
        "RunTarget": jsonschema.Draft202012Validator(
            {"$ref": f"{uri}#/components/schemas/RunTarget"}, registry=registry
        ),
        "WsCommandEvent": jsonschema.Draft202012Validator(
            json.loads(WS_COMMAND_SCHEMA_PATH.read_text(encoding="utf-8"))
        ),
        "WsServerMessage": jsonschema.Draft202012Validator(
            json.loads(WS_SERVER_SCHEMA_PATH.read_text(encoding="utf-8"))
        ),
        "WsClientMessage": jsonschema.Draft202012Validator(
            json.loads(WS_CLIENT_SCHEMA_PATH.read_text(encoding="utf-8"))
        ),
        "CliReadyMessage": jsonschema.Draft202012Validator(
            json.loads(CLI_JSON_SCHEMA_PATH.read_text(encoding="utf-8"))
        ),
        "Timeline": (
            lambda s: jsonschema.validators.validator_for(s)(s)
        )(json.loads(TIMELINE_SCHEMA_PATH.read_text(encoding="utf-8"))),
    }
    def _with_uri(schema: dict) -> dict:
        if isinstance(schema.get("$ref"), str) and schema["$ref"].startswith("#"):
            return {"$ref": f"{uri}{schema['$ref']}"}
        return schema

    for path, methods in openapi_doc["paths"].items():
        for method, op in methods.items():
            op_key = f"{method.upper()} {path}"
            if "requestBody" in op:
                req_schema = _with_uri(op["requestBody"]["content"]["application/json"]["schema"])
                validators[f"req:{op_key}"] = jsonschema.Draft202012Validator(req_schema, registry=registry)
            ok_content = op["responses"]["200"]["content"]
            if "application/json" in ok_content:
                resp_schema = _with_uri(ok_content["application/json"]["schema"])
                validators[f"resp:{op_key}"] = jsonschema.Draft202012Validator(resp_schema, registry=registry)
    return validators


def _assert_schema(validator: jsonschema.Draft202012Validator, instance: object, label: str) -> None:
    try:
        validator.validate(instance)
    except jsonschema.ValidationError as exc:
        raise AssertionError(f"{label}: schema validation failed: {exc.message}") from exc


def _openapi_route_path(raw_path: str) -> str:
    clean = "/" + raw_path.split("?", 1)[0].lstrip("/")
    if clean.startswith("/api/timeline/"):
        return "/api/timeline/{run_id}"
    return clean


def _validate_http_contracts(name: str, example: dict) -> None:
    if "request" not in example:
        return
    validators = _contract_validators()
    req = example["request"]
    method = req.get("method", "").upper()
    route_path = _openapi_route_path(req.get("path", ""))
    op_key = f"{method} {route_path}"
    status = example["status"]
    resp = example["response"]
    if status >= 400 and isinstance(resp, dict) and "error" in resp:
        _assert_schema(validators["ErrorResponse"], resp, f"{name} error response")
        op = _openapi_document()["paths"].get(route_path, {}).get(method.lower())
        if op is not None:
            assert str(status) in op["responses"], (name, op_key, status, sorted(op["responses"]))
        if status not in (400, 413, 415) and isinstance(req.get("body"), dict):
            req_v = validators.get(f"req:{op_key}")
            if req_v is not None:
                _assert_schema(req_v, req["body"], f"{name} request ({op_key})")
        return
    if status == 200:
        req_v = validators.get(f"req:{op_key}")
        if req_v is not None and isinstance(req.get("body"), dict):
            _assert_schema(req_v, req["body"], f"{name} request ({op_key})")
        resp_v = validators.get(f"resp:{op_key}")
        assert resp_v is not None, (name, f"missing OpenAPI response schema for {op_key}")
        _assert_schema(resp_v, resp, f"{name} response ({op_key})")
        if route_path == "/api/timeline/{run_id}":
            _assert_schema(validators["Timeline"], resp, f"{name} timeline schema")


def _validate_ws_event_payload(name: str, ev: object, *, is_live: bool = False) -> None:
    assert isinstance(ev, dict) and isinstance(ev.get("event"), str), (name, ev)
    assert type(ev.get("seq")) is int and ev["seq"] >= 0, (name, ev)
    if name == "ws-loose-fields":
        return
    kind = ev["event"]
    if is_live and kind != "mystery":
        validate_events_v4_event(ev)
    if kind == "run_start":
        assert "timeline" not in ev, (name, ev)
    elif kind == "state":
        _validate_run_state({k: v for k, v in ev.items() if k not in ("event", "seq", "t_ms", "run")}, envelope=False)
    elif kind == "result" and "status" in ev:
        assert isinstance(ev.get("launcher_ends"), bool), (name, ev)
        _validate_status_members(ev["status"], exact_keys=True)


def _validate_protocol_v4_example(name: str, example: dict, *, is_live: bool = False) -> None:
    """Validates HTTP/WS structural contracts via generated schemas and behavioral invariants (ordering/replay)."""
    _validate_http_contracts(name, example)
    if "server" not in example:
        return
    validators = _contract_validators()
    for idx, cmsg in enumerate(example.get("client", [])):
        _assert_schema(validators["WsClientMessage"], cmsg, f"{name} client[{idx}]")
    messages = example["server"]
    assert isinstance(messages, list) and messages, name
    in_replay = False
    hello_seq: int | None = None
    last_replay_seq: int | None = None
    last_live_seq: int | None = None
    for idx, msg in enumerate(messages):
        assert isinstance(msg, dict) and isinstance(msg.get("type"), str), (name, idx, msg)
        assert "snapshot_rev" not in msg and "problem_seq" not in msg and "event_seq" not in msg, (name, msg)
        mtype = msg["type"]
        if not (name == "ws-loose-fields" and mtype == "event"):
            _assert_schema(validators["WsServerMessage"], msg, f"{name} server[{idx}]")
        if mtype in _NO_STREAM_SEQ_TYPES:
            assert "stream_seq" not in msg, (name, msg)
        elif mtype in _STREAM_SEQ_TYPES:
            sseq = msg["stream_seq"]
            if mtype == "hello":
                hello_seq = sseq
                in_replay = True
                last_replay_seq = None
                last_live_seq = None
            elif mtype == "synced":
                if hello_seq is not None:
                    assert sseq >= hello_seq, (name, hello_seq, msg)
                if last_replay_seq is not None:
                    assert sseq >= last_replay_seq, (name, last_replay_seq, msg)
                in_replay = False
                last_live_seq = sseq
            elif in_replay:
                if hello_seq is not None:
                    assert sseq <= hello_seq, (name, hello_seq, msg)
                assert last_replay_seq is None or sseq > last_replay_seq, (name, last_replay_seq, msg)
                last_replay_seq = sseq
            else:
                assert last_live_seq is None or sseq > last_live_seq, (name, last_live_seq, msg)
                last_live_seq = sseq
        elif mtype == "error" and "stream_seq" in msg:
            sseq = msg["stream_seq"]
            if in_replay:
                if hello_seq is not None:
                    assert sseq <= hello_seq, (name, hello_seq, msg)
                assert last_replay_seq is None or sseq > last_replay_seq, (name, last_replay_seq, msg)
                last_replay_seq = sseq
            else:
                assert last_live_seq is None or sseq > last_live_seq, (name, last_live_seq, msg)
                last_live_seq = sseq

        if mtype == "event":
            _validate_ws_event_payload(name, msg["event"], is_live=is_live)
            if name != "ws-loose-fields" and msg["event"].get("event") == "command":
                _assert_schema(validators["WsCommandEvent"], msg, f"{name} server[{idx}] command event")
        elif mtype in ("hello", "session") and msg.get("session") and msg["session"].get("launcher_state") is not None:
            ls = msg["session"]["launcher_state"]
            _validate_run_state({k: v for k, v in ls.items() if k != "seq"}, envelope=False)


def _compare_check_payload(name: str, live: dict, saved: dict) -> None:
    assert live.get("vehicle_sil_check") == 1 == saved.get("vehicle_sil_check"), (name, live)
    for field in ("file", "result", "strict", "errors"):
        assert live.get(field) == saved.get(field), (name, field, live.get(field), saved.get(field))
    assert type(live.get("warnings")) is int and live["warnings"] >= 0, (name, live)
    assert type(live.get("notes")) is int and live["notes"] >= 0, (name, live)
    assert live.get("time_lock") == saved.get("time_lock"), (name, live.get("time_lock"), saved.get("time_lock"))
    if saved.get("scenario") is None:
        assert live.get("scenario") is None, (name, live.get("scenario"))
    else:
        assert isinstance(live.get("scenario"), dict), (name, live.get("scenario"))
        for key in ("file", "steps"):
            assert live["scenario"].get(key) == saved["scenario"].get(key), (name, key, live["scenario"], saved["scenario"])
    assert isinstance(live.get("participants"), list), (name, live)
    live_parts = [(p["name"], p["enabled"], p["manual"], p["required"], p["describe"]) for p in live["participants"]]
    saved_parts = [(p["name"], p["enabled"], p["manual"], p["required"], p["describe"]) for p in saved["participants"]]
    assert live_parts == saved_parts, (name, live_parts, saved_parts)
    for lp, sp in zip(live["participants"], saved["participants"]):
        if sp.get("variables") is None:
            assert lp.get("variables") is None, (name, lp["name"], lp.get("variables"))
        else:
            assert isinstance(lp.get("variables"), list), (name, lp["name"], lp.get("variables"))
            lvars = [(v["name"], v["type"], v["causality"], v["builtin"]) for v in lp["variables"]]
            svars = [(v["name"], v["type"], v["causality"], v["builtin"]) for v in sp["variables"]]
            assert lvars == svars, (name, lp["name"], lvars, svars)
    def _net_key(net: dict) -> tuple:
        out = net.get("outside")
        return (net["name"], net["kind"], None if out is None else out.get("target"))

    live_nets = [_net_key(n) for n in live["networks"]]
    saved_nets = [_net_key(n) for n in saved["networks"]]
    assert live_nets == saved_nets, (name, live_nets, saved_nets)
    live_err_codes = [(f["severity"], f["code"]) for f in live["findings"] if f["severity"] == "error"]
    saved_err_codes = [(f["severity"], f["code"]) for f in saved["findings"] if f["severity"] == "error"]
    assert live_err_codes == saved_err_codes, (name, live_err_codes, saved_err_codes)
    for f in live["findings"]:
        assert {"severity", "code", "message"} <= set(f.keys()), (name, f)


def _compare_session_summary(name: str, live: dict, saved: dict) -> None:
    for field in ("id", "file", "disable", "manual", "start_paused", "state", "stop", "phase"):
        assert live[field] == saved[field], (name, field, live[field], saved[field])
    assert live["launcher"]["running"] == saved["launcher"]["running"], (name, live["launcher"], saved["launcher"])
    if saved["result"] is None:
        assert live["result"] is None, (name, live["result"])
    else:
        assert isinstance(live["result"], dict), (name, live["result"])
        for field in ("result", "ok", "interrupted_by"):
            assert live["result"].get(field) == saved["result"].get(field), (name, field, live["result"], saved["result"])
    if saved["scenario"] is None:
        assert live["scenario"] is None, (name, live["scenario"])
    else:
        assert isinstance(live["scenario"], dict), (name, live["scenario"])
        for field in ("file", "status", "timeline_url"):
            assert live["scenario"].get(field) == saved["scenario"].get(field), (name, field, live["scenario"], saved["scenario"])
    assert {p["name"]: p["state"] for p in live["participants"]} == {
        p["name"]: p["state"] for p in saved["participants"]
    }, (name, live["participants"], saved["participants"])
    assert {p["code"] for p in live["problems"]} == {p["code"] for p in saved["problems"]}, (
        name, live["problems"], saved["problems"],
    )


def _compare_http_response(name: str, live: dict, saved: dict) -> None:
    assert set(live.keys()) == set(saved.keys()), (name, live, saved)
    if name in _EXACT_HTTP_EXAMPLES:
        assert live == saved, (name, live, saved)
        return
    if name == "app":
        for key in saved:
            if key != "launcher":
                assert live[key] == saved[key], (name, key, live[key], saved[key])
        assert set(live["launcher"].keys()) == set(saved["launcher"].keys()), (name, live["launcher"])
        for key in ("path", "usable", "problems"):
            assert live["launcher"][key] == saved["launcher"][key], (name, key)
        lv, sv = live["launcher"]["version"], saved["launcher"]["version"]
        assert set(lv.keys()) == set(sv.keys()), (name, lv, sv)
        assert lv["vehicle_sil_launch"] == sv["vehicle_sil_launch"], (name, lv)
        assert lv["vehicle_sil"] == sv["vehicle_sil"], (name, lv)
        assert lv["formats"] == sv["formats"], (name, lv["formats"], sv["formats"])
        assert isinstance(lv["features"], list) and set(REQUIRED_FEATURES) <= set(lv["features"]), (name, lv["features"])
        return
    if "error" in saved:
        err, serr = live["error"], saved["error"]
        assert set(err.keys()) == set(serr.keys()), (name, err, serr)
        assert err["code"] == serr["code"], (name, err, serr)
        assert isinstance(err["message"], str) and err["message"], (name, err)
        if isinstance(serr.get("detail"), dict):
            assert isinstance(err.get("detail"), dict), (name, err)
            assert set(err["detail"].keys()) == set(serr["detail"].keys()), (name, err["detail"], serr["detail"])
            if "result" in serr["detail"]:
                assert err["detail"]["result"] == serr["detail"]["result"], (name, err["detail"])
            if "findings" in serr["detail"]:
                assert [(f["severity"], f["code"]) for f in err["detail"]["findings"] if f["severity"] == "error"] == [
                    (f["severity"], f["code"]) for f in serr["detail"]["findings"] if f["severity"] == "error"
                ], (name, err["detail"]["findings"], serr["detail"]["findings"])
        else:
            assert err.get("detail") == serr.get("detail"), (name, err, serr)
        return
    if "check" in saved:
        for key in saved:
            if key != "check":
                assert live[key] == saved[key], (name, key, live[key], saved[key])
        _compare_check_payload(name, live["check"], saved["check"])
        return
    if "connections" in saved:
        for key in saved:
            if key != "connections":
                assert live[key] == saved[key], (name, key, live[key], saved[key])
        lc, sc = live["connections"], saved["connections"]
        assert set(lc.keys()) == set(sc.keys()), (name, lc, sc)
        for field in ("vehicle_sil_connections", "vehicle_sil", "file", "result"):
            assert lc[field] == sc[field], (name, field, lc[field], sc[field])
        assert [(n["name"], n["kind"], n["backend"]) for n in lc["networks"]] == [
            (n["name"], n["kind"], n["backend"]) for n in sc["networks"]
        ], (name, lc["networks"], sc["networks"])
        target_keys = {"value", "label", "detail", "usable", "reason", "in_use", "in_use_by", "in_use_count"}
        for conn in (lc, sc):
            for net in conn["networks"]:
                assert isinstance(net.get("list"), dict) and isinstance(net["list"].get("targets"), list), (name, net)
                for tgt in net["list"]["targets"]:
                    assert target_keys <= set(tgt.keys()), (name, tgt)
                    assert isinstance(tgt["usable"], bool) and isinstance(tgt["in_use"], bool), (name, tgt)
                    assert isinstance(tgt["in_use_by"], list) and type(tgt["in_use_count"]) is int, (name, tgt)
        return
    if "session" in saved:
        assert live["instance"] == saved["instance"], (name, live["instance"], saved["instance"])
        assert live["allowed"] == saved["allowed"], (name, live["allowed"], saved["allowed"])
        _compare_session_summary(name, live["session"], saved["session"])
        return
    raise AssertionError(f"{name}: unhandled non-exact HTTP example")


def _strip_stream_seq(messages: list[dict]) -> list[dict]:
    return [{k: v for k, v in m.items() if k != "stream_seq"} for m in messages]


def _compare_ws_example(name: str, live: dict, saved: dict) -> None:
    assert live["client"] == saved["client"], (name, live["client"], saved["client"])
    live_srv, saved_srv = live["server"], saved["server"]
    live_types = {m["type"] for m in live_srv}
    saved_types = {m["type"] for m in saved_srv}
    assert saved_types <= live_types, (name, "missing message types", sorted(saved_types - live_types))
    extra_types = live_types - saved_types
    assert extra_types <= {"runtime"}, (name, "unexpected extra message types", sorted(extra_types))

    if name in _EXACT_STRIP_STREAM_SEQ_WS:
        assert [m["type"] for m in live_srv] == [m["type"] for m in saved_srv], (name, live_srv, saved_srv)
        assert _strip_stream_seq(live_srv) == _strip_stream_seq(saved_srv), (name, live_srv, saved_srv)
        return

    if [m["type"] for m in saved_srv[:2]] == ["hello", "synced"]:
        assert [m["type"] for m in live_srv[:2]] == ["hello", "synced"], (name, [m["type"] for m in live_srv[:4]])

    if name in ("ws-dropped", "ws-kept-limit"):
        assert [m["type"] for m in live_srv] == [m["type"] for m in saved_srv], (name, live_srv, saved_srv)
    if name == "ws-replay":
        assert live_srv[0]["type"] == "hello" and live_srv[-1]["type"] == "synced", (name, live_srv)
        assert all(m["type"] == "event" for m in live_srv[1:-1]), (name, live_srv)
    if name == "ws-allowed":
        assert [m["allowed"] for m in live_srv if m["type"] == "allowed"] == [
            m["allowed"] for m in saved_srv if m["type"] == "allowed"
        ], (name, live_srv, saved_srv)

    if "session" in saved_types:
        live_last = [m["session"] for m in live_srv if m["type"] == "session"][-1]
        saved_last = [m["session"] for m in saved_srv if m["type"] == "session"][-1]
        _compare_session_summary(name, live_last, saved_last)
    elif saved_srv[0]["type"] == "hello" and saved_srv[0]["session"] is not None:
        assert live_srv[0]["session"] is not None, (name, live_srv[0])
        _compare_session_summary(name, live_srv[0]["session"], saved_srv[0]["session"])

    if "event" in saved_types:
        live_ev_kinds = {m["event"]["event"] for m in live_srv if m["type"] == "event"}
        saved_ev_kinds = {m["event"]["event"] for m in saved_srv if m["type"] == "event"}
        # Events v4 replaces per-run `hello` with `run_start`.
        expected_from_saved = saved_ev_kinds - {"hello"}
        if name == "ws-time":
            expected_from_saved = expected_from_saved - {"finding"}
        assert expected_from_saved <= live_ev_kinds, (name, sorted(expected_from_saved - live_ev_kinds))
        assert live_ev_kinds <= saved_ev_kinds | {"run_start"}, (name, sorted(live_ev_kinds - saved_ev_kinds))

    if "variables" in saved_types:
        live_vars = [m["variables"] for m in live_srv if m["type"] == "variables"]
        saved_vars = [m["variables"] for m in saved_srv if m["type"] == "variables"]
        assert [v["state"] for v in live_vars] == [v["state"] for v in saved_vars], (name, live_vars, saved_vars)
    if "values" in saved_types:
        def _val_sig(m: dict) -> tuple:
            return (
                m["id"], m["watch_id"], m["sim_ms"], m["paused"],
                {p: sorted(d["values"].keys()) for p, d in m["peers"].items()},
            )

        assert [_val_sig(m) for m in live_srv if m["type"] == "values"] == [
            _val_sig(m) for m in saved_srv if m["type"] == "values"
        ], (name, live_srv, saved_srv)
    if "operation" in saved_types:
        live_ops = [
            (m["seq"], m["state"], m["done"], {p: d["state"] for p, d in m["peers"].items()})
            for m in live_srv if m["type"] == "operation"
        ]
        saved_ops = [
            (m["seq"], m["state"], m["done"], {p: d["state"] for p, d in m["peers"].items()})
            for m in saved_srv if m["type"] == "operation"
        ]
        assert live_ops == saved_ops, (name, live_ops, saved_ops)
    if "error" in saved_types:
        assert [m["code"] for m in live_srv if m["type"] == "error"] == [
            m["code"] for m in saved_srv if m["type"] == "error"
        ], (name, live_srv, saved_srv)


def check_example(name: str, example: dict, gui, paths: dict[str, str] | None = None) -> None:
    assert name in EXAMPLES, f"{name}: add it to EXAMPLES"
    example = Normalizer(gui, paths)(example)
    _validate_protocol_v4_example(name, example, is_live=True)
    if name in SHOWN:
        shown(name, example)
        return
    path = DIRECTORY / f"{name}.json"
    text = json.dumps(example, indent=2, ensure_ascii=False) + "\n"
    if UPDATE:
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        return
    assert path.is_file(), f"{path} is missing: run the tests with UPDATE_PROTOCOL_EXAMPLES=1"
    saved = json.loads(path.read_text(encoding="utf-8"))
    _validate_protocol_v4_example(name, saved)
    if "request" in example:
        assert example["request"] == saved["request"], (name, example["request"], saved["request"])
        assert example["status"] == saved["status"], (name, example["status"], saved["status"])
        _compare_http_response(name, example["response"], saved["response"])
    elif "server" in example:
        _compare_ws_example(name, example, saved)


def http(description: str, method: str, path: str, body, status: int, response: dict) -> dict:
    request = {"method": method, "path": path}
    if body is not None:
        request["body"] = body
    return {"description": description, "request": request, "status": status, "response": response}


def ws(description: str, client: list[dict], server: list[dict]) -> dict:
    return {"description": description, "client": [{**m, "token": "<token>"} if "token" in m else m for m in client],
            "server": server}


def test_no_example_is_left_over() -> None:
    present = {p.stem for p in DIRECTORY.glob("*.json")}
    assert present <= EXAMPLES - set(SHOWN), \
        f"files that no test writes (remove them): {sorted(present - (EXAMPLES - set(SHOWN)))}"
    assert present == EXAMPLES - set(SHOWN), \
        f"examples that are missing (UPDATE_PROTOCOL_EXAMPLES=1): {sorted(EXAMPLES - set(SHOWN) - present)}"
    assert not [p.name for p in DIRECTORY.iterdir() if p.suffix != ".json"]


# ---------------------------------------------------------------- HTTP


def test_dashboard_examples(tmp_path: Path) -> None:
    root = tmp_path / "root"
    root.mkdir()
    example = DIRECTORY.parents[2] / "examples" / "ecu_main"
    for name in ("simulation.yaml", "forever.yaml"):
        shutil.copy(CONFIGS / name, root / name)
    shutil.copy(CONFIGS / "simulation.yaml", root / "bench.yaml")
    shutil.copy(example / "simulation.dashboard.yaml", root / "simulation.dashboard.yaml")
    shutil.copytree(example / "panels", root / "panels")
    (root / "bench.dashboard.yaml").write_text(
        "vehicle_sil_dashboard: 1\nsections:\n  - title: Engine\n    items:\n      - variable: speed\n"
        "      - variable: Engine.engine.speed\n        colour: red\n", encoding="utf-8")
    with gui_server(tmp_path, root=root) as gui:
        async def talk(c: Client) -> None:
            for name, file, text in (
                    ("dashboard", "simulation.yaml", "The dashboard file next to a configuration, as the server "
                                                     "checked it (the example of examples/ecu_main)."),
                    ("dashboard-errors", "bench.yaml", "A dashboard file with mistakes: each with its line, "
                                                       "where, and what to do (no dashboard)."),
                    ("dashboard-none", "forever.yaml", "No dashboard file: the screen lays out the variables "
                                                       "by itself."),
            ):
                path = f"api/dashboard?file={file}"
                status, body = await c.get(path)
                assert status == 200, body
                check_example(name, http(text, "GET", path, None, status, body), gui, {str(root): "/path/to/root"})
        run_async(talk, gui)


def test_http_examples(tmp_path: Path) -> None:
    # Slow steps, so that the summary read right after the first step is always the same.
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "600"}) as gui:
        async def talk(c: Client) -> None:
            for name, path, text in (("app", "api/app", "The server, the launcher it found, and the limits."),
                                     ("configs", "api/configs", "The configuration files under the root."),
                                     ("session-none", "api/session", "No run since the server started.")):
                status, body = await c.get(path)
                check_example(name, http(text, "GET", path, None, status, body), gui)
            prep_req = {"file": "simulation.yaml"}
            status, body = await c.post("api/prepare", prep_req)
            check_example("prepare", http("Warm up the persistent runtime for a configuration when eligible.",
                                          "POST", "api/prepare", prep_req, status, body), gui)
            for name, req, text in (
                    ("check-passed", {"file": "simulation.yaml", "disable": []}, "A check that passed."),
                    ("check-disabled", {"file": "simulation.yaml", "disable": ["Dashboard"]},
                     "A check with one participant left out."),
                    ("check-failed", {"file": "check-error.yaml", "disable": []},
                     "A check that found an error (still status 200: the check ran)."),
                    ("check-manual", {"file": "simulation.yaml", "disable": [], "manual": ["Dashboard"]},
                     "A check with one participant that the user starts (manual): its manual is true, and a note "
                     "participant-manual."),
            ):
                status, body = await c.post("api/check", req)
                check_example(name, http(text, "POST", "api/check", req, status, body), gui)
            req = {"file": "check-error.yaml", "disable": []}
            status, body = await c.post("api/run", req)
            check_example("error-check-failed", http("A start whose check found an error: no run is started, and "
                                                     "detail.findings are the check's findings.", "POST",
                                                     "api/run", req, status, body), gui)
            for code, path, req, kw, text in (
                    ("unauthorized", "api/check", {"file": "simulation.yaml"}, {"token": "wrong"},
                     "No token, or a wrong one."),
                    ("forbidden-host", "api/check", {"file": "simulation.yaml"}, {"host": "localhost:8765"},
                     "A Host that is not 127.0.0.1:<port> (DNS rebinding, or localhost)."),
                    ("forbidden-origin", "api/run", {"file": "simulation.yaml"}, {"origin": "http://evil.example"},
                     "A request from another page."),
                    ("outside-root", "api/check", {"file": "../secret.yaml"}, {}, "A path that leaves the root."),
                    ("invalid-path", "api/check", {"file": "sub\\simulation.yaml"}, {},
                     "A path that is not root-relative with /."),
                    ("not-a-configuration", "api/check", {"file": "notes.txt"}, {}, "Not a .yaml or .yml file."),
                    ("file-not-found", "api/check", {"file": "missing.yaml"}, {}, "No such file under the root."),
                    ("unknown-participant", "api/check", {"file": "simulation.yaml", "disable": ["Nobody"]}, {},
                     "A participant name that the check of the file does not have."),
                    ("invalid-field", "api/run", {"file": "simulation.yaml", "speed": 0}, {},
                     "A field of the wrong type or out of range."),
                    ("not-running", "api/time", {"command": "pause"}, {},
                     "A time command when no simulation is running."),
            ):
                status, body = await c.post(path, req, **kw)
                assert body["error"]["code"] == code, body
                check_example(f"error-{code}", http(text, "POST", path, req, status, body), gui)
            status, body = await c.post("api/check", raw=b"{not json")
            check_example("error-invalid-json", http("A body that is not a JSON object.", "POST", "api/check",
                                                     "{not json", status, body), gui)
            status, body = await c.post("api/check", raw=b"file=simulation.yaml",
                                        content_type="application/x-www-form-urlencoded")
            check_example("error-unsupported-media-type", http("A body that is not JSON (a form).", "POST",
                                                               "api/check", "file=simulation.yaml", status, body), gui)
            status, body = await c.post("api/run", raw=b'{"file": "' + b"x" * 70_000 + b'"}')
            check_example("error-too-large", http("A body larger than 64 KiB (of api/check: 6 MiB + 64 KiB).", "POST",
                                                  "api/run", "(70 kB)", status, body), gui)
            status, body = await c.get("api/nothing")
            check_example("error-not-found", http("No such API.", "GET", "api/nothing", None, status, body), gui)
            async with c.http.request("OPTIONS", f"{gui.base}/api/run", headers=gui.headers()) as r:
                check_example("error-cors-not-allowed", http("OPTIONS (a CORS preflight).", "OPTIONS", "api/run",
                                                             None, r.status, await r.json()), gui)
            async with c.http.post(f"{gui.base}/index.html", headers={"Origin": gui.origin}) as r:
                check_example("error-method-not-allowed", http("Not GET on a file of the screen.", "POST",
                                                               "index.html", None, r.status, await r.json()), gui)

            sock = await c.ws()
            await sock.until(is_synced)
            # ignore-stop.yaml: Logger does not end when asked, so the run is still there when the second stop comes
            req = {"file": "ignore-stop.yaml", "disable": []}
            status, run_ack = await c.post("api/run", req)
            check_example("run", http("Start a run: the summary of the new run.", "POST", "api/run", req, status,
                                      run_ack), gui)
            status, body = await c.post("api/run", req)
            check_example("error-already-running", http("Start while a run is starting or running.", "POST",
                                                        "api/run", req, status, body), gui)
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            status, body = await c.get("api/session")
            check_example("session-running", http("A run that is running (the summary).", "GET", "api/session",
                                                  None, status, body), gui)
            stop_req = {"run_id": run_ack["run_id"], "instance": run_ack["instance"], "force": False}
            status, body = await c.post("api/stop-session", stop_req)
            check_example("stop", http("Ask the run to stop in order (force false; again: nothing more).", "POST",
                                       "api/stop-session", stop_req, status, body), gui)
            # The second stop when the launcher has asked Logger to stop and waits for it: the summary then has
            # every line up to there (the stand-in hurries them out after the first stop), whatever the timing.
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "participant"
                             and m["event"].get("state") == "stop_requested")
            force_req = {"run_id": run_ack["run_id"], "instance": run_ack["instance"], "force": True}
            status, body = await c.post("api/stop-session", force_req)
            check_example("stop-force", http("Ask the run to stop without waiting (a second stop to the launcher).",
                                             "POST", "api/stop-session", force_req, status, body), gui)
            await sock.until(is_ended)
            await sock.close()
            status, body = await c.post("api/stop-session", stop_req)
            check_example("stop-ended", http("Stop when the run has ended: the summary, not an error.", "POST",
                                             "api/stop-session", stop_req, status, body), gui)
        run_async(talk, gui)


def test_the_error_of_a_feature_that_the_launcher_lacks(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:  # the default launcher is built without a control server
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            status, started = await c.post("api/run", {"file": "forever.yaml"})
            assert status == 200, started
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            req = {"run_id": started["run_id"], "writes": {"Engine": {"engine.demand": 40}}}
            status, body = await c.post("api/write", req)
            check_example("error-unsupported", http("A write to a run whose launcher was built without a control "
                                                    "server (it lacks the feature control-server).", "POST",
                                                    "api/write", req, status, body), gui)
            await c.post("api/stop-session", {"run_id": started["run_id"], "instance": started["instance"], "force": False})
            await sock.until(is_ended)
        run_async(talk, gui)


def test_connect_examples(tmp_path: Path) -> None:
    """Where a network can be connected, a check and a run connected there, and a target that was not listed."""
    tools = {str(REPLAY.parent): "/path/to/tools"}  # where the stand-in launcher says the bridges are
    with gui_server(tmp_path) as gui:
        async def talk(c: Client) -> None:
            req = {"file": "connect.yaml", "connect": {"ETH_EXT": "fake0"}, "stop_at_ms": 1000}
            status, body = await c.post("api/check", req)
            check_example("error-target-not-listed", http(
                "A target that the last list of that network (POST api/connections) did not give as usable: here, "
                "no list was made yet.", "POST", "api/check", req, status, body), gui)
            status, body = await c.post("api/connections", {"file": "connect.yaml", "network": "NOPE"})
            check_example("error-unknown-network", http("A network that the last check of the file does not have.",
                                                        "POST", "api/connections",
                                                        {"file": "connect.yaml", "network": "NOPE"}, status, body),
                          gui)
            lreq = {"file": "connect.yaml", "network": "ETH_EXT"}
            status, body = await c.post("api/connections", lreq)
            check_example("connections", http(
                "Where ETH_EXT can be connected on this computer (the launcher's connections --json of that network "
                "only). The targets with usable: true are the values that connect may give it.", "POST",
                "api/connections", lreq, status, body), gui, tools)
            status, body = await c.post("api/check", req)
            check_example("check-connect", http(
                "A check with ETH_EXT connected to a listed target: networks[].outside says where, and time_lock "
                "that the run keeps real time.", "POST", "api/check", req, status, body), gui, tools)
            sock = await c.ws()
            await sock.until(is_synced)
            status, body = await c.post("api/run", req)
            check_example("run-connect", http("Start a run with ETH_EXT connected to a listed target.", "POST",
                                              "api/run", req, status, body), gui, tools)
            await sock.until(is_ended)
        run_async(talk, gui)


def test_config_text_examples(tmp_path: Path) -> None:
    """The text of a configuration file, the check of an unsaved text, and describing the executables again. The
    launcher is text_launcher.py (the check of the saved file, played; the options of saved descriptions)."""
    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)
    (root / "latin1.yaml").write_bytes("vehicle_sil_simulation: 2\n# caf\u00e9\n".encode("latin-1"))
    (root / "large.yaml").write_bytes(b"vehicle_sil_simulation: 2\n" + b"#" * (1024 * 1024))
    paths = {str(root): "/path/to/root"}
    with gui_server(tmp_path, root=root, launcher=REPLAY.parent / "text_launcher.py") as gui:
        async def talk(c: Client) -> None:
            path = "api/config?file=simulation.yaml"
            status, body = await c.get(path)
            check_example("config", http("The text of a configuration file as it is saved, and the SHA-256 of its "
                                         "bytes.", "GET", path, None, status, body), gui, paths)
            # An edit that the check does not show (the stand-in plays the saved file's check)
            req = {"file": "simulation.yaml", "text": body["text"] + "# edited on the screen, not saved yet\n"}
            status, body = await c.post("api/check", req)
            check_example("check-text", http(
                "A check of an unsaved text of simulation.yaml: no process is started (the launcher takes each "
                "executable's --describe output from what the check of the saved file saved).", "POST", "api/check",
                req, status, body), gui, paths)
            req = {"file": "simulation.yaml"}
            status, body = await c.post("api/describe", req)
            check_example("describe", http("Describe the executables of the saved file again, for the next checks "
                                           "of its text.", "POST", "api/describe", req, status, body), gui, paths)
            req = {"file": "simulation.yaml", "text": "#" * (1024 * 1024 + 1)}
            status, body = await c.post("api/check", req)
            check_example("error-text-too-large", http("A text larger than 1 MiB as UTF-8.", "POST", "api/check",
                                                       {**req, "text": "(1 MiB + 1 byte)"}, status, body), gui, paths)
            for name, file, text in (("error-config-too-large", "large.yaml", "A file larger than 1 MiB."),
                                     ("error-config-unreadable", "latin1.yaml",
                                      "A configuration file that is not UTF-8 text.")):
                path = f"api/config?file={file}"
                status, body = await c.get(path)
                check_example(name, http(text, "GET", path, None, status, body), gui, paths)
            # Saving: read, edit, save with the stamp that was read
            status, body = await c.get("api/config?file=simulation.yaml")
            read = body
            req = {"file": "simulation.yaml", "text": read["text"] + "# saved from the screen\n", "stamp": read["stamp"]}
            status, body = await c.post("api/config", req)
            check_example("save-config", http(
                "Save an edited text of simulation.yaml with the stamp that GET api/config gave: the launcher's check "
                "says that it runs the same commands as the saved file, so it is saved by default. The answer has the "
                "stamp of what was saved.", "POST", "api/config", req, status, body), gui, paths)
            stamp = body["stamp"]
            text = req["text"].replace("[engine/vehicle_sil_engine_ecu]", "[engine/vehicle_sil_engine_ecu, --verbose]")
            req = {"file": "simulation.yaml", "text": text, "stamp": stamp}
            status, body = await c.post("api/config", req)
            check_example("error-commands-changed", http(
                "An edit that changes the commands that run, to a server started without --allow-command-changes: "
                "the message has the launcher's finding, and detail is its check (result, commands, findings).",
                "POST", "api/config", req, status, body), gui, paths)
            req = {**req, "stamp": read["stamp"]}
            status, body = await c.post("api/config", req)
            check_example("error-config-changed", http("A stamp that is no longer the file's.", "POST",
                                                       "api/config", req, status, body), gui, paths)
        run_async(talk, gui)
    with gui_server(tmp_path, root=root, launcher=REPLAY.parent / "text_launcher.py", args=("--no-save",)) as gui:
        async def talk_no_save(c: Client) -> None:
            req = {"file": "simulation.yaml", "text": "vehicle_sil_simulation: 2\n", "stamp": "0" * 64}
            status, body = await c.post("api/config", req)
            check_example("error-save-disabled", http("A server started with --no-save.", "POST", "api/config",
                                                      req, status, body), gui, paths)
        run_async(talk_no_save, gui)


def test_errors_of_the_launcher_and_of_load(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_CHECK_DELAY_MS": "1000"}, args=("--check-timeout", "0.2")) as gui:
        async def talk(c: Client) -> None:
            req = {"file": "simulation.yaml"}
            status, body = await c.post("api/check", req)
            check_example("error-check-timeout", http("A check that did not end in time (--check-timeout).",
                                                      "POST", "api/check", req, status, body), gui)
        run_async(talk, gui)
    with gui_server(tmp_path, env={"REPLAY_CHECK_DELAY_MS": "300"}) as gui:
        async def talk(c: Client) -> None:
            req = {"file": "check-garbage.yaml"}
            status, body = await c.post("api/check", req)
            check_example("error-launcher-output", http("check --json wrote no check object.", "POST",
                                                        "api/check", req, status, body), gui)
            req = {"file": "simulation.yaml"}
            replies = await asyncio.gather(*(c.post("api/check", req) for _ in range(8)))
            status, body = next((s, b) for s, b in replies if s == 503)
            check_example("error-check-busy", http("Too many checks waiting for the one that runs.", "POST",
                                                   "api/check", req, status, body), gui)
            socks = [await c.ws() for _ in range(32)]
            for sock in socks:
                await sock.until(is_synced)
            status, body = await c.get("ws")
            check_example("error-too-many-connections", http("A WebSocket over the limit (the handshake's answer).",
                                                             "GET", "ws", None, status, body), gui)
            for s in socks:
                await s.close()
        run_async(talk, gui)


@pytest.mark.skipif(not POSIX, reason="file modes and signals")
def test_errors_of_a_launcher_that_changed(tmp_path: Path) -> None:
    launcher = tmp_path / "vehicle_sil_launch"
    launcher.write_text(f"#!{sys.executable}\n" + REPLAY.read_text())
    launcher.chmod(0o755)
    paths = {str(launcher): "/path/to/vehicle_sil_launch"}
    with gui_server(tmp_path, launcher=launcher) as gui:
        async def talk(c: Client) -> None:
            stat = launcher.stat()
            launcher.chmod(0o644)  # not executable any more; same time and size, so not asked again
            os.utime(launcher, ns=(stat.st_atime_ns, stat.st_mtime_ns))
            req = {"file": "simulation.yaml"}
            status, body = await c.post("api/check", req)
            check_example("error-launcher-start", http("The launcher could not be started.", "POST", "api/check",
                                                       req, status, body), gui, paths)
            launcher.write_text(f"#!{sys.executable}\nimport json\n"
                                "print(json.dumps({'vehicle_sil_launch': 1, 'features': ['events']}))\n")
            launcher.chmod(0o755)
            status, body = await c.post("api/check", req)
            check_example("error-launcher-unusable", http("The launcher was replaced by one this GUI cannot use.",
                                                          "POST", "api/check", req, status, body), gui, paths)
        run_async(talk, gui)
    with gui_server(tmp_path, args=("--stop-timeout", "5")) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "ignore-stop.yaml"})
            await sock.until(lambda m: m.get("event", {}).get("event") == "time")
            gui.proc.send_signal(signal.SIGTERM)

            async def ending() -> bool:
                return (await c.post("api/check", {"file": "../outside.yaml"}))[0] == 503
            assert await until_async(ending)
            req = {"file": "simulation.yaml"}
            status, body = await c.post("api/run", req)
            check_example("error-shutting-down", http("A run asked for while the server is ending.", "POST",
                                                      "api/run", req, status, body), gui)
        run_async(talk, gui)


# ---------------------------------------------------------------- WebSocket


SCENARIOS = {
    "pass": ("simulation.yaml", "A run that passes: hello with no session, then everything of the run."),
    "participant-crash": ("participant-crash.yaml", "A required participant fails while running."),
    "launcher-crash": ("launcher-crash.yaml", "The launcher dies without a result: the launcher's exit in the "
                                              "summary, then no-result."),
    "bad-lines": ("bad-lines.yaml", "The launcher writes lines that are not events, an unknown event and a gap "
                                    "in seq: errors from the server, the unknown event passed on."),
    "bad-types": ("bad-types.yaml", "The launcher writes known events whose fields have the wrong types: each is "
                                    "reported (bad-event) and not passed on; the run goes on."),
    "log-dropped": ("log-dropped.yaml", "The launcher could not write some log lines (its reader was slow): a "
                                        "log_dropped event where they were, counted in the summary's log."),
    "large-fields": ("large-fields.yaml", "A participant event larger than 4 KiB (a long command): the event is "
                                          "passed on whole; the summary keeps the participant without the command "
                                          "and names it in omitted_fields (kept-limit, once)."),
    "control-trouble": ("control-trouble.yaml", "The control links: Engine does not say hello (no_hello), Dashboard "
                                                "says hello in another version (version_mismatch), Gauge does not "
                                                "offer its variables (not_offered, a note of the check), Logger has "
                                                "none; the control_link events before running, kept in the summary's "
                                                "participants (control_link), the warnings at the end."),
    "manual": ("manual.yaml", "A run with a participant that the user starts (manual: true in the file): the "
                              "participant event of state manual (command, working_directory, command_line) instead "
                              "of started, then joined when the user has started it; at the end the launcher's log "
                              "says that it has not left the simulation (the launcher does not end it)."),
    "loose-fields": ("loose-fields.yaml", "Known events whose fields that the server does not check are missing or "
                                          "of other types (a finding with only severity and message, a registry "
                                          "with uri null, a participant with a number as signal and message, a log "
                                          "without t_ms): passed on, and kept in the summary, as they are."),
}


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_ws_scenario_examples(tmp_path: Path, scenario: str) -> None:
    file, description = SCENARIOS[scenario]
    with gui_server(tmp_path) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            intent: dict[str, object] = {"file": file}
            if file != "participant-crash.yaml":
                intent["stop_at_ms"] = 1000
            _, started = await c.post("api/run", intent)
            await sock.until(is_ended)
            check_example(f"ws-{scenario}", ws(description, sock.sent, sock.received), gui)
            if scenario == "pass":
                again = await c.ws()
                await again.until(is_synced)
                check_example("ws-replay", ws("A screen that connects after the run: the summary in hello, then "
                                              "the kept log lines, then synced.", again.sent, again.received), gui)
                logs = [m["stream_seq"] for m in sock.received if m.get("event", {}).get("event") == "log"]
                resumed = await c.ws({
                    "instance": started["instance"],
                    "run_id": started["run_id"],
                    "after_stream_seq": logs[-1] - 1,
                })
                await resumed.until(is_synced)
                check_example("ws-resume", ws("A screen that reconnects and says what it already has: the summary, "
                                              "then only the log lines after after_stream_seq.", resumed.sent,
                                              resumed.received), gui)
                status, body = await c.get("api/session")
                check_example("session-ended", http("The summary after the run ended.", "GET", "api/session",
                                                    None, status, body), gui)
        run_async(talk, gui)


def test_ws_allowed_example(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "check-error.yaml"})  # refused by its check: no run is started
            await sock.until(lambda m: m["type"] == "allowed" and m["allowed"]["start"]["ok"])
            check_example("ws-allowed", ws("What the server accepts changed with no session message to carry it: a "
                                           "start begins (start is starting), then fails (start is ok again). Each "
                                           "`allowed` has a stream_seq that goes up with each change.", sock.sent,
                                           sock.received), gui)
        run_async(talk, gui)


def is_time(**fields):
    return lambda m: m["type"] == "event" and m["event"]["event"] == "time" and \
        all(m["event"].get(k) == v for k, v in fields.items())


def test_ws_time_examples(tmp_path: Path) -> None:
    # Slow steps, so that a command sent right after the time event it waits for lands before the next step.
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "600"}) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            _, started = await c.post("api/run", {"file": "forever.yaml", "disable": ["Logger"], "speed": "max",
                                                  "start_paused": True})
            await sock.until_stands(0)
            req = {"command": "step", "ms": 25, "run_id": started["run_id"], "instance": started["instance"]}
            status, body = await c.post("api/time", req)
            check_example("time", http("A time command: passed on to the launcher at once (what it did comes as its "
                                       "command and time events).", "POST", "api/time", req, status, body), gui)
            await sock.until_stands(25)
            await c.post("api/time", {"command": "speed", "value": 1})
            await sock.until_stands(speed=1)
            await c.post("api/time", {"command": "resume"})
            await sock.until(is_state(paused=False))
            await c.post("api/time", {"command": "step", "ms": 100})
            await sock.until(lambda m: m.get("event", {}).get("event") == "command")
            await c.post("api/time", {"command": "pause"})
            await sock.until_stands()
            await c.post("api/stop-session", {"run_id": started["run_id"], "instance": started["instance"], "force": False})
            await sock.until(is_ended)
            await sock.close()
            check_example("ws-time", ws("Started paused at the speed max: a step of 25 ms (the launcher rounds it up "
                                        "to its time step: ms 30), the speed 1, resume, a step it refuses (not "
                                        "paused), pause, stop.", sock.sent, sock.received), gui)
        run_async(talk, gui)


def variables_only(messages: list[dict]) -> list[dict]:
    """A conversation without what comes in an order of its own: the launcher's events (they come by another way
    than the variables), the values while it runs, and the first word of an operation (`sent`, which the news of
    it being queued may overtake)."""
    return [m for m in messages if m["type"] != "event" and not (m["type"] == "values" and not m["paused"])
            and not (m["type"] == "operation" and m["state"] == "sent")]


def test_variables_examples(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env={**control_env, "REPLAY_TICK_MS": "100"}) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            _, started = await c.post("api/run", {"file": "forever.yaml", "disable": ["Logger"], "start_paused": True})
            sid, instance = started["run_id"], started["instance"]
            await sock.until(lambda m: m["type"] == "variables" and m["variables"]["state"] == "ready")
            await sock.send({"type": "watch", "id": 1, "period_ms": 100,
                             "variables": [{"peer": "Engine", "name": "engine.demand"},
                                           {"peer": "Engine", "name": "engine.speed"}]})
            await sock.until(lambda m: m["type"] == "values" and m["paused"])
            req = {"run_id": sid, "writes": {"Engine": {"engine.demand": 40, "engine.mode": "on"}}}
            status, body = await c.post("api/write", req)
            check_example("write", http("A write: checked against the schema and given to the run's control server "
                                        "(what became of it comes as operation messages).", "POST", "api/write", req,
                                        status, body), gui)
            await sock.until(lambda m: m["type"] == "operation" and m["state"] == "queued")
            await c.post("api/time", {"command": "step", "ms": 200})
            await sock.until(lambda m: m["type"] == "values" and m["paused"] and m["sim_ms"] == 200)
            if not any(m["type"] == "operation" and m["done"] for m in sock.received):
                await sock.until(lambda m: m["type"] == "operation" and m["done"])
            status, body = await c.get("api/recording")
            check_example("recording", http("The writes of the run that were applied, as a scenario's YAML (the "
                                            "server writes no file: the screen saves the text).", "GET",
                                            "api/recording", None, status, body), gui)
            for code, req, text in (
                    ("unknown-variable", {"run_id": sid, "writes": {"Engine": {"engine.torque": 1}}},
                     "A variable the run does not have."),
                    ("not-writable", {"run_id": sid, "writes": {"Engine": {"engine.speed": 900}}},
                     "An output cannot be written."),
                    ("session-changed", {"run_id": f"{instance}-2", "writes": {"Engine": {"engine.demand": 1}}},
                     "A write meant for another run than the one that is running (it was sent while one run ended "
                     "and the next began): nothing is written.")):
                status, body = await c.post("api/write", req)
                check_example(f"error-{code}", http(text, "POST", "api/write", req, status, body), gui)
            await sock.send({"type": "watch", "id": 2, "variables": "all", "period_ms": 5})
            got = await sock.until(lambda m: m["type"] == "error")
            check_example("error-invalid-watch", ws("A watch that is wrong: an error for this connection, which "
                                                    "goes on.", sock.sent[-1:], got[-1:]), gui)
            await sock.send({"type": "unwatch", "id": 1})
            await c.post("api/stop-session", {"run_id": sid, "instance": instance, "force": False})
            await sock.until(is_ended)
            await sock.close()
            check_example("ws-variables", ws(
                "A run with variables, started paused: the schema (variables), a watch of two variables, a write "
                "before the first step, a step of 200 ms, the end. Left out here: the launcher's events, the values "
                "sent while it ran (paused false), and the operation's first state (sent).",
                sock.sent, variables_only(sock.received)), gui)
        run_async(talk, gui)


def test_availability_example(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env={**control_env, "REPLAY_TICK_MS": "100", "REPLAY_AVAILABILITY": "1"}) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            _, started = await c.post("api/run", {"file": "forever.yaml", "start_paused": True})
            sid, instance = started["run_id"], started["instance"]
            ready = await sock.until(lambda m: m["type"] == "variables" and m["variables"]["state"] == "ready")
            engine = {v["name"]: v for p in ready[-1]["variables"]["peers"] if p["name"] == "Engine"
                      for v in p["variables"]}
            assert [engine[n]["availability"] for n in ("ecu.connected", "engine.demand", "engine.speed")] == \
                ["active", "unknown", "unknown"]
            await c.post("api/time", {"command": "step", "ms": 100})
            told = await sock.until(lambda m: m["type"] == "availability")
            assert told[-1]["peers"]["Engine"] == {"engine.demand": "inactive", "engine.speed": "active"}
            await sock.until_stands(100)
            # A variable that is not in use is written as any other.
            status, _ = await c.post("api/write", {"run_id": sid, "writes": {"Engine": {"engine.demand": 40}}})
            assert status == 200
            await c.post("api/time", {"command": "step", "ms": 100})
            done = await sock.until(lambda m: m["type"] == "operation" and m["done"])
            assert done[-1]["state"] == "applied"
            await c.post("api/stop-session", {"run_id": sid, "instance": instance, "force": False})
            await sock.until(is_ended)
            await sock.close()
            shown = [m for m in sock.received if m["type"] in ("variables", "availability") or
                     (m["type"] == "operation" and m["state"] != "sent")]
            check_example("ws-availability", ws(
                "A run whose ECUs report whether they use some variables (the stand-in's engine.demand and "
                "engine.speed, unknown until the first step): each variable of the summary has its availability, an "
                "availability message tells the change after the first step, and a write to a variable that is not in "
                "use is applied as any other. Only the variables, availability and operation messages are shown "
                "(not the operation's first state, sent).", sock.sent, shown), gui)
        run_async(talk, gui)


def test_scenario_examples(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path) as gui:
        async def talk(c: Client) -> None:
            path = "api/scenarios?file=simulation.yaml"
            status, body = await c.get(path)
            check_example("scenarios", http("The scenario files of a configuration: the .yaml and .yml files in "
                                            "scenarios next to it.", "GET", path, None, status, body), gui)
            req = {"file": "simulation.yaml", "disable": [], "scenario": "scenarios/throttle.yaml"}
            status, body = await c.post("api/check", req)
            check_example("check-scenario", http("A check with a scenario: the check's scenario (its name and "
                                                 "number of steps); its mistakes are findings with the scenario's "
                                                 "file and line.", "POST", "api/check", req, status, body), gui)
            req = {"file": "simulation.yaml", "scenario": "forever.yaml"}
            status, body = await c.post("api/check", req)
            check_example("error-scenario-not-listed", http("A scenario that GET api/scenarios does not list for "
                                                            "this configuration.", "POST", "api/check", req, status,
                                                            body), gui)
            sock = await c.ws()
            await sock.until(is_synced)
            _, started = await c.post("api/run", {"file": "simulation.yaml", "scenario": "scenarios/fails.yaml"})
            await sock.until(is_ended)
            check_example("ws-scenario", ws("A run with a scenario whose second step fails: scenario_step events as "
                                            "the steps run and end, the finding scenario-failed with the scenario's "
                                            "file and line, the timeline file (--timeline: the launcher has the "
                                            "feature timeline), the run's end; the summary's scenario keeps the "
                                            "steps, and has timeline_url at the end.",
                                            sock.sent, sock.received), gui)
            tl_path = f"api/timeline/{started['run_id']}"
            status, body = await c.get(tl_path)
            check_example("timeline", http("The recorded timeline JSON of a completed scenario run.",
                                           "GET", tl_path, None, status, body), gui)
        run_async(talk, gui)
    # At speed 0.001 its second step runs for minutes: the scenario still runs when the write comes, however slow
    # the machine.
    env = {**control_env, "REPLAY_FEATURES": CONTROL_FEATURES}
    with gui_server(tmp_path, env=env) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            _, started = await c.post("api/run", {"file": "forever.yaml", "disable": ["Logger"],
                                                  "scenario": "scenarios/throttle.yaml", "speed": 0.001})
            await sock.until(lambda m: m["type"] == "variables" and m["variables"]["state"] == "ready")
            req = {"run_id": started["run_id"], "writes": {"Engine": {"engine.demand": 40}}}
            status, body = await c.post("api/write", req)
            check_example("error-scenario-running", http("A write while the run plays a scenario (a launcher with "
                                                         "scenario-control reads the variables then, and nothing "
                                                         "writes them).", "POST", "api/write", req, status, body),
                          gui)
            await c.post("api/stop-session", {"run_id": started["run_id"], "instance": started["instance"], "force": False})
            await sock.until(is_ended)
        run_async(talk, gui)


def test_variables_failed_examples(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env={**control_env, "REPLAY_CONTROL": "unreachable"}) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            _, started = await c.post("api/run", {"file": "forever.yaml", "disable": ["Logger"]})
            await sock.until(lambda m: m["type"] == "variables" and m["variables"]["reason"])
            req = {"run_id": started["run_id"], "writes": {"Engine": {"engine.demand": 40}}}
            status, body = await c.post("api/write", req)
            check_example("error-variables-not-ready", http("A write while the variables cannot be read (the reason "
                                                            "is the summary's variables.reason).", "POST",
                                                            "api/write", req, status, body), gui)
            await c.post("api/stop-session", {"run_id": started["run_id"], "instance": started["instance"], "force": False})
            await sock.until(is_ended)
            await sock.close()
            told = [m for m in sock.received if m["type"] == "variables"]
            check_example("ws-variables-failed", ws(
                "The run's control server cannot be reached: the run goes on, and the variables say why (only the "
                "variables messages are shown; while the server tries again, the same message is not sent twice).",
                sock.sent, told), gui)
        run_async(talk, gui)


def test_ws_late_example(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "600"}) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            _, started = await c.post("api/run", {"file": "late.yaml", "speed": 1})
            await sock.until(is_time(late_steps=1, late_ms=0))
            await c.post("api/stop-session", {"run_id": started["run_id"], "instance": started["instance"], "force": False})
            await sock.until(is_ended)
            await sock.close()
            check_example("ws-late", ws("A run at speed 1 whose third step starts 200 ms late: a time event while it "
                                        "stands (late_ms), the counts after it, the warning behind-real-time and the "
                                        "result's timing.", sock.sent, sock.received), gui)
        run_async(talk, gui)


def test_ws_stop_examples(tmp_path: Path) -> None:
    # Slow steps, so that a stop sent right after "running" always lands in the first step.
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "600"}) as gui:
        async def talk(c: Client) -> None:
            for scenario, file, stops, text in (
                    ("stop", "forever.yaml", [{}], "Stopping a run in order."),
                    ("force-stop", "ignore-stop.yaml", [{}, {"force": True}],
                     "A launcher that waits after the stop; force stops it without waiting."),
            ):
                sock = await c.ws()
                await sock.until(is_synced)
                _, started = await c.post("api/run", {"file": file})
                await sock.until(lambda m: m.get("event", {}).get("phase") == "running")
                for n, body in enumerate(stops):
                    await c.post("api/stop-session", {
                        "run_id": started["run_id"],
                        "instance": started["instance"],
                        "force": bool(body.get("force", False)),
                    })
                    if n + 1 < len(stops):
                        # The launcher has taken the stop (its command event) before the next one: on a slow
                        # machine a time event already on its way could come first.
                        await sock.until(lambda m: m.get("event", {}).get("event") == "command")
                await sock.until(is_ended)
                await sock.close()
                check_example(f"ws-{scenario}", ws(text, sock.sent, sock.received), gui)
        run_async(talk, gui)


@pytest.mark.skipif(not POSIX, reason="kills the process left behind by pid")
def test_ws_orphan_example(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_ORPHAN_S": "30"}) as gui:
        async def talk(c: Client) -> int:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "orphan.yaml", "stop_at_ms": 1000})
            await sock.until(is_ended)
            check_example("ws-orphan", ws("The launcher has exited, but a process it started holds its output: the "
                                          "server stops reading after a short time (output-left-open).",
                                          sock.sent, sock.received), gui)
            line = next(m["event"]["line"] for m in sock.received
                        if "left a process behind" in m.get("event", {}).get("line", ""))
            return int(line.rsplit(" ", 1)[1])
        orphan = run_async(talk, gui)
        try:
            os.kill(orphan, signal.SIGKILL)
        except ProcessLookupError:
            pass


def test_ws_dropped_example(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_FLOOD": "2100"}) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            _, started = await c.post("api/run", {"file": "flood.yaml", "stop_at_ms": 1000})
            await sock.until(is_ended)
            await sock.close()
            resumed = await c.ws({
                "instance": started["instance"],
                "run_id": started["run_id"],
                "after_stream_seq": 20,
            })
            got = await resumed.until(is_synced)
            hello, dropped = got[0], got[1]
            first = got[2]
            check_example("ws-dropped", ws("A screen that reconnects after more log lines than the server keeps: "
                                           "dropped (lines from after_stream_seq on that are not sent), then the kept "
                                           "lines (here only the first of them, and the end).",
                                           resumed.sent, [hello, dropped, first, got[-1]]), gui)
        run_async(talk, gui)


def test_ws_kept_limit_example(tmp_path: Path) -> None:
    # More findings than the summary keeps, and a result over its 4 MiB limit: the result in the summary is cut to
    # its short plain fields (no participants, findings or check).
    with gui_server(tmp_path, env={"REPLAY_FLOOD": "40000"}) as gui:
        async def talk(c: Client) -> None:
            live = await c.ws()
            await live.until(is_synced)
            await c.post("api/run", {"file": "large-result.yaml", "stop_at_ms": 1000})
            await live.until(is_ended, timeout=120)
            await live.close()
            later = await c.ws()
            got = await later.until(is_synced)
            hello, synced = got[0], got[-1]
            summary = hello["session"]
            assert summary["findings_omitted"] > 0 and "participants" not in summary["result"], summary["result"]
            assert {p["detail"]["what"] for p in summary["problems"] if p["code"] == "kept-limit"} >= \
                {"findings", "result"}
            hello = {**hello, "session": {**summary, "findings": summary["findings"][:2]}}
            check_example("ws-kept-limit", ws("A screen that connects after a run with more findings than the summary "
                                              "keeps and a result larger than 4 MiB: in the summary, the result has "
                                              "only its short plain fields, and the findings are counted in "
                                              "findings_omitted (here the findings are cut to the first two, and "
                                              "the log lines between hello and synced are left out).",
                                              later.sent, [hello, synced]), gui)
        run_async(talk, gui)


@pytest.mark.parametrize("kind", ["slow-client", "resync"])
def test_ws_slow_client_examples(tmp_path: Path, kind: str) -> None:
    # A screen that reads nothing while the launcher floods: log lines are dropped for it (dropped); findings
    # cannot be, so its queue is emptied and it is sent everything again (resync).
    file = "flood.yaml" if kind == "slow-client" else "finding-flood.yaml"
    with gui_server(tmp_path, env={"REPLAY_FLOOD": "100000"}) as gui:
        async def talk(c: Client) -> None:
            slow = await c.ws()
            fast = await c.ws()
            await fast.until(is_synced)
            await c.post("api/run", {"file": file, "stop_at_ms": 1000})
            await fast.until(is_ended, timeout=120)
            want = "dropped" if kind == "slow-client" else "resync"
            got = await slow.until(lambda m: m["type"] == want, timeout=120)
            message = got[-1]
            if want == "dropped":
                assert message["reason"] == "slow-client" and message["log"].get("Engine", 0) > 0
                message = {**message, "log": {"Engine": 1000}, "events": 0}
            check_example(f"ws-{kind}", ws("What a screen that reads too slowly gets (one message; the numbers "
                                           "are examples).", [], [message]), gui)
        run_async(talk, gui)


def test_ws_unauthorized_example(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws({"token": "wrong"})
            sock.sent[0]["token"] = "wrong"
            got = await sock.until(lambda m: True)
            check_example("ws-unauthorized", {"description": "A wrong token: an error, then the server closes "
                                                             "the connection with code 4401.",
                                              "client": sock.sent, "server": got}, gui)
        run_async(talk, gui)


def test_protocol_v4_validators_catch_drift() -> None:
    """Negative tests ensuring Protocol v4 validators detect unknown/missing types, bad order, and nested drift."""
    import copy

    allowed_ex = json.loads((DIRECTORY / "ws-allowed.json").read_text(encoding="utf-8"))
    _validate_protocol_v4_example("ws-allowed", allowed_ex)

    # Unknown message type
    bad_unknown = copy.deepcopy(allowed_ex)
    bad_unknown["server"].append({"type": "mystery_msg", "stream_seq": 99})
    with pytest.raises(AssertionError):
        _validate_protocol_v4_example("ws-allowed", bad_unknown)

    # Non-monotonic stream_seq after synced
    bad_seq = copy.deepcopy(allowed_ex)
    bad_seq["server"][-1]["stream_seq"] = bad_seq["server"][-2]["stream_seq"]
    with pytest.raises(AssertionError):
        _validate_protocol_v4_example("ws-allowed", bad_seq)

    # Missing required message type compared to saved example
    missing_type = copy.deepcopy(allowed_ex)
    missing_type["server"] = [m for m in missing_type["server"] if m["type"] != "allowed"]
    with pytest.raises(AssertionError):
        _compare_ws_example("ws-allowed", missing_type, allowed_ex)

    # Unexpected extra message type (non-runtime)
    extra_type = copy.deepcopy(allowed_ex)
    extra_type["server"].append({"type": "resync", "reason": "x", "message": "y"})
    with pytest.raises(AssertionError):
        _compare_ws_example("ws-allowed", extra_type, allowed_ex)

    # Corrupted nested allowed.save.caution
    bad_nested = copy.deepcopy(allowed_ex)
    del bad_nested["server"][0]["allowed"]["save"]["caution"]
    with pytest.raises(AssertionError):
        _validate_protocol_v4_example("ws-allowed", bad_nested)

    # HTTP app.json nested launcher.version.formats drift
    app_ex = json.loads((DIRECTORY / "app.json").read_text(encoding="utf-8"))
    bad_app = copy.deepcopy(app_ex["response"])
    bad_app["launcher"]["version"]["formats"]["run_request"] = 1
    with pytest.raises(AssertionError):
        _compare_http_response("app", bad_app, app_ex["response"])

    # HTTP connections.json self-validation passes (tolerates backend-specific keys) and catches sibling key drift
    conn_ex = json.loads((DIRECTORY / "connections.json").read_text(encoding="utf-8"))
    _compare_http_response("connections", conn_ex["response"], conn_ex["response"])
    bad_conn = copy.deepcopy(conn_ex["response"])
    bad_conn["network"] = "OTHER"
    with pytest.raises(AssertionError):
        _compare_http_response("connections", bad_conn, conn_ex["response"])

    # ws-allowed detects altered allowed payload values
    bad_allowed = copy.deepcopy(allowed_ex)
    bad_allowed["server"][2]["allowed"]["start"]["ok"] = True
    with pytest.raises(AssertionError):
        _compare_ws_example("ws-allowed", bad_allowed, allowed_ex)

    # in-replay error stream_seq monotonicity
    replay_ex = json.loads((DIRECTORY / "ws-replay.json").read_text(encoding="utf-8"))
    bad_replay = copy.deepcopy(replay_ex)
    bad_replay["server"].insert(2, {
        "type": "error", "stream_seq": bad_replay["server"][1]["stream_seq"],
        "run_id": "1", "code": "bad-line", "message": "x", "detail": None,
    })
    with pytest.raises(AssertionError):
        _validate_protocol_v4_example("ws-replay", bad_replay)

    # session.launcher_state missing nested caution is rejected
    pass_ex = json.loads((DIRECTORY / "ws-pass.json").read_text(encoding="utf-8"))
    bad_pass = copy.deepcopy(pass_ex)
    sess_with_ls = next(m["session"] for m in bad_pass["server"] if m["type"] == "session" and m["session"]["launcher_state"])
    del sess_with_ls["launcher_state"]["allowed"]["pause"]["caution"]
    with pytest.raises(AssertionError):
        _validate_protocol_v4_example("ws-pass", bad_pass)


def test_openapi_and_schemas_have_no_drift() -> None:
    """Committed gui/protocol/openapi.yaml and schemas/*.schema.json match offline generation byte-for-byte."""
    problems = check_openapi_drift(PROTOCOL_DIR)
    assert problems == [], "\n".join(problems)
    assert set(OPENAPI_TARGETS) == {
        "openapi.yaml",
        "schemas/ws-server.schema.json",
        "schemas/ws-client.schema.json",
        "schemas/ws-command-event.schema.json",
        "schemas/cli-json.schema.json",
    }


def test_error_tables_match_contracts_catalog() -> None:
    """Both EN and JA gui/protocol/README error tables match contracts.ErrorCode completely (no stale no-hello)."""
    expected_http = {code.value: status for code, status in HTTP_ERROR_CODES.items()}
    assert ERROR_TABLES, "expected EN and JA protocol README pages"
    for page_name, table in ERROR_TABLES.items():
        assert table == expected_http, f"{page_name} HTTP error table drifted from contracts.HTTP_ERROR_CODES"
    ws_codes = {code.value for code in ErrorCode if code.http_status is None}
    for page in manual_pages("gui/protocol/README.md"):
        text = page.read_text(encoding="utf-8")
        assert "`no-hello`" not in text, f"{page.name} still documents removed no-hello code"
        for code in sorted(ws_codes):
            assert f"`{code}`" in text, f"{page.name} is missing WebSocket/problem error code {code!r}"


def test_cli_json_schema_validates_readiness(tmp_path: Path) -> None:
    """Real `vehicle-sil-gui --json` stdout (`gui.info`) conforms to cli-json.schema.json across modes."""
    validator = _contract_validators()["CliReadyMessage"]
    with gui_server(tmp_path) as gui:
        assert gui.info is not None
        validator.validate(gui.info)
        assert "auth" not in gui.info and "dev_url" not in gui.info
    with gui_server(tmp_path, args=("--no-auth",)) as gui:
        assert gui.info is not None
        validator.validate(gui.info)
        assert gui.info.get("auth") is False
    with gui_server(tmp_path, args=("--dev-origin", "http://127.0.0.1:4200")) as gui:
        assert gui.info is not None
        validator.validate(gui.info)
        assert isinstance(gui.info.get("dev_url"), str) and gui.info["dev_url"].startswith("http://127.0.0.1:4200")
    for bad in (
        {"vehicle_sil_gui": 4, "url": "http://127.0.0.1:8765/", "port": 8765, "auth": True},
        {"vehicle_sil_gui": 4, "url": "http://127.0.0.1:8765/", "port": 8765, "auth": None},
        {"vehicle_sil_gui": 4, "url": "http://127.0.0.1:8765/", "port": 0},
        {"vehicle_sil_gui": 4, "url": "http://127.0.0.1:8765/", "port": 8765, "extra": 1},
    ):
        with pytest.raises(jsonschema.ValidationError):
            validator.validate(bad)


def test_protocol_contract_schemas_are_valid() -> None:
    """openapi.yaml (all 17 routes) and all 4 schemas/*.schema.json pass Draft 2020-12 schema checks."""
    openapi_doc = _openapi_document()
    assert openapi_doc["openapi"] == "3.1.0"
    routes = route_specs()
    assert len(routes) == 17
    for spec in routes:
        op = openapi_doc["paths"][spec.path][spec.method.lower()]
        assert op["operationId"] == spec.operation_id
        expected_statuses = {"200", *(str(s) for s in spec.error_statuses)}
        assert expected_statuses <= set(op["responses"].keys()), (spec.path, spec.method)
    assert "502" in openapi_doc["paths"]["/api/write"]["post"]["responses"]
    assert "bearerAuth" in openapi_doc["components"]["securitySchemes"]
    for schema_name, schema in openapi_doc["components"]["schemas"].items():
        jsonschema.Draft202012Validator.check_schema(schema)
    for schema_path in (WS_SERVER_SCHEMA_PATH, WS_CLIENT_SCHEMA_PATH, WS_COMMAND_SCHEMA_PATH, CLI_JSON_SCHEMA_PATH):
        jsonschema.Draft202012Validator.check_schema(json.loads(schema_path.read_text(encoding="utf-8")))


def test_protocol_contract_schemas_catch_drift() -> None:
    """Negative and positive checks proving OpenAPI and WS command schemas enforce exact wire contracts."""
    validators = _contract_validators()
    req_v = validators["TimeCommandRequest"]
    ack_v = validators["TimeCommandAck"]
    ws_v = validators["WsCommandEvent"]

    # Valid TimeCommandRequest shapes: targeted (both) and untargeted (neither)
    for good in (
        {"command": "pause"},
        {"command": "resume", "id": 0},
        {"command": "step", "ms": 1, "id": 1 << 53, "run_id": "abc-1", "instance": "abc"},
        {"command": "run_until", "at_ms": 0},
        {"command": "run_until", "at_ms": 10**12, "run_id": "abc-1", "instance": "abc"},
        {"command": "speed", "value": "max"},
        {"command": "speed", "value": 0.000001},
        {"command": "speed", "value": 1000000, "id": 7, "run_id": "abc-1", "instance": "abc"},
    ):
        req_v.validate(good)

    # Invalid TimeCommandRequest shapes: single target, bad ranges, bad types, extra fields
    for bad in (
        {"command": "pause", "run_id": "1"},
        {"command": "pause", "instance": "0123456789abcdef"},
        {"command": "pause", "run_id": "", "instance": "0123456789abcdef"},
        {"command": "pause", "run_id": "1", "instance": ""},
        {"command": "pause", "run_id": None, "instance": "0123456789abcdef"},
        {"command": "stop"},
        {"command": "pause", "ms": 10},
        {"command": "pause", "extra": 1},
        {"command": "step", "ms": 0},
        {"command": "step", "ms": -5},
        {"command": "step", "ms": 10**12 + 1},
        {"command": "step", "ms": True},
        {"command": "step", "ms": 1.5},
        {"command": "run_until", "at_ms": -1},
        {"command": "run_until", "at_ms": 10**12 + 1},
        {"command": "speed", "value": 0},
        {"command": "speed", "value": 1e-7},
        {"command": "speed", "value": 1_000_000.1},
        {"command": "speed", "value": "fast"},
        {"command": "speed", "value": True},
        {"command": "pause", "id": -1},
        {"command": "pause", "id": (1 << 53) + 1},
        {"command": "pause", "id": True},
        {"command": "pause", "id": "7"},
    ):
        with pytest.raises(jsonschema.ValidationError, match="."):
            req_v.validate(bad)

    # TimeCommandAck must not echo run_id/instance inside command
    ack_v.validate({"run_id": "abc-1", "command": {"command": "step", "ms": 25, "id": 3}})
    with pytest.raises(jsonschema.ValidationError):
        ack_v.validate({"run_id": "abc-1", "command": {"command": "step", "ms": 25, "run_id": "abc-1", "instance": "abc"}})

    # RunTarget enforces both non-empty strings with no extra keys
    target_v = validators["RunTarget"]
    target_v.validate({"run_id": "abc-1", "instance": "abc"})
    for bad_target in (
        {"run_id": "abc-1"},
        {"instance": "abc"},
        {"run_id": "", "instance": "abc"},
        {"run_id": "abc-1", "instance": "abc", "extra": 1},
    ):
        with pytest.raises(jsonschema.ValidationError):
            target_v.validate(bad_target)

    # Valid WS command events (accepted, refused, launcher-level refused with asked_run)
    for good_ws in (
        {
            "type": "event",
            "stream_seq": 10,
            "run_id": "abc-1",
            "event": {
                "event": "command", "seq": 5, "t_ms": 100, "run": "abc-1",
                "command": "step", "source": "stdin", "accepted": True,
                "message": None, "reason": None, "ms": 30, "at_ms": 30, "id": 7,
            },
        },
        {
            "type": "event",
            "stream_seq": 11,
            "run_id": "abc-1",
            "event": {
                "event": "command", "seq": 6, "t_ms": 110, "run": "abc-1",
                "command": "step", "source": "stdin", "accepted": False,
                "message": "step works while paused", "reason": "not-paused", "id": 8,
            },
        },
        {
            "type": "event",
            "stream_seq": 12,
            "run_id": "abc-1",
            "event": {
                "event": "command", "seq": 7, "t_ms": 120,
                "command": None, "source": "stdin", "accepted": False,
                "message": "wrong run", "reason": "run-changed", "asked_run": "abc-0", "id": None,
            },
        },
    ):
        ws_v.validate(good_ws)

    # Invalid WS command events
    base_ev = {
        "event": "command", "seq": 5, "t_ms": 100, "run": "abc-1",
        "command": "step", "source": "stdin", "accepted": True,
        "message": None, "reason": None, "id": 7,
    }
    for bad_ws in (
        # accepted: true with non-null reason
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "reason": "not-paused"}},
        # accepted: true with asked_run (only emitted on launcher-level refusals)
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "asked_run": "abc-1"}},
        # accepted: true with null command
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "command": None}},
        # ms present on a non-step command
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "command": "pause", "ms": 10}},
        # accepted: false with ms or at_ms
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "accepted": False, "reason": "not-paused", "ms": 10}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "accepted": False, "reason": "not-paused", "at_ms": 10}},
        # missing required id
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {k: v for k, v in base_ev.items() if k != "id"}},
        # unknown source
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "source": "http"}},
        # invalid id type/range
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "id": -1}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "id": True}},
        # extra key on envelope or event
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "extra": 1, "event": base_ev},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "extra": 1}},
    ):
        with pytest.raises(jsonschema.ValidationError):
            ws_v.validate(bad_ws)

    # Request schema precision: non-empty writes and unique participant lists
    write_v = validators["req:POST /api/write"]
    for bad_write in (
        {"run_id": "abc-1", "writes": {}},
        {"run_id": "abc-1", "writes": {"Engine": {}}},
    ):
        with pytest.raises(jsonschema.ValidationError):
            write_v.validate(bad_write)
    for route_key in ("req:POST /api/prepare", "req:POST /api/run", "req:POST /api/check"):
        for field in ("disable", "enable", "manual"):
            with pytest.raises(jsonschema.ValidationError):
                validators[route_key].validate({"file": "simulation.yaml", field: ["A", "A"]})

    # WS server schema precision: seq/t_ms on all launcher events, tightened summaries, non-empty availability, float scenario ms
    ws_srv_v = validators["WsServerMessage"]
    for bad_ev_frame in (
        {"type": "event", "stream_seq": 2, "run_id": "abc-1", "event": {"event": "phase", "phase": "running"}},
        {"type": "event", "stream_seq": 2, "run_id": "abc-1", "event": {"event": "phase", "seq": "bad", "phase": "running"}},
        {"type": "event", "stream_seq": 2, "run_id": "abc-1", "event": {"event": "phase", "seq": 1, "t_ms": "bad", "phase": "running"}},
        {"type": "event", "stream_seq": 2, "run_id": "abc-1", "event": {"event": "check", "seq": 1, "check": {"participants": []}}},
        {"type": "event", "stream_seq": 2, "run_id": "abc-1", "event": {"event": "result", "seq": 1, "result": {}}},
        {"type": "availability", "stream_seq": 2, "run_id": "abc-1", "peers": {}},
    ):
        with pytest.raises(jsonschema.ValidationError):
            ws_srv_v.validate(bad_ev_frame)

    import copy
    sc_ex = json.loads((DIRECTORY / "ws-scenario.json").read_text(encoding="utf-8"))
    end_sess = copy.deepcopy([m for m in sc_ex["server"] if m["type"] == "session"][-1])
    # Sub-millisecond float scenario timings are accepted by both WS and HTTP session schemas
    end_sess["session"]["scenario"]["over_at_ms"] = 12.5
    end_sess["session"]["scenario"]["steps"][0]["start_ms"] = 0.5
    end_sess["session"]["scenario"]["steps"][0]["at_ms"] = 10.25
    end_sess["session"]["scenario"]["steps"][0]["end_ms"] = 12.5
    ws_srv_v.validate(end_sess)
    # Missing required nested summary fields are rejected directly by ws-server.schema.json
    for mutate in (
        lambda s: s["time"].pop("event"),
        lambda s: s["registry"].clear(),
        lambda s: s["result"].clear(),
    ):
        broken = copy.deepcopy(end_sess)
        mutate(broken["session"])
        with pytest.raises(jsonschema.ValidationError):
            ws_srv_v.validate(broken)


def test_openapi_error_descriptions_match_http_error_codes() -> None:
    """Every HTTP status and error code in contracts.HTTP_ERROR_CODES (except router-level static 405)
    must appear under its matching status in openapi.ERROR_DESCRIPTIONS without extras or omissions."""
    static_only_statuses = {405}
    codes_for_405 = {code for code, status in HTTP_ERROR_CODES.items() if status in static_only_statuses}
    assert codes_for_405 == {ErrorCode.METHOD_NOT_ALLOWED}

    expected_statuses = {status for status in HTTP_ERROR_CODES.values() if status not in static_only_statuses}
    assert set(ERROR_DESCRIPTIONS) == expected_statuses

    for status, description in ERROR_DESCRIPTIONS.items():
        expected_codes = {code.value for code, st in HTTP_ERROR_CODES.items() if st == status}
        described_codes = set(re.findall(r"`([a-z][a-z0-9-]*)`", description))
        assert described_codes == expected_codes, (status, described_codes, expected_codes)
