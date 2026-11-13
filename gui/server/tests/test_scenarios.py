"""Scenarios through the GUI server: the listing of a configuration's scenario files and its limits, a check and a
run with one (the fake launcher plays it), the variables while it runs; and the record of the applied writes as a
scenario's YAML (GET api/recording)."""

from __future__ import annotations

import asyncio
import math
import os
import shutil
from pathlib import Path

import pytest
import yaml
from conftest import CONTROL_FEATURES, Client, gui_server, is_ended, is_synced, is_variables, run_async

from vehicle_sil_gui import recording
from vehicle_sil_gui.paths import ConfigRoot, PathRejected
from vehicle_sil_gui.recording import Recording, key, number, quoted, scenario_yaml
from vehicle_sil_gui.scenarios import list_scenarios, resolve_scenario

PASS, FAIL = "scenarios/throttle.yaml", "scenarios/fails.yaml"


def is_step(status: str, index: int | None = None):
    return lambda m: m["type"] == "event" and m["event"]["event"] == "scenario_step" and \
        m["event"]["status"] == status and (index is None or m["event"]["index"] == index)


# ---------------------------------------------------------------- the listing and its limits


def test_only_the_files_in_scenarios_next_to_the_configuration_are_listed_and_taken(tmp_path: Path) -> None:
    root = tmp_path / "root"
    (root / "bench" / "scenarios" / "deeper").mkdir(parents=True)
    (root / "bench" / "sim.yaml").write_text("vehicle_sil_simulation: 1\n")
    scenarios = root / "bench" / "scenarios"
    for name in ("a.yaml", "b.yml", ".hidden.yaml", "notes.txt", "deeper/c.yaml", "x" * 250 + ".yaml"):
        (scenarios / name).write_text("vehicle_sil_scenario: 1\n")
    outside = tmp_path / "outside.yaml"
    outside.write_text("vehicle_sil_scenario: 1\n")
    links = hasattr(os, "symlink")
    try:
        os.symlink(outside, scenarios / "link.yaml")
    except OSError:
        links = False
    config = ConfigRoot(root).resolve("bench/sim.yaml")
    directory, found, files, truncated = list_scenarios(ConfigRoot(root), config)
    assert (directory, found, truncated) == ("bench/scenarios", True, False)
    assert [f["name"] for f in files] == ["a.yaml", "b.yml", "x" * 250 + ".yaml"]  # no hidden, link, subdirectory
    cfg = ConfigRoot(root)
    assert resolve_scenario(cfg, config, "bench/scenarios/a.yaml") == "bench/scenarios/a.yaml"
    assert resolve_scenario(cfg, config, None) is None
    for wanted, code in (("bench/scenarios/../scenarios/a.yaml", "outside-root"), ("/etc/passwd.yaml", "outside-root"),
                         ("bench/scenarios/a.yaml\x00", "invalid-path"), ("bench\\scenarios\\a.yaml", "invalid-path"),
                         ("bench/scenarios/" + "y" * 1100 + ".yaml", "invalid-path"), (5, "invalid-path"),
                         ("", "invalid-path"), ("bench/scenarios/.hidden.yaml", "scenario-not-listed"),
                         ("bench/scenarios/deeper/c.yaml", "scenario-not-listed"),
                         ("bench/scenarios/notes.txt", "scenario-not-listed"),
                         ("bench/sim.yaml", "scenario-not-listed"),
                         *((("bench/scenarios/link.yaml", "scenario-not-listed"),) if links else ())):
        with pytest.raises(PathRejected) as e:
            resolve_scenario(cfg, config, wanted)
        assert e.value.code == code, wanted
    if links:  # a scenarios directory that is a link (or under one) is not read
        shutil.rmtree(scenarios)
        (tmp_path / "elsewhere").mkdir()
        (tmp_path / "elsewhere" / "a.yaml").write_text("vehicle_sil_scenario: 1\n")
        os.symlink(tmp_path / "elsewhere", scenarios)
        assert list_scenarios(cfg, config)[1:] == (False, [], False)
        with pytest.raises(PathRejected) as e:
            resolve_scenario(cfg, config, "bench/scenarios/a.yaml")
        assert e.value.code == "scenario-not-listed" and "symbolic link" in str(e.value)


def test_the_listing_and_a_check_with_a_scenario(gui) -> None:
    async def talk(c: Client) -> None:
        status, body = await c.get("api/scenarios?file=simulation.yaml")
        assert status == 200 and body["directory"] == "scenarios" and body["found"] is True
        assert [f["path"] for f in body["scenarios"]] == [FAIL, PASS]
        status, body = await c.get("api/scenarios?file=../x.yaml")
        assert (status, body["error"]["code"]) == (400, "outside-root")
        status, body = await c.post("api/check", {"file": "simulation.yaml", "scenario": PASS})
        assert status == 200 and body["scenario"] == PASS
        assert body["check"]["scenario"] == {"file": PASS, "name": "throttle up", "steps": 3}
        status, body = await c.post("api/check", {"file": "simulation.yaml", "scenario": "simulation.yaml"})
        assert (status, body["error"]["code"]) == (400, "scenario-not-listed")
        assert "GET api/scenarios?file=simulation.yaml" in body["error"]["message"]
        status, body = await c.post("api/run", {"file": "simulation.yaml", "scenario": PASS, "start_paused": True})
        assert (status, body["error"]["code"]) == (400, "invalid-field") and "start_paused" in body["error"]["message"]
    run_async(talk, gui)


def test_a_launcher_without_scenarios_is_not_given_one(tmp_path: Path) -> None:
    features = "events,stdin-commands,check-variables,speed,start-paused,time-commands,control-links"
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_FEATURES": features}) as gui:
        async def talk(c: Client) -> None:
            status, body = await c.post("api/check", {"file": "simulation.yaml", "scenario": PASS})
            assert (status, body["error"]["code"]) == (400, "unsupported")
        run_async(talk, gui)


# ---------------------------------------------------------------- runs


@pytest.mark.parametrize("scenario, outcome", [(PASS, "passed"), (FAIL, "failed")])
def test_a_run_with_a_scenario_keeps_its_steps_in_the_summary(gui, scenario: str, outcome: str) -> None:
    async def talk(c: Client) -> None:
        ws = await c.ws()
        await ws.until(is_synced)
        status, body = await c.post("api/run", {"file": "simulation.yaml", "scenario": scenario})
        assert status == 200, body
        assert body["session"]["scenario"]["file"] == scenario and body["session"]["scenario"]["steps"] == []
        got = await ws.until(is_ended)
        assert any(is_step("running", 1)(m) for m in got)
        _, body = await c.get("api/session")
        s = body["session"]["scenario"]
        assert (s["status"], s["step_count"], s["steps_omitted"]) == (outcome, 3, 0)
        assert [step["status"] for step in s["steps"]] == \
            (["passed"] * 3 if outcome == "passed" else ["passed", "failed", "not_run"])
        assert body["session"]["result"]["simulation"] == "ended" and body["session"]["result"]["result"] == outcome
        if outcome == "failed":
            failed = s["steps"][1]
            assert failed["code"] == "scenario-failed" and failed["line"] == 8 and failed["message"]
            assert any(f["code"] == "scenario-failed" and f["file"] == FAIL and f["line"] == 8
                       for f in body["session"]["findings"])
        # A screen opened later has the same from hello
        late = await c.ws()
        hello = (await late.until(lambda m: m["type"] == "hello"))[-1]
        assert hello["session"]["scenario"] == s
        _, rec = await c.get("api/recording")
        assert rec["recording"]["available"] is False and "scenario" in rec["recording"]["reason"]
    run_async(talk, gui)


def test_without_scenario_control_a_scenario_run_has_no_variables(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env=control_env) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            status, body = await c.post("api/run", {"file": "simulation.yaml", "scenario": PASS})
            assert status == 200, body
            v = body["session"]["variables"]
            assert v["state"] == "off" and "scenario-control" in v["reason"]
            await ws.until(is_ended)
            _, body = await c.get("api/session")
            assert body["session"]["result"]["result"] == "passed"  # the launcher was not given --control
        run_async(talk, gui)


def test_with_scenario_control_variables_are_read_and_writes_refused(tmp_path: Path, control_env: dict) -> None:
    env = {**control_env, "FAKE_LAUNCHER_FEATURES": CONTROL_FEATURES + ",scenario-control",
           "FAKE_LAUNCHER_TICK_MS": "200"}
    with gui_server(tmp_path, env=env) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            status, body = await c.post("api/run", {"file": "simulation.yaml", "scenario": PASS})
            assert status == 200, body
            session = body["session"]["id"]
            await ws.until(is_variables("ready"))
            await ws.send({"type": "watch", "id": 1, "variables": "all", "period_ms": 20})
            await ws.until(lambda m: m["type"] == "values" and m["peers"])
            status, body = await c.post("api/write", {"session": session,
                                                      "writes": {"Engine": {"engine.demand": 1}}})
            assert (status, body["error"]["code"]) == (409, "scenario-running")
            assert "scenario" in body["error"]["message"] and PASS in body["error"]["message"]
            await ws.until(is_ended)
        run_async(talk, gui)


# ---------------------------------------------------------------- the record of the writes


def test_the_applied_writes_come_back_as_a_scenario(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env={**control_env, "FAKE_LAUNCHER_TICK_MS": "50"}) as gui:
        async def talk(c: Client) -> None:
            _, body = await c.get("api/recording")
            assert body == {"recording": None}
            ws = await c.ws()
            await ws.until(is_synced)
            status, body = await c.post("api/run", {"file": "forever.yaml", "start_paused": True})
            session = body["session"]["id"]
            await ws.until(is_variables("ready"))
            _, body = await c.get("api/recording")
            r = body["recording"]
            assert (r["available"], r["writes"], r["yaml"]) == (True, 0, None) and "no write" in r["reason"]
            # Before the first step: a parameter (an enum by its name) and an input, applied at 0
            status, body = await c.post("api/write", {"session": session, "writes": {
                "Engine": {"engine.mode": "on", "engine.demand": 30}, "Dashboard": {"ecu.connected": False}}})
            assert status == 200, body
            first = body["operation"]
            await c.post("api/time", {"command": "resume"})
            await ws.until(lambda m: m["type"] == "operation" and m["seq"] == first and m["done"])
            status, body = await c.post("api/write", {"session": session,
                                                      "writes": {"Engine": {"engine.demand": 62.5}}})
            second = body["operation"]
            got = await ws.until(lambda m: m["type"] == "operation" and m["seq"] == second and m["done"])
            applied = got[-1]["peers"]["Engine"]["applied_at_ms"]
            # A write that is refused is not recorded
            status, _ = await c.post("api/write", {"session": session, "writes": {"Engine": {"engine.mode": "off"}}})
            await asyncio.sleep(0.3)
            _, body = await c.get("api/recording")
            r = body["recording"]
            assert (r["available"], r["writes"], r["omitted"], r["steps"]) == (True, 4, 0, 2), r
            scenario = yaml.safe_load(r["yaml"])
            assert scenario == {"vehicle_sil_scenario": 1, "name": f"recorded from forever.yaml (run {session})",
                                "steps": [{"at_ms": 0, "set": {"Engine.engine.mode": "on", "Engine.engine.demand": 30.0,
                                                                "Dashboard.ecu.connected": False}},
                                          {"at_ms": applied, "set": {"Engine.engine.demand": 62.5}}]}
            assert '"on"' in r["yaml"]  # an enum's name is quoted: `on` alone would be a bool in YAML 1.1
            await c.post("api/stop")
            await ws.until(is_ended)
            _, body = await c.get("api/recording")  # the run that ended last
            assert body["recording"]["state"] == "ended" and body["recording"]["yaml"] == r["yaml"]
        run_async(talk, gui)


def test_the_yaml_is_safe_for_any_name_and_value() -> None:
    assert key("Engine.engine.speed") == "Engine.engine.speed"
    for text in ("a: b", "x #y", "1.5", "true", "-x", "{a}", "é.ü", "a\nb", "a\tb", "﻿a", "a b", "'q'",
                 '"dq"', "a\\b", "\x85x", "😀.x", "null", "~", ".inf", "a,b", "[x]", "& *a", "!tag", "%x", "@x"):
        written = key(text)
        if written != text:
            assert written.startswith('"') and all(0x20 <= ord(ch) < 0x7F for ch in written), written
        assert yaml.safe_load(f"{written}: 1") == {text: 1}, text
    with pytest.raises(ValueError):
        quoted("\udc80")
    for value in (0.0, -0.0, 1.0, 62.5, 1e20, 1.5e-7, 1e300, 5e-324, -2.5e-12, 2 ** 63 - 1, -(2 ** 63), 7, 0):
        text = number(value)
        assert yaml.safe_load(f"v: {text}")["v"] == value, text
        if isinstance(value, float):
            assert "." in text.partition("e")[0], text  # a number in YAML 1.1 too
    for bad in (math.nan, math.inf, -math.inf, True, "1"):
        with pytest.raises(ValueError):
            number(bad)
    enums = {("E", "mode"): [{"name": "on", "value": 1}, {"name": "1", "value": 2}, {"name": "x", "value": 3},
                             {"name": "x", "value": 4}]}
    entries = [(0, 1, "E", "mode", 1), (0, 2, "E", "mode", 2), (5, 3, "E", "mode", 3), (5, 4, "E", "real", math.nan),
               (9, 5, "a: b", "c\nd", True), (9, 6, "E", "mode", 4)]
    text, steps, skipped = scenario_yaml(entries, "name: with\nbreak", enums, 0)
    data = yaml.safe_load(text)
    assert (steps, skipped) == (3, 1)
    assert data["name"] == "name: with\nbreak"
    assert data["steps"] == [{"at_ms": 0, "set": {"E.mode": "1"}},  # the last value at one time
                             {"at_ms": 5, "set": {"E.mode": 3}},  # a name that is not unique: the number
                             {"at_ms": 9, "set": {"a: b.c\nd": True, "E.mode": 4}}]
    assert "1 more applied writes" in text


def test_the_record_is_bounded_and_waits_for_late_results() -> None:
    r = Recording()
    r.on_operation({"seq": 1, "done": True, "peers": {"E": {"state": "applied", "applied_at_ms": 0}}})
    assert r.entries == []  # not a write of this server
    r.wrote(1, [("E", "a", 1)], {"seq": 1, "done": True, "peers": {"E": {"state": "applied", "applied_at_ms": 10}}})
    r.wrote(2, [("E", "a", 2), ("F", "b", True)], None)
    r.on_operation({"seq": 2, "done": False, "peers": {"E": {"state": "applied", "applied_at_ms": 20},
                                                       "F": {"state": "queued", "applied_at_ms": None}}})
    r.on_operation({"seq": 2, "done": True, "peers": {"E": {"state": "applied", "applied_at_ms": 20},
                                                      "F": {"state": "late", "applied_at_ms": None}}})
    assert [(e[0], e[2], e[3], e[4]) for e in r.entries] == [(10, "E", "a", 1), (20, "E", "a", 2)]
    for seq in range(3, 3 + recording.MAX_WRITES):
        r.wrote(seq, [("E", "a", seq)], {"seq": seq, "done": True,
                                         "peers": {"E": {"state": "applied", "applied_at_ms": seq}}})
    assert len(r.entries) == recording.MAX_WRITES and r.omitted == 2
    out = r.to_json(1, "s.yaml", "running", {})
    assert out["writes"] == recording.MAX_WRITES and out["omitted"] == 2
    assert "2 more applied writes are not in this file" in out["yaml"]
