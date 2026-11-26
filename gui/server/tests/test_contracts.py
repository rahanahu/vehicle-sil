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
        (C.StopSessionRequest, {"run_id": "r1", "instance": "i1", "force": None}),
        (C.WriteRequest, {"run_id": "r1", "instance": None, "writes": {"E": {"x": 1}}}),
        (C.WriteRequest, {"run_id": "r1", "writes": {"E": {"x": 1}}, "late": None}),
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

    mystery = {"type": "event", "stream_seq": 4, "run_id": "r1", "event": {"event": "mystery", "extra": 1}}
    C.WS_SERVER_MESSAGE_ADAPTER.validate_python(mystery)
    ws_v.validate(mystery)

    for bad in (
        {"type": "event", "stream_seq": 1, "run_id": "r1", "event": {"event": "time", "sim_ms": "NOT_AN_INT"}},
        {"type": "event", "stream_seq": 2, "run_id": "r1", "event": {"event": "log", "source": 123, "line": None}},
        {"type": "event", "stream_seq": 3, "run_id": "r1", "event": {"event": "command", "accepted": "nope"}},
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

    # 2. PeerValuesSnapshot.values carries None for non-finite real ECU values
    v.on_peers([PeerInfo("Engine", "inc1", True, [
        {"name": "temp", "type": "real", "role": "output", "unit": "C", "min": None, "max": None,
         "step": None, "description": "", "enum": None, "availability": None},
    ])], [])
    v.on_synced()
    v.on_samples({"Engine": ListSample("inc1", 10_000_000, 1, [None])})
    values_msg = v.values_message(Watch(1, None, 0.02, 0.0, lambda _: None))
    assert values_msg["peers"]["Engine"]["values"]["temp"] is None
    C.WsValuesMessage.model_validate(values_msg)

    # 3. Session.summary() with dropped-check stop_at_ms=None AND control_link preceding participant (no state)
    sess = Session("r1", "simulation.yaml", [], None, False, limits=SessionLimits(object_bytes=120))
    sess.feed_event({"event": "check", "seq": 1, "check": {"stop_at_ms": None, "pad": "x" * 200}}, 260)
    sess.feed_event({"event": "control_link", "seq": 2, "name": "Engine", "state": "connected", "variables": 2}, 90)
    ws_summary = sess.summary(variables=False)
    assert ws_summary["stop_at_ms"] is None
    assert "state" not in ws_summary["participants"][0]
    C.SessionWsSummary.model_validate(ws_summary)
    C.SessionSummary.model_validate(sess.summary(variables=True))


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
    http_resp_models = {
        ("GET", "api/app"): C.AppResponse,
        ("GET", "api/configs"): C.ConfigsResponse,
        ("GET", "api/config"): C.ConfigGetResponse,
        ("POST", "api/config"): C.ConfigSaveResponse,
        ("POST", "api/check"): C.CheckResponse,
        ("POST", "api/describe"): C.DescribeResponse,
        ("POST", "api/connections"): C.ConnectionsResponse,
        ("POST", "api/prepare"): C.PrepareResponse,
        ("POST", "api/run"): C.RunResponse,
        ("POST", "api/stop-session"): C.StopSessionResponse,
        ("POST", "api/time"): C.TimeCommandAck,
        ("POST", "api/write"): C.WriteResponse,
        ("GET", "api/session"): C.SessionResponse,
        ("GET", "api/dashboard"): C.DashboardResponse,
        ("GET", "api/scenarios"): C.ScenariosResponse,
        ("GET", "api/recording"): C.RecordingResponse,
    }
    http_req_models = {
        ("POST", "api/config"): C.ConfigSaveRequest,
        ("POST", "api/describe"): C.DescribeRequest,
        ("POST", "api/connections"): C.ConnectionsRequest,
        ("POST", "api/prepare"): C.PrepareRequest,
        ("POST", "api/run"): C.RunRequest,
        ("POST", "api/stop-session"): C.StopSessionRequest,
        ("POST", "api/write"): C.WriteRequest,
    }
    examples = sorted(EXAMPLES_DIR.glob("*.json"))
    assert len(examples) >= 58
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
            route = (req["method"], req["path"].split("?", 1)[0])
            model = C.ErrorResponse if status >= 400 else http_resp_models[route]
            model.model_validate(data["response"])
            if status < 400 and "body" in req:
                if route == ("POST", "api/time"):
                    C.TIME_COMMAND_REQUEST_ADAPTER.validate_python(req["body"])
                elif route == ("POST", "api/check"):
                    req_model = C.CheckTextRequest if "text" in req["body"] else C.CheckFileRequest
                    req_model.model_validate(req["body"])
                elif route in http_req_models:
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


def test_contracts_constants_and_boundaries_match_stdlib_control_protocol() -> None:
    assert C.EVENTS_FORMAT == protocol.EVENTS_FORMAT
    assert C.SPEED_MIN == protocol.SPEED_MIN
    assert C.SPEED_MAX == protocol.SPEED_MAX
    assert C.STEP_MS_MAX == protocol.STEP_MS_MAX
    assert C.MAX_ID == protocol.MAX_ID
    assert C.TIME_COMMANDS == protocol.TIME_COMMANDS
    assert C.INTENT_FIELDS == protocol.INTENT_FIELDS
    assert C.REQUEST_FIELDS == protocol.REQUEST_FIELDS
    assert C.TEXT_FIELDS == protocol.TEXT_FIELDS
    assert C.DESCRIBE_FIELDS == protocol.DESCRIBE_FIELDS
    assert C.CONNECTIONS_FIELDS == protocol.CONNECTIONS_FIELDS
    assert C.NO_CONNECTION == protocol.NO_CONNECTION

    time_cases = [
        {"command": "pause"},
        {"command": "resume", "id": 0},
        {"command": "pause", "id": protocol.MAX_ID},
        {"command": "pause", "id": protocol.MAX_ID + 1},
        {"command": "pause", "id": -1},
        {"command": "pause", "id": True},
        {"command": "step", "ms": 1},
        {"command": "step", "ms": protocol.STEP_MS_MAX},
        {"command": "step", "ms": 0},
        {"command": "step", "ms": protocol.STEP_MS_MAX + 1},
        {"command": "step", "ms": True},
        {"command": "run_until", "at_ms": 0},
        {"command": "run_until", "at_ms": protocol.STEP_MS_MAX},
        {"command": "run_until", "at_ms": -1},
        {"command": "run_until", "at_ms": protocol.STEP_MS_MAX + 1},
        {"command": "speed", "value": "max"},
        {"command": "speed", "value": protocol.SPEED_MIN},
        {"command": "speed", "value": protocol.SPEED_MAX},
        {"command": "speed", "value": 1},
        {"command": "speed", "value": 0},
        {"command": "speed", "value": protocol.SPEED_MIN / 10},
        {"command": "speed", "value": protocol.SPEED_MAX + 0.1},
        {"command": "speed", "value": 10**400},
        {"command": "speed", "value": True},
        {"command": "speed", "value": math.nan},
        {"command": "speed", "value": math.inf},
        {"command": "stop"},
        {"command": "pause", "ms": 10},
    ]
    for case in time_cases:
        try:
            proto_out = protocol.check_time_command(case)
            proto_ok = True
        except protocol.InvalidRequest:
            proto_out = None
            proto_ok = False
        try:
            contract_out, _, _ = C.parse_time_command_request(case)
            contract_ok = True
        except (ValidationError, ValueError):
            contract_out = None
            contract_ok = False
        assert contract_ok == proto_ok, (case, proto_ok, contract_ok)
        if proto_ok:
            assert contract_out == proto_out, (case, proto_out, contract_out)

    run_cases = [
        {"file": "a.yaml"},
        {"file": "a.yaml", "vehicle_sil_run_request": 1},
        {"file": "a.yaml", "vehicle_sil_run_request": 2},
        {"file": "a.yaml", "vehicle_sil_run_request": True},
        {"file": "a.yaml", "speed": "max"},
        {"file": "a.yaml", "speed": protocol.SPEED_MIN},
        {"file": "a.yaml", "speed": protocol.SPEED_MAX},
        {"file": "a.yaml", "speed": 0},
        {"file": "a.yaml", "speed": 10**400},
        {"file": "a.yaml", "stop_at_ms": 1},
        {"file": "a.yaml", "stop_at_ms": protocol.STEP_MS_MAX},
        {"file": "a.yaml", "stop_at_ms": 0},
        {"file": "a.yaml", "stop_at_ms": protocol.STEP_MS_MAX + 1},
        {"file": "a.yaml", "late_tolerance_ms": 0},
        {"file": "a.yaml", "late_tolerance_ms": protocol.STEP_MS_MAX},
        {"file": "a.yaml", "late_tolerance_ms": -1},
        {"file": "a.yaml", "late_tolerance_ms": protocol.STEP_MS_MAX + 1},
        {"file": "a.yaml", "disable": ["A", "A"]},
        {"file": "a.yaml", "unknown_key": 1},
        {"file": "a.yaml", "variant": None},
    ]
    for case in run_cases:
        try:
            protocol.check_fields("api/run", case, protocol.REQUEST_FIELDS)
            protocol.check_speed(case.get("speed"))
            protocol.check_run_millis(case, "stop_at_ms", 1)
            protocol.check_run_millis(case, "late_tolerance_ms", 0)
            protocol.check_name_list("disable", case.get("disable"))
            proto_ok = True
        except protocol.InvalidRequest:
            proto_ok = False
        try:
            C.RunRequest.model_validate(case)
            contract_ok = True
        except ValidationError:
            contract_ok = False
        assert contract_ok == proto_ok, (case, proto_ok, contract_ok)
