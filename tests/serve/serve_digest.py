# SPDX-License-Identifier: MIT
"""What the tests of vehicle_sil_launch serve compare and check, without running anything (standard library only).

- deterministic(result, timeline): the parts of a run's result (the object of run --json, and of the "result" of a
  serve Run's result event) and of its timeline file that are the same from run to run of one request, whichever
  process ran it. Left out: what depends on the process, the computer or the wall clock (the registry URI with its
  free port, timing.wall_ms and actual_speed, the paths of log files and of the timeline, the timeline's own file
  names). digest() makes one short text of it; differences() names the keys where two of them differ.
- stream_problems(events): the rules of the one stream of serve's events (one hello first, seq without gaps, t_ms
  not going back, every line between a Run's run_start and its result carries that Run, no line of a Run after its
  result, idle only between Runs, run_start only after idle, the Run IDs of one launcher). A list of sentences, empty
  when every rule holds.

The names of the stream that the launcher's documentation settles are in serve_support.py; this module takes them
as arguments so that it needs no launcher.
"""
from __future__ import annotations

import hashlib
import json
from typing import Any, Iterable, List, Mapping, Optional, Sequence

RESULT_KEYS = ("result", "end", "ok", "message", "strict", "errors", "warnings", "notes", "simulation",
               "interrupted_by", "stop_at_ms")
TIMING_KEYS = ("speed", "time_step_ms", "ecu_step_ms", "step_delay_ms", "late_tolerance_ms", "sim_ms",
               "paced_steps", "late_steps", "max_late_ms", "slipped_ms", "late_ms", "paused_ms")
PARTICIPANT_KEYS = ("name", "required", "enabled", "manual", "for_network", "ended", "exit_code", "signal",
                    "left_processes", "control_link")
FINDING_KEYS = ("severity", "code", "participant", "network", "message", "detail")
SCENARIO_KEYS = ("name", "status", "over_at_ms", "steps", "inputs", "lines")
TIMELINE_KEYS = ("stage", "variant", "params", "time_step_ms", "variables", "steps", "series", "lines", "findings",
                 "limits")


def _findings(items: Optional[Iterable[Mapping]]) -> list:
    return [{k: f.get(k) for k in FINDING_KEYS} for f in items or ()]


def _check(check: Optional[Mapping]) -> dict:
    """The check's verdict, its findings and what each participant reported (the paths of the file and of the
    commands are those of the request, the same in both kinds of run)."""
    check = check or {}
    out = {k: check.get(k) for k in ("result", "strict", "errors", "warnings", "notes", "stop_at_ms", "time_step_ms",
                                     "registry_uri", "commands", "time_lock")}
    out["findings"] = _findings(check.get("findings"))
    out["participants"] = [{k: p.get(k) for k in ("name", "command", "describe", "required", "enabled", "manual",
                                                  "role", "step_ms", "status", "reported", "variables", "network_map",
                                                  "j1939_nodes", "control")}
                           for p in check.get("participants") or ()]
    out["networks"] = check.get("networks")
    return out


def deterministic_result(result: Mapping) -> dict:
    """The parts of a run's result that do not depend on the process or the wall clock."""
    out: dict = {k: result.get(k) for k in RESULT_KEYS}
    timing = result.get("timing") or {}
    out["timing"] = {k: timing.get(k) for k in TIMING_KEYS}
    out["participants"] = [{k: p.get(k) for k in PARTICIPANT_KEYS} for p in result.get("participants") or ()]
    out["outside"] = result.get("outside")
    out["traffic"] = result.get("traffic")
    out["findings"] = _findings(result.get("findings"))
    scenario = result.get("scenario")
    if scenario is not None:
        out["scenario"] = {k: scenario.get(k) for k in SCENARIO_KEYS}
    out["check"] = _check(result.get("check"))
    return out


def deterministic_timeline(timeline: Mapping) -> dict:
    """The parts of a timeline file that do not depend on where it was written."""
    out: dict = {k: timeline.get(k) for k in TIMELINE_KEYS}
    scenario = dict(timeline.get("scenario") or {})
    scenario.pop("file", None)
    out["scenario"] = scenario
    return out


def deterministic(result: Mapping, timeline: Optional[Mapping] = None) -> dict:
    out = {"result": deterministic_result(result)}
    if timeline is not None:
        out["timeline"] = deterministic_timeline(timeline)
    return out


def digest(result: Mapping, timeline: Optional[Mapping] = None) -> str:
    """A short text that is the same for two runs exactly when deterministic() is."""
    text = json.dumps(deterministic(result, timeline), sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(text.encode()).hexdigest()[:16]


def differences(a: Any, b: Any, path: str = "", limit: int = 12) -> List[str]:
    """Where two values (such as two deterministic() objects) differ: "<path>: <a> != <b>", at most `limit`."""
    out: List[str] = []

    def walk(x: Any, y: Any, at: str) -> None:
        if len(out) >= limit:
            return
        if isinstance(x, Mapping) and isinstance(y, Mapping):
            for k in sorted(set(x) | set(y), key=str):
                walk(x.get(k, "<missing>"), y.get(k, "<missing>"), f"{at}.{k}" if at else str(k))
        elif isinstance(x, list) and isinstance(y, list) and len(x) == len(y):
            for i, (p, q) in enumerate(zip(x, y)):
                walk(p, q, f"{at}[{i}]")
        elif x != y:
            out.append(f"{at or '<top>'}: {_short(x)} != {_short(y)}")

    walk(a, b, path)
    return out


def _short(value: Any, width: int = 160) -> str:
    text = json.dumps(value, sort_keys=True)
    return text if len(text) <= width else text[:width] + "..."


def first_time_ms(events: Sequence[Mapping]) -> Optional[int]:
    """The sim_ms of the first time event among `events` (None if there is none)."""
    return next((e.get("sim_ms") for e in events if e.get("event") == "time"), None)


def first_points_ms(timeline: Mapping) -> List[int]:
    """The time of the first point of each series of a timeline file."""
    return [s["points"][0][0] for s in timeline.get("series") or () if s.get("points")]


def stream_problems(events: Sequence[Mapping], *, run_key: str = "run", launcher_key: str = "launcher",
                    window_free: Iterable[str] = ("heartbeat",), launcher_commands: str = "command",
                    complete: bool = True) -> List[str]:
    """What breaks the rules of serve's event stream in `events` (all the lines of one launcher, in order).

    `run_key`: the key that names the Run on a line. `launcher_key`: the key of hello with the launcher's instance.
    `window_free`: events that may come inside a Run's window without the Run (heartbeat). `launcher_commands`: the
    event of an answer of the launcher itself, which carries no Run and refuses (accepted false) wherever it comes.
    `complete`: the stream ended (the process has ended): a Run that began has its result.
    """
    problems: List[str] = []

    def bad(index: int, what: str) -> None:
        if len(problems) < 30:
            e = events[index] if 0 <= index < len(events) else {}
            problems.append(f"line {index} ({e.get('event')}, seq {e.get('seq')}): {what}")

    if not events:
        return ["no line at all"]
    if events[0].get("event") != "hello":
        bad(0, "the first line is not hello")
    instance = events[0].get(launcher_key)
    if events[0].get("event") == "hello" and not isinstance(instance, str):
        bad(0, f"hello has no {launcher_key!r} (the launcher's instance)")
    window_free = set(window_free)
    current: Optional[str] = None  # the Run whose window is open
    ended: set = set()  # Runs whose result was written
    started: List[str] = []
    idle_since_result = True  # an idle came after the last result (or nothing ran yet)
    seen_idle = False
    launcher_ends = False
    last_t = None
    rev_of: dict = {}
    for i, e in enumerate(events):
        name = e.get("event")
        if e.get("seq") != i:
            bad(i, f"seq is {e.get('seq')}, not {i} (a line is missing, or seq starts again)")
        t = e.get("t_ms")
        if not isinstance(t, int):
            bad(i, "no t_ms")
        elif last_t is not None and t < last_t:
            bad(i, f"t_ms goes back from {last_t} to {t}")
        else:
            last_t = t
        if name == "hello" and i != 0:
            bad(i, "a second hello")
        run = e.get(run_key)
        if launcher_ends and i > 0:
            bad(i, "a line after the result that said the launcher ends")
        if run is not None and run in ended:
            bad(i, f"a line of Run {run} after its result")
        if name == "run_start":
            if current is not None:
                bad(i, f"run_start while Run {current} has not written its result")
            if not isinstance(run, str):
                bad(i, "run_start without a Run")
                continue
            if not idle_since_result:
                bad(i, "run_start before the idle that follows the previous result")
            if not seen_idle:
                bad(i, "run_start before the first idle")
            if run in started:
                bad(i, f"Run {run} starts a second time")
            if isinstance(instance, str):
                prefix = instance + "-"
                number = run[len(prefix):] if run.startswith(prefix) else ""
                if not number.isdigit():
                    bad(i, f"Run ID {run} is not <launcher {instance}>-<number>")
                elif int(number) != len(started) + 1:
                    bad(i, f"Run ID {run} is not the next number ({len(started) + 1}) of this launcher")
            started.append(run)
            current = run
            idle_since_result = False
            continue
        if name == "idle":
            seen_idle = True
            if current is not None:
                bad(i, f"idle inside the window of Run {current}")
            if run is not None:
                bad(i, "idle carries a Run")
            idle_since_result = True
            continue
        if current is not None:
            refusal = name == launcher_commands and run is None and e.get("accepted") is False
            if run is None and not (name in window_free or refusal):
                bad(i, f"a line without the Run inside the window of Run {current}")
            elif run is not None and run != current:
                bad(i, f"a line of Run {run} inside the window of Run {current}")
            if name == "state" and run == current:
                rev = e.get("rev")
                if isinstance(rev, int):
                    if rev <= rev_of.get(run, 0):
                        bad(i, f"state rev {rev} does not grow in Run {run}")
                    rev_of[run] = rev
            if name == "result" and run == current:
                ended.add(run)
                current = None
                if not isinstance(e.get("launcher_ends"), bool):
                    bad(i, "a Run's result without launcher_ends")
                elif e["launcher_ends"]:
                    launcher_ends = True
            continue
        # Outside every window: the launcher's own lines only.
        if run is not None:
            bad(i, f"a line of Run {run} outside its window")
        elif name == "result":
            # Only the launcher's own end (a usage error at the start): the last line.
            if i != len(events) - 1:
                bad(i, "the launcher's result is not the last line")
        elif name not in ("hello", "heartbeat", launcher_commands):
            bad(i, f"{name} outside a Run's window")
        elif name == launcher_commands and e.get("accepted") is not False:
            bad(i, "the launcher's own command line that is not a refusal")
    if complete and current is not None:
        problems.append(f"Run {current} has no result (the stream ended inside its window)")
    return problems


def run_window(events: Sequence[Mapping], run: str, run_key: str = "run") -> List[Mapping]:
    """The lines of one Run (its run_start to its result, the lines that carry it)."""
    return [e for e in events if e.get(run_key) == run]
