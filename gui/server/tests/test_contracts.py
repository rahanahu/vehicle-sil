"""Unit, schema-parity, live-producer, and cross-module parity tests for `vehicle_sil_gui.contracts`."""

from __future__ import annotations

import json
import math
import re
from pathlib import Path

import jsonschema
import pytest
from pydantic import ValidationError
from vehicle_sil_control import protocol

from vehicle_sil_gui import config_save, contracts as C, runtime
from vehicle_sil_gui.launcher import InvalidRequest
from vehicle_sil_gui.refusal import Refused
from vehicle_sil_gui.server import route_specs
from vehicle_sil_gui.session import Limits as SessionLimits, Session
from vehicle_sil_gui.source import ListSample, OperationInfo, PeerInfo, TargetInfo
from vehicle_sil_gui.variables import Limits as VariableLimits, Rejected, Variables, Watch, check_watch

REPOSITORY = Path(__file__).resolve().parents[3]
EXAMPLES_DIR = REPOSITORY / "gui" / "protocol" / "examples"


# ------------------------------------------------------------ Strict coercion & numeric bounds


def test_huge_int_and_non_finite_speed_raise_validation_error_not_overflow() -> None:
    """`10**400`, `NaN`, `Inf`, `bool`, and out-of-range speeds produce ValidationError (never OverflowError)."""
    for bad in (10**400, -(10**400), math.nan, math.inf, -math.inf, 0, -1, 1e-7, 1_000_000.1, True, False, "fast", None):
        with pytest.raises(ValidationError):
            C.TIME_COMMAND_REQUEST_ADAPTER.validate_python({"command": "speed", "value": bad})
        with pytest.raises(ValidationError):
            C.RunRequest.model_validate({"file": "simulation.yaml", "speed": bad})

    for good in ("max", 1, 1_000_000, 0.000001, 1.5, 1_000_000.0):
        cmd, run_id, inst = C.parse_time_command_request({"command": "speed", "value": good})
        assert cmd["command"] == "speed" and run_id is None and inst is None
        req = C.RunRequest.model_validate({"file": "simulation.yaml", "speed": good})
        assert req.speed == good


def test_bool_and_float_are_rejected_for_strict_int_fields() -> None:
    for payload in (
        {"command": "step", "ms": True},
        {"command": "step", "ms": False},
        {"command": "step", "ms": 1.5},
        {"command": "step", "ms": "10"},
        {"command": "run_until", "at_ms": True},
        {"command": "pause", "id": True},
        {"command": "pause", "id": "7"},
    ):
        with pytest.raises(ValidationError):
            C.TIME_COMMAND_REQUEST_ADAPTER.validate_python(payload)

    for payload in (
        {"file": "a.yaml", "vehicle_sil_run_request": True},
        {"file": "a.yaml", "stop_at_ms": True},
        {"file": "a.yaml", "late_tolerance_ms": False},
        {"file": "a.yaml", "strict": 1},
        {"file": "a.yaml", "start_paused": 1},
    ):
        with pytest.raises(ValidationError):
            C.RunRequest.model_validate(payload)


def test_parse_strict_json_rejects_non_finite_and_deep_nesting(monkeypatch: pytest.MonkeyPatch) -> None:
    for raw in ("NaN", "Infinity", "-Infinity", '{"x": 1e999}'):
        with pytest.raises(ValueError):
            C.parse_strict_json(raw)
    assert C.parse_strict_json('{"a": 1, "b": 2.5}') == {"a": 1, "b": 2.5}

    def raise_recursion(*_args: object, **_kwargs: object) -> object:
        raise RecursionError("too deep")

    monkeypatch.setattr(json, "loads", raise_recursion)
    with pytest.raises(ValueError, match="too deep"):
        C.parse_strict_json("{}")
    with pytest.raises(ValueError, match="not JSON"):
        C.parse_json_object("{}")


# ------------------------------------------------------------ OmitOnly runtime & JSON Schema parity


def _collect_null_schemas(node: object, path: str = "") -> list[str]:
    found: list[str] = []
    if isinstance(node, dict):
        if node == {"type": "null"} or node.get("type") == "null":
            found.append(path)
        if "default" in node and node["default"] is None:
            found.append(f"{path}.default=null")
        for k, v in node.items():
            found.extend(_collect_null_schemas(v, f"{path}.{k}" if path else k))
    elif isinstance(node, list):
        for i, item in enumerate(node):
            found.extend(_collect_null_schemas(item, f"{path}[{i}]"))
    return found


def test_omit_only_fields_reject_null_at_runtime_and_emit_no_null_in_json_schema() -> None:
    for model, bad in (
        (C.RunRequest, {"file": "a.yaml", "variant": None}),
        (C.RunRequest, {"file": "a.yaml", "speed": None}),
        (C.RunRequest, {"file": "a.yaml", "disable": None}),
        (C.RunRequest, {"file": "a.yaml", "$schema": None}),
        (C.RunRequest, {"file": "a.yaml", "stop_at_ms": None}),
        (C.RunRequest, {"file": "a.yaml", "scenario": None}),
        (C.RunRequest, {"file": "a.yaml", "after_scenario": None}),
        (C.StopSessionRequest, {"run_id": "r1", "instance": "i1", "force": None}),
        (C.WriteRequest, {"run_id": "r1", "instance": None, "writes": {"E": {"x": 1}}}),
        (C.WriteRequest, {"run_id": "r1", "writes": {"E": {"x": 1}}, "late": None}),
        (C.WatchPayload, {"id": 1, "variables": "all", "period_ms": None}),
        (C.CliReadyMessage, {"vehicle_sil_gui": 4, "url": "http://127.0.0.1:8765/", "port": 8765, "dev_url": None}),
        (C.CliReadyMessage, {"vehicle_sil_gui": 4, "url": "http://127.0.0.1:8765/", "port": 8765, "auth": None}),
    ):
        with pytest.raises(ValidationError):
            model.model_validate(bad)

    for model in (C.RunRequest, C.StopSessionRequest, C.PrepareRequest, C.DescribeRequest, C.CliReadyMessage):
        schema = model.model_json_schema(schema_generator=C.ContractJsonSchemaGenerator)
        jsonschema.Draft202012Validator.check_schema(schema)
        assert _collect_null_schemas(schema) == [], (model.__name__, schema)

    err_schema = C.ErrorResponse.model_json_schema(schema_generator=C.ContractJsonSchemaGenerator)
    jsonschema.Draft202012Validator.check_schema(err_schema)
    assert "detail" in err_schema["$defs"]["ErrorBody"]["required"]

    save_schema = C.ConfigSaveRequest.model_json_schema(schema_generator=C.ContractJsonSchemaGenerator)
    jsonschema.Draft202012Validator.check_schema(save_schema)
    stamp_branches = save_schema["properties"]["stamp"]["anyOf"]
    assert any(b.get("pattern") == C.STAMP_PATTERN for b in stamp_branches)


# ------------------------------------------------------------ TimeCommand & WS schema drift checks


def test_time_command_and_ack_json_schemas_enforce_exact_contracts() -> None:
    req_schema = C.TIME_COMMAND_REQUEST_ADAPTER.json_schema(schema_generator=C.ContractJsonSchemaGenerator)
    ack_schema = C.TimeCommandAck.model_json_schema(schema_generator=C.ContractJsonSchemaGenerator)
    jsonschema.Draft202012Validator.check_schema(req_schema)
    jsonschema.Draft202012Validator.check_schema(ack_schema)
    assert _collect_null_schemas(req_schema) == []

    req_v = jsonschema.Draft202012Validator(req_schema)
    ack_v = jsonschema.Draft202012Validator(ack_schema)

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
        with pytest.raises(jsonschema.ValidationError):
            req_v.validate(bad)

    ack_v.validate({"run_id": "abc-1", "command": {"command": "step", "ms": 25, "id": 3}})
    with pytest.raises(jsonschema.ValidationError):
        ack_v.validate({"run_id": "abc-1", "command": {"command": "step", "ms": 25, "run_id": "abc-1", "instance": "abc"}})


def test_ws_command_event_enforces_all_six_conditional_rules() -> None:
    ws_schema = C.WsCommandEventMessage.model_json_schema(schema_generator=C.ContractJsonSchemaGenerator)
    jsonschema.Draft202012Validator.check_schema(ws_schema)
    ws_v = jsonschema.Draft202012Validator(ws_schema)

    base_ev = {
        "event": "command", "seq": 5, "t_ms": 100, "run": "abc-1",
        "command": "step", "source": "stdin", "accepted": True,
        "message": None, "reason": None, "id": 7,
    }
    for good in (
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "ms": 30, "at_ms": 30}},
        {"type": "event", "stream_seq": 11, "run_id": "abc-1",
         "event": {**base_ev, "command": "pause", "at_ms": 30}},
        {"type": "event", "stream_seq": 12, "run_id": "abc-1",
         "event": {**base_ev, "accepted": False, "reason": "not-paused", "message": "step works while paused"}},
        {"type": "event", "stream_seq": 13, "run_id": "abc-1",
         "event": {"event": "command", "seq": 7, "t_ms": 120, "command": None, "source": "stdin",
                   "accepted": False, "message": "wrong run", "reason": "run-changed",
                   "asked_run": "abc-0", "id": None}},
    ):
        C.WsCommandEventMessage.model_validate(good)
        ws_v.validate(good)

    for bad in (
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "reason": "not-paused"}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "asked_run": "abc-1"}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "command": None}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "command": "pause", "ms": 10}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1",
         "event": {**base_ev, "accepted": False, "reason": "not-paused", "ms": 10}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1",
         "event": {**base_ev, "accepted": False, "reason": "not-paused", "at_ms": 10}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1",
         "event": {k: v for k, v in base_ev.items() if k != "id"}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "source": "http"}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "id": -1}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "id": True}},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "extra": 1, "event": base_ev},
        {"type": "event", "stream_seq": 10, "run_id": "abc-1", "event": {**base_ev, "extra": 1}},
    ):
        with pytest.raises(ValidationError):
            C.WsCommandEventMessage.model_validate(bad)
        with pytest.raises(jsonschema.ValidationError):
            ws_v.validate(bad)


def test_ws_server_event_adapter_rejects_malformed_known_events_and_allows_unknown() -> None:
    """Known Events v4 kinds cannot bypass validation through UnknownLauncherEvent; unknown kinds pass."""
    ws_schema = C.WS_SERVER_MESSAGE_ADAPTER.json_schema(schema_generator=C.ContractJsonSchemaGenerator)
    jsonschema.Draft202012Validator.check_schema(ws_schema)
    ws_v = jsonschema.Draft202012Validator(ws_schema)

    mystery = {"type": "event", "stream_seq": 4, "run_id": "r1", "event": {"event": "mystery", "seq": 1, "extra": 1}}
    C.WS_SERVER_MESSAGE_ADAPTER.validate_python(mystery)
    ws_v.validate(mystery)

    for bad in (
        {"type": "event", "stream_seq": 1, "run_id": "r1", "event": {"event": "time", "seq": 1, "sim_ms": "NOT_AN_INT"}},
        {"type": "event", "stream_seq": 2, "run_id": "r1", "event": {"event": "log", "seq": 2, "source": 123, "line": None}},
        {"type": "event", "stream_seq": 3, "run_id": "r1", "event": {"event": "command", "seq": 3, "accepted": "nope"}},
        {"type": "event", "stream_seq": 4, "run_id": "r1", "event": {"event": "phase", "phase": "running"}},
        {"type": "event", "stream_seq": 5, "run_id": "r1", "event": {"event": "phase", "seq": "bad", "phase": "running"}},
        {"type": "event", "stream_seq": 6, "run_id": "r1", "event": {"event": "phase", "seq": 1, "t_ms": "bad", "phase": "running"}},
        {"type": "event", "stream_seq": 7, "run_id": "r1", "event": {"event": "mystery", "extra": 1}},
        {
            "type": "event", "stream_seq": 10, "run_id": "abc-1",
            "event": {
                "event": "command", "seq": 5, "t_ms": 100, "run": "abc-1",
                "command": "pause", "source": "stdin", "accepted": True,
                "message": None, "reason": "not-paused", "ms": 10, "id": 7,
            },
        },
    ):
        with pytest.raises(ValidationError):
            C.WS_SERVER_MESSAGE_ADAPTER.validate_python(bad)
        with pytest.raises(jsonschema.ValidationError):
            ws_v.validate(bad)


# ------------------------------------------------------------ Live producer-to-contract validation


def test_live_producer_edge_cases_match_contract_models() -> None:
    """Covers real server outputs absent from static examples (failed_write int, non-finite real None,
    dropped-check stop_at_ms=None, and control_link preceding participant)."""
    # 1. OperationPeerResult.failed_write is a 0-based NonNegInt (not str)
    v = Variables("r1", lambda _: None)
    op_entry = v._operation(OperationInfo(
        seq=3,
        done=True,
        targets=[TargetInfo(peer="Engine", status="rejected", applied_at_ns=None, late=False,
                            reason="out of range", applied_writes=0, failed_write=0)],
    ))
    v._keep_operation(op_entry)
    C.WsOperationMessage.model_validate({"type": "operation", "stream_seq": 12, "run_id": "r1", **op_entry})
    C.VariablesSummary.model_validate(v.summary())
    with pytest.raises(ValidationError):
        C.OperationPeerResult.model_validate({**op_entry["peers"]["Engine"], "failed_write": "bad"})

    # 2. PeerValuesSnapshot.values carries None for non-finite real ECU values, and VariableSpec requires strict builtin
    v.on_peers([PeerInfo("Engine", "inc1", True, [
        {"name": "ecu.connected", "type": "bool", "role": "input", "unit": "", "min": None, "max": None,
         "step": None, "description": "", "enum": None, "builtin": True, "availability": None},
        {"name": "temp", "type": "real", "role": "output", "unit": "C", "min": None, "max": None,
         "step": None, "description": "", "enum": None, "builtin": False, "availability": None},
    ])], [])
    v.on_synced()
    summary = C.VariablesSummary.model_validate(v.summary())
    assert [(var.name, var.builtin) for var in summary.peers[0].variables] == [
        ("ecu.connected", True),
        ("temp", False),
    ]
    v.on_samples({"Engine": ListSample("inc1", 10_000_000, 1, [True, None])})
    values_msg = v.values_message(Watch(1, None, 0.02, 0.0, lambda _: None))
    assert values_msg["peers"]["Engine"]["values"]["temp"] is None
    C.WsValuesMessage.model_validate(values_msg)
    base_var = v.summary()["peers"][0]["variables"][0]
    ws_schema = C.WS_SERVER_MESSAGE_ADAPTER.json_schema(schema_generator=C.ContractJsonSchemaGenerator)
    ws_v = jsonschema.Draft202012Validator(ws_schema)
    for bad_var in (
        {k: val for k, val in base_var.items() if k != "builtin"},
        {**base_var, "builtin": None},
        {**base_var, "builtin": 1},
        {**base_var, "builtin": "true"},
    ):
        with pytest.raises(ValidationError):
            C.VariableSpec.model_validate(bad_var)
        bad_msg = {
            "type": "variables",
            "stream_seq": 1,
            "run_id": "r1",
            "variables": {
                **v.summary(),
                "peers": [{"name": "Engine", "connected": True, "variables": [bad_var]}],
            },
        }
        with pytest.raises(jsonschema.ValidationError):
            ws_v.validate(bad_msg)

    # 3. Session.summary() with dropped-check stop_at_ms=None AND control_link preceding participant (no state)
    sess = Session("r1", "simulation.yaml", [], None, False, limits=SessionLimits(object_bytes=120))
    sess.feed_event({"event": "check", "seq": 1, "check": {"stop_at_ms": None, "pad": "x" * 200}}, 260)
    sess.feed_event({"event": "control_link", "seq": 2, "name": "Engine", "state": "connected", "variables": 2}, 90)
    ws_summary = sess.summary(variables=False)
    assert ws_summary["stop_at_ms"] is None
    assert "state" not in ws_summary["participants"][0]
    C.SessionWsSummary.model_validate(ws_summary)
    C.SessionSummary.model_validate(sess.summary(variables=True))

    # 4. Sub-millisecond float scenario timings (start_ms, at_ms, end_ms, over_at_ms) match C++ w.real(...)
    sc_sess = Session("r2", "simulation.yaml", [], None, False, scenario="scenarios/throttle.yaml")
    sc_sess.feed_event({
        "event": "scenario_step", "seq": 1, "t_ms": 10, "index": 1, "status": "passed",
        "start_ms": 0.5, "at_ms": 10.25, "end_ms": 12.5,
    }, 120)
    sc_sess.feed_event({
        "event": "result", "seq": 2, "t_ms": 20,
        "result": {
            "result": "passed", "ok": True,
            "scenario": {
                "file": "scenarios/throttle.yaml", "name": "sub-ms", "status": "passed",
                "over_at_ms": 12.5,
                "steps": [{"index": 1, "status": "passed", "start_ms": 0.5, "at_ms": 10.25, "end_ms": 12.5}],
            },
        },
    }, 240)
    C.SessionSummary.model_validate(sc_sess.summary(variables=True))
    C.WS_SERVER_MESSAGE_ADAPTER.validate_python(sc_sess.message())


# ------------------------------------------------------------ Protocol v4 input compatibility


def test_protocol_v4_input_compatibility_preserved() -> None:
    # POST /api/write accepts explicit at_ms: null (equivalent to omitting at_ms)
    v = Variables("r1", lambda _: None)
    v.on_peers([PeerInfo("Engine", "inc1", True, [
        {"name": "throttle", "type": "real", "role": "input", "unit": "%", "min": 0.0, "max": 100.0,
         "step": None, "description": "", "enum": None, "availability": None},
    ])], [])
    v.on_synced()
    v.source = object()  # type: ignore[assignment]
    writes, at_ns, late = v.check_write({"run_id": "r1", "writes": {"Engine": {"throttle": 50}}, "at_ms": None})
    assert writes == [("Engine", "throttle", 50.0)] and at_ns is None and late == "reject"

    # WS watch ignores unknown extra top-level and variable-item keys (matching auth/unwatch)
    limits = VariableLimits()
    watch_id, wanted, period_s = check_watch(
        {
            "type": "watch",
            "id": 7,
            "variables": [{"peer": "Engine", "name": "rpm", "extra_item": 1}],
            "period_ms": 100,
            "extra_top": True,
        },
        limits,
    )
    assert (watch_id, wanted, period_s) == (7, [("Engine", "rpm")], 0.1)

    # WS auth accepts nullable optional fields and ignores unknown keys (matching server.py)
    auth = C.WsAuthMessage.model_validate(
        {"type": "auth", "token": "tok", "instance": None, "run_id": None, "after_stream_seq": None, "extra": 1}
    )
    assert auth.token == "tok" and auth.run_id is None

    # Non-string scenario defers to resolve_scenario (invalid-path), while explicit null still fails invalid-field
    req, _ = C.validate_model_defer_file(C.RunRequest, {"file": "a.yaml", "scenario": 123})
    assert req.file == "a.yaml"
    with pytest.raises(ValidationError):
        C.validate_model_defer_file(C.RunRequest, {"file": "a.yaml", "scenario": None})


# ------------------------------------------------------------ All captured protocol examples & ErrorCode catalog


def test_all_captured_protocol_examples_validate_against_typed_models() -> None:
    specs = route_specs()
    http_resp_models = {
        (spec.method, spec.path.lstrip("/")): spec.response_model
        for spec in specs
        if spec.response_model is not None
    }
    http_req_models = {
        (spec.method, spec.path.lstrip("/")): spec.request_model
        for spec in specs
        if spec.request_model is not None
    }
    examples = sorted(EXAMPLES_DIR.glob("*.json"))
    assert len(examples) >= 60
    timeline_schema = json.loads((REPOSITORY / "schema" / "timeline.schema.json").read_text(encoding="utf-8"))
    timeline_validator = jsonschema.validators.validator_for(timeline_schema)(timeline_schema)
    for path in examples:
        data = json.loads(path.read_text(encoding="utf-8"))
        if "server" in data:
            for msg in data["server"]:
                C.WS_SERVER_MESSAGE_ADAPTER.validate_python(msg)
                if msg.get("type") == "event" and msg["event"].get("event") == "command":
                    C.WsCommandEventMessage.model_validate(msg)
        else:
            status = data["status"]
            req = data["request"]
            raw_path = req["path"].split("?", 1)[0]
            route = (req["method"], raw_path)
            if status < 400 and req["method"] == "GET" and raw_path.startswith("api/timeline/"):
                timeline_validator.validate(data["response"])
                continue
            model = C.ErrorResponse if status >= 400 else http_resp_models[route]
            model.model_validate(data["response"])
            if status < 400 and "body" in req:
                if route == ("POST", "api/time"):
                    C.TIME_COMMAND_REQUEST_ADAPTER.validate_python(req["body"])
                elif route == ("POST", "api/check"):
                    req_model = C.CheckTextRequest if "text" in req["body"] else C.CheckFileRequest
                    req_model.model_validate(req["body"])
                else:
                    http_req_models[route].model_validate(req["body"])


def test_http_error_codes_match_protocol_readme_tables() -> None:
    readme = (REPOSITORY / "gui" / "protocol" / "README.md").read_text(encoding="utf-8")
    doc_codes = {
        code: int(status)
        for status, code in re.findall(r"^\| (\d{3}) \| `([a-z-]+)` \|", readme, re.MULTILINE)
    }
    catalog = {code.value: status for code, status in C.HTTP_ERROR_CODES.items()}
    assert catalog == doc_codes


# ------------------------------------------------------------ Events v4 & subprocess parsers


def test_validate_launcher_event_passthrough_and_bad_shapes() -> None:
    assert C.validate_launcher_event({"event": "future_metric", "seq": 1, "payload": [1, 2]}) is None
    assert C.validate_launcher_event({"event": "phase", "phase": "running", "future_key": 42}) is None
    assert C.validate_launcher_event({"event": "check", "check": {"participants": [{"name": "E"}], "extra": 1}}) is None
    assert C.validate_launcher_event({"event": "heartbeat", "seq": 2}) is None

    for bad in (
        {"event": "run_start", "run": ""},
        {"event": "phase", "phase": 5},
        {"event": "check", "check": "not-an-object"},
        {"event": "check", "check": {"participants": [{"name": 1}]}},
        {"event": "registry", "uri": 123},
        {"event": "participant", "name": "E", "state": "running", "pid": True},
        {"event": "log", "source": None, "line": "hi"},
        {"event": "finding", "severity": "error", "message": 123},
        {"event": "time", "sim_ms": True},
        {"event": "log_dropped", "count": -1, "sources": {}},
        {"event": "log_dropped", "count": 1, "sources": {"E": True}},
        {"event": "command", "command": "step", "accepted": "yes"},
        {"event": "command", "command": "step", "accepted": True, "id": -1},
        {"event": "state", "rev": True, "allowed": {}},
        {"event": "result", "result": []},
        {"event": "control_link", "name": "E", "state": 1},
        {"event": "scenario_step", "index": 0, "status": "ok"},
    ):
        reason = C.validate_launcher_event(bad)
        assert isinstance(reason, str) and reason, bad


def test_runtime_inspection_parsers_fail_closed() -> None:
    with pytest.raises(Refused) as exc:
        runtime.parse_file_only_check({"result": "passed", "participants": "bad", "hot_eligibility": {}})
    assert (exc.value.status, exc.value.code) == (502, "launcher-output")

    assert runtime.parse_prepare_event({"accepted": False, "status": {"eligibility": {}}, "metadata": {}}) is None
    insp = runtime.parse_prepare_event({
        "accepted": True,
        "status": {"eligibility": {"eligible": True}},
        "metadata": {"participants": ["E"], "networks": ["CAN0"], "file_sha256": "a" * 64},
        "findings": [],
    })
    assert insp is not None and insp.hot.hot and insp.participants == ["E"] and insp.networks == ["CAN0"]


# ------------------------------------------------------------ Dynamic VariableLimits & 413 text-too-large


def test_dynamic_variable_limits_and_config_save_413_precedence() -> None:
    limits = VariableLimits(watch_variables=1, period_ms_min=50, period_ms_max=100, writes_per_request=2)
    assert check_watch({"type": "watch", "id": 1, "variables": "all", "period_ms": 50}, limits) == (1, None, 0.05)
    with pytest.raises(Rejected) as e_period:
        check_watch({"id": 1, "variables": "all", "period_ms": 49}, limits)
    assert e_period.value.code == "invalid-watch" and "50 to 100" in str(e_period.value)
    with pytest.raises(Rejected) as e_vars:
        check_watch({"id": 1, "variables": [{"peer": "A", "name": "x"}, {"peer": "A", "name": "y"}]}, limits)
    assert e_vars.value.code == "invalid-watch" and "at most 1" in str(e_vars.value)

    variables = Variables("r1", lambda _: None, limits=limits)
    with pytest.raises(Rejected) as e_writes:
        variables.check_write({"writes": {"E": {"a": 1, "b": 2, "c": 3}}})
    assert e_writes.value.code == "invalid-field" and "at most 2" in str(e_writes.value)

    with pytest.raises(Refused) as e_big:
        config_save.save_request({"file": "a.yaml", "text": "あ" * 4, "stamp": None}, max_text=10)
    assert (e_big.value.status, e_big.value.code) == (413, "text-too-large")
    with pytest.raises(InvalidRequest) as e_surr:
        config_save.save_request({"file": "a.yaml", "text": "\ud800", "stamp": None}, max_text=10)
    assert e_surr.value.code == "invalid-field"


# ------------------------------------------------------------ Parity with stdlib-only vehicle_sil_control.protocol


def test_contracts_constants_and_request_fields_stay_in_sync() -> None:
    assert C.EVENTS_FORMAT == protocol.EVENTS_FORMAT
    assert C.SPEED_MIN == protocol.SPEED_MIN
    assert C.SPEED_MAX == protocol.SPEED_MAX
    assert C.STEP_MS_MAX == protocol.STEP_MS_MAX
    assert C.MAX_ID == protocol.MAX_ID
    assert C.TIME_COMMANDS == protocol.TIME_COMMANDS
    assert set(C.REQUEST_FIELDS) == set(C.model_allowed_fields(C.RunRequest))
    assert len(C.REQUEST_FIELDS) == len(set(C.REQUEST_FIELDS))
    assert set(C.CHECK_FIELDS) == set(C.REQUEST_FIELDS) | {"text"}


def test_after_scenario_recording_end_ms_and_handover_state_contracts() -> None:
    for ok_req in (
        {"file": "a.yaml", "scenario": "scenarios/a.yaml", "after_scenario": "stop"},
        {"file": "a.yaml", "scenario": "scenarios/a.yaml", "after_scenario": "pause"},
    ):
        req = C.RunRequest.model_validate(ok_req)
        assert req.after_scenario == ok_req["after_scenario"]
        chk = C.CheckFileRequest.model_validate(ok_req)
        assert chk.after_scenario == ok_req["after_scenario"]

    for bad_req in (
        {"file": "a.yaml", "after_scenario": "hold"},
        {"file": "a.yaml", "after_scenario": 1},
        {"file": "a.yaml", "after_scenario": True},
        {"file": "a.yaml", "after_scenario": None},
    ):
        with pytest.raises(ValidationError):
            C.RunRequest.model_validate(bad_req)

    with pytest.raises(ValidationError):
        C.CheckTextRequest.model_validate({"file": "a.yaml", "text": "x", "after_scenario": "pause"})
    with pytest.raises(ValidationError):
        C.CheckTextRequest.model_validate({"file": "a.yaml", "text": "x", "record": ["Engine.engine.mode"]})
    with pytest.raises(ValidationError):
        C.CheckTextRequest.model_validate({"file": "a.yaml", "text": "x", "record_after_fail_ms": 300})
    with pytest.raises(ValidationError):
        C.CheckTextRequest.model_validate({"file": "a.yaml", "text": "x", "record_events": True})

    rec_req = C.RunRequest.model_validate({"file": "a.yaml", "record": ["Engine.engine.mode", "Dashboard.speed"]})
    assert rec_req.record == ["Engine.engine.mode", "Dashboard.speed"]
    assert C.CheckFileRequest.model_validate({"file": "a.yaml", "record": []}).record == []
    assert C.SessionIntentSummary.model_validate({"record": ["Engine.engine.mode"]}).record == ["Engine.engine.mode"]
    for bad_rec in (
        {"file": "a.yaml", "record": None},
        {"file": "a.yaml", "record": "Engine.engine.mode"},
        {"file": "a.yaml", "record": [""]},
        {"file": "a.yaml", "record": [123]},
    ):
        with pytest.raises(ValidationError):
            C.RunRequest.model_validate(bad_rec)

    tail_req = C.RunRequest.model_validate({"file": "a.yaml", "scenario": "s.yaml", "record_after_fail_ms": 300})
    assert tail_req.record_after_fail_ms == 300
    assert C.CheckFileRequest.model_validate({"file": "a.yaml", "record_after_fail_ms": 600000}).record_after_fail_ms == 600000
    assert C.SessionIntentSummary.model_validate({"record_after_fail_ms": 1}).record_after_fail_ms == 1
    for bad_tail in (
        {"file": "a.yaml", "record_after_fail_ms": None},
        {"file": "a.yaml", "record_after_fail_ms": 0},
        {"file": "a.yaml", "record_after_fail_ms": -1},
        {"file": "a.yaml", "record_after_fail_ms": 600001},
        {"file": "a.yaml", "record_after_fail_ms": True},
        {"file": "a.yaml", "record_after_fail_ms": False},
        {"file": "a.yaml", "record_after_fail_ms": 300.0},
        {"file": "a.yaml", "record_after_fail_ms": "300"},
    ):
        with pytest.raises(ValidationError):
            C.RunRequest.model_validate(bad_tail)

    ev_req = C.RunRequest.model_validate({"file": "a.yaml", "record_events": True})
    assert ev_req.record_events is True
    assert C.CheckFileRequest.model_validate({"file": "a.yaml", "record_events": False}).record_events is False
    assert C.SessionIntentSummary.model_validate({"record_events": True}).record_events is True
    for bad_ev in (
        {"file": "a.yaml", "record_events": None},
        {"file": "a.yaml", "record_events": 1},
        {"file": "a.yaml", "record_events": 0},
        {"file": "a.yaml", "record_events": "true"},
    ):
        with pytest.raises(ValidationError):
            C.RunRequest.model_validate(bad_ev)

    base_rec = {
        "run_id": "1",
        "file": "a.yaml",
        "state": "ended",
        "available": True,
        "reason": None,
        "writes": 1,
        "omitted": 0,
        "steps": 1,
        "yaml": "vehicle_sil_scenario: 1\n",
    }
    assert C.RecordingSummary.model_validate({**base_rec, "end_ms": 0}).end_ms == 0
    assert C.RecordingSummary.model_validate({**base_rec, "end_ms": 1200}).end_ms == 1200
    assert C.RecordingSummary.model_validate({**base_rec, "end_ms": C.STEP_MS_MAX}).end_ms == C.STEP_MS_MAX
    assert C.RecordingSummary.model_validate({**base_rec, "end_ms": None}).end_ms is None
    for bad_end in (True, -1, C.STEP_MS_MAX + 1, 12.5, "100"):
        with pytest.raises(ValidationError):
            C.RecordingSummary.model_validate({**base_rec, "end_ms": bad_end})

    base_sc = {
        "file": "scenarios/a.yaml",
        "name": "a",
        "status": "passed",
        "step_count": 1,
        "over_at_ms": 100,
        "steps": [],
        "steps_omitted": 0,
        "timeline_url": None,
    }
    assert C.SessionScenarioSummary.model_validate({**base_sc, "handed_over": True}).handed_over is True
    with pytest.raises(ValidationError):
        C.SessionScenarioSummary.model_validate(base_sc)

    st_default = C.StateEvent.model_validate({"event": "state", "seq": 1, "rev": 1, "allowed": {}})
    assert st_default.handed_over is False and st_default.scenario_over_at_ms is None
    st_ho = C.StateEvent.model_validate({
        "event": "state", "seq": 2, "rev": 2, "allowed": {}, "handed_over": True, "scenario_over_at_ms": 500,
    })
    assert st_ho.handed_over is True and st_ho.scenario_over_at_ms == 500
    for bad_state in (
        {"event": "state", "seq": 1, "rev": 1, "allowed": {}, "handed_over": "yes"},
        {"event": "state", "seq": 1, "rev": 1, "allowed": {}, "handed_over": 1},
        {"event": "state", "seq": 1, "rev": 1, "allowed": {}, "scenario_over_at_ms": True},
        {"event": "state", "seq": 1, "rev": 1, "allowed": {}, "scenario_over_at_ms": -1},
        {"event": "state", "seq": 1, "rev": 1, "allowed": {}, "scenario_over_at_ms": 12.5},
    ):
        assert C.validate_launcher_event(bad_state) is not None
        with pytest.raises(ValidationError):
            C.LauncherStateSummary.model_validate(bad_state)


def test_scenario_provenance_and_case_contracts() -> None:
    # 1. ScenarioCallFrame & ScenarioStepSummary with provenance fields and without them
    frame = C.ScenarioCallFrame.model_validate({
        "file": "scenarios/cases.yaml",
        "line": 31,
        "name": "run check",
        "params": {"throttle": 50, "ok": True, "ratio": 0.5, "label": "定格"},
    })
    assert frame.line == 31 and frame.params["label"] == "定格"
    for bad_frame in (
        {"line": -1},
        {"line": True},
        {"params": {"x": None}},
        {"params": {"x": [1]}},
    ):
        with pytest.raises(ValidationError):
            C.ScenarioCallFrame.model_validate(bad_frame)

    step_with = C.ScenarioStepSummary.model_validate({
        "index": 1,
        "status": "skipped",
        "file": "scenarios/templates/check.yaml",
        "line": 22,
        "name": "定格: apply",
        "call_stack": [frame.model_dump()],
    })
    assert step_with.file == "scenarios/templates/check.yaml"
    assert step_with.name == "定格: apply"
    assert step_with.call_stack is not None and len(step_with.call_stack) == 1

    step_legacy = C.ScenarioStepSummary.model_validate({"index": 1, "status": "passed"})
    assert step_legacy.file is None and step_legacy.name is None and step_legacy.call_stack is None

    # 2. SessionScenarioSummary with case present, None, or omitted
    base_sc = {
        "file": "scenarios/cases.yaml",
        "name": "load cases",
        "status": "passed",
        "step_count": 1,
        "over_at_ms": 100,
        "steps": [step_with.model_dump()],
        "steps_omitted": 0,
        "timeline_url": None,
        "handed_over": False,
    }
    assert C.SessionScenarioSummary.model_validate(base_sc).case is None
    assert C.SessionScenarioSummary.model_validate({**base_sc, "case": "定格"}).case == "定格"

    # 3. CheckObject with CheckScenario (case, cases, params, only, skipped_steps, unchecked_expects, end_ms)
    chk = C.CheckObject.model_validate({
        "result": "passed",
        "participants": [],
        "findings": [],
        "scenario": {
            "file": "scenarios/cases.yaml",
            "name": "load cases",
            "case": "定格",
            "cases": ["無負荷", "定格", "上限直前"],
            "steps": 3,
            "only": ["medium"],
            "params": [{"name": "target_throttle", "value": 50, "from": "case"}],
            "skipped_steps": 1,
            "unchecked_expects": 1,
            "end_ms": 1000,
        },
    })
    assert chk.scenario is not None
    assert chk.scenario.case == "定格"
    assert chk.scenario.cases == ["無負荷", "定格", "上限直前"]
    assert chk.scenario.params is not None and chk.scenario.params[0].from_ == "case"

    # 4. Session.summary() + Session.message() validation with template call_stack & case
    sess = Session("p1", "simulation.yaml", [], None, False, scenario="scenarios/cases.yaml")
    sess.feed_event({
        "event": "check", "seq": 1, "t_ms": 1,
        "check": {"result": "passed", "participants": [], "scenario": {
            "file": "scenarios/cases.yaml", "name": "load cases", "case": "無負荷", "steps": 1,
        }},
    }, 150)
    sess.feed_event({
        "event": "scenario_step", "seq": 2, "t_ms": 2,
        "index": 1, "file": "scenarios/templates/check.yaml", "line": 22, "kind": "set",
        "name": "無負荷: apply", "text": "set Engine.engine.throttle = 0", "status": "passed",
        "start_ms": 0, "at_ms": 0, "end_ms": 0, "code": None, "message": None,
        "call_stack": [{"file": "scenarios/cases.yaml", "line": 31, "name": "run check",
                        "params": {"throttle": 0}}],
    }, 280)
    C.SessionSummary.model_validate(sess.summary(variables=True))
    C.WS_SERVER_MESSAGE_ADAPTER.validate_python(sess.message())


def test_scenario_provenance_contracts_reject_lone_surrogates() -> None:
    for payload in (
        {"file": "bad\ud800.yaml", "line": 1, "name": "ok", "params": {}},
        {"file": "ok.yaml", "line": 1, "name": "bad\ud800", "params": {}},
        {"file": "ok.yaml", "line": 1, "name": "ok", "params": {"k\ud800": "v"}},
        {"file": "ok.yaml", "line": 1, "name": "ok", "params": {"k": "v\ud800"}},
    ):
        with pytest.raises(ValidationError):
            C.ScenarioCallFrame.model_validate(payload)
    for field in ("file", "name", "text", "status", "kind", "code", "message"):
        step = {"index": 1, "status": "passed", field: "bad\ud800"}
        with pytest.raises(ValidationError):
            C.ScenarioStepSummary.model_validate(step)
