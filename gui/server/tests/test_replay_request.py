"""The replay launcher must distinguish the timing intents sent to the real launcher."""

import json

import pytest

from replay_launcher import expand_request


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
