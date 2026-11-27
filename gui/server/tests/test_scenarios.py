"""Scenarios through the GUI server: the listing of a configuration's scenario files and its limits, a check and a
run with one (the stand-in launcher replays a recording of it), the variables while it runs; and the record of the applied writes as a
scenario's YAML (GET api/recording)."""

from __future__ import annotations

import asyncio
import math
import os
import re
import shutil
from pathlib import Path

import pytest
import yaml
from conftest import (CONTROL_FEATURES, REPOSITORY, Client, gui_server, is_ended, is_synced, is_variables, manual_pages,
                      run_async)

from vehicle_sil_gui import recording
from vehicle_sil_gui.paths import ConfigFile, ConfigRoot, PathRejected
from vehicle_sil_gui.recording import Recording, key, number, quoted, scenario_yaml
from vehicle_sil_gui.scenarios import list_scenarios, relative_to_config, resolve_scenario

PASS, FAIL = "scenarios/throttle.yaml", "scenarios/fails.yaml"


def is_step(status: str, index: int | None = None):
    return lambda m: m["type"] == "event" and m["event"]["event"] == "scenario_step" and \
        m["event"]["status"] == status and (index is None or m["event"]["index"] == index)


# ---------------------------------------------------------------- the listing and its limits


def test_only_the_files_in_scenarios_next_to_the_configuration_are_listed_and_taken(tmp_path: Path) -> None:
    root = tmp_path / "root"
    (root / "bench" / "scenarios" / "deeper").mkdir(parents=True)
    (root / "bench" / "sim.yaml").write_text("vehicle_sil_simulation: 2\n")
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
        # A directory above scenarios that is a link inside the root: not read either
        (root / "other" / "scenarios").mkdir(parents=True)
        (root / "other" / "scenarios" / "a.yaml").write_text("vehicle_sil_scenario: 1\n")
        (root / "other" / "sim.yaml").write_text("vehicle_sil_simulation: 2\n")
        os.symlink(root / "other", root / "linked")
        linked = cfg.resolve("linked/sim.yaml")  # a configuration may be reached through a link in the root
        assert list_scenarios(cfg, linked)[1:] == (False, [], False)
        with pytest.raises(PathRejected):
            resolve_scenario(cfg, linked, "linked/scenarios/a.yaml")


def test_the_scenario_of_a_request_file_is_from_the_directory_of_the_configuration_file() -> None:
    assert relative_to_config(ConfigFile("simulation.yaml", Path("/r/simulation.yaml")),
                              "scenarios/a.yaml") == "scenarios/a.yaml"
    assert relative_to_config(ConfigFile("a/b/sim.yaml", Path("/r/a/b/sim.yaml")),
                              "a/b/scenarios/-x.yaml") == "scenarios/-x.yaml"


def test_the_listing_and_a_check_with_a_scenario(gui) -> None:
    async def talk(c: Client) -> None:
        status, body = await c.get("api/scenarios?file=simulation.yaml")
        assert status == 200 and body["directory"] == "scenarios" and body["found"] is True
        assert [f["path"] for f in body["scenarios"]] == [FAIL, PASS]
        status, body = await c.get("api/scenarios?file=../x.yaml")
        assert (status, body["error"]["code"]) == (400, "outside-root")
        status, body = await c.post("api/check", {"file": "simulation.yaml", "scenario": PASS})
        assert status == 200 and body["scenario"] == PASS
        assert body["check"]["scenario"] == {"file": PASS, "name": "throttle up", "steps": 3, "inputs": [], "lines": [],
                                             "end_ms": None}
        status, body = await c.post("api/check", {"file": "simulation.yaml", "scenario": "simulation.yaml"})
        assert (status, body["error"]["code"]) == (400, "scenario-not-listed")
        assert "GET api/scenarios?file=simulation.yaml" in body["error"]["message"]
        # The launcher refuses a scenario that starts paused (the check says it): the run is not started
        status, body = await c.post("api/run", {"file": "simulation.yaml", "scenario": PASS, "start_paused": True})
        assert (status, body["error"]["code"]) == (422, "check-failed"), body
        assert body["error"]["detail"]["result"] == "usage" and body["error"]["detail"]["findings"][0]["code"] == "usage"
        status, body = await c.get("api/session")
        assert body["session"] is None
    run_async(talk, gui)


# ---------------------------------------------------------------- runs


@pytest.mark.parametrize("scenario, outcome", [(PASS, "passed"), (FAIL, "failed")])
def test_a_run_with_a_scenario_keeps_its_steps_in_the_summary(gui, scenario: str, outcome: str) -> None:
    async def talk(c: Client) -> None:
        ws = await c.ws()
        await ws.until(is_synced)
        status, body = await c.post("api/run", {"file": "simulation.yaml", "scenario": scenario})
        assert status == 200 and body["run_id"], body
        got = await ws.until(is_ended)
        first_sess = next(m["session"] for m in got if m["type"] == "session")
        assert first_sess["scenario"]["file"] == scenario
        assert any(is_step("running", 1)(m) for m in got)
        _, body = await c.get("api/session")
        s = body["session"]["scenario"]
        assert (s["status"], s["step_count"], s["steps_omitted"], s["handed_over"]) == (outcome, 3, 0, False)
        assert [step["status"] for step in s["steps"]] == \
            (["passed"] * 3 if outcome == "passed" else ["passed", "failed", "not_run"])
        assert body["session"]["result"]["simulation"] == "ended" and body["session"]["result"]["result"] == outcome
        if outcome == "failed":
            failed = s["steps"][1]
            assert failed["code"] == "scenario-failed" and failed["line"] == 7 and failed["message"]  # the step's line
            assert any(f["code"] == "scenario-failed" and f["file"] == FAIL and f["line"] == 7
                       for f in body["session"]["findings"])
        # A screen opened later has the same from hello
        late = await c.ws()
        hello = (await late.until(lambda m: m["type"] == "hello"))[-1]
        assert hello["session"]["scenario"] == s
        _, rec = await c.get("api/recording")
        assert rec["recording"]["available"] is False and "scenario" in rec["recording"]["reason"]
    run_async(talk, gui)


def test_with_scenario_control_variables_are_read_and_writes_refused(tmp_path: Path, control_env: dict) -> None:
    # At speed 0.001 the second step (a wait of 500 ms of virtual time) runs for minutes, and the file has no
    # stop_at_ms: the scenario is still running when the write comes, however slow the machine.
    env = {**control_env, "REPLAY_FEATURES": CONTROL_FEATURES}
    with gui_server(tmp_path, env=env) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            status, started = await c.post("api/run", {"file": "forever.yaml", "scenario": PASS, "speed": 0.001})
            assert status == 200, started
            run_id = started["run_id"]
            await ws.until(is_variables("ready"))
            await ws.send({"type": "watch", "id": 1, "variables": "all", "period_ms": 20})
            await ws.until(lambda m: m["type"] == "values" and m["peers"])
            status, body = await c.post("api/write", {"run_id": run_id,
                                                      "writes": {"Engine": {"engine.demand": 1}}})
            assert (status, body["error"]["code"]) == (409, "scenario-running")
            assert "scenario" in body["error"]["message"]  # the launcher's own sentence (its allowed.write)
            await c.post("api/stop-session", {"run_id": run_id, "instance": started["instance"], "force": False})
            await ws.until(is_ended)
        run_async(talk, gui)


def test_the_time_commands_go_to_the_launcher_while_a_scenario_runs(tmp_path: Path, control_env: dict) -> None:
    """The server's time commands are the launcher's standard input, which a scenario's run takes as any run does
    (docs/scenario.md; only the control server's clients are refused them): pause, a step, a speed, resume."""
    env = {**control_env, "REPLAY_FEATURES": CONTROL_FEATURES}
    with gui_server(tmp_path, env=env) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            status, started = await c.post("api/run", {"file": "forever.yaml", "scenario": PASS, "speed": 0.001})
            assert status == 200, started
            await ws.until(lambda m: m["type"] == "event" and m["event"]["event"] == "scenario_step"
                           and m["event"]["index"] == 2)
            for command in ({"command": "pause"}, {"command": "step", "ms": 10}, {"command": "speed", "value": 0.002},
                            {"command": "resume"}):
                status, body = await c.post("api/time", command)
                assert status == 200, (command, body)
                got = await ws.until(lambda m: m["type"] == "event" and m["event"]["event"] == "command"
                                     and m["event"]["command"] == command["command"])
                assert got[-1]["event"]["accepted"] is True, got[-1]
            await c.post("api/stop-session", {"run_id": started["run_id"], "instance": started["instance"], "force": False})
            got = await ws.until(is_ended)
            assert got[-1]["session"]["scenario"]["status"] == "incomplete"
        run_async(talk, gui)


# ---------------------------------------------------------------- the record of the writes


def test_the_applied_writes_come_back_as_a_scenario(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env={**control_env, "REPLAY_TICK_MS": "50"}) as gui:
        async def talk(c: Client) -> None:
            _, body = await c.get("api/recording")
            assert body == {"recording": None}
            ws = await c.ws()
            await ws.until(is_synced)
            status, started = await c.post("api/run", {"file": "forever.yaml", "start_paused": True})
            run_id = started["run_id"]
            await ws.until(is_variables("ready"))
            _, body = await c.get("api/recording")
            r = body["recording"]
            assert (r["available"], r["writes"], r["yaml"]) == (True, 0, None) and "no write" in r["reason"]
            # Before the first step: a parameter (an enum by its name) and an input, applied at 0
            status, body = await c.post("api/write", {"run_id": run_id, "writes": {
                "Engine": {"engine.mode": "on", "engine.demand": 30}, "Dashboard": {"ecu.connected": False}}})
            assert status == 200, body
            first = body["operation"]
            await c.post("api/time", {"command": "resume"})
            await ws.until(lambda m: m["type"] == "operation" and m["seq"] == first and m["done"])
            status, body = await c.post("api/write", {"run_id": run_id,
                                                      "writes": {"Engine": {"engine.demand": 62.5}}})
            second = body["operation"]
            got = await ws.until(lambda m: m["type"] == "operation" and m["seq"] == second and m["done"])
            applied = got[-1]["peers"]["Engine"]["applied_at_ms"]
            # A write that is refused is not recorded
            status, body = await c.post("api/write", {"run_id": run_id, "writes": {"Engine": {"engine.mode": "off"}}})
            assert status == 200, body
            refused = body["operation"]
            await ws.until(lambda m: m["type"] == "operation" and m["seq"] == refused and m["done"])
            _, body = await c.get("api/recording")
            r = body["recording"]
            assert (r["available"], r["writes"], r["omitted"], r["steps"], r["end_ms"]) == (True, 4, 0, 2, None), r
            scenario = yaml.safe_load(r["yaml"])
            assert scenario == {"vehicle_sil_scenario": 1, "name": f"recorded from forever.yaml (run {run_id})",
                                "steps": [{"at_ms": 0, "set": {"Engine.engine.mode": "on", "Engine.engine.demand": 30.0,
                                                                "Dashboard.ecu.connected": False}},
                                          {"at_ms": applied, "set": {"Engine.engine.demand": 62.5}}]}
            assert '"on"' in r["yaml"]  # an enum's name is quoted: `on` alone would be a bool in YAML 1.1
            assert "expect:" not in r["yaml"] and "hold_for_ms:" not in r["yaml"]
            await c.post("api/stop-session", {"run_id": run_id, "instance": started["instance"], "force": False})
            got = await ws.until(is_ended)
            end_sim_ms = got[-1]["session"]["result"]["timing"]["sim_ms"]
            _, body = await c.get("api/recording")  # the run that ended last
            r_end = body["recording"]
            assert r_end["state"] == "ended" and r_end["end_ms"] == end_sim_ms
            assert yaml.safe_load(r_end["yaml"]) == {**scenario, "end_ms": end_sim_ms}
            assert "expect:" not in r_end["yaml"] and "hold_for_ms:" not in r_end["yaml"]
        run_async(talk, gui)


def test_the_yaml_is_safe_for_any_name_and_value() -> None:
    assert key("Engine.engine.speed") == "Engine.engine.speed"
    for text in ("a: b", "x #y", "1.5", "true", "-x", "{a}", "é.ü", "a\nb", "a\tb", "﻿a", "a b", "'q'",
                 '"dq"', "a\\b", "\x85x", "😀.x", "null", "~", ".inf", "a,b", "[x]", "& *a", "!tag", "%x", "@x"):
        written = key(text)
        if written != text:
            assert written.startswith('"') and all(0x20 <= ord(ch) < 0x7F for ch in written), written
        assert yaml.safe_load(f"{written}: 1") == {text: 1}, text
        assert not re.search(r"\\x", written), written  # the launcher reads \xNN as one byte, not as U+00NN
    assert quoted("°C µs é") == '"\\u00b0C \\u00b5s \\u00e9"'
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
    assert "# Left out: 1 values that a scenario cannot hold" in text
    # A participant whose name has a '.' cannot be named in a scenario: left out, and said
    text, steps, skipped = scenario_yaml([(0, 1, "Bench.1", "x", 1), (0, 2, "E", "mode", 1)], "n", enums, 0, 40)
    data = yaml.safe_load(text)
    assert (steps, skipped, data["steps"], data["end_ms"]) == (1, 1, [{"at_ms": 0, "set": {"E.mode": "on"}}], 40)
    assert "# Left out: 1 variables of participants whose name has a '.'" in text and "#   Bench.1.x" in text
    assert "end_ms: 40\n" in text and "expect:" not in text and "hold_for_ms:" not in text
    # Mid-run (end_ms=None) omits end_ms, while end_ms==last (0) emits end_ms: 0
    mid_text, _, _ = scenario_yaml([(0, 1, "E", "mode", 1)], "n", enums, 0, None)
    assert "# No end_ms recorded: this scenario ends at its last step (0 ms)." in mid_text
    assert "end_ms" not in yaml.safe_load(mid_text)
    zero_text, _, _ = scenario_yaml([(0, 1, "E", "mode", 1)], "n", enums, 0, 0)
    assert yaml.safe_load(zero_text)["end_ms"] == 0

    # Invalid end_ms (< last, < 0, > STEP_MS_MAX, bool) is omitted in scenario_yaml and returned as None in Recording.to_json
    from vehicle_sil_gui.launcher import STEP_MS_MAX
    for bad_end in (200, -1, STEP_MS_MAX + 1, True):
        bad_text, bad_steps, _ = scenario_yaml([(350, 1, "Engine", "engine.throttle", 12.5)], "n", {}, 0, bad_end)
        assert bad_steps == 1
        assert "# No end_ms recorded: this scenario ends at its last step (350 ms)." in bad_text
        assert "end_ms" not in yaml.safe_load(bad_text)
        rec_bad = Recording()
        rec_bad.wrote(1, [("Engine", "engine.throttle", 12.5)],
                      {"seq": 1, "done": True, "peers": {"Engine": {"state": "applied", "applied_at_ms": 350}}})
        out_bad = rec_bad.to_json("1", "s.yaml", "ended", {}, bad_end)
        assert out_bad["end_ms"] is None
        assert "end_ms" not in yaml.safe_load(out_bad["yaml"])

    # Skipped entries (e.g., dotted participant) do not constrain end_ms; empty/all-skipped keeps valid end_ms only
    rec_dotted = Recording()
    rec_dotted.wrote(1, [("E", "x", 1), ("Bad.Peer", "y", 2)], {
        "seq": 1, "done": True,
        "peers": {
            "E": {"state": "applied", "applied_at_ms": 100},
            "Bad.Peer": {"state": "applied", "applied_at_ms": 500},
        },
    })
    out_dotted = rec_dotted.to_json("1", "s.yaml", "ended", {}, 200)
    assert out_dotted["end_ms"] == 200 and yaml.safe_load(out_dotted["yaml"])["end_ms"] == 200
    assert Recording().to_json("1", "s.yaml", "ended", {}, 500)["end_ms"] == 500
    assert Recording().to_json("1", "s.yaml", "ended", {}, STEP_MS_MAX + 1)["end_ms"] is None


def test_the_example_of_the_docs_is_what_the_server_writes() -> None:
    text, _, _ = scenario_yaml([(0, 1, "Engine", "engine.throttle", 40.0), (350, 2, "Engine", "engine.throttle", 12.5)],
                               "recorded from simulation.yaml (run 3)", {}, 0, 1000)
    for page in manual_pages("docs/gui.md"):
        name = page.relative_to(REPOSITORY).as_posix()
        # The example is the YAML block that begins as the scenarios the server writes do.
        blocks = page.read_text(encoding="utf-8").split("```yaml\n")[1:]
        block = next((b.split("```", 1)[0] for b in blocks if b.startswith("# Written by vehicle-sil-gui:")), None)
        assert block == text, f"{name}: write the example (```yaml, # Written by vehicle-sil-gui: ...) as " \
                              "scenario_yaml writes it"


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
    assert "# 2 more applied writes are not here: the record keeps at most 2000." in out["yaml"]


def test_recording_large_result_preserves_end_ms_and_not_started_omits_it(tmp_path: Path, control_env: dict) -> None:
    (tmp_path / "big").mkdir()
    (tmp_path / "ns").mkdir()
    # 1. Large result (> Limits.object_bytes) still preserves result.timing.sim_ms as recording end_ms
    with gui_server(tmp_path / "big", env={**control_env, "REPLAY_FAULT": "large-result"}) as gui:
        async def talk_big(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            status, started = await c.post("api/run", {"file": "simulation.yaml", "start_paused": True, "stop_at_ms": 200})
            assert status == 200, started
            run_id = started["run_id"]
            await ws.until(is_variables("ready"))
            status, w = await c.post("api/write", {"run_id": run_id, "writes": {"Engine": {"engine.demand": 25}}})
            assert status == 200, w
            await c.post("api/time", {"command": "resume"})
            got = await ws.until(is_ended)
            assert any(m["type"] == "error" and m["code"] == "kept-limit" for m in got)
            _, rec = await c.get("api/recording")
            r = rec["recording"]
            assert r["state"] == "ended" and r["end_ms"] == 200, r
            assert yaml.safe_load(r["yaml"])["end_ms"] == 200
        run_async(talk_big, gui)

    # 2. Run stopped while simulation == "not_started" (waiting for manual participant) leaves end_ms None
    with gui_server(tmp_path / "ns", env=control_env) as gui:
        async def talk_ns(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            status, started = await c.post("api/run", {"file": "simulation.yaml", "manual": ["Engine"]})
            assert status == 200, started
            if not any(m["type"] == "event" and m["event"]["event"] == "phase"
                       and m["event"]["phase"] == "waiting" for m in ws.received):
                await ws.until(lambda m: m["type"] == "event" and m["event"]["event"] == "phase"
                               and m["event"]["phase"] == "waiting")
            await c.post("api/stop-session", {"run_id": started["run_id"], "instance": started["instance"], "force": False})
            got = await ws.until(is_ended)
            assert got[-1]["session"]["result"]["simulation"] == "not_started"
            _, rec = await c.get("api/recording")
            assert rec["recording"]["end_ms"] is None
        run_async(talk_ns, gui)


def test_after_scenario_validation_and_handover_lifecycle(tmp_path: Path, control_env: dict) -> None:
    (tmp_path / "old").mkdir()
    (tmp_path / "ho").mkdir()
    # 1. Validation against launcher without scenario-handover feature
    old_features = ",".join(f for f in CONTROL_FEATURES.split(",") if f != "scenario-handover")
    with gui_server(tmp_path / "old", env={**control_env, "REPLAY_FEATURES": old_features}) as gui_old:
        async def talk_old(c: Client) -> None:
            st, body = await c.post("api/run", {"file": "simulation.yaml", "scenario": PASS, "after_scenario": "pause"})
            assert (st, body["error"]["code"]) == (409, "launcher-unsupported"), body
            st2, body2 = await c.post("api/check", {"file": "simulation.yaml", "scenario": PASS, "after_scenario": "pause"})
            assert (st2, body2["error"]["code"]) == (409, "launcher-unsupported"), body2
        run_async(talk_old, gui_old)

    # 2. Handover lifecycle + request validation + negative invariants
    env = {**control_env, "REPLAY_FEATURES": CONTROL_FEATURES}
    with gui_server(tmp_path / "ho", env=env) as gui:
        async def talk(c: Client) -> None:
            # Invalid combinations rejected before starting
            for endpoint in ("api/run", "api/check"):
                for bad_body in (
                    {"file": "simulation.yaml", "after_scenario": "pause"},
                    {"file": "simulation.yaml", "scenario": PASS, "after_scenario": "pause", "stop_at_ms": 500},
                    {"file": "simulation.yaml", "scenario": PASS, "after_scenario": "hold"},
                    {"file": "simulation.yaml", "scenario": PASS, "after_scenario": None},
                ):
                    st, body = await c.post(endpoint, bad_body)
                    assert (st, body["error"]["code"]) == (400, "invalid-field"), (endpoint, bad_body, st, body)
            st_txt, body_txt = await c.post("api/check", {"file": "simulation.yaml", "text": "x", "after_scenario": "pause"})
            assert (st_txt, body_txt["error"]["code"]) == (400, "invalid-field"), body_txt

            ws = await c.ws()
            await ws.until(is_synced)

            # A. Handover run with speed="max"
            st, started = await c.post("api/run", {
                "file": "simulation.yaml", "scenario": PASS, "after_scenario": "pause", "speed": "max",
            })
            assert st == 200, started
            sid, inst = started["run_id"], started["instance"]
            await ws.until(is_variables("ready"))
            ho_pred = lambda m: (
                m["type"] == "event" and m["event"]["event"] == "state" and m["event"].get("handed_over") is True
            )
            if not any(ho_pred(m) for m in ws.received):
                await ws.until(ho_pred)
            ho_state = next(m["event"] for m in reversed(ws.received) if ho_pred(m))
            assert ho_state["paused"] is True and ho_state["paused_at_ms"] == 1000
            assert ho_state["scenario"] is True and ho_state["scenario_over_at_ms"] == 1000
            assert ho_state["allowed"]["write"]["ok"] is True

            _, sess_body = await c.get("api/session")
            sc = sess_body["session"]["scenario"]
            assert sc["handed_over"] is True and sc["status"] == "passed" and sc["over_at_ms"] == 1000

            # Holds at speed="max" without advancing
            await asyncio.sleep(0.25)
            _, sess_held = await c.get("api/session")
            assert sess_held["session"]["time"]["sim_ms"] == 1000
            assert sess_held["session"]["launcher_state"]["paused"] is True

            # Manual write + step while handed over
            before_step = len(ws.received)
            st_w, w_body = await c.post("api/write", {"run_id": sid, "writes": {"Engine": {"engine.demand": 75}}})
            assert st_w == 200, w_body
            queued_pred = lambda m: m["type"] == "operation" and m["seq"] == w_body["operation"] and m["state"] == "queued"
            if not any(queued_pred(m) for m in ws.received[before_step:]):
                await ws.until(queued_pred)
            st_step, _ = await c.post("api/time", {"command": "step", "ms": 100})
            assert st_step == 200
            done_pred = lambda m: m["type"] == "operation" and m["seq"] == w_body["operation"] and m["done"]
            if not any(done_pred(m) for m in ws.received[before_step:]):
                await ws.until(done_pred)
            step_time_pred = lambda m: m["type"] == "event" and m["event"]["event"] == "time" and m["event"]["sim_ms"] == 1100
            if not any(step_time_pred(m) for m in ws.received[before_step:]):
                await ws.until(step_time_pred)

            await c.post("api/stop-session", {"run_id": sid, "instance": inst, "force": False})
            got_end = await ws.until(is_ended)
            end_sc = got_end[-1]["session"]["scenario"]
            assert end_sc["handed_over"] is True and end_sc["status"] == "passed"

            # B. Negative cases: normal stop, failed scenario, incomplete stop all keep handed_over False
            for run_body, stop_early, expected_status in (
                ({"file": "simulation.yaml", "scenario": PASS, "after_scenario": "stop"}, False, "passed"),
                ({"file": "simulation.yaml", "scenario": FAIL, "after_scenario": "pause"}, False, "failed"),
                ({"file": "forever.yaml", "scenario": PASS, "after_scenario": "pause", "speed": 0.001}, True, "incomplete"),
            ):
                before_len = len(ws.received)
                st_n, s_n = await c.post("api/run", run_body)
                assert st_n == 200, s_n
                if stop_early:
                    step2 = lambda m: (
                        m["type"] == "event" and m["event"]["event"] == "scenario_step" and m["event"]["index"] == 2
                    )
                    if not any(step2(m) for m in ws.received[before_len:]):
                        await ws.until(step2)
                    await c.post("api/stop-session", {"run_id": s_n["run_id"], "instance": s_n["instance"], "force": False})
                if not any(is_ended(m) for m in ws.received[before_len:]):
                    await ws.until(is_ended)
                neg_events = ws.received[before_len:]
                assert not any(m["type"] == "event" and m["event"]["event"] == "state"
                               and m["event"].get("handed_over") is True for m in neg_events)
                _, neg_sess = await c.get("api/session")
                assert neg_sess["session"]["scenario"]["status"] == expected_status
                assert neg_sess["session"]["scenario"]["handed_over"] is False
            await ws.close()
        run_async(talk, gui)


def test_templates_subdir_is_excluded_and_multicase_returns_clean_error(tmp_path: Path) -> None:
    """Templates in scenarios/templates/ are excluded from listing/selection, and launcher check errors
    (such as a multi-case scenario selected without CLI --case or a direct template file) return clean errors."""
    root = tmp_path / "root"
    (root / "scenarios" / "templates").mkdir(parents=True)
    (root / "simulation.yaml").write_text("vehicle_sil_simulation: 2\n", encoding="utf-8")
    (root / "scenarios" / "templates" / "shared.yaml").write_text(
        "vehicle_sil_scenario_template: 1\nsteps:\n  - set: { Engine.engine.throttle: 10 }\n", encoding="utf-8")
    (root / "scenarios" / "direct_template.yaml").write_text(
        "vehicle_sil_scenario_template: 1\nsteps:\n  - set: { Engine.engine.throttle: 10 }\n", encoding="utf-8")
    (root / "scenarios" / "cases.yaml").write_text(
        "vehicle_sil_scenario: 1\ncases:\n  idle: {}\nsteps:\n  - set: { Engine.engine.throttle: 10 }\n",
        encoding="utf-8")

    # Custom fake launcher simulating real vehicle_sil_launch behavior on direct template and multi-case without --case
    from vehicle_sil_gui import __version__
    from vehicle_sil_gui.launcher import REQUIRED_FEATURES

    fake = tmp_path / "fake_launch.py"
    fake.write_text(
        "import json, sys\n"
        "from pathlib import Path\n"
        f"FEATURES = {list(REQUIRED_FEATURES)!r}\n"
        f"VERSION = {__version__!r}\n"
        "args = sys.argv[1:]\n"
        "if args[0] == 'version':\n"
        "    print(json.dumps({'vehicle_sil_launch': 1, 'vehicle_sil': VERSION,\n"
        "        'formats': {'check': 1, 'run': 2, 'events': 4, 'timeline': 1},\n"
        "        'features': FEATURES}))\n"
        "    raise SystemExit(0)\n"
        "if args[0] == 'check':\n"
        "    if '--request' in args:\n"
        "        req = json.loads(Path(args[args.index('--request') + 1]).read_text(encoding='utf-8'))\n"
        "        sc = req.get('scenario', '')\n"
        "    elif '--scenario' in args:\n"
        "        sc = args[args.index('--scenario') + 1]\n"
        "    else:\n"
        "        sc = ''\n"
        "    cfg_dir = Path(args[-1]).parent if '--' in args else Path('.')\n"
        "    text = (cfg_dir / sc).read_text(encoding='utf-8') if sc else ''\n"
        "    if 'vehicle_sil_scenario_template' in text:\n"
        "        print(json.dumps({'vehicle_sil_check': 1, 'file': 'simulation.yaml', 'result': 'failed',\n"
        "            'errors': 1, 'warnings': 0, 'notes': 0, 'participants': [], 'networks': [],\n"
        "            'findings': [{'severity': 'error', 'code': 'not-a-scenario', 'file': sc, 'line': 1,\n"
        "                'participant': None, 'network': None, 'message': 'not a runnable scenario'}],\n"
        "            'scenario': {'file': sc, 'name': None, 'steps': 0, 'inputs': None, 'lines': None}}))\n"
        "        raise SystemExit(1)\n"
        "    if 'cases:' in text:\n"
        "        print(json.dumps({'vehicle_sil_check': 1, 'file': 'simulation.yaml', 'result': 'usage',\n"
        "            'errors': 1, 'warnings': 0, 'notes': 0, 'participants': [], 'networks': [],\n"
        "            'findings': [{'severity': 'error', 'code': 'usage', 'file': None, 'line': None,\n"
        "                'participant': None, 'network': None,\n"
        "                'message': 'the scenario defines cases: give --case <name> with one of idle'}]}))\n"
        "        raise SystemExit(4)\n"
        "    print(json.dumps({'vehicle_sil_check': 1, 'file': 'simulation.yaml', 'result': 'passed',\n"
        "        'errors': 0, 'warnings': 0, 'notes': 0, 'participants': [], 'networks': [],\n"
        "        'findings': [], 'hot_eligibility': {'eligible': False, 'reason': None, 'message': None}}))\n"
        "    raise SystemExit(0)\n"
        "if args[0] == 'serve':\n"
        "    inst = '0123456789abcdef'\n"
        "    def mk_status(state, run=None, last=None, rev=0):\n"
        "        ok = {'ok': True, 'reason': None, 'message': None, 'caution': None}\n"
        "        return {'instance': inst, 'status_rev': rev, 'launcher_state': state,\n"
        "            'config': args[-1], 'eligibility': {'eligible': False, 'reason': None, 'message': None},\n"
        "            'launcher_allowed': {'prepare': ok, 'run': ok}, 'active_run_id': run,\n"
        "            'last_run_id': last, 'run_state': None}\n"
        "    print(json.dumps({'event': 'hello', 'seq': 0, 't_ms': 0, 'vehicle_sil_events': 4,\n"
        "        'vehicle_sil': VERSION, 'pid': 1, 'file': args[-1], 'arguments': args, 'launcher': inst}), flush=True)\n"
        "    print(json.dumps({'event': 'status', 'seq': 1, 't_ms': 0, 'id': None, **mk_status('unprepared')}), flush=True)\n"
        "    seq = 2\n"
        "    for raw in sys.stdin:\n"
        "        cmd = json.loads(raw)\n"
        "        if cmd.get('command') == 'run':\n"
        "            req = json.loads(Path(cmd['request']).read_text(encoding='utf-8'))\n"
        "            sc = req.get('scenario', '')\n"
        "            text = (Path(args[-1]).parent / sc).read_text(encoding='utf-8')\n"
        "            code = 'not-a-scenario' if 'vehicle_sil_scenario_template' in text else 'usage'\n"
        "            res_kind = 'check_failed' if code == 'not-a-scenario' else 'usage'\n"
        "            chk_res = 'failed' if code == 'not-a-scenario' else 'usage'\n"
        "            rid = f'{inst}-1'\n"
        "            print(json.dumps({'event': 'run_start', 'seq': seq, 't_ms': 1, 'run': rid, 'id': cmd.get('id')}), flush=True)\n"
        "            seq += 1\n"
        "            chk = {'vehicle_sil_check': 1, 'file': 'simulation.yaml', 'result': chk_res,\n"
        "                'errors': 1, 'warnings': 0, 'notes': 0, 'participants': [], 'networks': [],\n"
        "                'findings': [{'severity': 'error', 'code': code, 'message': code}]}\n"
        "            print(json.dumps({'event': 'check', 'seq': seq, 't_ms': 1, 'run': rid, 'check': chk}), flush=True)\n"
        "            seq += 1\n"
        "            print(json.dumps({'event': 'result', 'seq': seq, 't_ms': 1, 'run': rid,\n"
        "                'launcher_ends': True, 'status': mk_status('ending', None, rid, 2),\n"
        "                'result': {'vehicle_sil_run': 2, 'result': res_kind, 'ok': False,\n"
        "                    'message': 'refused', 'errors': 1, 'warnings': 0, 'notes': 0,\n"
        "                    'simulation': 'not_started', 'participants': [], 'check': chk,\n"
        "                    'findings': chk['findings']}}), flush=True)\n"
        "            raise SystemExit(1 if code == 'not-a-scenario' else 4)\n",
        encoding="utf-8",
    )
    fake.chmod(0o755)
    # Use sys.executable wrapper so shebang isn't needed
    launcher = tmp_path / "launch_wrap"
    launcher.write_text(f"#!{os.sys.executable}\nimport runpy\nrunpy.run_path({str(fake)!r}, run_name='__main__')\n",
                        encoding="utf-8")
    launcher.chmod(0o755)

    with gui_server(tmp_path, root=root, launcher=launcher) as gui:
        async def talk(c: Client) -> None:
            st, body = await c.get("api/scenarios?file=simulation.yaml")
            assert st == 200
            assert [f["path"] for f in body["scenarios"]] == ["scenarios/cases.yaml", "scenarios/direct_template.yaml"]

            # Template in scenarios/templates/ cannot be checked or run
            for ep in ("api/check", "api/run"):
                st, b = await c.post(ep, {"file": "simulation.yaml", "scenario": "scenarios/templates/shared.yaml"})
                assert (st, b["error"]["code"]) == (400, "scenario-not-listed"), (ep, b)

            # Multi-case without --case returns structured error cleanly
            st, chk_case = await c.post("api/check", {"file": "simulation.yaml", "scenario": "scenarios/cases.yaml"})
            assert st == 200 and chk_case["exit_code"] == 4 and chk_case["check"]["result"] == "usage"
            st, run_case = await c.post("api/run", {"file": "simulation.yaml", "scenario": "scenarios/cases.yaml"})
            assert (st, run_case["error"]["code"]) == (422, "check-failed"), run_case
            assert run_case["error"]["detail"]["result"] == "usage"

            # Direct template in scenarios/ returns not-a-scenario cleanly
            st, chk_tpl = await c.post("api/check", {"file": "simulation.yaml", "scenario": "scenarios/direct_template.yaml"})
            assert st == 200 and chk_tpl["exit_code"] == 1
            assert chk_tpl["check"]["findings"][0]["code"] == "not-a-scenario"
            st, run_tpl = await c.post("api/run", {"file": "simulation.yaml", "scenario": "scenarios/direct_template.yaml"})
            assert (st, run_tpl["error"]["code"]) == (422, "check-failed"), run_tpl
            assert run_tpl["error"]["detail"]["findings"][0]["code"] == "not-a-scenario"

            # Server remains healthy
            st, sess = await c.get("api/session")
            assert st == 200 and sess["session"] is None
        run_async(talk, gui)


def test_stub_launcher_stringifies_param_values_for_check_contract(tmp_path: Path) -> None:
    import json
    import subprocess
    import sys

    from vehicle_sil_gui.contracts import CheckObject

    root = tmp_path / "root"
    (root / "scenarios").mkdir(parents=True)
    shutil.copy(REPOSITORY / "gui/server/tests/configs/simulation.yaml", root / "simulation.yaml")
    (root / "scenarios" / "typed_values.yaml").write_text(
        "vehicle_sil_scenario: 1\n"
        "name: typed values\n"
        "parameters:\n"
        "  step_val:\n"
        "    type: integer\n"
        "    values: [0, 10, 20]\n"
        "    default: 10\n"
        "  gain:\n"
        "    type: number\n"
        "    values: [0.0, 1.0, 2.5]\n"
        "    default: 1.0\n"
        "  enabled:\n"
        "    type: boolean\n"
        "    values: [true, false]\n"
        "    default: true\n"
        "cases:\n"
        "  c1:\n"
        "    params:\n"
        "      step_val: 10\n"
        "steps:\n"
        "  - at_ms: 0\n"
        "    set: { Engine.engine.demand: 50 }\n",
        encoding="utf-8",
    )
    stub = REPOSITORY / "gui/server/tests/stub_launcher.py"
    proc = subprocess.run(
        [
            sys.executable,
            str(stub),
            "check",
            "--json",
            "--scenario",
            "scenarios/typed_values.yaml",
            "--case",
            "c1",
            str(root / "simulation.yaml"),
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    data = json.loads(proc.stdout)
    validated = CheckObject.model_validate(data)
    assert validated.scenario is not None
    by_name = {m.name: m.values for m in (validated.scenario.param_meta or [])}
    assert by_name["step_val"] == ["0", "10", "20"]
    assert by_name["gain"] == ["0.0", "1.0", "2.5"]
    assert by_name["enabled"] == ["true", "false"]
