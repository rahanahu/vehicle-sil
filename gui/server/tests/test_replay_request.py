"""The replay launcher must distinguish the timing intents sent to the real launcher."""

import json

import pytest

from replay_launcher import expand_request
from record_launcher import STOP, cases


@pytest.mark.parametrize("command", ["check", "run"])
def test_replay_request_preserves_timing_intent(tmp_path, command):
    request = tmp_path / "request.json"
    request.write_text(json.dumps({"speed": 1, "stop_at_ms": 1003, "late_tolerance_ms": 0}), encoding="utf-8")
    assert expand_request([command, "--json", "--request", str(request), "--", "simulation.yaml"]) == [
        command, "--json", "--speed", "1", "--stop-at-ms", "1003", "--late-tolerance-ms", "0",
        "--", "simulation.yaml",
    ]


def test_replay_check_preserves_paused_intent(tmp_path):
    request = tmp_path / "request.json"
    request.write_text(json.dumps({"start_paused": True}), encoding="utf-8")
    assert expand_request(["check", "--json", "--request", str(request), "--", "simulation.yaml"]) == [
        "check", "--json", "--start-paused", "--", "simulation.yaml",
    ]


def test_recordings_have_explicit_completion_conditions():
    recorded = {case.name: case for case in cases()}
    for name, stop in (("run-simulation", "1000"), ("run-result-then-wait", "300"),
                       ("control-simulation-resume", "1000"), ("run-connect", "1000")):
        argv = recorded[name].argv
        assert argv[argv.index("--stop-at-ms") + 1] == stop
    assert recorded["run-connect-unused"].request["stop_at_ms"] == 1000
    # Scenarios and participant failure already supply their own termination condition.
    assert "--stop-at-ms" not in recorded["run-scenario-passes"].argv
    assert "--stop-at-ms" not in recorded["run-participant-crash"].argv
    for case in recorded.values():
        if case.argv[0] != "run" or "--stop-at-ms" in case.argv or "--scenario" in case.argv:
            continue
        if case.name in {"run-participant-crash", "run-participant-crash-speed1"}:
            continue  # the participant process fails and ends the run
        assert any(action[0] in {"close", "hang"} or action[0] == "send" and action[1] == STOP
                   for action in case.script), f"{case.name}: no end condition"
