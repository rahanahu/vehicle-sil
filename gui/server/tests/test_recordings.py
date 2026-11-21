"""The recordings (recordings/) and the program that plays them (replay_launcher.py), without a real launcher: what
a recording may hold, that each replays as the event stream docs/simulation-events.md promises, that each is of
the text of its configuration as it is now, and what the program says when no recording fits.
(test_real_launcher.py compares the recordings with the real launcher when there is one.)"""

from __future__ import annotations

import getpass
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
from pathlib import Path

import pytest
from conftest import CONFIGS, REPLAY

RECORDINGS = REPLAY.parent / "recordings"
EXAMPLES = REPLAY.parents[2] / "protocol" / "examples"
sys.path.insert(0, str(REPLAY.parent))
import replay_launcher  # noqa: E402
from record_launcher import Case, Recorder, normalize_advertised_addresses  # noqa: E402

ALL = sorted(p.stem for p in RECORDINGS.glob("*.jsonl"))
RUNS = [name for name in ALL if replay_launcher.read_recording(RECORDINGS / f"{name}.jsonl")[0]["shape"][0] == "run"]


def recording(name: str) -> tuple[dict, list[dict]]:
    return replay_launcher.read_recording(RECORDINGS / f"{name}.jsonl")


def test_manual_participant_addresses_are_normalized(tmp_path: Path) -> None:
    """The hint may list IPv4 and IPv6 addresses from the recording computer."""
    line = ("from other computers, join at silkit://198.51.100.9:8501 or "
            "silkit://[2001:db8::9]:8501 (this computer's addresses): give it as --registry-uri")
    expected = ("from other computers, join at silkit://192.0.2.1:8501 or "
                "silkit://192.0.2.1:8501 (this computer's addresses): give it as --registry-uri")
    recorder = Recorder(Path("launcher"), tmp_path, tmp_path)
    case = Case("address-hint", ["version"], "manual address hint")
    _, entries = recorder.normalize(case, [(0.0, "out", json.dumps({"event": "log", "line": line}))], 0)
    assert entries[0]["print"]["line"] == expected
    assert normalize_advertised_addresses(expected) == expected
    assert normalize_advertised_addresses("registry at silkit://127.0.0.1:8501") == "registry at silkit://127.0.0.1:8501"
    with pytest.raises(RuntimeError, match="an advertised address of the recording host"):
        recorder.forbid(case, {}, [{"out": {"event": "log", "line": line}}])


def test_no_recording_contains_unmasked_advertised_addresses() -> None:
    for path in [*RECORDINGS.glob("*.jsonl"), *EXAMPLES.glob("*.json")]:
        text = path.read_text(encoding="utf-8")
        assert normalize_advertised_addresses(text) == text, f"{path.name} has an unmasked advertised address"


def test_no_path_user_host_or_token_of_the_computer_that_recorded() -> None:
    """The recordings, and the examples of gui/protocol made from them."""
    user = getpass.getuser()
    host = socket.gethostname().split(".")[0]
    for path in [*RECORDINGS.glob("*.jsonl"), *EXAMPLES.glob("*.json")]:
        text = path.read_text(encoding="utf-8")
        found = [what for what, pattern in (
            ("a home directory", r"/home/|/Users/|[A-Za-z]:\\\\Users"),
            ("a temporary directory", r"/tmp/|\\\\Temp\\\\"),
            ("a control token", r"<control-token>")) if re.search(pattern, text)]
        for what, name in (("the user's name", user), ("the host name", host)):
            if len(name) >= 3 and re.search(rf"\b{re.escape(name)}\b", text, re.IGNORECASE):
                found.append(what)
        assert not found, f"{path.name} has {', '.join(found)}: record it again (record_launcher.py replaces them; " \
                          "for an example, its normalizer in test_protocol_examples.py)"


def test_every_recording_is_of_its_configuration_as_it_is_now() -> None:
    for name in ALL:
        head, _ = recording(name)
        assert head["case"] == name
        for key, sha in (("config", "config_sha256"), ("scenario", "scenario_sha256")):
            if key in head:
                assert replay_launcher.text_sha256(str(CONFIGS / head[key])) == head[sha], \
                    f"{name}: configs/{head[key]} has changed since it was recorded: record it again " \
                    f"(record_launcher.py --launcher <vehicle_sil_launch> {name})"


def check_rules(events: list[dict]) -> None:
    """What docs/simulation-events.md promises about every stream."""
    assert events and events[0]["event"] == "hello" and events[-1]["event"] == "result"
    assert [e["seq"] for e in events] == list(range(len(events))), "seq goes up by one from 0"
    assert all(a["t_ms"] <= b["t_ms"] for a, b in zip(events, events[1:])), "t_ms does not go back"
    assert all(list(e)[:3] == ["event", "seq", "t_ms"] for e in events), "every line starts with event, seq, t_ms"
    phases = [e["phase"] for e in events if e["event"] == "phase"]
    rank = ["checking", "starting", "waiting", "running", "stopping", "ended"]
    assert [rank.index(p) for p in phases] == sorted(rank.index(p) for p in phases), phases
    started = {e["name"]: e["seq"] for e in events if e["event"] == "participant" and e["state"] == "started"}
    for e in events:
        if e["event"] == "log" and e["source"] in started:
            assert e["seq"] > started[e["source"]], "a participant's log comes after its started"
    starting = next((e["seq"] for e in events if e.get("phase") == "starting"), None)
    check_seq = next((e["seq"] for e in events if e["event"] == "check"), None)
    for e in events:
        if e["event"] == "participant" and e["state"] == "disabled":
            assert check_seq is not None and check_seq < e["seq"] and (starting is None or e["seq"] < starting), \
                "disabled comes between the check and the phase starting"
    times = [e for e in events if e["event"] == "time"]
    running = next((e["seq"] for e in events if e.get("phase") == "running"), None)
    if running is not None:
        assert times and times[0]["seq"] > running and times[0]["sim_ms"] == 0, "time 0 when running starts"
        assert all(a["sim_ms"] <= b["sim_ms"] for a, b in zip(times, times[1:])), "sim_ms does not go back"
        # Whether it stands still and the speed set are the state's to say.
        assert not any("paused" in e or "speed" in e for e in times), \
            "a time event has paused or speed: the recording is from an older launcher (the two are in the state " \
            "event now): record it again (record_launcher.py --launcher <vehicle_sil_launch> <case>)"
        states = [e for e in events if e["event"] == "state"]
        assert not states or states[-1]["paused"] is False, "an ended simulation is not paused"
    first_command = next((e["seq"] for e in events if e["event"] == "command"), None)
    stopping = next((e["seq"] for e in events if e.get("phase") == "stopping"), None)
    if first_command is not None and stopping is not None and events[-1]["result"]["interrupted_by"]:
        assert first_command < stopping, "the command comes before what it makes happen"


def replay(argv: list[str], lines: list[str | None], env: dict | None = None, timeout: float = 20) \
        -> tuple[int, list[dict], str]:
    """Runs the replay in configs/ with these lines on its standard input (None: the input ends there; else it
    stays open, as the GUI server keeps it, until the replay ends)."""
    proc = subprocess.Popen([sys.executable, str(REPLAY), *argv], cwd=CONFIGS, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env={**os.environ, "REPLAY_TICK_MS": "1", "REPLAY_HEARTBEAT_MS": "0",
                                 "REPLAY_FAULT": "none", **(env or {})})
    out: list[bytes] = []
    err: list[bytes] = []
    readers = [threading.Thread(target=lambda: out.append(proc.stdout.read()), daemon=True),
               threading.Thread(target=lambda: err.append(proc.stderr.read()), daemon=True)]
    for r in readers:
        r.start()
    for line in lines:
        if line is None:
            proc.stdin.close()
            break
        proc.stdin.write(line.encode() + b"\n")
        proc.stdin.flush()
    try:
        proc.wait(timeout)
    except subprocess.TimeoutExpired:
        proc.send_signal(signal.SIGTERM)
        proc.wait(10)
    for r in readers:
        r.join(10)
    if not proc.stdin.closed:
        proc.stdin.close()
    events = [json.loads(line) for line in b"".join(out).decode().splitlines() if line.startswith("{")]
    return proc.returncode, events, b"".join(err).decode()


def argv_of(head: dict) -> list[str]:
    return [head.get("config") if a == "<file>" else head.get("scenario") if a == "<scenario>" else
            "control-credentials.txt" if a == "<credentials>" else a for a in head["shape"]]


@pytest.mark.parametrize("name", RUNS)
def test_every_run_replays_by_the_rules_of_the_event_stream(name: str, tmp_path: Path, request) -> None:
    head, entries = recording(name)
    env = {}
    if "--control" in head["shape"]:  # the stand-in control server
        env["VEHICLE_SIL_CONTROL_STUBS"] = str(request.getfixturevalue("control_stubs"))
    argv = argv_of(head)
    if "--control-credentials" in argv:
        credentials = tmp_path / "credentials.txt"
        credentials.write_text("operator gui t0ken\n")
        credentials.chmod(0o600)
        argv[argv.index("--control-credentials") + 1] = str(credentials)
    timeline = tmp_path / "out" / "run.timeline.json"
    if "--timeline" in argv:
        argv[argv.index("--timeline") + 1] = str(timeline)
    sends = [e["send"] for e in entries if "send" in e]
    code, events, err = replay(argv, sends, env, timeout=5 if any("hang" in e for e in entries) else 20)
    if "--timeline" in argv:  # written where the launcher wrote it: before the line that names it
        written = json.loads(timeline.read_text(encoding="utf-8"))
        assert written["vehicle_sil_timeline"] == 1 and written["file"] == head["config"], written
        assert any(e["event"] == "log" and e["line"] == f"timeline: {timeline}" for e in events), err
    if any("hang" in e for e in entries):
        assert code == 143 and events[-1]["event"] != "result", err  # waits on, as the launcher did, until a signal
        return
    check_rules(events)
    assert code == next(e["exit"] for e in entries if "exit" in e), err
    assert sum(e["event"] == "command" for e in events) == sum(
        e.get("out", {}).get("event") == "command" for e in entries)


def test_a_command_line_without_a_recording_says_what_to_do() -> None:
    code, _, err = replay(["run", "--events", "--stdin-commands", "--speed", "7", "--", "simulation.yaml"], [])
    assert code == 2 and "no recording of `run --events --stdin-commands --speed 7 -- simulation.yaml`" in err
    assert "record_launcher.py" in err


def test_a_file_changed_since_it_was_recorded_says_so(tmp_path: Path) -> None:
    changed = CONFIGS / "simulation.yaml"
    copy = tmp_path / "simulation.yaml"
    copy.write_text(changed.read_text(encoding="utf-8") + "# changed\n", encoding="utf-8")
    proc = subprocess.run([sys.executable, str(REPLAY), "check", "--json", "--", "simulation.yaml"], cwd=tmp_path,
                          capture_output=True, text=True, timeout=20)
    assert proc.returncode == 2 and "has changed since they were made" in proc.stderr, proc.stderr


def test_a_copy_of_a_configuration_plays_the_recording_of_its_text(tmp_path: Path) -> None:
    (tmp_path / "nested").mkdir()
    (tmp_path / "nested" / "copy.yml").write_text((CONFIGS / "simulation.yaml").read_text(encoding="utf-8"))
    proc = subprocess.run([sys.executable, str(REPLAY), "check", "--json", "--", "nested/copy.yml"], cwd=tmp_path,
                          capture_output=True, text=True, timeout=20)
    assert proc.returncode == 0, proc.stderr
    assert json.loads(proc.stdout)["file"] == "nested/copy.yml"


def test_a_command_that_no_recording_has_next_ends_the_replay() -> None:
    code, events, err = replay(["run", "--events", "--stdin-commands", "--", "forever.yaml"],
                               ['{"command":"speed","value":3}'])
    assert code == 3 and events[-1]["event"] != "result"
    assert 'it got {"command":"speed","value":3}' in err and "record a case" in err


def test_a_recording_of_an_older_launcher_is_told_to_be_recorded_again() -> None:
    """The time events have no paused and no speed (the state event says both): a recording that has them is from
    an older launcher, and the stand-in would not know from it when the simulation stands still."""
    def stream(time: dict) -> list[dict]:
        out = [{"event": "hello"}, {"event": "phase", "phase": "running"}, {"event": "time", "sim_ms": 0, **time},
               {"event": "result", "result": {"result": "interrupted", "interrupted_by": None}}]
        return [{"event": e["event"], "seq": seq, "t_ms": seq, **e} for seq, e in enumerate(out)]

    check_rules(stream({}))
    for old in ({"paused": False}, {"speed": "max"}):
        with pytest.raises(AssertionError, match="a time event has paused or speed.*record it again"):
            check_rules(stream(old))


def test_a_stop_goes_on_with_the_recording_that_has_it() -> None:
    """forever.yaml has recordings with one stop, two stops, a refused step, the end of input: each is chosen by
    what comes."""
    for lines, interrupted_by, exit_code, result in (
            ([replay_launcher.json.dumps({"command": "stop"})], "stop command", 0, "stopped"),
            ([None], "end of input", 130, "interrupted")):
        code, events, err = replay(["run", "--events", "--stdin-commands", "--", "forever.yaml"], lines)
        assert code == exit_code and events[-1]["result"]["interrupted_by"] == interrupted_by, err
        assert events[-1]["result"]["result"] == result, err
        check_rules(events)
    code, events, err = replay(["run", "--events", "--stdin-commands", "--", "forever.yaml"],
                               ['{"command":"step","ms":10}', '{"command":"stop"}'])
    commands = [(e["command"], e["accepted"]) for e in events if e["event"] == "command"]
    assert commands == [("step", False), ("stop", True)], err


def test_a_command_more_than_any_recording_has_ends_the_replay() -> None:
    """One stop more than recorded (forever.yaml has recordings with one and with two): the replay does not take it
    silently, so a test sees a GUI server that writes a command too many."""
    stop = '{"command":"stop"}'
    code, events, err = replay(["run", "--events", "--stdin-commands", "--", "forever.yaml"], [stop, stop])
    assert code == 0 and [e["accepted"] for e in events if e["event"] == "command"] == [True, True], err
    code, events, err = replay(["run", "--events", "--stdin-commands", "--", "forever.yaml"], [stop, stop, stop])
    assert code == 3 and events[-1]["event"] != "result", err
    assert f"{stop} came where no recording of this command line has a command" in err
