#!/usr/bin/env python3
"""Deterministic Events v4 scenario launcher stub for GUI server and Playwright E2E tests."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import threading
import time

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(TESTS.parent / "src"))

from vehicle_sil_gui import __version__  # noqa: E402

DEFAULT_FEATURES = [
    "events", "stdin-commands", "request", "check-variables", "speed", "start-paused",
    "time-commands", "stdin-run-until", "state", "control-links", "scenario",
    "manual-participants", "timeline", "run-stop-at", "scenario-end-ms", "scenario-handover",
    "scenario-cases", "scenario-templates",
    "connections", "connect", "check-text", "compare-commands", "serve", "check-file-only",
    "serve-prepare", "control-metrics", "external-registry", "record-signals", "record-after-fail",
    "manual-timeline", "record-events",
]

def _peer_variables(name: str) -> list[dict]:
    base = "engine" if name in ("Engine", "-name") else name.lower()
    vars_list = [
        {"name": "ecu.connected", "type": "bool", "causality": "input", "unit": "", "min": None, "max": None,
         "step": None, "description": "on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on",
         "items": [], "builtin": True},
        {"name": f"{base}.demand", "type": "real", "causality": "input", "unit": "%", "min": 0, "max": 100,
         "step": None, "description": "requested level", "items": [], "builtin": False},
        {"name": f"{base}.speed", "type": "integer", "causality": "output", "unit": "rpm", "min": None,
         "max": None, "step": None, "description": "", "items": [], "builtin": False},
        {"name": f"{base}.mode", "type": "enum", "causality": "parameter", "unit": "", "min": None, "max": None,
         "step": None, "description": "operating mode",
         "items": [{"name": "off", "value": 0}, {"name": "on", "value": 1}], "builtin": False},
    ]
    if os.environ.get("REPLAY_VARIABLES") == "more":
        vars_list += [
            {"name": f"{base}.enabled", "type": "bool", "causality": "input", "unit": "", "min": None, "max": None,
             "step": None, "description": "switched on", "items": [], "builtin": False},
            {"name": f"{base}.warning", "type": "bool", "causality": "output", "unit": "", "min": None, "max": None,
             "step": None, "description": "the demand is above 50", "items": [], "builtin": False},
            {"name": f"{base}.offset", "type": "real", "causality": "input", "unit": "rpm", "min": None, "max": None,
             "step": None, "description": "added to the speed", "items": [], "builtin": False},
        ]
    return vars_list


def _version() -> str:
    return os.environ.get("REPLAY_VERSION") or __version__


def _features() -> list[str]:
    if "REPLAY_FEATURES" in os.environ:
        return [f for f in os.environ["REPLAY_FEATURES"].split(",") if f]
    return list(DEFAULT_FEATURES)


def _fault_of(text: str) -> str | None:
    if env_fault := os.environ.get("REPLAY_FAULT"):
        return env_fault
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("# replay-fault:"):
            return s.split(":", 1)[1].strip()
    if "failing_dashboard" in text:
        return "participant-crash"
    return None


def _record_seen_request(argv: list[str], req_path: str | None) -> None:
    seen_dir = os.environ.get("REPLAY_SEEN_REQUESTS")
    if not seen_dir or not req_path:
        return
    try:
        import stat
        d = Path(seen_dir)
        idx = len(list(d.iterdir()))
        st_mode = stat.S_IMODE(os.stat(req_path).st_mode)
        txt = Path(req_path).read_text(encoding="utf-8")
        (d / f"{idx}.json").write_text(
            json.dumps({"argv": argv, "path": req_path, "mode": st_mode, "text": txt}),
            encoding="utf-8",
        )
    except OSError:
        pass


def _read_request(argv: list[str]) -> tuple[dict, str | None]:
    req: dict = {}
    req_path: str | None = None
    if "--request" in argv:
        idx = argv.index("--request")
        if idx + 1 < len(argv):
            req_path = argv[idx + 1]
            _record_seen_request(argv, req_path)
            try:
                req = json.loads(Path(req_path).read_text(encoding="utf-8"))
            except Exception:
                req = {}
    dis = list(req.get("disable", []))
    ena = list(req.get("enable", []))
    man = list(req.get("manual", []))
    rec = list(req.get("record", []))
    variant = req.get("variant")
    scenario = req.get("scenario")
    scenario_case = req.get("case")
    record_after_fail_ms = req.get("record_after_fail_ms")
    record_events = bool(req.get("record_events"))
    i = 0
    while i < len(argv):
        if argv[i] == "--disable" and i + 1 < len(argv):
            dis.append(argv[i + 1]); i += 2
        elif argv[i] == "--enable" and i + 1 < len(argv):
            ena.append(argv[i + 1]); i += 2
        elif argv[i] == "--manual" and i + 1 < len(argv):
            man.append(argv[i + 1]); i += 2
        elif argv[i] == "--record" and i + 1 < len(argv):
            rec.append(argv[i + 1]); i += 2
        elif argv[i] == "--record-after-fail-ms" and i + 1 < len(argv):
            try:
                record_after_fail_ms = int(argv[i + 1])
            except ValueError:
                record_after_fail_ms = argv[i + 1]
            i += 2
        elif argv[i] == "--variant" and i + 1 < len(argv):
            variant = argv[i + 1]; i += 2
        elif argv[i] == "--scenario" and i + 1 < len(argv):
            scenario = argv[i + 1]; i += 2
        elif argv[i] == "--case" and i + 1 < len(argv):
            scenario_case = argv[i + 1]; i += 2
        elif argv[i] == "--record-events":
            record_events = True; i += 1
        else:
            i += 1
    if dis:
        req["disable"] = dis
    if ena:
        req["enable"] = ena
    if man:
        req["manual"] = man
    if rec:
        req["record"] = list(dict.fromkeys(rec))
    if variant is not None:
        req["variant"] = variant
    if scenario is not None:
        req["scenario"] = scenario
    if scenario_case is not None:
        req["case"] = scenario_case
    if record_after_fail_ms is not None:
        req["record_after_fail_ms"] = record_after_fail_ms
    if record_events:
        req["record_events"] = True
    return req, req_path


def inspect_config(
    cfg_path: str,
    req: dict | None = None,
    *,
    req_path: str | None = None,
    file_only: bool = False,
    variant_only: bool = False,
) -> tuple[int, dict, str | None]:
    req = req or {}
    p = Path(cfg_path)
    try:
        raw = p.read_bytes()
        text = raw.decode("utf-8")
    except (OSError, UnicodeDecodeError):
        return 3, {
            "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
            "result": "unreadable", "strict": False, "errors": 1, "warnings": 0, "notes": 0,
            "participants": [], "networks": [], "findings": [{
                "severity": "error", "code": "unreadable", "file": cfg_path, "line": 1,
                "participant": None, "network": None, "message": f"cannot read {cfg_path}",
            }],
            "file_sha256": None,
            "hot_eligibility": {"eligible": False, "reason": "config-unreadable", "message": "configuration could not be read"},
        }, None
    sha = hashlib.sha256(raw).hexdigest()
    fault = _fault_of(text)
    name = Path(cfg_path).name

    if "networks: not-a-list" in text:
        return 1, {
            "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
            "result": "failed", "strict": False, "errors": 1, "warnings": 0, "notes": 0,
            "participants": [], "networks": [], "findings": [{
                "severity": "error", "code": "invalid-networks", "file": cfg_path, "line": 2,
                "participant": None, "network": None, "message": "networks must be a list",
            }],
            "file_sha256": sha,
            "hot_eligibility": {"eligible": False, "reason": "config-unreadable", "message": "networks must be a list"},
        }, fault

    if name == "control-trouble.yaml" or fault == "control-trouble":
        known_names = ["Engine", "Dashboard", "Gauge", "Logger"]
    elif name == "dash-name.yaml":
        known_names = ["-name", "Dashboard"]
    else:
        known_names = []
        for ln in text.splitlines():
            s = ln.strip()
            if s.startswith("- name:"):
                val = s.split(":", 1)[1].strip().strip("'\"")
                if val not in ("J1939_CAN", "ETH_EXT", "CAN1", "BodyCAN") and val not in known_names:
                    known_names.append(val)
        if not known_names:
            known_names = ["Engine", "Dashboard", "Logger"]

    disable = list(req.get("disable", ()))
    enable = list(req.get("enable", ()))
    manual = list(req.get("manual", ()))
    variant = req.get("variant")
    scenario = req.get("scenario")
    scenario_case = req.get("case")
    connect = req.get("connect") or {}
    time_policy = req.get("time_policy")
    stop_at_ms = req.get("stop_at_ms")
    params = req.get("params") or {}

    findings: list[dict] = []
    # Params validation
    if isinstance(params, dict):
        for k in params:
            if not k or not (k[0].isalpha() or k[0] == "_"):
                f_path = req_path or cfg_path
                return 2, {
                    "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
                    "result": "usage", "strict": False, "errors": 1, "warnings": 0, "notes": 0,
                    "participants": [], "networks": [], "findings": [{
                        "severity": "error", "code": "usage", "file": f_path, "line": 5,
                        "participant": None, "network": None,
                        "message": f"{f_path}, line 5: params: a name is letters",
                    }],
                    "file_sha256": sha,
                    "hot_eligibility": {"eligible": False, "reason": "usage", "message": "invalid params"},
                }, fault

    # Variants validation
    has_variants = name == "variants.yaml" or "variants:" in text
    valid_variants = ("small", "big", "quiet")
    if has_variants and not variant_only:
        if variant is None or variant not in valid_variants:
            if not file_only or variant is not None:
                return 2, {
                    "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
                    "result": "usage", "variant": None, "variants": list(valid_variants),
                    "strict": False, "errors": 1, "warnings": 0, "notes": 0,
                    "participants": [], "networks": [], "findings": [{
                        "severity": "error", "code": "usage", "file": cfg_path, "line": 1,
                        "participant": None, "network": None,
                        "message": f"{cfg_path} has variants (small, big, quiet): choose a variant",
                    }],
                    "file_sha256": sha,
                    "hot_eligibility": {"eligible": True, "reason": None, "message": None},
                }, fault

    # Unknown participant override check
    if not variant_only:
        unknown = [n for n in (*disable, *manual) if n not in known_names]
        conflict = set(disable) & set(manual)
        if unknown or conflict:
            return 4 if file_only else 2, {
                "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
                "result": "usage", "strict": False, "errors": 1, "warnings": 0, "notes": 0,
                "participants": [], "networks": [], "findings": [{
                    "severity": "error", "code": "usage", "file": cfg_path, "line": None,
                    "participant": None, "network": None, "message": "invalid participant override",
                }],
                "file_sha256": sha,
                "hot_eligibility": {"eligible": False, "reason": "usage", "message": "invalid participant override"},
            }, fault

    # Scenario + start_paused check
    if scenario and req.get("start_paused") is True and not file_only:
        return 2, {
            "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
            "result": "usage", "strict": False, "errors": 1, "warnings": 0, "notes": 0,
            "participants": [], "networks": [], "findings": [{
                "severity": "error", "code": "usage", "file": req_path or cfg_path, "line": 1,
                "participant": None, "network": None,
                "message": "a scenario run cannot start paused",
            }],
            "file_sha256": sha,
            "hot_eligibility": {"eligible": True, "reason": None, "message": None},
        }, fault

    if scenario_case and not scenario and not file_only:
        return 4, {
            "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
            "result": "usage", "strict": False, "errors": 1, "warnings": 0, "notes": 0,
            "participants": [], "networks": [], "findings": [{
                "severity": "error", "code": "usage", "file": req_path or cfg_path, "line": 1,
                "participant": None, "network": None,
                "message": "case chooses a case of a scenario: give \"scenario\" in the request, or leave case out",
            }],
            "file_sha256": sha,
            "hot_eligibility": {"eligible": True, "reason": None, "message": None},
        }, fault

    after_scenario = req.get("after_scenario")
    if after_scenario is not None and after_scenario not in ("stop", "pause"):
        return 2, {
            "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
            "result": "usage", "strict": False, "errors": 1, "warnings": 0, "notes": 0,
            "participants": [], "networks": [], "findings": [{
                "severity": "error", "code": "usage", "file": req_path or cfg_path, "line": 1,
                "participant": None, "network": None,
                "message": "after_scenario must be 'stop' or 'pause'",
            }],
            "file_sha256": sha,
            "hot_eligibility": {"eligible": True, "reason": None, "message": None},
        }, fault
    if after_scenario == "pause" and not file_only and (not scenario or stop_at_ms is not None):
        msg = "after_scenario 'pause' needs --scenario" if not scenario else "after_scenario 'pause' cannot be used with stop_at_ms"
        return 2, {
            "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
            "result": "usage", "strict": False, "errors": 1, "warnings": 0, "notes": 0,
            "participants": [], "networks": [], "findings": [{
                "severity": "error", "code": "usage", "file": req_path or cfg_path, "line": 1,
                "participant": None, "network": None,
                "message": msg,
            }],
            "file_sha256": sha,
            "hot_eligibility": {"eligible": True, "reason": None, "message": None},
        }, fault

    record_after_fail_ms = req.get("record_after_fail_ms")
    if record_after_fail_ms is not None and not file_only:
        if (
            isinstance(record_after_fail_ms, bool)
            or not isinstance(record_after_fail_ms, int)
            or record_after_fail_ms < 1
            or record_after_fail_ms > 600000
        ):
            return 4, {
                "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
                "result": "usage", "strict": False, "errors": 1, "warnings": 0, "notes": 0,
                "participants": [], "networks": [], "findings": [{
                    "severity": "error", "code": "usage", "file": req_path or cfg_path, "line": 1,
                    "participant": None, "network": None,
                    "message": "record_after_fail_ms must be an integer between 1 and 600000",
                }],
                "file_sha256": sha,
                "hot_eligibility": {"eligible": True, "reason": None, "message": None},
            }, fault
        if not scenario:
            return 4, {
                "vehicle_sil_check": 1, "vehicle_sil": _version(), "file": cfg_path,
                "result": "usage", "strict": False, "errors": 1, "warnings": 0, "notes": 0,
                "participants": [], "networks": [], "findings": [{
                    "severity": "error", "code": "usage", "file": req_path or cfg_path, "line": 1,
                    "participant": None, "network": None,
                    "message": "--record-after-fail-ms needs a scenario",
                }],
                "file_sha256": sha,
                "hot_eligibility": {"eligible": True, "reason": None, "message": None},
            }, fault

    # Build participants
    participants: list[dict] = []
    for pname in known_names:
        is_disabled = pname in disable and pname not in enable
        yaml_manual = (name in ("manual.yaml", "network.yaml") or "manual: true" in text) and pname == "Dashboard"
        is_manual = (yaml_manual or pname in manual) and not is_disabled
        is_logger = pname == "Logger"
        is_gauge = pname == "Gauge"
        if is_disabled:
            pvars = None
            status = "disabled"
        elif is_logger or is_gauge:
            pvars = [] if is_gauge else None
            status = "manual" if is_manual else "not_described"
        elif pname in ("Engine", "-name"):
            pvars = _peer_variables("Engine")
            status = "manual" if is_manual else "described"
        else:
            pvars = _peer_variables("Dashboard")
            status = "manual" if is_manual else "described"
        participants.append({
            "name": pname,
            "enabled": not is_disabled,
            "manual": is_manual,
            "required": not is_logger,
            "describe": not is_logger,
            "status": status,
            "step_ms": 10 if (pname in ("Engine", "-name") or name == "simulation-step10.yaml") else (None if is_logger else 5),
            "command": [f"{pname.lower()}/vehicle_sil_{pname.lower()}_ecu"],
            "variables": pvars,
        })

    # Networks
    has_eth = name == "connect.yaml" or "ETH_EXT" in text
    net_parts = [pt["name"] for pt in participants if pt["enabled"] and pt["name"] in ("Engine", "Dashboard", "-name")]
    networks: list[dict] = [{"name": "J1939_CAN", "kind": "can", "participants": net_parts, "outside": None}]
    if has_eth:
        eth_target = connect.get("ETH_EXT")
        outside = {"target": eth_target, "backend": "packet"} if eth_target and eth_target != "none" else None
        networks.append({"name": "ETH_EXT", "kind": "ethernet", "participants": net_parts, "outside": outside})
    known_net_names = {n["name"] for n in networks}
    for net_name, target in connect.items():
        if net_name not in known_net_names and target != "none":
            findings.append({
                "severity": "note", "code": "connect-network-unused",
                "file": req_path or cfg_path, "line": None,
                "participant": None, "network": net_name,
                "message": f"connect names {net_name!r}, which the configuration does not have",
            })
    if (
        has_eth
        and connect.get("ETH_EXT")
        and connect.get("ETH_EXT") != "none"
        and time_policy != "free"
        and req.get("speed") is None
    ):
        findings.append({
            "severity": "note", "code": "outside-speed-set",
            "file": cfg_path, "line": None,
            "participant": None, "network": "ETH_EXT",
            "message": "--connect connects ETH_EXT to a real bus, which does not wait for the virtual time: the run goes at speed 1 (real time), not as fast as possible",
        })

    if "0.0.0.0" in text or name == "network.yaml":
        findings.append({
            "severity": "note", "code": "registry-on-network", "file": cfg_path, "line": 4,
            "participant": None, "network": None,
            "message": "registry_uri listens on a network address (0.0.0.0)",
        })
    for pt in participants:
        if pt["manual"]:
            env_hint = " with VEHICLE_SIL_LISTEN_ON_NETWORK=1" if ("0.0.0.0" in text or name == "network.yaml") else ""
            findings.append({
                "severity": "note", "code": "participant-manual", "file": cfg_path, "line": None,
                "participant": pt["name"], "network": None,
                "message": f"start {pt['name']} manually{env_hint}: {pt['name'].lower()}_ecu --name {pt['name']}",
            })

    # Step mismatch or check-error
    result = "passed"
    exit_code = 0
    if not file_only:
        if name == "check-error.yaml" or fault == "check-error":
            result = "failed"
            exit_code = 1
            findings.insert(0, {
                "severity": "error", "code": "step-mismatch", "file": cfg_path, "line": 11,
                "participant": "Engine", "network": None,
                "message": "participant 'Engine': step_ms is 20, but the executable's step is 10 ms: write step_ms: 10, or remove step_ms",
            })
        elif name == "simulation-step10.yaml" and isinstance(stop_at_ms, int) and stop_at_ms % 10 != 0:
            result = "failed"
            exit_code = 1
            findings.insert(0, {
                "severity": "error", "code": "stop-at-ms", "file": req_path or cfg_path, "line": 1,
                "participant": None, "network": None,
                "message": f"stop_at_ms ({stop_at_ms}) is not a multiple of the time step (10 ms)",
            })
        record_list = req.get("record") or []
        if record_list and not variant_only:
            parts_by_name = {pt["name"]: pt for pt in participants}
            for entry in dict.fromkeys(record_list):
                if not isinstance(entry, str) or "." not in entry:
                    result, exit_code = "failed", 1
                    findings.append({
                        "severity": "error", "code": "record-variable-format", "file": req_path or cfg_path,
                        "line": None, "participant": None, "network": None,
                        "message": f"record '{entry}': expected '<participant>.<variable>'",
                    })
                    continue
                pname, vname = entry.split(".", 1)
                if not pname or not vname:
                    result, exit_code = "failed", 1
                    findings.append({
                        "severity": "error", "code": "record-variable-format", "file": req_path or cfg_path,
                        "line": None, "participant": None, "network": None,
                        "message": f"record '{entry}': expected '<participant>.<variable>'",
                    })
                    continue
                pt = parts_by_name.get(pname)
                if pt is None:
                    result, exit_code = "failed", 1
                    findings.append({
                        "severity": "error", "code": "record-unknown-participant", "file": req_path or cfg_path,
                        "line": None, "participant": None, "network": None,
                        "message": f"record '{entry}': unknown participant '{pname}'",
                    })
                    continue
                if not pt["enabled"] or pt.get("variables") is None:
                    result, exit_code = "failed", 1
                    findings.append({
                        "severity": "error", "code": "record-participant-unusable", "file": req_path or cfg_path,
                        "line": None, "participant": pname, "network": None,
                        "message": f"record '{entry}': participant '{pname}' is not usable for recording",
                    })
                    continue
                known_vars = {v["name"] for v in pt["variables"]}
                if vname not in known_vars:
                    result, exit_code = "failed", 1
                    findings.append({
                        "severity": "error", "code": "record-unknown-variable", "file": req_path or cfg_path,
                        "line": None, "participant": pname, "network": None,
                        "message": f"record '{entry}': participant '{pname}' has no variable '{vname}'",
                    })

    # Hot eligibility
    if (
        "registry_uri:" in text
        or name in ("result-then-wait.yaml", "orphan.yaml", "launcher-crash.yaml")
        or fault in ("result-then-wait", "orphan", "launcher-crash")
    ):
        elig = {"eligible": False, "reason": "fixed-registry", "message": "configuration uses a fixed registry_uri"}
    elif any(pt["enabled"] and pt["manual"] for pt in participants):
        elig = {"eligible": False, "reason": "manual-participants", "message": "configuration waits for manual participants"}
    elif os.environ.get("VEHICLE_SIL_TRACE"):
        elig = {"eligible": False, "reason": "trace-enabled", "message": "serve does not run with VEHICLE_SIL_TRACE set"}
    else:
        elig = {"eligible": True, "reason": None, "message": None}

    time_step_ms = 10 if (name == "simulation-step10.yaml" or "Dashboard" in disable) else 5
    doc: dict = {
        "vehicle_sil_check": 1,
        "vehicle_sil": _version(),
        "file": cfg_path,
        "result": result,
        **({"variant": variant, "variants": list(valid_variants)} if has_variants else {}),
        "strict": bool(req.get("strict", False)),
        "errors": sum(1 for f in findings if f["severity"] == "error"),
        "warnings": sum(1 for f in findings if f["severity"] == "warning"),
        "notes": sum(1 for f in findings if f["severity"] == "note"),
        "time_step_ms": time_step_ms,
        "stop_at_ms": stop_at_ms if isinstance(stop_at_ms, int) else (1000 if "stop_at_ms: 1000" in text else None),
        "participants": participants,
        "networks": networks,
        "findings": findings,
        "file_sha256": sha,
        "hot_eligibility": elig,
    }
    if has_eth and connect.get("ETH_EXT") and connect.get("ETH_EXT") != "none":
        doc["time_lock"] = {"locked": time_policy != "free", "because": ["ETH_EXT"]}
    else:
        doc["time_lock"] = {"locked": False, "because": []}
    if scenario:
        sname = "impossible speed" if "fails" in scenario else "throttle up"
        sc_obj: dict = {"file": scenario, "name": sname, "steps": 3, "inputs": [], "lines": [], "end_ms": None}
        sc_path = p.parent / scenario
        sc_data: dict | None = None
        if sc_path.is_file():
            try:
                import yaml as _yaml
                parsed = _yaml.safe_load(sc_path.read_text(encoding="utf-8"))
                if isinstance(parsed, dict):
                    sc_data = parsed
            except Exception:
                sc_data = None
        if sc_data is not None:
            if isinstance(sc_data.get("name"), str) and sc_data["name"]:
                sc_obj["name"] = sc_data["name"]
            raw_cases = sc_data.get("cases")
            if isinstance(raw_cases, dict) and raw_cases:
                case_names = [str(k) for k in raw_cases]
                case_details = []
                for cname, cbody in raw_cases.items():
                    only_val = cbody.get("only") if isinstance(cbody, dict) else None
                    if isinstance(only_val, str):
                        only_list: list[str] | None = [only_val]
                    elif isinstance(only_val, list):
                        only_list = [str(x) for x in only_val]
                    else:
                        only_list = None
                    case_details.append({"name": str(cname), "only": only_list})
                sc_obj["cases"] = case_names
                sc_obj["case_details"] = case_details
                raw_sc_params = sc_data.get("params") if isinstance(sc_data.get("params"), dict) else {}
                raw_param_defs = sc_data.get("parameters") if isinstance(sc_data.get("parameters"), dict) else {}
                case_params: dict = {}
                if scenario_case in raw_cases and isinstance(raw_cases[scenario_case], dict):
                    cp = raw_cases[scenario_case].get("params")
                    if isinstance(cp, dict):
                        case_params = cp
                param_meta: list[dict] = []
                meta_by_name: dict[str, dict] = {}
                defaults_by_name: dict[str, object] = {}
                for pk, pdef in raw_param_defs.items():
                    meta_entry: dict = {"name": str(pk)}
                    if isinstance(pdef, dict):
                        if isinstance(pdef.get("type"), str):
                            meta_entry["type"] = pdef["type"]
                        if isinstance(pdef.get("values"), list):
                            meta_entry["values"] = [
                                ("true" if v else "false") if isinstance(v, bool) else str(v)
                                for v in pdef["values"]
                            ]
                        if isinstance(pdef.get("description"), str):
                            meta_entry["description"] = pdef["description"]
                        if "default" in pdef:
                            defaults_by_name[str(pk)] = pdef["default"]
                    param_meta.append(meta_entry)
                    meta_by_name[str(pk)] = meta_entry
                if param_meta:
                    sc_obj["param_meta"] = param_meta
                merged_params: list[dict] = []
                missing_params: list[str] = []
                all_keys = list(
                    dict.fromkeys([
                        *raw_param_defs.keys(),
                        *raw_sc_params.keys(),
                        *case_params.keys(),
                        *params.keys(),
                    ])
                )
                for pk in all_keys:
                    sk = str(pk)
                    entry: dict | None = None
                    if pk in params:
                        entry = {"name": sk, "value": params[pk], "from": "request"}
                    elif pk in case_params:
                        entry = {"name": sk, "value": case_params[pk], "from": "case"}
                    elif pk in raw_sc_params:
                        entry = {"name": sk, "value": raw_sc_params[pk], "from": "scenario"}
                    elif sk in defaults_by_name:
                        entry = {"name": sk, "value": defaults_by_name[sk], "from": "default"}
                    elif sk in meta_by_name:
                        missing_params.append(sk)
                    if entry is not None:
                        meta = meta_by_name.get(sk, {})
                        for mk in ("type", "values", "description"):
                            if mk in meta:
                                entry[mk] = meta[mk]
                        merged_params.append(entry)
                if merged_params:
                    sc_obj["params"] = merged_params
                if missing_params:
                    sc_obj["missing_params"] = missing_params
                if scenario_case is None or scenario_case not in raw_cases:
                    sc_obj["case"] = None
                    if not file_only:
                        msg = (
                            f"{scenario} has cases ({', '.join(case_names)}): choose a case with --case"
                            if scenario_case is None
                            else f"{scenario} has no case {scenario_case!r}: the cases are {', '.join(case_names)}"
                        )
                        findings.insert(0, {
                            "severity": "error", "code": "usage", "file": req_path or cfg_path, "line": None,
                            "participant": None, "network": None, "message": msg,
                        })
                        doc["result"] = "usage"
                        doc["errors"] = sum(1 for f in findings if f["severity"] == "error")
                        doc["warnings"] = sum(1 for f in findings if f["severity"] == "warning")
                        doc["notes"] = sum(1 for f in findings if f["severity"] == "note")
                        exit_code = 4
                else:
                    sc_obj["case"] = scenario_case
            elif scenario_case is not None and not file_only:
                findings.insert(0, {
                    "severity": "error", "code": "usage", "file": req_path or cfg_path, "line": None,
                    "participant": None, "network": None,
                    "message": f"{scenario} has no cases: leave case out",
                })
                doc["result"] = "usage"
                doc["errors"] = sum(1 for f in findings if f["severity"] == "error")
                exit_code = 4
            elif params:
                sc_obj["params"] = [{"name": str(pk), "value": pv, "from": "request"} for pk, pv in params.items()]
        elif scenario_case is not None:
            sc_obj["case"] = scenario_case
            if params:
                sc_obj["params"] = [{"name": str(pk), "value": pv, "from": "request"} for pk, pv in params.items()]
        doc["scenario"] = sc_obj
    return exit_code, doc, fault


def _emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def _handle_connections(argv: list[str]) -> int:
    if delay_ms := int(os.environ.get("REPLAY_CHECK_DELAY_MS", "0")):
        time.sleep(delay_ms / 1000.0)
    cfg = argv[-1]
    net = argv[argv.index("--network") + 1] if "--network" in argv else ""
    if net == "ETH_EXT":
        usable = ["fake0", "fake1", "fake-echo", "fake-wifi", "fake-down", *[f"fake{i}" for i in range(2, 12)]]
        _emit({
            "vehicle_sil_connections": 1, "vehicle_sil": _version(), "file": cfg,
            "result": "listed", "findings": [],
            "networks": [
                {"name": "ETH_EXT", "kind": "ethernet", "backend": "packet",
                 "list": {"targets": [{
                     "value": v, "label": v,
                     "detail": "up, connected; Fake Ethernet adapter",
                     "usable": True, "reason": None,
                     "in_use": False, "in_use_by": [], "in_use_count": 0,
                 } for v in usable]}},
                {"name": "ETH_EXT", "kind": "ethernet", "backend": "tap",
                 "list": {"targets": [{
                     "value": "tap:lo", "label": "lo",
                     "detail": "not Ethernet (link type 772), down",
                     "usable": False, "reason": "not Ethernet (link type 772)",
                     "in_use": False, "in_use_by": [], "in_use_count": 0,
                 }]}},
            ],
        })
        return 0
    if net == "J1939_CAN":
        _emit({
            "vehicle_sil_connections": 1, "vehicle_sil": _version(), "file": cfg,
            "result": "listed", "findings": [],
            "networks": [{"name": "J1939_CAN", "kind": "can", "backend": "socketcan", "list": {"targets": []}}],
        })
        return 0
    _emit({
        "vehicle_sil_connections": 1, "vehicle_sil": _version(), "file": cfg,
        "result": "usage", "networks": [],
        "findings": [{
            "severity": "error", "code": "usage", "file": cfg, "line": None,
            "participant": None, "network": net,
            "message": f"--network names the network {net!r}, which the file does not have: the networks are J1939_CAN, ETH_EXT",
        }],
    })
    return 4


def _allowed_map(
    paused: bool,
    running: bool = True,
    scenario: bool = False,
    has_control: bool = True,
    outside_locked: bool = False,
    handed_over: bool = False,
) -> dict:
    if not running:
        nr = {"ok": False, "reason": "not-running", "message": "the simulation is not running", "caution": None}
        return {"pause": nr, "resume": nr, "step": nr, "speed": nr, "run_until": nr, "stop": nr, "write": nr}
    ok = {"ok": True, "reason": None, "message": None, "caution": None}
    not_paused = {"ok": False, "reason": "not-paused", "message": "the simulation is not paused", "caution": None}
    not_paused_step = {
        "ok": False, "reason": "not-paused",
        "message": "step works while the simulation is paused: pause it first",
        "caution": None, "min_ms": 1, "max_ms": 1000000000000,
    }
    ok_step = {"ok": True, "reason": None, "message": None, "caution": None, "min_ms": 1, "max_ms": 1000000000000}
    already_paused = {"ok": False, "reason": "already-paused", "message": "the simulation is already paused", "caution": None}
    if scenario and not handed_over:
        wr = {
            "ok": False, "reason": "scenario-running",
            "message": "this run plays a scenario: variables cannot be written while it runs",
            "caution": None,
        }
    elif not has_control:
        wr = {
            "ok": False, "reason": "no-control-server",
            "message": "this run has no control server",
            "caution": None,
        }
    else:
        wr = ok
    if outside_locked:
        out_msg = (
            "this run is connected outside the simulation and keeps to real time: {cmd} is refused, "
            "because what is connected does not wait while the virtual time stands still, jumps or "
            "goes at another speed (a resume, a speed of 1 and a stop are taken)"
        )
        return {
            "pause": {"ok": False, "reason": "outside-real-time", "message": out_msg.format(cmd="pause"), "caution": None},
            "resume": ok,
            "step": {
                "ok": False, "reason": "outside-real-time", "message": out_msg.format(cmd="step"),
                "caution": None, "min_ms": 1, "max_ms": 1000000000000,
            },
            "speed": {"ok": True, "reason": None, "message": None, "caution": None, "min": 1, "max": 1},
            "run_until": {"ok": False, "reason": "outside-real-time", "message": out_msg.format(cmd="run_until"), "caution": None},
            "stop": ok,
            "write": wr,
        }
    return {
        "pause": ok,
        "resume": ok,
        "step": ok_step if paused else not_paused_step,
        "speed": {"ok": True, "reason": None, "message": None, "caution": None, "min": 1e-6, "max": 1000000.0},
        "run_until": ok,
        "stop": ok,
        "write": wr,
    }


def _serve_main(argv: list[str]) -> int:
    once = "--once" in argv
    cfg = argv[-1]
    cred_path: Path | None = None
    if "--control-credentials" in argv:
        cred_path = Path(argv[argv.index("--control-credentials") + 1])
    ext_reg_uri: str | None = None
    if "--external-registry" in argv:
        ext_reg_uri = argv[argv.index("--external-registry") + 1]
    want_control = "--control" in argv and cred_path is not None

    _, _, init_fault = inspect_config(cfg, file_only=True)
    if init_fault == "deaf" and sys.platform == "linux":
        import fcntl
        try:
            fcntl.fcntl(0, 1031, 4096)  # F_SETPIPE_SZ
        except OSError:
            pass

    instance = f"feed{os.getpid():012x}"[-16:]
    seq = 0
    t_ms = 0
    status_rev = 0
    run_counter = 0
    current_run: str | None = None
    last_run: str | None = None
    elig_state: dict = {"eligible": False, "reason": None, "message": None}
    launcher_state = "unprepared"
    stop_requests = 0

    def next_ev(kind: str, **fields) -> dict:
        nonlocal seq, t_ms
        ev = {"event": kind, "seq": seq, "t_ms": t_ms, **fields}
        seq += 1
        t_ms += 10
        return ev

    def _allowed_entry(ok: bool, reason: str | None = None, message: str | None = None) -> dict:
        return {"ok": ok, "reason": reason, "message": message, "caution": None}

    def status_payload(run_state: dict | None = None) -> dict:
        if launcher_state == "contaminated":
            msg = "the launcher is contaminated after an earlier run and cannot continue: start a new launcher"
            prep_allowed = _allowed_entry(False, "contaminated", msg)
            run_allowed = _allowed_entry(False, "contaminated", msg)
        elif launcher_state == "ending":
            msg = "the launcher has been asked to end and takes no run any more: start a new launcher for the next run"
            prep_allowed = _allowed_entry(False, "launcher-ending", msg)
            run_allowed = _allowed_entry(False, "launcher-ending", msg)
        elif launcher_state in ("preparing", "running"):
            suffix = f" ({current_run})" if current_run else ""
            msg = f"a run or prepare is under way{suffix}: wait for it to finish (nothing is queued)"
            prep_allowed = _allowed_entry(False, "busy", msg)
            run_allowed = _allowed_entry(False, "busy", msg)
        else:
            prep_allowed = _allowed_entry(True)
            if os.environ.get("VEHICLE_SIL_TRACE") and not once:
                run_allowed = _allowed_entry(False, "trace-enabled", "serve does not run with VEHICLE_SIL_TRACE set")
            elif launcher_state == "cold_only":
                run_allowed = _allowed_entry(
                    False,
                    elig_state.get("reason") or "not-prepared",
                    elig_state.get("message") or "configuration is not eligible for serve",
                )
            else:
                run_allowed = _allowed_entry(True)
        return {
            "instance": instance,
            "status_rev": status_rev,
            "launcher_state": launcher_state,
            "config": cfg,
            "eligibility": elig_state,
            "launcher_allowed": {
                "prepare": prep_allowed,
                "run": run_allowed,
            },
            "active_run_id": current_run,
            "last_run_id": last_run,
            "run_state": run_state,
        }

    events_ver = 99 if init_fault == "future-events" else 4
    _emit(next_ev("hello", vehicle_sil_events=events_ver, vehicle_sil=_version(), pid=os.getpid(),
                  file=cfg, arguments=["serve", *argv[1:]], launcher=instance))
    _emit(next_ev("status", id=None, **status_payload()))

    cmd_queue: queue.Queue[dict | None] = queue.Queue()

    def stdin_reader() -> None:
        try:
            for raw in sys.stdin:
                line = raw.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                    cmd_queue.put(obj)
                    if init_fault == "deaf" and isinstance(obj, dict) and obj.get("command") == "run":
                        while True:
                            time.sleep(10)
                except Exception:
                    pass
        except Exception:
            pass
        cmd_queue.put(None)

    threading.Thread(target=stdin_reader, daemon=True).start()

    while True:
        cmd = cmd_queue.get()
        if cmd is None:
            if launcher_state in ("unprepared", "cold_only", "ready"):
                launcher_state = "ending"
                status_rev += 1
                try:
                    _emit(next_ev("status", id=None, **status_payload()))
                except BrokenPipeError:
                    pass
            return 0
        cname = cmd.get("command")
        if cname == "status":
            _emit(next_ev("status", id=cmd.get("id"), **status_payload()))
        elif cname == "prepare":
            status_rev += 1
            launcher_state = "preparing"
            _emit(next_ev("status", id=None, **status_payload()))
            _, pdoc, _ = inspect_config(cfg, cmd, file_only=True)
            elig_state = pdoc["hot_eligibility"]
            status_rev += 1
            launcher_state = "ready" if elig_state["eligible"] else "cold_only"
            _emit(next_ev(
                "prepare",
                id=cmd.get("id"),
                accepted=True,
                reason=None,
                message=None,
                status=status_payload(),
                metadata={
                    "participants": [pt["name"] for pt in pdoc["participants"]],
                    "networks": [nt["name"] for nt in pdoc["networks"]],
                    "variants": ["small", "big", "quiet"] if Path(cfg).name == "variants.yaml" else [],
                    "file_sha256": pdoc["file_sha256"],
                },
                findings=pdoc["findings"],
            ))
        elif cname in ("stop", "pause", "resume", "step", "run_until", "speed"):
            _emit(next_ev("command", command=cname, source="stdin", accepted=False,
                          reason="not-running", asked_run=cmd.get("run"), id=cmd.get("id"),
                          message="no simulation is running"))
        elif cname == "run":
            if os.environ.get("VEHICLE_SIL_TRACE") and not once:
                _emit(next_ev(
                    "command", command="run", source="stdin", accepted=False,
                    reason="trace-enabled",
                    message="serve does not run with VEHICLE_SIL_TRACE set",
                    asked_run=None, id=cmd.get("id"),
                ))
                continue
            if launcher_state == "cold_only":
                _emit(next_ev(
                    "command", command="run", source="stdin", accepted=False,
                    reason=elig_state.get("reason") or "not-prepared",
                    message=elig_state.get("message") or "configuration is not eligible for serve",
                    asked_run=None, id=cmd.get("id"),
                ))
                continue
            run_counter += 1
            run_id = f"{instance}-{run_counter}"
            current_run = run_id
            stop_requests = 0
            status_rev += 1
            launcher_state = "running"
            req_file = cmd.get("request")
            timeline_file = cmd.get("timeline")
            req_dict: dict = {}
            if isinstance(req_file, str) and req_file:
                _record_seen_request(argv, req_file)
                try:
                    req_dict = json.loads(Path(req_file).read_text(encoding="utf-8"))
                except Exception:
                    req_dict = {}

            _emit(next_ev("run_start", run=run_id, id=cmd.get("id")))
            state_rev = 1
            start_paused = bool(req_dict.get("start_paused", False))
            speed: float | str = req_dict.get("speed") if req_dict.get("speed") is not None else 1
            has_scenario = bool(req_dict.get("scenario"))
            cur_run_state = {
                "rev": state_rev, "phase": "checking", "paused": start_paused, "paused_at_ms": None,
                "holding": False, "speed": speed, "pacing": "not-paced" if speed == "max" else "on-time",
                "stop": {"requests": 0, "by": None}, "scenario": has_scenario,
                "handed_over": False, "scenario_over_at_ms": None,
                "allowed": _allowed_map(start_paused, False, has_scenario, want_control), "outside": [],
            }
            _emit(next_ev("status", id=None, **status_payload(cur_run_state)))
            _emit(next_ev("phase", run=run_id, phase="checking"))

            if delay_ms := int(os.environ.get("REPLAY_CHECK_DELAY_MS", "0")):
                time.sleep(delay_ms / 1000.0)

            def settle_run_status(*, ends: bool | None = None) -> dict:
                nonlocal last_run, current_run, status_rev, launcher_state
                will_end = once if ends is None else ends
                last_run = run_id
                current_run = None
                status_rev += 1
                if will_end:
                    launcher_state = "ending"
                else:
                    launcher_state = "ready" if elig_state.get("eligible") else "unprepared"
                return status_payload()

            chk_code, chk_doc, fault = inspect_config(cfg, req_dict, req_path=req_file, file_only=False)
            if fault == "overlong-check":
                huge = dict(chk_doc, test_padding="x" * (17 * 1024 * 1024))
                _emit(next_ev("check", run=run_id, check=huge))
            else:
                _emit(next_ev("check", run=run_id, check=chk_doc))

            if chk_doc["result"] != "passed":
                for f in chk_doc["findings"]:
                    _emit(next_ev("finding", run=run_id, **f))
                _emit(next_ev("phase", run=run_id, phase="ended"))
                res_kind = "usage" if chk_doc["result"] == "usage" else "check_failed"
                _emit(next_ev("result", run=run_id, launcher_ends=once, status=settle_run_status(ends=once), result={
                    "vehicle_sil_run": 2, "vehicle_sil": _version(), "file": cfg,
                    "result": res_kind, "end": res_kind, "ok": False, "simulation": "not_started",
                    "interrupted_by": None, "message": "check failed",
                    "errors": chk_doc["errors"], "warnings": chk_doc["warnings"],
                    "notes": chk_doc["notes"], "check": chk_doc, "participants": [], "findings": chk_doc["findings"],
                }))
                if once:
                    return chk_code or 1
                continue

            try:
                rc = _execute_run(
                    cfg=cfg,
                    run_id=run_id,
                    once=once,
                    want_control=want_control,
                    cred_path=cred_path,
                    ext_reg_uri=ext_reg_uri,
                    req_dict=req_dict,
                    timeline_file=timeline_file,
                    chk_doc=chk_doc,
                    fault=fault,
                    cmd_queue=cmd_queue,
                    next_ev=next_ev,
                    settle_run_status=settle_run_status,
                )
            except BrokenPipeError:
                return 0
            if rc is not None:
                return rc


def _result_participants(started_parts: list[dict]) -> list[dict]:
    out: list[dict] = []
    for sp in started_parts:
        st = sp["state"]
        is_man = bool(sp.get("manual", False))
        ended = (
            "not_started"
            if is_man or st in ("disabled", "manual", "not_started")
            else ("killed" if st == "killed" else ("stopped" if sp.get("signal") else "exited"))
        )
        out.append({
            "name": sp["name"],
            "required": sp.get("required", True),
            "enabled": sp.get("enabled", st != "disabled"),
            "manual": is_man,
            "for_network": None,
            "ended": ended,
            "exit_code": None if is_man else sp.get("exit_code"),
            "signal": sp.get("signal"),
            "left_processes": False,
            "log": None,
            "control_link": sp.get("control_link", {
                "state": "none", "variables": None, "version": None, "late_messages": 0,
            }),
        })
    return out


def _execute_run(
    *,
    cfg: str,
    run_id: str,
    once: bool,
    want_control: bool,
    cred_path: Path | None,
    ext_reg_uri: str | None = None,
    req_dict: dict,
    timeline_file: str | None,
    chk_doc: dict,
    fault: str | None,
    cmd_queue: queue.Queue[dict | None],
    next_ev,
    settle_run_status,
) -> int | None:
    def emit_accepted_command(command: str, cmd_id: object, **extra) -> None:
        _emit(next_ev(
            "command", run=run_id, command=command, source="stdin",
            accepted=True, message=None, reason=None, id=cmd_id, **extra,
        ))
    name = Path(cfg).name
    start_paused = bool(req_dict.get("start_paused", False))
    speed: float | str = req_dict.get("speed") if req_dict.get("speed") is not None else 1
    stop_at_ms = chk_doc.get("stop_at_ms")
    time_step_ms = chk_doc.get("time_step_ms") or 5
    scenario = req_dict.get("scenario")
    has_scenario = bool(scenario)
    tick_s = int(os.environ.get("REPLAY_TICK_MS", "20")) / 1000.0
    use_sleepers = os.environ.get("REPLAY_NO_SLEEPERS", "1") == "0"
    trouble = os.environ.get("REPLAY_CONTROL")
    availability_mode = os.environ.get("REPLAY_AVAILABILITY") == "1"
    has_outside_eth = req_dict.get("connect", {}).get("ETH_EXT") == "fake0"
    outside_locked = has_outside_eth and req_dict.get("time_policy") != "free"
    is_control_trouble = fault == "control-trouble" or name == "control-trouble.yaml"

    sleepers: list[subprocess.Popen] = []
    if fault == "ignore-stop":
        for sig_name in ("SIGTERM", "SIGINT", "SIGBREAK"):
            sig = getattr(signal, sig_name, None)
            if sig is not None:
                try:
                    signal.signal(sig, signal.SIG_IGN)
                except Exception:
                    pass

    def spawn_sleeper(ignore_sig: bool = False) -> int:
        if not use_sleepers:
            return os.getpid() + len(sleepers) + 10
        ppid = os.getpid()
        code = "import os, signal, time\n"
        if ignore_sig:
            code += "signal.signal(signal.SIGTERM, signal.SIG_IGN)\nsignal.signal(signal.SIGINT, signal.SIG_IGN)\n"
        code += f"while os.getppid() == {ppid}:\n    time.sleep(0.05)\n"
        proc = subprocess.Popen(
            [sys.executable, "-c", code],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        sleepers.append(proc)
        return proc.pid

    def cleanup_sleepers(force_kill: bool = True) -> None:
        for proc in sleepers:
            if proc.poll() is None:
                try:
                    if force_kill:
                        proc.kill()
                    else:
                        proc.terminate()
                except OSError:
                    pass
        for proc in sleepers:
            try:
                proc.wait(timeout=1.0)
            except Exception:
                try:
                    proc.kill()
                except OSError:
                    pass

    import atexit
    atexit.register(cleanup_sleepers)

    _emit(next_ev("phase", run=run_id, phase="starting"))
    if ext_reg_uri is not None:
        suffix = hashlib.sha256(run_id.encode("utf-8")).hexdigest()[:16]
        reg_uri = f"{ext_reg_uri}/vsil-{suffix}"
    else:
        reg_uri = "tcp://0.0.0.0:28500" if name == "network.yaml" else "silkit://localhost:8500"
    _emit(next_ev("registry", run=run_id, uri=reg_uri))
    if fault != "flood":
        _emit(next_ev("log", run=run_id, source="vehicle_sil_launch", line=f"registry at {reg_uri}"))

    ctrl_server = None
    if want_control and cred_path is not None:
        token: str | None = None
        if cred_path.exists():
            try:
                for ln in cred_path.read_text(encoding="utf-8").splitlines():
                    parts = ln.strip().split()
                    if len(parts) >= 2 and parts[0] == "operator":
                        token = parts[-1]
            except OSError:
                pass
            if once:
                try:
                    cred_path.unlink()
                except OSError:
                    pass
        if trouble == "bad-address":
            _emit(next_ev("control_server", run=run_id, address="0.0.0.0:9999", tls=False, credentials=True))
        elif trouble == "unreachable":
            _emit(next_ev("control_server", run=run_id, address="127.0.0.1:1", tls=False, credentials=True))
        else:
            import fake_control
            peers_map = {
                pt["name"]: fake_control.variables(pt["name"])
                for pt in chk_doc["participants"]
                if pt["enabled"] and pt["name"] in ("Engine", "Dashboard")
            }
            avail_map = (
                {pname: fake_control.availability(pname) for pname in peers_map}
                if availability_mode
                else None
            )
            srv_token = "wrong-token" if trouble == "wrong-token" else token
            ctrl_server = fake_control.FakeControl(peers_map, token=srv_token, availability=avail_map)
            if has_scenario:
                ctrl_server.refuse_writes = "this run plays a scenario: variables cannot be written while it runs"
            addr = ctrl_server.start()
            _emit(next_ev("control_server", run=run_id, address=addr, tls=False, credentials=True))

    # Participants start
    started_parts: list[dict] = []
    has_manual_wait = False
    dashboard_running = False
    for pt in chk_doc["participants"]:
        pname = pt["name"]
        req_flag = bool(pt.get("required", True))
        if not pt["enabled"]:
            _emit(next_ev("participant", run=run_id, name=pname, state="disabled"))
            started_parts.append({
                "name": pname, "state": "disabled", "manual": False,
                "required": req_flag, "enabled": False,
            })
        elif pt["manual"]:
            env_map = {"VEHICLE_SIL_LISTEN_ON_NETWORK": "1"} if name == "network.yaml" else None
            ev_data = {
                "name": pname, "state": "manual", "command": pt["command"],
                "command_line": f"{pt['command'][0]} --name {pname} --registry-uri {reg_uri}",
                "working_directory": str(Path(cfg).resolve().parent),
            }
            if env_map is not None:
                ev_data["environment"] = env_map
            _emit(next_ev("participant", run=run_id, **ev_data))
            if name == "manual.yaml" and not req_dict.get("manual"):
                _emit(next_ev("participant", run=run_id, name=pname, state="joined"))
                started_parts.append({
                    "name": pname, "state": "joined", "manual": True,
                    "required": req_flag, "enabled": True, "exit_code": 0,
                })
            else:
                has_manual_wait = True
                started_parts.append({
                    "name": pname, "state": "manual", "manual": True,
                    "required": req_flag, "enabled": True,
                })
        else:
            pid = spawn_sleeper(ignore_sig=(fault == "ignore-stop" and pname == "Logger"))
            cmd_list = pt["command"]
            if fault == "large-fields" and pname == "Engine":
                cmd_list = [f"--arg-{i}-" + ("x" * 100) for i in range(45)]
            if fault not in ("flood", "participant-crash"):
                _emit(next_ev("log", run=run_id, source="vehicle_sil_launch", line=f"starting {pname}"))
            _emit(next_ev("participant", run=run_id, name=pname, state="started", pid=pid,
                          command=cmd_list, log=None))
            cl_info = {"state": "none", "variables": None, "version": None, "late_messages": 0}
            if pt.get("describe"):
                _emit(next_ev("participant", run=run_id, name=pname, state="joined"))
                if pname == "Dashboard":
                    dashboard_running = True
                if is_control_trouble:
                    if pname == "Engine":
                        _emit(next_ev("control_link", run=run_id, name="Engine", state="no_hello"))
                        cl_info = {"state": "no_hello", "variables": None, "version": None, "late_messages": 0}
                    elif pname == "Dashboard":
                        _emit(next_ev("control_link", run=run_id, name="Dashboard", state="version_mismatch", version=1))
                        cl_info = {"state": "version_mismatch", "variables": None, "version": 1, "late_messages": 0}
                    elif pname == "Gauge":
                        _emit(next_ev("control_link", run=run_id, name="Gauge", state="not_offered"))
                        cl_info = {"state": "not_offered", "variables": None, "version": None, "late_messages": 0}
                else:
                    vcount = len(pt.get("variables") or ()) or 4
                    _emit(next_ev("control_link", run=run_id, name=pname, state="connected", variables=vcount))
                    cl_info = {"state": "connected", "variables": vcount, "version": None, "late_messages": 0}
            else:
                _emit(next_ev("control_link", run=run_id, name=pname, state="none"))
            started_parts.append({
                "name": pname, "state": "started", "pid": pid, "manual": False,
                "required": req_flag, "enabled": True, "control_link": cl_info,
            })

    if has_outside_eth:
        _emit(next_ev("outside", run=run_id, network="ETH_EXT", to_outside=1, from_outside=1))

    if dashboard_running and fault not in ("flood", "participant-crash"):
        _emit(next_ev("log", run=run_id, source="Dashboard", line="t=0 ms engine speed 0 rpm (0 messages)"))

    state_rev = 1
    sim_ms = 0
    paused = start_paused
    holding = start_paused and want_control
    stop_reqs = 0
    paused_ms_total = 0
    handed_over = False
    scenario_over_at_ms: int | None = None

    def emit_state(phase_name: str) -> None:
        nonlocal state_rev
        state_rev += 1
        outside_rows = (
            [{
                "network": "ETH_EXT",
                "target": "fake0",
                "participant": "bridge.ETH_EXT",
                "link": "unknown",
                "bus": None,
                "nothing_received": False,
                "condition": "ok",
                "reasons": [],
            }]
            if has_outside_eth and phase_name in ("starting", "waiting", "running", "stopping")
            else []
        )
        _emit(next_ev(
            "state", run=run_id, rev=state_rev, phase=phase_name,
            paused=paused if phase_name in ("waiting", "running") else False,
            paused_at_ms=(0 if holding else sim_ms) if (paused and phase_name in ("waiting", "running")) else None,
            holding=holding if phase_name == "waiting" else False,
            speed=speed,
            pacing="not-paced" if speed == "max" else "on-time",
            stop={"requests": stop_reqs, "by": "stop command" if stop_reqs else None},
            scenario=has_scenario,
            handed_over=handed_over,
            scenario_over_at_ms=scenario_over_at_ms,
            allowed=_allowed_map(
                paused,
                phase_name in ("waiting", "running"),
                has_scenario,
                want_control,
                outside_locked=outside_locked,
                handed_over=handed_over,
            ),
            outside=outside_rows,
        ))

    def write_manual_timeline(recorded_to_ms: int | None) -> bool:
        record_entries = list(dict.fromkeys(req_dict.get("record") or ()))
        record_events_on = bool(req_dict.get("record_events"))
        if has_scenario or not timeline_file or (not record_entries and not record_events_on):
            return False
        parts_by_name = {pt["name"]: pt for pt in chk_doc.get("participants", [])}
        tl_vars: list[dict] = []
        tl_series: list[dict] = []
        for entry in record_entries:
            if not isinstance(entry, str) or "." not in entry:
                continue
            pname, vname = entry.split(".", 1)
            pt = parts_by_name.get(pname) or {}
            vmeta = next((v for v in (pt.get("variables") or ()) if v.get("name") == vname), None)
            vtype = vmeta["type"] if vmeta else "real"
            tl_vars.append({
                "name": entry,
                "participant": pname,
                "type": vtype,
                "causality": vmeta["causality"] if vmeta else "output",
                "unit": vmeta["unit"] if vmeta else "",
                "min": vmeta["min"] if vmeta else None,
                "max": vmeta["max"] if vmeta else None,
                "items": vmeta["items"] if vmeta else [],
                "ecu_step_ms": pt.get("step_ms") or 10,
                "written": False,
                "watched": False,
                "recorded": True,
            })
            if recorded_to_ms is None:
                pts: list[list] = []
                first_ms = None
                last_ms = None
                sample_count = 0
            else:
                end_t = max(0, recorded_to_ms)
                mid_t = end_t // 2
                if vtype == "bool":
                    pts = [[0, False], [mid_t, True]] if mid_t > 0 else [[0, True]]
                elif vtype == "enum":
                    pts = [[0, 0], [mid_t, 1]] if mid_t > 0 else [[0, 1]]
                else:
                    pts = [[0, 10], [mid_t, 42], [end_t, 65]] if end_t > mid_t > 0 else [[0, 42]]
                first_ms = 0
                last_ms = end_t
                sample_count = max(1, (end_t // max(1, time_step_ms)) + 1)
            tl_series.append({
                "variable": entry, "source": "sampled", "encoding": "changes",
                "columns": ["at_ms", "value"], "samples": sample_count,
                "first_ms": first_ms, "last_ms": last_ms,
                "not_applied_from": None, "points": pts,
            })
        tl_doc = {
            "vehicle_sil_timeline": 1,
            "vehicle_sil": _version(),
            "stage": "run",
            "file": cfg,
            "variant": req_dict.get("variant"),
            "params": [],
            "time_step_ms": time_step_ms,
            "scenario": None,
            "recorded_to_ms": recorded_to_ms,
            "variables": tl_vars,
            "steps": [],
            "series": tl_series,
            "lines": [],
            "findings": [],
            "limits": {"points_per_series": 100000, "points_total": 1000000, "dropped": []},
        }
        if record_events_on:
            ev_list: list[dict] = []
            if recorded_to_ms is not None:
                end_t = max(0, recorded_to_ms)
                ev_list.append({
                    "at_ms": 0, "participant": "Engine", "level": "info",
                    "text": "J1939 address claimed (0x00)",
                })
                if end_t >= 100:
                    ev_list.append({
                        "at_ms": 100, "participant": "Dashboard", "level": "info",
                        "text": "J1939 address claimed (0x17)",
                    })
                if end_t >= 300:
                    ev_list.append({
                        "at_ms": 300, "participant": "Engine", "level": "warning",
                        "text": "bus disconnected (ecu.connected=false)",
                    })
            tl_doc["events"] = ev_list
            tl_doc["limits"]["events_per_participant"] = 2000
            tl_doc["limits"]["events_total"] = 5000
            tl_doc["limits"]["events_dropped"] = []
        Path(timeline_file).write_text(json.dumps(tl_doc) + "\n", encoding="utf-8")
        return True

    def ctrl_step_to(target_ms: int, *, is_paused: bool) -> None:
        if ctrl_server is None:
            return
        if ctrl_server.steps == 0:
            ctrl_server.step(0)
        if target_ms > 0 and ctrl_server.last_step_ms != target_ms:
            ctrl_server.step(target_ms)
        ctrl_server.set_time(target_ms, is_paused)

    # If waiting for manual participant or holding before step 0 with control server:
    if has_manual_wait or holding:
        _emit(next_ev("phase", run=run_id, phase="waiting"))
        if holding:
            _emit(next_ev("log", run=run_id, source="vehicle_sil_launch",
                          line="standing still before the first step (start-paused)"))
            if ctrl_server is not None:
                ctrl_server.set_time(0, True)
        emit_state("waiting")

        while has_manual_wait or holding:
            cmd = cmd_queue.get()
            if cmd is None:
                cleanup_sleepers()
                if ctrl_server is not None:
                    ctrl_server.stop()
                return 0
            cname = cmd.get("command")
            asked_run = cmd.get("run")
            if asked_run is not None and asked_run != run_id:
                _emit(next_ev("command", command=cname, source="stdin", accepted=False,
                              reason="run-changed", asked_run=asked_run, id=cmd.get("id"), message="not current run"))
                continue
            if cname == "stop":
                stop_reqs = max(stop_reqs, 2 if cmd.get("force") is True else 1)
                emit_accepted_command("stop", cmd.get("id"))
                _emit(next_ev("phase", run=run_id, phase="stopping"))
                emit_state("stopping")
                for sp in started_parts:
                    if sp["state"] == "started":
                        _emit(next_ev("participant", run=run_id, name=sp["name"], state="ended", exit_code=0, signal=None))
                        sp["state"] = "ended"
                        sp["exit_code"] = 0
                _emit(next_ev("phase", run=run_id, phase="ended"))
                emit_state("ended")
                cleanup_sleepers()
                if ctrl_server is not None:
                    ctrl_server.set_time(sim_ms, False, running=False, ended=True)
                    ctrl_server.stop()
                wrote_manual_tl = write_manual_timeline(None if has_manual_wait else sim_ms)
                res_hold: dict = {
                    "vehicle_sil_run": 2, "vehicle_sil": _version(), "file": cfg,
                    "result": "stopped", "end": "stopped", "ok": True,
                    "simulation": "not_started" if has_manual_wait else "ended",
                    "interrupted_by": "stop command", "message": "stopped by command",
                    "errors": 0, "warnings": 0, "notes": 0, "check": chk_doc,
                    "participants": _result_participants(started_parts), "findings": [],
                    "timing": {"speed": speed, "time_step_ms": time_step_ms, "wall_ms": 100, "paused_ms": 50,
                               "late_steps": 0, "max_late_ms": 0, "slipped_ms": 0, "sim_ms": sim_ms},
                }
                if wrote_manual_tl:
                    res_hold["timeline"] = timeline_file
                _emit(next_ev("result", run=run_id, launcher_ends=once, status=settle_run_status(), result=res_hold))
                return 0 if once else None
            if holding and cname == "pause":
                emit_accepted_command("pause", cmd.get("id"), at_ms=0)
            elif holding and cname == "speed":
                speed = cmd.get("value", speed)
                emit_accepted_command("speed", cmd.get("id"))
                emit_state("waiting")
            elif holding and cname == "step":
                raw_ms = int(cmd.get("ms", time_step_ms))
                rem = raw_ms % time_step_ms
                step_ms = raw_ms if rem == 0 else raw_ms + (time_step_ms - rem)
                holding = False
                paused = True
                paused_ms_total += 50
                emit_accepted_command("step", cmd.get("id"), ms=step_ms, at_ms=step_ms)
                _emit(next_ev("phase", run=run_id, phase="running"))
                sim_ms = step_ms
                ctrl_step_to(sim_ms, is_paused=True)
                _emit(next_ev("time", run=run_id, sim_ms=sim_ms, actual_speed=None,
                              late_steps=0, max_late_ms=0, slipped_ms=0, late_ms=0))
                emit_state("running")
            elif holding and cname == "resume":
                holding = False
                paused = False
                paused_ms_total += 50
                emit_accepted_command("resume", cmd.get("id"))
                _emit(next_ev("phase", run=run_id, phase="running"))
                ctrl_step_to(0, is_paused=False)
                _emit(next_ev("time", run=run_id, sim_ms=0, actual_speed=1.0,
                              late_steps=0, max_late_ms=0, slipped_ms=0, late_ms=0))
                emit_state("running")
    else:
        _emit(next_ev("phase", run=run_id, phase="running"))
        if fault not in ("flood", "participant-crash"):
            _emit(next_ev("log", run=run_id, source="vehicle_sil_launch", line="simulation running"))
        ctrl_step_to(0, is_paused=paused)
        _emit(next_ev("time", run=run_id, sim_ms=0, actual_speed=None if paused else 1.0,
                      late_steps=0, max_late_ms=0, slipped_ms=0, late_ms=0))
        emit_state("running")

    # Fault injections during running phase
    if fault == "silent":
        time.sleep(float(os.environ.get("REPLAY_SILENT_S", "4")))
    elif fault == "log-dropped":
        _emit(next_ev("log", run=run_id, source="Engine", line="before drop"))
        _emit(next_ev("log_dropped", run=run_id, count=125, sources={"Engine": 120, "vehicle_sil_launch": 5}))
        _emit(next_ev("log", run=run_id, source="Engine", line="after drop"))
    elif fault == "launcher-crash":
        if paused and ctrl_server is not None:
            # Wait for resume before crashing (used by test_the_launcher_dying_ends_the_variables_and_the_open_operations)
            while paused:
                cmd = cmd_queue.get()
                if cmd is None or cmd.get("command") in ("resume", "step", "stop"):
                    break
        sys.stderr.write("replay_launcher: dying on purpose (fault launcher-crash)\n")
        sys.stderr.flush()
        cleanup_sleepers()
        if ctrl_server is not None:
            ctrl_server.stop()
        os._exit(70)
    elif fault == "participant-crash":
        _emit(next_ev("log", run=run_id, source="Dashboard", line="error: something went wrong, exiting with code 3"))
        _emit(next_ev("participant", run=run_id, name="Dashboard", state="ended", exit_code=3, signal=None))
        _emit(next_ev("participant", run=run_id, name="Dashboard", state="failed",
                      reason="process_ended", message="Dashboard exited with code 3"))
        for sp in started_parts:
            if sp["name"] == "Dashboard":
                sp["state"] = "ended"
                sp["exit_code"] = 3
            elif sp["state"] == "started":
                _emit(next_ev("participant", run=run_id, name=sp["name"], state="ended", exit_code=0, signal=None))
                sp["state"] = "ended"
                sp["exit_code"] = 0
        f_err = {
            "severity": "error", "code": "participant-failed", "file": cfg, "line": None,
            "participant": "Dashboard", "network": None, "message": "Dashboard exited with code 3",
        }
        _emit(next_ev("finding", run=run_id, **f_err))
        _emit(next_ev("phase", run=run_id, phase="ended"))
        emit_state("ended")
        cleanup_sleepers()
        if ctrl_server is not None:
            ctrl_server.stop()
        _emit(next_ev("result", run=run_id, launcher_ends=once, status=settle_run_status(), result={
            "vehicle_sil_run": 2, "vehicle_sil": _version(), "file": cfg,
            "result": "failed", "end": "failed", "ok": False, "simulation": "ended",
            "interrupted_by": None, "message": "The run failed: Dashboard exited with code 3",
            "errors": 1, "warnings": 0, "notes": 0, "check": chk_doc,
            "participants": _result_participants(started_parts), "findings": [f_err],
        }))
        return 1 if once else None
    elif fault == "bad-lines":
        sys.stdout.write("not json at all\n")
        sys.stdout.write("[1, 2, 3]\n")
        sys.stdout.flush()
        next_ev("skipped_gap")  # increments seq by 1 without emitting, creating seq-gap
        _emit(next_ev("mystery", run=run_id, colour="blue"))
        _emit(next_ev("log", run=run_id, source="Engine", line="hello", colour="blue"))
    elif fault == "bad-types":
        for bad_ev in (
            {"event": "check", "check": {"participants": 5}},
            {"event": "phase", "phase": 5},
            {"event": "registry", "uri": 5},
            {"event": "participant", "name": 5, "state": "started"},
            {"event": "control_link", "name": 5, "state": "connected"},
            {"event": "log", "source": None, "line": "x"},
            {"event": "finding", "severity": 5, "message": "x"},
            {"event": "time", "sim_ms": "x"},
            {"event": "state", "rev": "x", "allowed": {}},
        ):
            ev = next_ev(bad_ev["event"], run=run_id, **{k: v for k, v in bad_ev.items() if k != "event"})
            _emit(ev)
    elif fault == "non-finite":
        ev1 = next_ev("time", run=run_id, sim_ms=100, actual_speed=1.0)
        sys.stdout.write(json.dumps(ev1).replace("1.0", "NaN") + "\n")
        ev2 = next_ev("time", run=run_id, sim_ms=200, actual_speed=1.0)
        sys.stdout.write(json.dumps(ev2).replace("1.0", "1e999") + "\n")
        sys.stdout.flush()
    elif fault == "flood":
        _emit(next_ev("log", run=run_id, source="Dashboard", line="t=0 ms engine speed 0 rpm (0 messages)"))
        count = int(os.environ.get("REPLAY_FLOOD", "2500"))
        for i in range(count):
            _emit(next_ev("log", run=run_id, source="Engine", line=f"flood line {i}"))
    elif fault == "finding-flood":
        count = int(os.environ.get("REPLAY_FLOOD", "2500"))
        for i in range(count):
            _emit(next_ev("finding", run=run_id, severity="note", code="flood", message=f"finding {i}"))
    elif fault == "large-result":
        count = min(int(os.environ.get("REPLAY_FLOOD", "1500")), 1500)
        for i in range(count):
            _emit(next_ev("finding", run=run_id, severity="note", code="flood", message=f"finding {i}"))
    elif fault == "loose-fields":
        _emit(next_ev("finding", run=run_id, severity="note", message="plain finding"))
        _emit(next_ev("registry", run=run_id, uri=None))
        _emit(next_ev("participant", run=run_id, name="Engine", state="started", signal=9, message=123))
        ev_log = next_ev("log", run=run_id, source="Engine", line="no t_ms")
        ev_log.pop("t_ms", None)
        _emit(ev_log)
    elif fault == "orphan":
        orphan_s = float(os.environ.get("REPLAY_ORPHAN_S", "30"))
        child = subprocess.Popen(
            [sys.executable, "-c", f"import time; time.sleep({orphan_s})"],
            start_new_session=True,
        )
        _emit(next_ev("log", run=run_id, source="Engine", line=f"left a process behind: pid {child.pid}"))
        for sp in started_parts:
            if sp["state"] == "started":
                _emit(next_ev("participant", run=run_id, name=sp["name"], state="ended", exit_code=0, signal=None))
                sp["state"] = "ended"
                sp["exit_code"] = 0
        _emit(next_ev("phase", run=run_id, phase="ended"))
        emit_state("ended")
        cleanup_sleepers()
        _emit(next_ev("result", run=run_id, launcher_ends=True, status=settle_run_status(ends=True), result={
            "vehicle_sil_run": 2, "vehicle_sil": _version(), "file": cfg,
            "result": "passed", "end": "completed", "ok": True, "simulation": "ended",
            "interrupted_by": None, "message": "The run reached its end without errors",
            "errors": 0, "warnings": 0, "notes": 0, "check": chk_doc,
            "participants": _result_participants(started_parts), "findings": [],
        }))
        os._exit(0)
    elif fault == "result-then-wait":
        for sp in started_parts:
            if sp["state"] == "started":
                _emit(next_ev("participant", run=run_id, name=sp["name"], state="ended", exit_code=0, signal=None))
                sp["state"] = "ended"
                sp["exit_code"] = 0
        _emit(next_ev("phase", run=run_id, phase="ended"))
        emit_state("ended")
        _emit(next_ev("result", run=run_id, launcher_ends=True, status=settle_run_status(ends=True), result={
            "vehicle_sil_run": 2, "vehicle_sil": _version(), "file": cfg,
            "result": "passed", "end": "completed", "ok": True, "simulation": "ended",
            "interrupted_by": None, "message": "The run reached its end without errors",
            "errors": 0, "warnings": 0, "notes": 0, "check": chk_doc,
            "participants": _result_participants(started_parts), "findings": [],
        }))
        while True:
            cmd = cmd_queue.get()
            if cmd is None or cmd.get("command") == "stop":
                cleanup_sleepers()
                os._exit(0)

    # Scenario execution
    if has_scenario:
        is_fail_sc = "fails" in str(scenario)
        sc_title = "speed too high" if is_fail_sc else "throttle up"
        if timeline_file:
            sc_steps = [
                {
                    "index": 1, "line": 5, "kind": "set", "text": "set Engine.engine.demand = 50",
                    "status": "passed", "start_ms": 0, "at_ms": 0, "end_ms": 0,
                    "code": None, "message": None, "lines": [0], "watch": None, "failed": None,
                },
                {
                    "index": 2, "line": 7, "kind": "expect", "text": "expect Engine.engine.speed ge 5000",
                    "status": "failed" if is_fail_sc else "passed",
                    "start_ms": 100, "at_ms": 100, "end_ms": 100,
                    "code": "scenario-failed" if is_fail_sc else None,
                    "message": (
                        "not true at 100 ms: Engine.engine.speed ge 5000, but it was 550 (its sample of 100 ms)"
                        if is_fail_sc else None
                    ),
                    "lines": [],
                    "watch": {
                        "mode": "at", "from_ms": 100, "to_ms": None,
                        "conditions": [{"variable": "Engine.engine.speed", "compare": [{"op": "ge", "value": 5000, "tolerance": None}]}],
                    },
                    "failed": (
                        {
                            "at_ms": 100, "code": "scenario-failed",
                            "conditions": [{"condition": 0, "value": 550, "sample_ms": 100}],
                            "writes": [], "write": None,
                        }
                        if is_fail_sc else None
                    ),
                },
                {
                    "index": 3, "line": 9, "kind": "expect", "text": "expect Engine.engine.speed eq 0",
                    "status": "not_run" if is_fail_sc else "passed",
                    "start_ms": None if is_fail_sc else 500,
                    "at_ms": None if is_fail_sc else 500,
                    "end_ms": None if is_fail_sc else 500,
                    "code": None, "message": None, "lines": [],
                    "watch": {
                        "mode": "at", "from_ms": None, "to_ms": None,
                        "conditions": [{"variable": "Engine.engine.speed", "compare": [{"op": "eq", "value": 0, "tolerance": None}]}],
                    },
                    "failed": None,
                },
            ]
            tail_ms = (
                req_dict.get("record_after_fail_ms")
                if is_fail_sc
                and isinstance(req_dict.get("record_after_fail_ms"), int)
                and not isinstance(req_dict.get("record_after_fail_ms"), bool)
                and req_dict.get("record_after_fail_ms") > 0
                else 0
            )
            fail_recorded_to = 100 + tail_ms
            speed_points = [[0, 50], [50, 300], [100, 550]]
            if tail_ms > 0:
                mid_t = 100 + max(1, tail_ms // 2)
                speed_points.append([mid_t, 720])
                if fail_recorded_to > mid_t:
                    speed_points.append([fail_recorded_to, 890])
            tl_doc = {
                "vehicle_sil_timeline": 1,
                "vehicle_sil": _version(),
                "stage": "run",
                "file": cfg,
                "variant": req_dict.get("variant"),
                "params": [],
                "time_step_ms": time_step_ms,
                "scenario": {
                    "file": scenario,
                    "name": sc_title,
                    "status": "failed" if is_fail_sc else "passed",
                    "over_at_ms": 101 if is_fail_sc else 1000,
                    "recorded_to_ms": fail_recorded_to if is_fail_sc else 1000,
                    "end_ms": None,
                },
                "variables": [
                    {"name": "Engine.engine.demand", "participant": "Engine", "type": "real", "causality": "input",
                     "unit": "%", "min": 0, "max": 100, "items": [], "ecu_step_ms": 10, "written": True, "watched": False},
                    {"name": "Engine.engine.speed", "participant": "Engine", "type": "integer", "causality": "output",
                     "unit": "rpm", "min": None, "max": None, "items": [], "ecu_step_ms": 10, "written": False, "watched": True},
                ],
                "steps": sc_steps,
                "series": [
                    {"variable": "Engine.engine.demand", "source": "written", "encoding": "all",
                     "columns": ["at_ms", "value"], "samples": None, "first_ms": None, "last_ms": None,
                     "not_applied_from": None, "points": [[0, 50]]},
                    {"variable": "Engine.engine.speed", "source": "sampled", "encoding": "changes",
                     "columns": ["at_ms", "value"], "samples": 11 + (tail_ms // 10), "first_ms": 0,
                     "last_ms": fail_recorded_to if is_fail_sc else 1000,
                     "not_applied_from": None, "points": speed_points},
                ],
                "lines": [
                    {"variable": "Engine.engine.demand", "kind": "set", "line": 5, "step": 1,
                     "from_ms": 0, "to_ms": 0, "base": 50, "base_source": "written", "shape": None,
                     "text": "Engine.engine.demand = 50"},
                ],
                "findings": [],
                "limits": {"points_per_series": 100000, "points_total": 1000000, "dropped": []},
            }
            rec_to = tl_doc["scenario"]["recorded_to_ms"]
            parts_by_name = {pt["name"]: pt for pt in chk_doc.get("participants", [])}
            tl_vars_by_name = {v["name"]: v for v in tl_doc["variables"]}
            sampled_names = {s["variable"] for s in tl_doc["series"] if s["source"] == "sampled"}
            for entry in dict.fromkeys(req_dict.get("record") or ()):
                if not isinstance(entry, str) or "." not in entry:
                    continue
                pname, vname = entry.split(".", 1)
                existing = tl_vars_by_name.get(entry)
                if existing is not None:
                    existing["recorded"] = True
                    vtype = existing["type"]
                else:
                    pt = parts_by_name.get(pname) or {}
                    vmeta = next((v for v in (pt.get("variables") or ()) if v.get("name") == vname), None)
                    vtype = vmeta["type"] if vmeta else "real"
                    new_var = {
                        "name": entry,
                        "participant": pname,
                        "type": vtype,
                        "causality": vmeta["causality"] if vmeta else "output",
                        "unit": vmeta["unit"] if vmeta else "",
                        "min": vmeta["min"] if vmeta else None,
                        "max": vmeta["max"] if vmeta else None,
                        "items": vmeta["items"] if vmeta else [],
                        "ecu_step_ms": pt.get("step_ms") or 10,
                        "written": False,
                        "watched": False,
                        "recorded": True,
                    }
                    tl_doc["variables"].append(new_var)
                    tl_vars_by_name[entry] = new_var
                if entry not in sampled_names:
                    pts = (
                        [[0, False], [50, True]] if vtype == "bool"
                        else [[0, 0], [50, 1]] if vtype == "enum"
                        else [[0, 10], [50, 42], [100, 65]]
                    )
                    if tail_ms > 0:
                        mid_t = 100 + max(1, tail_ms // 2)
                        if vtype == "bool":
                            pts.append([mid_t, False])
                        elif vtype == "enum":
                            pts.append([mid_t, 0])
                        else:
                            pts.append([mid_t, 95])
                            if fail_recorded_to > mid_t:
                                pts.append([fail_recorded_to, 120])
                    tl_doc["series"].append({
                        "variable": entry, "source": "sampled", "encoding": "changes",
                        "columns": ["at_ms", "value"], "samples": 11 + (tail_ms // 10), "first_ms": 0, "last_ms": rec_to,
                        "not_applied_from": None, "points": pts,
                    })
                    sampled_names.add(entry)
            if bool(req_dict.get("record_events")):
                sc_events = [
                    {"at_ms": 0, "participant": "Engine", "level": "info", "text": "J1939 address claimed (0x00)"},
                    {"at_ms": 0, "participant": "Dashboard", "level": "info", "text": "J1939 address claimed (0x17)"},
                    {
                        "at_ms": 50, "participant": "Engine", "level": "warning",
                        "text": "demand ramp active",
                    },
                    {
                        "at_ms": 100, "participant": "Engine", "level": "error" if is_fail_sc else "info",
                        "text": "speed below target" if is_fail_sc else "speed check nominal",
                    },
                ]
                if is_fail_sc and tail_ms > 0:
                    sc_events.append({
                        "at_ms": 100 + max(1, tail_ms // 2),
                        "participant": "Dashboard",
                        "level": "warning",
                        "text": "post-failure recovery check",
                    })
                tl_doc["events"] = [ev for ev in sc_events if ev["at_ms"] <= rec_to]
                tl_doc["limits"]["events_per_participant"] = 2000
                tl_doc["limits"]["events_total"] = 5000
                tl_doc["limits"]["events_dropped"] = []
            Path(timeline_file).write_text(json.dumps(tl_doc) + "\n", encoding="utf-8")
        _emit(next_ev("scenario_step", run=run_id, index=1, status="running", at_ms=0, name="step 1"))
        _emit(next_ev("scenario_step", run=run_id, index=1, status="passed", at_ms=0, name="step 1"))
        _emit(next_ev("scenario_step", run=run_id, index=2, status="running", at_ms=500, name="step 2"))
        if isinstance(speed, (int, float)) and speed <= 0.01:
            # Slow scenario: wait for commands until stopped
            while True:
                cmd = cmd_queue.get()
                if cmd is None:
                    cleanup_sleepers()
                    if ctrl_server is not None:
                        ctrl_server.stop()
                    return 0
                cname = cmd.get("command")
                if cname == "stop":
                    stop_reqs = max(stop_reqs, 2 if cmd.get("force") is True else 1)
                    emit_accepted_command("stop", cmd.get("id"))
                    for sp in started_parts:
                        if sp["state"] == "started":
                            _emit(next_ev("participant", run=run_id, name=sp["name"], state="ended", exit_code=0, signal=None))
                            sp["state"] = "ended"
                            sp["exit_code"] = 0
                    _emit(next_ev("phase", run=run_id, phase="ended"))
                    emit_state("ended")
                    cleanup_sleepers()
                    if ctrl_server is not None:
                        ctrl_server.stop()
                    sc_res: dict = {
                        "file": scenario, "name": sc_title, "status": "incomplete",
                        "over_at_ms": None, "end_ms": None,
                        "timeline": timeline_file,
                        "steps": [
                            {"index": 1, "status": "passed"}, {"index": 2, "status": "incomplete"}, {"index": 3, "status": "not_run"},
                        ],
                    }
                    _emit(next_ev("result", run=run_id, launcher_ends=once, status=settle_run_status(), result={
                        "vehicle_sil_run": 2, "vehicle_sil": _version(), "file": cfg,
                        "result": "stopped", "end": "stopped", "ok": True, "simulation": "ended",
                        "interrupted_by": "stop command", "message": "stopped by command",
                        "errors": 0, "warnings": 0, "notes": 0,
                        "check": chk_doc, "participants": _result_participants(started_parts), "findings": [],
                        "scenario": sc_res,
                    }))
                    return 0 if once else None
                if cname in ("pause", "resume", "step", "speed", "run_until"):
                    if cname == "pause":
                        paused = True
                    elif cname == "resume":
                        paused = False
                    elif cname == "speed":
                        speed = cmd.get("value", speed)
                    emit_accepted_command(cname, cmd.get("id"))
                    emit_state("running")
        if is_fail_sc:
            msg = f"{scenario}, line 7: expected Engine.engine.speed == 9999"
            _emit(next_ev("scenario_step", run=run_id, index=2, status="failed", at_ms=500,
                          code="scenario-failed", line=7, message=msg))
            f_sc = {"severity": "error", "code": "scenario-failed", "file": scenario, "line": 7,
                    "participant": "Engine", "network": None, "message": msg}
            _emit(next_ev("finding", run=run_id, **f_sc))
            for sp in started_parts:
                if sp["state"] == "started":
                    _emit(next_ev("participant", run=run_id, name=sp["name"], state="ended", exit_code=0, signal=None))
                    sp["state"] = "ended"
                    sp["exit_code"] = 0
            _emit(next_ev("phase", run=run_id, phase="ended"))
            emit_state("ended")
            cleanup_sleepers()
            if ctrl_server is not None:
                ctrl_server.stop()
            sc_res = {
                "file": scenario, "name": sc_title, "status": "failed",
                "over_at_ms": 500, "end_ms": None,
                "timeline": timeline_file,
                "steps": [
                    {"index": 1, "status": "passed"},
                    {"index": 2, "status": "failed", "code": "scenario-failed", "line": 7, "message": msg},
                    {"index": 3, "status": "not_run"},
                ],
            }
            if req_dict.get("case"):
                sc_res["case"] = req_dict["case"]
            if chk_doc.get("scenario", {}).get("params"):
                sc_res["params"] = chk_doc["scenario"]["params"]
            _emit(next_ev("result", run=run_id, launcher_ends=once, status=settle_run_status(), result={
                "vehicle_sil_run": 2, "vehicle_sil": _version(), "file": cfg,
                "result": "failed", "end": "failed", "ok": False, "simulation": "ended",
                "interrupted_by": None, "message": f"The run failed: {msg}",
                "errors": 1, "warnings": 0, "notes": 0,
                "check": chk_doc, "participants": _result_participants(started_parts), "findings": [f_sc],
                "scenario": sc_res,
            }))
            return 1 if once else None
        else:
            _emit(next_ev("scenario_step", run=run_id, index=2, status="passed", at_ms=500, name="step 2"))
            _emit(next_ev("scenario_step", run=run_id, index=3, status="running", at_ms=1000, name="step 3"))
            _emit(next_ev("scenario_step", run=run_id, index=3, status="passed", at_ms=1000, name="step 3"))
            sim_ms = 1000
            if req_dict.get("after_scenario") == "pause":
                handed_over = True
                scenario_over_at_ms = sim_ms
                paused = True
                stop_at_ms = None
                if ctrl_server is not None:
                    ctrl_server.refuse_writes = None
                ctrl_step_to(sim_ms, is_paused=True)
                _emit(next_ev("time", run=run_id, sim_ms=sim_ms, actual_speed=None,
                              late_steps=0, max_late_ms=0, slipped_ms=0, late_ms=0))
                emit_state("running")
            else:
                stop_at_ms = 1000

    # Interactive / finite simulation loop
    ignoring_first_stop = False
    late_emitted = False
    while True:
        if not paused and not ignoring_first_stop and stop_at_ms is not None and sim_ms >= stop_at_ms:
            if sim_ms == 0 and stop_at_ms > 0:
                sim_ms = stop_at_ms
                ctrl_step_to(sim_ms, is_paused=False)
                _emit(next_ev("time", run=run_id, sim_ms=sim_ms, actual_speed=1.5,
                              late_steps=0, max_late_ms=0, slipped_ms=0, late_ms=0))
            break

        if fault == "late" and not late_emitted:
            late_emitted = True
            _emit(next_ev("time", run=run_id, sim_ms=10, actual_speed=1.0,
                          late_steps=0, max_late_ms=200, slipped_ms=200, late_ms=200))
            _emit(next_ev("time", run=run_id, sim_ms=20, actual_speed=1.0,
                          late_steps=1, max_late_ms=200, slipped_ms=200, late_ms=0))
            sim_ms = 20
        elif fault == "jitter" and sim_ms < 1000:
            for j_ms, l_ms in ((200, 0), (400, 3), (600, 0), (800, 3), (1000, 0)):
                sim_ms = j_ms
                _emit(next_ev("time", run=run_id, sim_ms=sim_ms, actual_speed=1.0,
                              late_steps=0, max_late_ms=0, slipped_ms=0, late_ms=l_ms))

        try:
            no_run_on = os.environ.get("REPLAY_RUN_ON") == "0" and stop_at_ms is None
            timeout = None if (paused or ignoring_first_stop or no_run_on) else tick_s
            cmd = cmd_queue.get(timeout=timeout)
        except queue.Empty:
            if fault == "deaf":
                _emit(next_ev("log", run=run_id, source="Engine", line=f"deaf tick at {sim_ms}"))
            sim_ms += 100
            if stop_at_ms is not None and sim_ms > stop_at_ms:
                sim_ms = stop_at_ms
            ctrl_step_to(sim_ms, is_paused=False)
            if ctrl_server is not None and trouble == "closes" and sim_ms >= 300:
                ctrl_server.stop()
            _emit(next_ev("time", run=run_id, sim_ms=sim_ms, actual_speed=1.5,
                          late_steps=1 if fault == "late" else 0,
                          max_late_ms=200 if fault == "late" else 0,
                          slipped_ms=200 if fault == "late" else 0,
                          late_ms=0))
            if dashboard_running and fault not in ("flood", "participant-crash") and sim_ms > 0 and sim_ms % 500 == 0:
                _emit(next_ev("log", run=run_id, source="Dashboard",
                              line=f"t={sim_ms} ms engine speed 1200 rpm ({sim_ms // 100} messages)"))
            if stop_at_ms is not None and sim_ms >= stop_at_ms:
                break
            continue

        if cmd is None:
            if fault == "ignore-stop":
                while True:
                    time.sleep(10)
            cleanup_sleepers()
            if ctrl_server is not None:
                ctrl_server.stop()
            return 0

        cname = cmd.get("command")
        asked_run = cmd.get("run")
        if asked_run is not None and asked_run != run_id:
            _emit(next_ev("command", command=cname, source="stdin", accepted=False,
                          reason="run-changed", asked_run=asked_run, id=cmd.get("id"), message="not current run"))
            continue

        if cname == "stop":
            stop_reqs = max(stop_reqs, 2 if cmd.get("force") is True else 1)
            emit_accepted_command("stop", cmd.get("id"))
            if fault == "ignore-stop" and stop_reqs < 2:
                ignoring_first_stop = True
                _emit(next_ev("phase", run=run_id, phase="stopping"))
                emit_state("stopping")
                _emit(next_ev("participant", run=run_id, name="Logger", state="stop_requested"))
                continue
            # Perform stop
            _emit(next_ev("phase", run=run_id, phase="stopping"))
            emit_state("stopping")
            for sp in started_parts:
                if sp["state"] == "started":
                    if fault == "ignore-stop" and sp["name"] == "Logger":
                        _emit(next_ev("participant", run=run_id, name="Logger", state="killed"))
                        sp["state"] = "killed"
                        sp["signal"] = "SIGKILL"
                    else:
                        _emit(next_ev("participant", run=run_id, name=sp["name"], state="ended", exit_code=0, signal=None))
                        sp["state"] = "ended"
                        sp["exit_code"] = 0
            res_findings: list[dict] = []
            if fault == "late":
                w_late = {"severity": "warning", "code": "behind-real-time", "file": cfg, "line": None,
                          "participant": None, "network": None, "message": "behind real time (1 late step, max 200 ms)"}
                _emit(next_ev("finding", run=run_id, **w_late))
                res_findings.append(w_late)
            if fault in ("late", "jitter"):
                n_stop = {"severity": "note", "code": "stopped", "file": cfg, "line": None,
                          "participant": None, "network": None, "message": "stopped by command"}
                _emit(next_ev("finding", run=run_id, **n_stop))
                res_findings.append(n_stop)
            _emit(next_ev("phase", run=run_id, phase="ended"))
            paused = False
            emit_state("ended")
            cleanup_sleepers()
            if ctrl_server is not None:
                ctrl_server.set_time(sim_ms, False, running=False, ended=True)
                ctrl_server.stop()
            res_stop: dict = {
                "vehicle_sil_run": 2, "vehicle_sil": _version(), "file": cfg,
                "result": "stopped", "end": "stopped", "ok": True, "simulation": "ended",
                "interrupted_by": "stop command", "message": "stopped by command",
                "errors": 0, "warnings": len([f for f in res_findings if f["severity"] == "warning"]),
                "notes": len([f for f in res_findings if f["severity"] == "note"]),
                "check": chk_doc, "participants": _result_participants(started_parts), "findings": res_findings,
                "timing": {
                    "speed": speed, "time_step_ms": time_step_ms, "wall_ms": 1000,
                    "paused_ms": paused_ms_total,
                    "late_steps": 1 if fault == "late" else 0,
                    "max_late_ms": 200 if fault == "late" else 0,
                    "slipped_ms": 200 if fault == "late" else 0,
                    "sim_ms": sim_ms,
                },
            }
            if has_scenario:
                res_stop["scenario"] = {
                    "file": scenario, "name": chk_doc.get("scenario", {}).get("name") or "throttle up",
                    "status": "passed" if handed_over else "incomplete",
                    "over_at_ms": scenario_over_at_ms if handed_over else None,
                    "end_ms": None,
                    "timeline": timeline_file,
                    "steps": [{"index": 1, "status": "passed"}, {"index": 2, "status": "passed"}, {"index": 3, "status": "passed"}],
                }
                if req_dict.get("case"):
                    res_stop["scenario"]["case"] = req_dict["case"]
                if chk_doc.get("scenario", {}).get("params"):
                    res_stop["scenario"]["params"] = chk_doc["scenario"]["params"]
            elif write_manual_timeline(sim_ms):
                res_stop["timeline"] = timeline_file
            _emit(next_ev("result", run=run_id, launcher_ends=once, status=settle_run_status(), result=res_stop))
            return 0 if once else None
        elif cname == "pause":
            if paused:
                _emit(next_ev("command", run=run_id, command="pause", source="stdin", accepted=False,
                              reason="already-paused", id=cmd.get("id"), message="the simulation is already paused"))
            else:
                paused = True
                paused_ms_total += 50
                emit_accepted_command("pause", cmd.get("id"), at_ms=sim_ms)
                if ctrl_server is not None:
                    ctrl_server.set_time(sim_ms, True)
                _emit(next_ev("time", run=run_id, sim_ms=sim_ms, actual_speed=None,
                              late_steps=0, max_late_ms=0, slipped_ms=0, late_ms=0))
                emit_state("running")
        elif cname == "resume":
            if not paused:
                _emit(next_ev("command", run=run_id, command="resume", source="stdin", accepted=False,
                              reason="not-paused", id=cmd.get("id"), message="the simulation is not paused"))
            else:
                paused = False
                paused_ms_total += 50
                emit_accepted_command("resume", cmd.get("id"))
                if ctrl_server is not None:
                    ctrl_server.set_time(sim_ms, False)
                emit_state("running")
        elif cname == "step":
            if not paused:
                _emit(next_ev("command", run=run_id, command="step", source="stdin", accepted=False,
                              reason="not-paused", id=cmd.get("id"),
                              message="step works while the simulation is paused: pause it first"))
            else:
                raw_ms = int(cmd.get("ms", time_step_ms))
                rem = raw_ms % time_step_ms
                step_ms = raw_ms if rem == 0 else raw_ms + (time_step_ms - rem)
                sim_ms += step_ms
                paused_ms_total += 50
                emit_accepted_command("step", cmd.get("id"), ms=step_ms, at_ms=sim_ms)
                ctrl_step_to(sim_ms, is_paused=True)
                _emit(next_ev("time", run=run_id, sim_ms=sim_ms, actual_speed=None,
                              late_steps=0, max_late_ms=0, slipped_ms=0, late_ms=0))
                emit_state("running")
        elif cname == "speed":
            speed = cmd.get("value", speed)
            emit_accepted_command("speed", cmd.get("id"))
            emit_state("running")
        elif cname == "run_until":
            target_ms = int(cmd.get("at_ms", sim_ms))
            if target_ms > sim_ms:
                sim_ms = target_ms
            paused = True
            emit_accepted_command("run_until", cmd.get("id"), at_ms=sim_ms)
            ctrl_step_to(sim_ms, is_paused=True)
            _emit(next_ev("time", run=run_id, sim_ms=sim_ms, actual_speed=None,
                          late_steps=0, max_late_ms=0, slipped_ms=0, late_ms=0))
            emit_state("running")

    # Natural completion (stop_at_ms or scenario passed)
    _emit(next_ev("log", run=run_id, source="Engine", line="simulation completed"))
    end_findings: list[dict] = []
    if is_control_trouble:
        w1 = {
            "severity": "warning", "code": "control-link-no-hello", "file": cfg, "line": None,
            "participant": "Engine", "network": None,
            "message": "participant 'Engine': it describes variables (--describe), but its control agent did not say hello",
        }
        w2 = {
            "severity": "warning", "code": "control-link-version", "file": cfg, "line": None,
            "participant": "Dashboard", "network": None,
            "message": "participant 'Dashboard': its control agent speaks version 1 of the control messages",
        }
        _emit(next_ev("finding", run=run_id, **w1))
        _emit(next_ev("finding", run=run_id, **w2))
        end_findings.extend([w1, w2])
    _emit(next_ev("phase", run=run_id, phase="stopping"))
    emit_state("stopping")
    for sp in started_parts:
        if sp["state"] in ("started", "joined") and not sp.get("manual"):
            _emit(next_ev("participant", run=run_id, name=sp["name"], state="ended", exit_code=0, signal=None))
            sp["state"] = "ended"
            sp["exit_code"] = 0
        elif sp.get("manual") and sp["state"] == "joined" and (fault == "manual-stay" or name == "manual.yaml"):
            _emit(next_ev(
                "log", run=run_id, source="vehicle_sil_launch",
                line=f"{sp['name']} (started by you, manual) has not left the simulation 10000 ms after it ended: the launcher cannot end it, so end it yourself",
            ))
    _emit(next_ev("phase", run=run_id, phase="ended"))
    paused = False
    emit_state("ended")
    cleanup_sleepers()
    if ctrl_server is not None:
        ctrl_server.set_time(sim_ms, False, running=False, ended=True)
        ctrl_server.stop()
    res_obj: dict = {
        "vehicle_sil_run": 2, "vehicle_sil": _version(), "file": cfg,
        "result": "passed", "end": "completed", "ok": True, "simulation": "ended",
        "interrupted_by": None, "message": "The run reached its end without errors",
        "errors": 0, "warnings": len(end_findings), "notes": 0,
        "check": chk_doc, "participants": _result_participants(started_parts), "findings": end_findings,
        "timing": {
            "speed": speed, "time_step_ms": time_step_ms, "wall_ms": 1000,
            "paused_ms": paused_ms_total, "late_steps": 0, "max_late_ms": 0, "slipped_ms": 0, "sim_ms": sim_ms,
        },
    }
    if has_scenario:
        sc_pass: dict = {
            "file": scenario, "name": chk_doc.get("scenario", {}).get("name") or "throttle up", "status": "passed",
            "over_at_ms": scenario_over_at_ms if handed_over else sim_ms,
            "end_ms": None,
            "timeline": timeline_file,
            "steps": [{"index": 1, "status": "passed"}, {"index": 2, "status": "passed"}, {"index": 3, "status": "passed"}],
        }
        if req_dict.get("case"):
            sc_pass["case"] = req_dict["case"]
        if chk_doc.get("scenario", {}).get("params"):
            sc_pass["params"] = chk_doc["scenario"]["params"]
        res_obj["scenario"] = sc_pass
    elif write_manual_timeline(sim_ms):
        res_obj["timeline"] = timeline_file
    if fault == "large-result":
        res_obj["blob"] = "z" * (5 * 1024 * 1024)
    _emit(next_ev("result", run=run_id, launcher_ends=once, status=settle_run_status(), result=res_obj))
    return 0 if once else None


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if calls_path := os.environ.get("REPLAY_CALLS"):
        with open(calls_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(argv) + "\n")

    if argv == ["version", "--json"]:
        _emit({
            "vehicle_sil_launch": 1,
            "vehicle_sil": _version(),
            "features": _features(),
            "formats": {
                "check": 1, "run": 2, "events": 4, "describe": 1,
                "test": 1, "timeline": 1, "connections": 1, "control_metrics": 1,
            },
        })
        return 0

    if argv[:1] == ["connections"]:
        return _handle_connections(argv)

    if argv[:1] == ["check"]:
        file_only = "--file-only" in argv
        variant_only = "--variant-only" in argv
        if not file_only:
            if delay_ms := int(os.environ.get("REPLAY_CHECK_DELAY_MS", "0")):
                time.sleep(delay_ms / 1000.0)
        cfg = argv[-1]
        req, req_path = _read_request(argv)
        code, doc, fault = inspect_config(
            cfg, req, req_path=req_path, file_only=file_only, variant_only=variant_only,
        )
        if fault == "check-garbage" and not file_only:
            sys.stdout.write("not json\n")
            sys.stdout.flush()
            return 1
        _emit(doc)
        return code

    if argv[:1] == ["serve"]:
        return _serve_main(argv)

    sys.stderr.write(f"stub_launcher: unsupported command: {argv!r}\n")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
