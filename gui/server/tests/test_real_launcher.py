"""The fake launcher against the real one: the same scenarios through both, and the shapes of what they write
compared (event kinds and their order, the keys of each event and their JSON types, exit codes, version --json,
check --json), so that the fake the other tests use cannot drift from vehicle_sil_launch.

Runs only when VEHICLE_SIL_GUI_LAUNCHER names a real vehicle_sil_launch (scripts/verify-gui.py --launcher
<path>) and the ecu_main example ECUs are built next to it (or VEHICLE_SIL_GUI_EXAMPLES names their
directory); otherwise every test here is skipped, with the reason. Linux only (it kills an ECU by its pid).
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import threading
import time
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import pytest
from conftest import FAKE
from real_configs import examples_dir, usable, write

LAUNCHER = os.environ.get("VEHICLE_SIL_GUI_LAUNCHER")
EXAMPLES = examples_dir(LAUNCHER)
REASON = ("no real launcher: set VEHICLE_SIL_GUI_LAUNCHER to build/<preset>/tools/launch/vehicle_sil_launch "
          "(scripts/verify-gui.py --launcher <path>)" if not LAUNCHER or not Path(LAUNCHER).is_file()
          else f"no ecu_main example executables in {EXAMPLES}: build them, or set VEHICLE_SIL_GUI_EXAMPLES"
          if not usable(EXAMPLES) else "Linux only" if sys.platform != "linux" else None)
pytestmark = pytest.mark.skipif(REASON is not None, reason=REASON or "")

RUN_TIMEOUT_S = 60
# Events whose number and place depend on timing, not on what happens: left out of the order compared (time: the
# real launcher writes it at most every 100 ms while the time moves; check_rules checks where it must be).
NOT_IN_ORDER = {"log", "heartbeat", "log_dropped", "time"}
# Findings that depend on how the run went on this machine (a real ECU may be late on a busy machine; the real
# launcher may drop a hello or a message of the control links): compared, place included, when both launchers wrote
# them; left out of the order when only one did.
TIMING_FINDINGS = {"finding:behind-real-time", "finding:control-link-dropped"}


@dataclass
class Run:
    events: list[dict]
    exit_code: int
    stderr: str
    pids: dict[str, int] = field(default_factory=dict)


def launch(argv: list[str], cwd: Path, *, env: dict | None = None, stop_on: str | None = None, stops: int = 1,
           stop_after_s: float = 0.0, close_stdin: bool = False, kill_on_running: str | None = None,
           pause_reading_s: float = 0.0, script: tuple = ()) -> Run:
    """Runs a launcher with --events and --stdin-commands and reads its events to the result. stop_on: the phase
    on which `stops` stop commands are written (stop_after_s later); close_stdin: standard input is closed at
    once; kill_on_running: the participant killed (SIGKILL) half a second after the phase running;
    pause_reading_s: how long the reading stops when the phase running comes (a slow reader); script: (condition
    on an event, lines) in order: the lines are written when an event after the one before meets the condition."""
    script = list(script)
    proc = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env={**os.environ, **(env or {})}, start_new_session=True)
    run = Run([], -1, "")
    err: list[bytes] = []
    threading.Thread(target=lambda: err.append(proc.stderr.read()), daemon=True).start()
    timer = threading.Timer(RUN_TIMEOUT_S, lambda: os.killpg(proc.pid, signal.SIGKILL))
    timer.start()
    try:
        if close_stdin:
            proc.stdin.close()
        for line in proc.stdout:
            event = json.loads(line)
            run.events.append(event)
            if event["event"] == "participant" and event.get("state") == "started":
                run.pids[event["name"]] = event["pid"]
            if event["event"] == "phase" and event["phase"] == "running":
                if kill_on_running:
                    pid = run.pids[kill_on_running]
                    threading.Timer(0.5, lambda: os.kill(pid, signal.SIGKILL)).start()
                if pause_reading_s:
                    time.sleep(pause_reading_s)
            if script and script[0][0](event):
                proc.stdin.write(script.pop(0)[1])
                proc.stdin.flush()
            if event["event"] == "phase" and event["phase"] == stop_on:
                time.sleep(stop_after_s)
                proc.stdin.write(b'{"command":"stop"}\n' * stops)
                proc.stdin.flush()
            if event["event"] == "result":
                break
        if not proc.stdin.closed:
            proc.stdin.close()
        run.exit_code = proc.wait(RUN_TIMEOUT_S)
    finally:
        timer.cancel()
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
    time.sleep(0.05)
    run.stderr = b"".join(err).decode("utf-8", "replace")
    return run


def real(args: list[str], cwd: Path, **kw) -> Run:
    return launch([LAUNCHER, *args], cwd, **kw)


def fake(args: list[str], cwd: Path, scenario: str | None = None, **kw) -> Run:
    env = {"FAKE_LAUNCHER_TICK_MS": "20", **({"FAKE_LAUNCHER_SCENARIO": scenario} if scenario else {})}
    return launch([sys.executable, str(FAKE), *args], cwd, env=env, **kw)


# ---------------------------------------------------------------- what is compared


def token(event: dict) -> str:
    kind = event["event"]
    if kind == "phase":
        return f"phase:{event['phase']}"
    if kind == "participant":
        return f"participant:{event['state']}"
    if kind == "finding":
        return f"finding:{event['code']}"
    if kind == "command":
        return f"command:{event['source']}"
    return kind


def order(run: Run) -> list[str]:
    """The kinds of events in order, without those that depend on timing (log, heartbeat, log_dropped), the
    command events (they come when the command does) and the participant state `failed` (not every launcher
    writes it yet). Participant events between two other events are sorted: their order across participants
    depends on timing."""
    out: list[str] = []
    group: list[str] = []
    for event in run.events:
        if event["event"] in NOT_IN_ORDER or event["event"] == "command" or token(event) == "participant:failed":
            continue
        if event["event"] == "participant":
            group.append(token(event))
            continue
        out += sorted(group) + [token(event)]
        group = []
    return out + sorted(group)


def check_rules(run: Run) -> None:
    """What docs/simulation-config.md promises about every stream."""
    events = run.events
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
            assert check_seq is not None and check_seq < e["seq"] and (starting is None or e["seq"] < starting)
    times = [e for e in events if e["event"] == "time"]
    running = next((e["seq"] for e in events if e.get("phase") == "running"), None)
    if running is not None:
        assert times and times[0]["seq"] > running and times[0]["sim_ms"] == 0, "time 0 when running starts"
        assert all(a["sim_ms"] <= b["sim_ms"] for a, b in zip(times, times[1:])), "sim_ms does not go back"
        assert times[-1]["paused"] is False, "an ended simulation is not paused"
    first_command = next((e["seq"] for e in events if e["event"] == "command"), None)
    stopping = next((e["seq"] for e in events if e.get("phase") == "stopping"), None)
    if first_command is not None and stopping is not None and run.events[-1]["result"]["interrupted_by"]:
        assert first_command < stopping, "the command comes before what it makes happen"


def json_type(value: object) -> str:
    return {bool: "boolean", int: "number", float: "number", str: "string", list: "array", dict: "object",
            type(None): "null"}[type(value)]


class Shapes:
    """The keys of each kind of event (participant: of each state) and the JSON types seen for each key."""

    def __init__(self):
        self.keys: dict[str, set[frozenset]] = defaultdict(set)
        self.types: dict[tuple[str, str], set[str]] = defaultdict(set)

    def add(self, run: Run) -> None:
        for event in run.events:
            kind = token(event) if event["event"] in ("participant", "phase", "command") else event["event"]
            kind = kind.split(":")[0] + (f":{event['state']}" if event["event"] == "participant" else "")
            self.keys[kind].add(frozenset(event))
            for k, v in event.items():
                self.types[(kind, k)].add(json_type(v))
            for name, value in (("check", event.get("check")), ("result", event.get("result"))):
                if isinstance(value, dict):
                    self._object(f"{kind}.{name}", value)
            if event["event"] == "result" and isinstance(event["result"].get("check"), dict):
                self._object("result.result.check", event["result"]["check"])
            if event["event"] == "result" and isinstance(event["result"].get("timing"), dict):
                self._object("result.result.timing", event["result"]["timing"])

    def _object(self, where: str, value: dict) -> None:
        self.keys[where].add(frozenset(value))
        for k, v in value.items():
            self.types[(where, k)].add(json_type(v))
            if isinstance(v, list):
                for item in v:
                    if isinstance(item, dict):
                        self.keys[f"{where}.{k}[]"].add(frozenset(item))


# Participant states that these runs may make only one launcher write: killed (the real ECUs end when asked),
# failed (not every launcher writes it yet), not_started (the fake has no scenario for it).
ONE_SIDED = {"participant:killed", "participant:failed", "participant:not_started"}


def compare_shapes(real_shapes: Shapes, fake_shapes: Shapes) -> list[str]:
    differences = []
    for kind in sorted(set(real_shapes.keys) | set(fake_shapes.keys)):
        r, f = real_shapes.keys.get(kind), fake_shapes.keys.get(kind)
        if r is None or f is None:
            if kind not in ONE_SIDED:
                differences.append(f"{kind}: written only by the {'fake' if r is None else 'real'} launcher")
            continue
        if r != f:
            differences.append(f"{kind}: keys differ: real {sorted(map(sorted, r))}, fake {sorted(map(sorted, f))}")
    for key in sorted(set(real_shapes.types) & set(fake_shapes.types)):
        r = real_shapes.types[key] - {"null"}
        f = fake_shapes.types[key] - {"null"}
        if r and f and r != f:
            differences.append(f"{key[0]}.{key[1]}: types differ: real {sorted(r)}, fake {sorted(f)}")
    return differences


# ---------------------------------------------------------------- the scenarios


SCENARIOS = {
    # name: (arguments, keyword arguments of launch, fake scenario or None, keyword arguments only for the real)
    "pass": (["simulation.yaml"], {}, None, {}),
    "disable": (["--disable", "Dashboard", "simulation.yaml"], {}, None, {}),
    "stop": (["forever.yaml"], {"stop_on": "running", "stop_after_s": 0.3}, None, {}),
    "stop-twice": (["forever.yaml"], {"stop_on": "running", "stops": 2, "stop_after_s": 0.3}, None, {}),
    "end-of-input": (["simulation.yaml"], {"close_stdin": True}, None, {}),
    "check-error": (["missing-executable.yaml"], {}, None, {}),
    "unreadable": (["missing.yaml"], {}, None, {}),
    "usage": (["--bogus", "simulation.yaml"], {}, None, {}),
    "unknown-name": (["--disable", "Nobody", "simulation.yaml"], {}, None, {}),
    "participant-fails": (["forever.yaml"], {}, "participant-crash", {"kill_on_running": "Engine"}),
    "paced": (["--speed", "4", "simulation.yaml"], {}, None, {}),
    "time-commands": (["--start-paused", "--speed", "max", "forever.yaml"], {"script": (
        (lambda e: e["event"] == "time" and e["paused"], b'{"command":"step","ms":25}\n'),
        (lambda e: e["event"] == "time" and e["paused"] and e["sim_ms"] > 0, b'{"command":"speed","value":1}\n'),
        (lambda e: e["event"] == "time" and e["speed"] == 1, b'{"command":"resume"}\n'),
        (lambda e: e["event"] == "time" and not e["paused"], b'{"command":"step","ms":10}\n{"command":"pause"}\n'),
        (lambda e: e["event"] == "time" and e["paused"], b'{"command":"stop"}\n'),
    )}, None, {}),
}


@pytest.fixture(scope="module")
def runs(tmp_path_factory) -> dict[str, tuple[Run, Run]]:
    directory = tmp_path_factory.mktemp("real")
    write(directory, EXAMPLES)
    out = {}
    for name, (args, kw, scenario, real_only) in SCENARIOS.items():
        argv = ["run", "--events", "--stdin-commands", *args]
        out[name] = (real(argv, directory, **kw, **real_only), fake(argv, directory, scenario, **kw))
    return out


@pytest.mark.parametrize("name", SCENARIOS)
def test_the_same_events_in_the_same_order(runs, name: str) -> None:
    real_run, fake_run = runs[name]
    check_rules(real_run)
    check_rules(fake_run)
    assert fake_run.exit_code == real_run.exit_code, f"{name}: exit code"
    real_order, fake_order = order(real_run), order(fake_run)
    for kind in TIMING_FINDINGS:
        if (kind in real_order) != (kind in fake_order):
            real_order = [k for k in real_order if k != kind]
            fake_order = [k for k in fake_order if k != kind]
    assert fake_order == real_order, f"{name}: the order of the events differs"
    # The findings of the result in the same order (errors, warnings, notes), timing ones as above
    def codes(run: Run) -> list[str]:
        return [f["code"] for f in run.events[-1]["result"]["findings"]]
    real_codes, fake_codes = codes(real_run), codes(fake_run)
    for code in (kind.split(":", 1)[1] for kind in TIMING_FINDINGS):
        if (code in real_codes) != (code in fake_codes):
            real_codes = [c for c in real_codes if c != code]
            fake_codes = [c for c in fake_codes if c != code]
    assert fake_codes == real_codes, f"{name}: the findings of the result differ"
    def commands(run: Run) -> list[tuple]:
        return [(token(e), e["command"], e["accepted"], e.get("ms")) for e in run.events if e["event"] == "command"]
    assert commands(fake_run) == commands(real_run), name
    real_result, fake_result = real_run.events[-1]["result"], fake_run.events[-1]["result"]
    for key in ("result", "simulation", "interrupted_by"):
        assert fake_result[key] == real_result[key], f"{name}: result.{key}"
    assert [p["ended"] for p in fake_result["participants"]] == [p["ended"] for p in real_result["participants"]] \
        or name == "participant-fails", f"{name}: how the participants ended"
    assert bool(fake_run.stderr.strip()) == bool(real_run.stderr.strip()), \
        f"{name}: standard error: real {real_run.stderr[:200]!r}, fake {fake_run.stderr[:200]!r}"


def test_the_same_keys_and_types(runs, tmp_path: Path) -> None:
    real_shapes, fake_shapes = Shapes(), Shapes()
    for real_run, fake_run in runs.values():
        real_shapes.add(real_run)
        fake_shapes.add(fake_run)
    # A heartbeat and a log_dropped from each side: a run that waits (the real launcher waits for a participant
    # that never joins; the fake steps slowly), and a reader that stops reading for a while.
    write(tmp_path, EXAMPLES)
    argv = ["run", "--events", "--stdin-commands"]
    real_shapes.add(real([*argv, "not-joined.yaml"], tmp_path))
    fake_shapes.add(launch([sys.executable, str(FAKE), *argv, "forever.yaml"], tmp_path,
                           env={"FAKE_LAUNCHER_TICK_MS": "1500"}, stop_on="running", stop_after_s=2.5))
    slow = real([*argv, "forever.yaml"], tmp_path, stop_on="running", pause_reading_s=5)
    real_shapes.add(slow)
    fake_shapes.add(fake([*argv, "simulation.yaml"], tmp_path, "log-dropped"))
    fake_shapes.add(fake([*argv, "forever.yaml"], tmp_path, "ignore-stop", stop_on="running", stops=2))
    assert "heartbeat" in real_shapes.keys and "heartbeat" in fake_shapes.keys
    if not any(e["event"] == "log_dropped" for e in slow.events):
        fake_shapes.keys.pop("log_dropped", None)  # the ECUs did not write enough to fill the queue
        print("skipped: log_dropped (the real launcher did not drop any log line in 5 s)")
    differences = compare_shapes(real_shapes, fake_shapes)
    assert not differences, "the fake launcher writes other shapes than the real one:\n" + "\n".join(differences)


def test_version_and_check_json(tmp_path: Path) -> None:
    write(tmp_path, EXAMPLES)

    def output(argv: list[str]) -> tuple[int, dict]:
        p = subprocess.run(argv, cwd=tmp_path, capture_output=True, timeout=RUN_TIMEOUT_S)
        return p.returncode, json.loads(p.stdout)

    real_version = output([LAUNCHER, "version", "--json"])
    fake_version = output([sys.executable, str(FAKE), "version", "--json"])
    assert fake_version[0] == real_version[0] == 0
    assert list(fake_version[1]) == list(real_version[1])
    assert fake_version[1]["formats"] == real_version[1]["formats"]
    assert fake_version[1]["features"] == real_version[1]["features"], \
        "the fake's default features (FAKE_LAUNCHER_FEATURES) must be the real launcher's"
    real_text = subprocess.run([LAUNCHER, "version", "--json"], capture_output=True, text=True).stdout
    fake_text = subprocess.run([sys.executable, str(FAKE), "version", "--json"], capture_output=True, text=True).stdout
    assert fake_text.replace(fake_version[1]["vehicle_sil"], "<v>") == \
        real_text.replace(real_version[1]["vehicle_sil"], "<v>"), "the same layout"
    for args in (["simulation.yaml"], ["--disable", "Dashboard", "simulation.yaml"], ["missing.yaml"],
                 ["--disable", "Nobody", "simulation.yaml"], ["--bogus", "simulation.yaml"]):
        real_code, real_check = output([LAUNCHER, "check", "--json", *args])
        fake_code, fake_check = output([sys.executable, str(FAKE), "check", "--json", *args])
        assert fake_code == real_code, args
        assert list(fake_check) == list(real_check), args
        assert fake_check["result"] == real_check["result"], args
        for p, q in zip(fake_check["participants"], real_check["participants"]):
            assert list(p) == list(q), args
            for v, w in zip(p["variables"] or [], q["variables"] or []):
                assert list(v) == list(w), args
