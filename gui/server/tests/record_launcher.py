#!/usr/bin/env python3
"""Records what the real vehicle_sil_launch writes, for replay_launcher.py (the stand-in of the GUI server's tests
and of the screen's e2e).

    record_launcher.py --launcher <vehicle_sil_launch> [--examples <dir>] [--out <dir>] [<case>...]
    record_launcher.py --launcher <vehicle_sil_launch> --compare [<case>...]

Each case of CASES below runs the real launcher in a copy of configs/ (with engine/ and dashboard/ linked to the
ecu_main example's executables, which are built next to the launcher: build/<preset>/examples/ecu_main, or
--examples), writes the commands of its script to the launcher's standard input when its output says so, and
writes what came back to recordings/<case>.jsonl (--out: another directory). Without cases, every case.

A case with --timeline <timeline> has the launcher write its timeline file into the copy; the recording holds the
file (a timeline_file entry, before the line that names the file) and the mark <timeline> for its name.

The cases that connect a network outside (tools) run the copy of the launcher that sits next to the bridges built on
the fake drivers (--connect-tools, default: tests/launch/connect_tools of the launcher's build, made by the CMake
target vehicle_sil_launch_connect_tools), with VEHICLE_SIL_TEST_PCAP_LIBRARY set to the fake pcap library
(--fake-pcap, default: tests/eth_link/fake_pcap/vehicle_sil_fake_pcap.so of that build), so the Ethernet bridge lists
and opens its adapters fake0 and fake-echo. Where the bridges are is replaced with the mark <tools>. Without those
files the cases are skipped, and the summary line says so. The list of a network (connections) is recorded in a
network namespace of its own (unshare -rn), where the only interface is lo, so that the list of the backend tap holds
no interface of this computer; --compare runs it without one where unshare -rn cannot be used (it compares only the
shapes of a list), and recording it there fails with what to do.

What changes from run to run is replaced with a fixed mark: the copy's directory (<root>), the pids ("<pid>"), the
registry's port (silkit://127.0.0.1:8500), the control server's address (<control>), times (no seq, no t_ms, the
time between lines in steps of 100 ms), how fast it went (actual_speed 1.5, wall_ms 1000, paused_ms 500 when it
stood still) and lateness (0). A recording that still has a path of this computer, the user's name or the control
token in it is not written (the case fails with what was found).

--compare records into a temporary directory and compares with recordings/ (outline() and Shapes say exactly
what): the events in order (time, state, log and log_dropped aside: how many and where depends on timing; of the
participant events between two others, the order across participants is not compared, the order of one
participant's states is), the values the launcher decides (findings' codes, commands accepted or not, states and
exit codes, the result, the check, version's features and formats), the keys and JSON types of every kind of
event and of everything in it, the commands written, the exit code and whether anything came on standard error.
Cases that need a feature the launcher does not have (the control server) are skipped, and that feature is left
out of version's features on both sides; both are said. A case whose run fell behind real time (a busy machine)
is recorded again, up to three times; the summary line says which were. It says which case differs and how;
record again when the change is meant (then `git diff gui/server/tests/recordings` shows what changed).

When the launcher changes: record again, read the diff, and fix the GUI where it uses what changed. When a test
needs a command line or commands that no recording has, replay_launcher.py says so: add a case here and record it.
Linux only (it links the executables and writes shell scripts). Exit code 0, 1 when a case failed or
differs, 2 on a usage error.
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import re
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

HERE = Path(__file__).resolve().parent
CONFIGS = HERE / "configs"
RECORDINGS = HERE / "recordings"
FORMAT = 1
REGISTRY = "silkit://127.0.0.1:8500"
RUN = ["run", "--events", "--stdin-commands"]
LAUNCHER = "vehicle_sil_launch"
STOP = '{"command":"stop"}'
PAUSE, RESUME = '{"command":"pause"}', '{"command":"resume"}'
TIMEOUT_S = 60
# Findings that come from how busy the machine was, not from the launcher: a case that has one is recorded again
BUSY = ("behind-real-time",)  # a step later than its real time: only a busy machine does that to the example ECUs
ATTEMPTS = 3


class Busy(RuntimeError):
    pass


def step(ms: int) -> str:
    return json.dumps({"command": "step", "ms": ms}, separators=(",", ":"))


def speed(value) -> str:
    return '{"command":"speed","value":"max"}' if value == "max" else f'{{"command":"speed","value":{value}}}'


# ---------------------------------------------------------------- what the script waits for


def running_time(n: int) -> Callable[[list[dict]], bool]:
    """The n-th time event (from 1) after the phase running."""
    def seen(events: list[dict]) -> bool:
        started = False
        count = 0
        for e in events:
            if e.get("event") == "phase" and e.get("phase") == "running":
                started = True
            elif started and e.get("event") == "time":
                count += 1
        return count >= n
    return seen


def event(kind: str, nth: int = 1, **fields) -> Callable[[list[dict]], bool]:
    """The nth event of this kind with these fields."""
    def seen(events: list[dict]) -> bool:
        return sum(e.get("event") == kind and all(e.get(k) == v for k, v in fields.items()) for e in events) >= nth
    return seen


@dataclass
class Case:
    name: str
    argv: list[str]
    about: str
    # ("until", predicate), ("send", line), ("close",), ("start", participant): start a manual participant as its
    # event says, ("sleep", s), ("hang",): the launcher waits on (the recording ends here; it is killed)
    script: list[tuple] = field(default_factory=list)
    default: bool = False  # of the recordings of one command line, the one to play until a command says otherwise
    # A request file to give instead of the options of argv (which still name the case: replay_launcher.py turns
    # a --request back into such options). For what options cannot say: `check --start-paused` is refused as an
    # option, and a request file may have start_paused for check (the launcher's rules about it are checked there);
    # a network of the request file's connect that the configuration lacks is left out with a note, not an error.
    request: dict | None = None
    tools: bool = False  # run the launcher next to the bridges on the fake drivers (see the top of this file)
    own_network: bool = False  # in a network namespace of its own (unshare -rn): this computer's interfaces unseen


def run_case(name: str, file: str, about: str, *script, extra: tuple = (), default: bool = False) -> Case:
    return Case(name, [*RUN, *extra, "--", file], about, list(script), default)


def finite_run(case: Case, stop_ms: int) -> Case:
    """Give a formerly configuration-bounded run the same explicit run limit."""
    at = min((case.argv.index(option) for option in ("--start-paused", "--connect", "--")
              if option in case.argv), default=len(case.argv))
    case.argv[at:at] = ["--stop-at-ms", str(stop_ms)]
    if case.request is not None:
        case.request["stop_at_ms"] = stop_ms
    return case


CREDENTIALS = ("--control", "0", "--control-credentials", "<credentials>")


# ---------------------------------------------------------------- recording


class Recorder:
    def __init__(self, launcher: Path, examples: Path, root: Path, tools: tuple[Path, Path] | None = None,
                 compare: bool = False):
        self.launcher = launcher
        self.examples = examples
        self.root = root
        self.tools = tools  # (the directory of the launcher next to the bridges, the fake pcap library)
        self.compare = compare
        self.namespaces = shutil.which("unshare") is not None and subprocess.run(
            ["unshare", "-rn", "true"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        self.token = secrets.token_hex(16)

    def prepare(self) -> None:
        shutil.copytree(CONFIGS, self.root, dirs_exist_ok=True)
        for sub, exe in (("engine", "vehicle_sil_engine_ecu"), ("dashboard", "vehicle_sil_dashboard_ecu")):
            (self.root / sub).mkdir(exist_ok=True)
            os.symlink(self.examples / sub / exe, self.root / sub / exe)
        scripts = {
            "logger/can_logger": "# stands for a logger that does not describe itself\nexec sleep 100000\n",
            "logger/stubborn_logger": "# a logger that does not end when asked to (SIGTERM)\ntrap '' TERM\n"
                                      "while :; do sleep 1; done\n",
            "dashboard/failing_dashboard": (
                "# the example's dashboard, killed after 0.2 s, and exit code 3 (its check is the dashboard's)\n"
                'here=$(dirname "$0")\n'
                'case " $* " in *" --describe "*) exec "$here/vehicle_sil_dashboard_ecu" "$@";; esac\n'
                '"$here/vehicle_sil_dashboard_ecu" "$@" & pid=$!\n'
                "sleep 0.2\n"
                'echo "error: something went wrong, exiting with code 3"\n'
                'kill -9 $pid; wait $pid\nexit 3\n'),
        }
        for name, body in scripts.items():
            path = self.root / name
            path.parent.mkdir(exist_ok=True)
            path.write_text("#!/bin/sh\n" + body)
            path.chmod(0o755)
        credentials = self.root / ".control-credentials"
        credentials.write_text(f"operator gui {self.token}\n")
        credentials.chmod(0o600)

    def timeline_path(self, case: Case) -> Path:
        return self.root / ".timelines" / f"{case.name}.timeline.json"

    def record(self, case: Case) -> tuple[dict, list[dict]]:
        argv = [str(self.root / ".control-credentials") if a == "<credentials>" else
                str(self.timeline_path(case)) if a == "<timeline>" else a for a in case.argv]
        if case.request is not None:
            path = self.root / ".request.json"
            path.write_text(json.dumps({"vehicle_sil_run_request": 1, **case.request}), encoding="utf-8")
            # check: its --json; run: the options of RUN; the rest is in the request
            keep = argv.index("--json") + 1 if "--json" in argv else len(RUN)
            argv = [*argv[:keep], "--request", str(path), "--", argv[-1]]
        launcher, env = self.launcher, None
        if case.tools:
            assert self.tools is not None
            launcher = self.tools[0] / LAUNCHER
            env = {**os.environ, "VEHICLE_SIL_TEST_PCAP_LIBRARY": str(self.tools[1])}
        prefix = []
        if case.own_network and self.namespaces:
            prefix = ["unshare", "-rn"]
        elif case.own_network and not self.compare:
            raise RuntimeError(f"{case.name}: `unshare -rn true` fails here, so the list would show this computer's "
                               "interfaces: record it where unprivileged user namespaces can be made")
        proc = subprocess.Popen([*prefix, str(launcher), *argv], cwd=self.root, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True, env=env)
        lines: list[tuple[float, str, str]] = []  # (time, "out" | "err" | "send", text)
        events: list[dict] = []
        lock = threading.Condition()

        def read(stream, kind: str) -> None:
            for raw in stream:
                text = raw.decode("utf-8", "replace").rstrip("\r\n")
                with lock:
                    lines.append((time.monotonic(), kind, text))
                    if kind == "out":
                        try:
                            parsed = json.loads(text)
                            if isinstance(parsed, dict):
                                events.append(parsed)
                        except ValueError:
                            pass
                    lock.notify_all()

        readers = [threading.Thread(target=read, args=(proc.stdout, "out"), daemon=True),
                   threading.Thread(target=read, args=(proc.stderr, "err"), daemon=True)]
        for r in readers:
            r.start()
        manual: list[subprocess.Popen] = []
        try:
            for action in case.script:
                if action[0] == "until":
                    with lock:
                        if not lock.wait_for(lambda: action[1](events) or proc.poll() is not None, TIMEOUT_S):
                            raise RuntimeError(f"{case.name}: the launcher did not write what the script waits for "
                                               f"in {TIMEOUT_S} s")
                        if not action[1](events):
                            raise RuntimeError(f"{case.name}: the launcher ended before what the script waits for")
                elif action[0] == "send":
                    with lock:
                        lines.append((time.monotonic(), "send", action[1]))
                    proc.stdin.write(action[1].encode() + b"\n")
                    proc.stdin.flush()
                elif action[0] == "close":
                    with lock:
                        lines.append((time.monotonic(), "send", None))
                    proc.stdin.close()
                elif action[0] == "start":
                    with lock:
                        e = next(e for e in events if e.get("event") == "participant" and e.get("name") == action[1]
                                 and e.get("state") == "manual")
                    manual.append(subprocess.Popen(e["command"], cwd=e["working_directory"],
                                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                                   stderr=subprocess.DEVNULL))
                elif action[0] == "sleep":
                    time.sleep(action[1])
                elif action[0] == "hang":  # the launcher waits on: the recording ends here, and so does it
                    with lock:
                        lines.append((time.monotonic(), "hang", ""))
                    os.killpg(proc.pid, signal.SIGKILL)
            if not proc.stdin.closed and proc.poll() is None:
                proc.wait(TIMEOUT_S)  # the input stays open until the launcher ends, as the GUI server keeps it
            code = proc.wait(TIMEOUT_S)
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            for m in manual:
                m.kill()
                m.wait()
        for r in readers:
            r.join(5)
        timeline = None
        if "<timeline>" in case.argv and self.timeline_path(case).is_file():
            timeline = json.loads(self.timeline_path(case).read_text(encoding="utf-8"))
        return self.normalize(case, lines, code, timeline)

    # ---- the marks

    def normalize(self, case: Case, lines: list[tuple[float, str, str]], code: int,
                  timeline: object = None) -> tuple[dict, list[dict]]:
        file = case.argv[case.argv.index("--") + 1] if "--" in case.argv else None
        scenario = case.argv[case.argv.index("--scenario") + 1] if "--scenario" in case.argv else None
        registry = control = None
        pids = set()
        for _, kind, text in lines:
            if kind != "out":
                continue
            try:
                e = json.loads(text)
            except ValueError:
                continue
            if isinstance(e, dict):
                registry = e.get("uri") if e.get("event") == "registry" else registry
                control = e.get("address") if e.get("event") == "control_server" else control
                if isinstance(e.get("pid"), int):
                    pids.add(e["pid"])

        def text_marks(s: str) -> str:
            if "<timeline>" in case.argv:
                for path in sorted({str(self.timeline_path(case).resolve()), str(self.timeline_path(case))},
                                   key=len, reverse=True):
                    s = s.replace(path, "<timeline>")
            marked = {p: "<root>" for p in (str(self.root.resolve()), str(self.root), str(self.examples.resolve()),
                                             str(self.examples))}
            if self.tools is not None:
                marked |= {str(self.tools[0].resolve()): "<tools>", str(self.tools[0]): "<tools>"}
            for path in sorted(marked, key=len, reverse=True):
                s = s.replace(path, marked[path])
            if registry:
                s = s.replace(registry, REGISTRY)
            if control:
                s = s.replace(control, "<control>")
            if file and (s == file or s.startswith(file + ":")):
                s = "<file>" + s[len(file):]
            if scenario and (s == scenario or s.startswith(scenario + ":")):
                s = "<scenario>" + s[len(scenario):]
            return s.replace(self.token, "<control-token>")

        def marks(value, key=None):
            if isinstance(value, dict):
                out = {k: marks(v, k) for k, v in value.items()}
                if isinstance(out.get("actual_speed"), (int, float)) and not isinstance(out["actual_speed"], bool):
                    out["actual_speed"] = 1.5
                for k in ("late_ms", "late_steps", "max_late_ms", "slipped_ms"):
                    if isinstance(out.get(k), int):
                        out[k] = 0
                if isinstance(out.get("wall_ms"), int):
                    out["wall_ms"] = 1000
                if isinstance(out.get("paused_ms"), int) and out["paused_ms"] > 0:
                    out["paused_ms"] = 500
                return out
            if isinstance(value, list):
                return [marks(v) for v in value]
            if key == "pid" and isinstance(value, int):
                return "<pid>"
            if isinstance(value, str):
                return text_marks(value)
            return value

        if "--events" in case.argv:
            self.stream_rules(case, lines)
        entries: list[dict] = []
        out_text = [t for _, k, t in lines if k == "out"]
        printed = None
        if "--events" not in case.argv:
            try:
                printed = json.loads("\n".join(out_text))
            except ValueError:
                printed = None
        # The time between lines in steps of 100 ms, counted from the start (so that short gaps add up), and from
        # a command written (the time the recorder waited before it is not the launcher's)
        origin = lines[0][0] if lines else 0.0
        shown = 0
        for when, kind, text in lines:
            if kind == "send":
                entries.append({"send": text})
                origin, shown = when, 0
                continue
            if kind == "hang":
                entries.append({"hang": True})
                break
            if kind == "out" and printed is None:
                at = int((when - origin) * 10 + 0.5) * 100
                if at - shown >= 100:
                    entries.append({"gap_ms": at - shown})
                    shown = at
            if kind == "err":
                entries.append({"err": text_marks(text)})
            elif printed is not None:
                continue
            else:
                try:
                    e = json.loads(text)
                except ValueError:
                    entries.append({"text": text_marks(text)})
                    continue
                if not isinstance(e, dict) or "event" not in e:
                    entries.append({"text": text_marks(text)})
                    continue
                if e["event"] == "heartbeat":
                    continue
                found = [f.get("code") for f in [e] + (e.get("result") or {}).get("findings", [])
                         if isinstance(f, dict)]
                busy = [code for code in found if code in BUSY]
                if busy:
                    raise Busy(f"{case.name}: the run wrote {busy[0]} (the machine was busy, or the launcher's timing "
                               "has changed): record it again on a "
                               "machine that is not busy")
                e = {k: v for k, v in e.items() if k not in ("seq", "t_ms")}
                entries.append({"out": marks(e)})
        if printed is not None:
            entries.insert(0, {"print": marks(printed)})
        if timeline is not None:
            # Where the file was whole: before the first line that names it (the launcher writes the file, then
            # says so), else before the result
            def names_it(e: dict) -> bool:
                return "<timeline>" in json.dumps(e.get("out", e.get("err", "")), ensure_ascii=False) and \
                    e.get("out", {}).get("event") != "hello"
            at = next((i for i, e in enumerate(entries) if names_it(e) or e.get("out", {}).get("event") == "result"),
                      len(entries))
            entries.insert(at, {"timeline_file": marks(timeline)})
        if not entries or "hang" not in entries[-1]:
            entries.append({"exit": code})
        head = {"vehicle_sil_recording": FORMAT, "case": case.name, "about": case.about,
                "shape": shape_of(case.argv)}
        if case.default:
            head["default"] = True
        if file is not None:
            head["config"] = file
            head["config_sha256"] = text_sha256(self.root / file)
        if scenario is not None:
            head["scenario"] = scenario
            head["scenario_sha256"] = text_sha256(self.root / scenario)
        self.forbid(case, head, entries)
        return head, entries

    @staticmethod
    def stream_rules(case: Case, lines: list[tuple[float, str, str]]) -> None:
        """What docs/simulation-events.md promises about the event stream, on the real launcher's own lines (the
        recording keeps neither seq nor t_ms, and no heartbeat): every line an object that starts with event, seq,
        t_ms; seq from 0 up by one (heartbeats counted); t_ms never back; a heartbeat has nothing else."""
        events = []
        for _, kind, text in lines:
            if kind != "out":
                continue
            try:
                e = json.loads(text)
            except ValueError:
                raise RuntimeError(f"{case.name}: the launcher wrote a line that is not JSON: {text[:200]}") from None
            events.append(e)
        broken = []
        if not all(isinstance(e, dict) and list(e)[:3] == ["event", "seq", "t_ms"] for e in events):
            broken.append("every line starts with event, seq, t_ms")
        elif [e["seq"] for e in events] != list(range(len(events))):
            broken.append("seq goes up by one from 0")
        elif not all(a["t_ms"] <= b["t_ms"] for a, b in zip(events, events[1:])):
            broken.append("t_ms does not go back")
        elif any(e["event"] == "heartbeat" and list(e) != ["event", "seq", "t_ms"] for e in events):
            broken.append("a heartbeat has only event, seq and t_ms")
        if broken:
            raise RuntimeError(f"{case.name}: the real launcher's events break the rule '{broken[0]}' that the GUI "
                               "server relies on (docs/simulation-events.md): fix the launcher, or the GUI server and "
                               "replay_launcher.py with it")

    def forbid(self, case: Case, head: dict, entries: list[dict]) -> None:
        text = json.dumps([head, entries], ensure_ascii=False)
        found = [what for what, needle in (
            ("the control token", self.token), ("the home directory", str(Path.home())),
            ("the temporary directory", tempfile.gettempdir() + "/"), ("the user's name", getpass.getuser()),
            ("the directory of the executables", str(self.examples))) if needle and needle in text]
        if re.search(r"/home/|/Users/|/tmp/claude", text):
            found.append("a path of this computer")
        host = socket.gethostname().split(".")[0]
        if len(host) >= 3 and re.search(rf"\b{re.escape(host)}\b", text, re.IGNORECASE):
            found.append("the host name")
        if found:
            raise RuntimeError(f"{case.name}: the recording still has {', '.join(found)} in it: add a mark for it "
                               "in Recorder.normalize")


def shape_of(argv: list[str]) -> list[str]:
    import replay_launcher
    return replay_launcher.shape([a if a != "<credentials>" else "x" for a in argv])[0]


def text_sha256(path: Path) -> str:
    import replay_launcher
    return replay_launcher.text_sha256(str(path))


def needs(case: Case) -> set[str]:
    """The launcher's features (version --json) that a case uses."""
    out = set()
    if "--timeline" in case.argv:
        out.add("timeline")
    if "--control" in case.argv:
        out.add("control-server")
        if "--scenario" in case.argv:
            # Both: the recordings say what such a launcher writes (its write refusal names what a client can do).
            out.update(("scenario-control", "scenario-time"))
    return out


def launcher_features(launcher: Path) -> set[str]:
    p = subprocess.run([str(launcher), "version", "--json"], capture_output=True, text=True, timeout=TIMEOUT_S)
    return set(json.loads(p.stdout).get("features") or [])


def record_once(recorder: Recorder, case: Case, again: dict[str, int]) -> tuple[dict, list[dict]]:
    """A case, recorded again (up to ATTEMPTS times) while its run fell behind real time; again counts how often."""
    for attempt in range(1, ATTEMPTS + 1):
        try:
            return recorder.record(case)
        except Busy as e:
            if attempt == ATTEMPTS:
                raise RuntimeError(f"{case.name}: {ATTEMPTS} times {', '.join(BUSY)}: unless the machine is busy, the "
                                   "launcher's timing has changed") from e
            again[case.name] = attempt
    raise AssertionError("not reached")


def write(path: Path, head: dict, entries: list[dict]) -> None:
    path.write_text("\n".join(json.dumps(x, ensure_ascii=False) for x in [head, *entries]) + "\n", encoding="utf-8",
                    newline="\n")


# ---------------------------------------------------------------- comparing


NOT_IN_ORDER = {"time", "log", "log_dropped", "state"}
# The cases that stop twice at once: a participant that is not required (Logger) either ends on the first stop (SIGTERM)
# or is killed first; which one happens is a race, so both count as the same ending there. No other case is lenient.
EITHER_END = {"forever-stop-twice", "control-force", "control-paused-resume-force", "control-scenario-slow-force",
              "control-simulation-force"}
ENDS_AT_STOP = ("stopped", "killed")
SOMETIMES = {"log_dropped"}  # a kind of event that only a slow reader makes the launcher write  # how many and where depend on timing: their shapes are compared


def json_type(value) -> str:
    return {bool: "boolean", int: "number", float: "number", str: "string", list: "array", dict: "object",
            type(None): "null"}[type(value)]


def decided(e: dict, either_end: frozenset = frozenset()) -> tuple:
    """The values of an event that the launcher decides, not the timing (compared as they are). either_end: the
    participants whose ending by a stop or by force (killed, or ended by SIGTERM) is taken as the same."""
    kind = e.get("event")
    if kind == "finding":
        return e.get("severity"), e.get("code"), e.get("participant")
    if kind == "command":
        return e.get("command"), e.get("source"), e.get("accepted"), e.get("ms")
    if kind == "participant":
        if e.get("name") in either_end and e.get("state") in ENDS_AT_STOP:
            return e.get("name"), "ended at the stop", None, None, e.get("reason")
        return e.get("name"), e.get("state"), e.get("exit_code"), e.get("signal"), e.get("reason")
    if kind in ("phase", "control_link"):
        return e.get("phase"), e.get("name"), e.get("state")
    if kind == "scenario_step":
        return e.get("index"), e.get("status"), e.get("code")
    if kind == "check":
        return checked(e.get("check"))
    if kind == "result" and isinstance(e.get("result"), dict):
        r = e["result"]
        return (r.get("result"), r.get("simulation"), r.get("interrupted_by"),
                tuple(f.get("code") for f in r.get("findings") or []),
                tuple((p.get("name"), "ended at the stop", None, None)
                      if p.get("name") in either_end and p.get("ended") in ENDS_AT_STOP else
                      (p.get("name"), p.get("ended"), p.get("exit_code"), p.get("signal"))
                      for p in r.get("participants") or []), checked(r.get("check")),
                (r.get("scenario") or {}).get("status"))
    return ()


def checked(c) -> tuple:
    if not isinstance(c, dict):
        return ()
    return (c.get("result"), tuple((f.get("severity"), f.get("code"), f.get("participant"))
                                   for f in c.get("findings") or []),
            tuple((p.get("name"), p.get("status"), p.get("enabled"), p.get("manual"))
                  for p in c.get("participants") or []))


def outline(entries: list[dict], unsure: frozenset = frozenset(), either_end: bool = False) -> list:
    """What is compared in order: the events but time, state, log and log_dropped, with the values the launcher decides
    (participant events between two others in any order: that order is timing), what check --json and version
    --json print, the commands written, the exit code, and whether anything came on standard error.
    either_end: the end of a participant that is not required counts the same whether it was stopped or killed."""
    either = frozenset(
        p.get("name") for e in entries if "out" in e and e["out"].get("event") == "result"
        and isinstance(e["out"].get("result"), dict)
        for p in e["out"]["result"].get("participants") or [] if p.get("required") is False) if either_end \
        else frozenset()
    out: list = []
    group: list = []
    for e in entries:
        if "out" in e:
            kind = e["out"].get("event")
            if kind in NOT_IN_ORDER:
                continue
            item = ("event", kind, decided(e["out"], either))
            if kind == "participant":
                group.append(item)
                continue
            out += sorted(group, key=lambda g: str(g[2][0])) + [item]  # stable: one participant's order stays
            group = []
        elif "print" in e:
            p = e["print"]
            out.append(("print", checked(p) if "vehicle_sil_check" in p else
                        (tuple(f for f in p.get("features") or () if f not in unsure), p.get("formats"))
                        if "vehicle_sil_launch" in p else ()))
        elif "exit" in e:
            out.append(("exit", e["exit"]))
    out += sorted(group, key=lambda g: str(g[2][0]))
    out.append(("commands", tuple(e["send"] for e in entries if "send" in e)))
    written = next((e["timeline_file"] for e in entries if "timeline_file" in e), None)
    out.append(("timeline file", tuple(sorted(written)) if isinstance(written, dict) else None))
    out.append(("standard error", any("err" in e for e in entries)))
    return out


class Shapes:
    """The keys of each kind of event (participant: of each state) and of every object in it, all items of its
    arrays too, and the JSON types seen for each key (null aside: it comes and goes with the values)."""

    def __init__(self):
        self.keys: dict[str, set] = {}
        self.types: dict[tuple, set] = {}

    def add(self, entries: list[dict]) -> None:
        for e in entries:
            if "out" in e:
                kind = e["out"].get("event")
                self.walk(f"{kind}:{e['out'].get('state')}" if kind == "participant" else str(kind), e["out"])
            elif "print" in e:
                self.walk("check --json" if "vehicle_sil_check" in e["print"] else "version --json", e["print"])

    def walk(self, where: str, value) -> None:
        if isinstance(value, dict):
            self.keys.setdefault(where, set()).add(frozenset(value))
            for k, v in value.items():
                if v is not None:
                    self.types.setdefault((where, k), set()).add(json_type(v))
                self.walk(f"{where}.{k}", v)
        elif isinstance(value, list):
            for item in value:
                self.walk(f"{where}[]", item)

    def differences(self, other: "Shapes") -> list[str]:
        out = []
        for where in sorted(set(self.keys) | set(other.keys)):
            if where.split(".")[0] in SOMETIMES and (where not in self.keys or where not in other.keys):
                continue  # written only when the reader is slow: compared when both sides have it
            a, b = self.keys.get(where, set()), other.keys.get(where, set())
            if a != b:
                out.append(f"{where}: keys recorded {sorted(map(sorted, a - b))[:3]}, "
                           f"now {sorted(map(sorted, b - a))[:3]}")
        for key in sorted(set(self.types) | set(other.types), key=repr):
            if key[0].split(".")[0] in SOMETIMES and (key not in self.types or key not in other.types):
                continue
            a, b = self.types.get(key, set()), other.types.get(key, set())
            if a != b:
                out.append(f"{key[0]}.{key[1]}: types recorded {sorted(a)}, now {sorted(b)}")
        return out


def compare(name: str, tracked: Path, fresh: Path, shapes: tuple[Shapes, Shapes],
            unsure: frozenset = frozenset()) -> list[str]:
    """unsure: features of version --json left out on both sides (the ones the launcher was built without)."""
    import replay_launcher
    if not tracked.is_file():
        return [f"{name}: recordings/{name}.jsonl is missing: record it"]
    _, a = replay_launcher.read_recording(tracked)
    _, b = replay_launcher.read_recording(fresh)
    shapes[0].add(a)
    shapes[1].add(b)
    either = name in EITHER_END
    left, right = outline(a, unsure, either), outline(b, unsure, either)
    if left == right:
        return []
    at = next((i for i, (x, y) in enumerate(zip(left, right)) if x != y), min(len(left), len(right)))

    def show(seq):
        return str(seq[at])[:300] if at < len(seq) else "(the end)"
    return [f"{name}: differs from the real launcher at item {at + 1}: recorded {show(left)}, now {show(right)}: "
            f"record it again ({Path(__file__).name} --launcher <vehicle_sil_launch> {name}) and read the diff"]


# ---------------------------------------------------------------- the cases


def cases() -> list[Case]:
    return CASES


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="record_launcher.py", description=__doc__.split("\n\n")[0])
    parser.add_argument("--launcher", required=True, type=Path, help="a real vehicle_sil_launch")
    parser.add_argument("--examples", type=Path,
                        help="the ecu_main example's build directory (default: the one next to the launcher)")
    parser.add_argument("--out", type=Path, default=RECORDINGS, help="where to write (default: recordings/)")
    parser.add_argument("--compare", action="store_true", help="compare with recordings/ instead of writing")
    parser.add_argument("--connect-tools", type=Path,
                        help="the launcher next to the bridges on the fake drivers (default: tests/launch/connect_tools "
                             "of the launcher's build)")
    parser.add_argument("--fake-pcap", type=Path, help="the fake pcap library (default: "
                                                       "tests/eth_link/fake_pcap/vehicle_sil_fake_pcap.so of the "
                                                       "launcher's build)")
    parser.add_argument("cases", nargs="*", help="the cases (default: all)")
    args = parser.parse_args(argv)
    sys.path.insert(0, str(HERE))
    from real_configs import examples_dir, usable
    examples = args.examples or examples_dir(str(args.launcher))
    if not args.launcher.is_file() or not usable(examples):
        print(f"record_launcher.py: no launcher at {args.launcher} or no ecu_main example executables in "
              f"{examples}: build them, or give --examples", file=sys.stderr)
        return 2
    if sys.platform != "linux":
        print("record_launcher.py: Linux only", file=sys.stderr)
        return 2
    every = cases()
    unknown = [c for c in args.cases if c not in {x.name for x in every}]
    if unknown:
        print(f"record_launcher.py: no case {', '.join(unknown)}: the cases are in CASES of record_launcher.py",
              file=sys.stderr)
        return 2
    chosen = [c for c in every if not args.cases or c.name in args.cases]
    has = launcher_features(args.launcher)
    lacking = [c for c in chosen if not needs(c) <= has]
    missing = frozenset(set().union(*(needs(c) for c in lacking)) - has)
    if lacking:  # e.g. a launcher built without its control server (VEHICLE_SIL_BUILD_CONTROL_SERVER=OFF)
        print(f"record_launcher: {len(lacking)} cases skipped: they need {', '.join(sorted(missing))}, which this "
              f"launcher does not have; version --json is compared without {', '.join(sorted(missing))}",
              file=sys.stderr)
    chosen = [c for c in chosen if c not in lacking]
    build = args.launcher.resolve().parent.parent.parent
    tools = (args.connect_tools or build / "tests" / "launch" / "connect_tools",
             args.fake_pcap or build / "tests" / "eth_link" / "fake_pcap" / "vehicle_sil_fake_pcap.so")
    no_tools = [c for c in chosen if c.tools] if not ((tools[0] / LAUNCHER).is_file() and tools[1].is_file()) else []
    if no_tools:
        print(f"record_launcher: {len(no_tools)} cases skipped: they connect a network with the bridges on the fake "
              f"drivers, and {tools[0] / LAUNCHER} or {tools[1]} is missing: build the targets "
              "vehicle_sil_launch_connect_tools and vehicle_sil_fake_pcap, or give --connect-tools and --fake-pcap",
              file=sys.stderr)
    chosen = [c for c in chosen if c not in no_tools]
    again: dict[str, int] = {}
    failed = []
    with tempfile.TemporaryDirectory(prefix="record-launcher-") as tmp:
        root = Path(tmp) / "root"
        root.mkdir()
        recorder = Recorder(args.launcher.resolve(), examples.resolve(), root, None if no_tools else tools,
                            args.compare)
        recorder.prepare()
        out = Path(tmp) / "fresh" if args.compare else args.out
        shapes = (Shapes(), Shapes())
        out.mkdir(parents=True, exist_ok=True)
        for case in chosen:
            try:
                head, entries = record_once(recorder, case, again)
            except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
                failed.append(str(e) if str(e).startswith(case.name + ":") else f"{case.name}: {e}")
                continue
            write(out / f"{case.name}.jsonl", head, entries)
            if args.compare:
                failed += compare(case.name, RECORDINGS / f"{case.name}.jsonl", out / f"{case.name}.jsonl", shapes,
                                  missing)
        if args.compare:  # the shapes of every kind of event, over the cases compared (the same on both sides)
            failed += [f"shapes: {d}: record again and read the diff" for d in shapes[0].differences(shapes[1])]
    for line in failed:
        print(line, file=sys.stderr)
    cases_failed = {line.split(":", 1)[0] for line in failed if not line.startswith("shapes:")}
    shapes_differ = any(line.startswith("shapes:") for line in failed)
    print(f"record_launcher: {len(chosen) - len(cases_failed)} of {len(chosen)} cases "
          f"{'as recorded' if args.compare else 'recorded'}" + (f" (into {args.out})" if not args.compare else "")
          + ("; the shapes of the events differ" if shapes_differ else "")
          + (f"; {len(lacking)} skipped (no {', '.join(sorted(missing))})" if lacking else "")
          + (f"; {len(no_tools)} skipped (no connect tools)" if no_tools else "")
          + (f"; recorded again ({', '.join(BUSY)}): " + ", ".join(f"{n} {k}x" for n, k in sorted(again.items()))
             if again else ""))
    return 1 if failed else 0


def state_paused(events: list[dict], value: bool, **fields) -> bool:
    """The latest state event says paused `value` (and has these fields). The launcher writes the state that says
    so right after the time event that says where (the same look of the simulation); a command sent between the two
    is decided first, and the recording then has its events in another order than a reader that waits for the state
    would get."""
    states = [e for e in events if e.get("event") == "state"]
    return bool(states) and states[-1].get("paused") is value and all(states[-1].get(k) == v for k, v in fields.items())


def paused(sim_ms: int | None = None, **fields) -> Callable[[list[dict]], bool]:
    """The latest state says paused (and has these fields, such as speed) and where (paused_at_ms: at sim_ms, if
    given), and the time event of that time has come (the launcher's look writes it; the state may come first)."""
    def seen(events: list[dict]) -> bool:
        states = [e for e in events if e.get("event") == "state"]
        at = states[-1].get("paused_at_ms") if states else None
        return state_paused(events, True, **fields) and isinstance(at, int) and (sim_ms is None or at == sim_ms) and \
            any(e.get("event") == "time" and e.get("sim_ms") == at for e in events)
    return seen


def going() -> Callable[[list[dict]], bool]:
    """The simulation goes on and has said so: the latest state is not paused, and a time event of a later time
    than the one it last stood at has come (a recording of a resume has the time moving in it)."""
    def seen(events: list[dict]) -> bool:
        stood = [e["paused_at_ms"] for e in events
                 if e.get("event") == "state" and isinstance(e.get("paused_at_ms"), int)]
        after = stood[-1] if stood else -1
        return state_paused(events, False) and any(
            e.get("event") == "time" and isinstance(e.get("sim_ms"), int) and e["sim_ms"] > after for e in events)
    return seen


def log(text: str) -> Callable[[list[dict]], bool]:
    def seen(events: list[dict]) -> bool:
        return any(e.get("event") == "log" and text in str(e.get("line")) for e in events)
    return seen


def command(name: str, n: int = 1) -> Callable[[list[dict]], bool]:
    return event("command", n, command=name)


RUNNING = ("until", event("phase", phase="running"))
STOPPED = ("until", event("phase", phase="ended"))
STANDS = ("until", log("standing still before the first step"))  # --start-paused with a control server


def checks() -> list[Case]:
    files = ["simulation", "forever", "bad-lines", "bad-types", "check-error", "check-garbage", "control-trouble",
             "dash-name", "deaf", "finding-flood", "flood", "future-events", "ignore-stop", "jitter", "large-fields",
             "large-result", "late", "launcher-crash", "log-dropped", "loose-fields", "manual", "orphan",
             "participant-crash", "result-then-wait", "network"]
    out = [Case(f"check-{f}", ["check", "--json", "--", f"{f}.yaml"], f"the check of {f}.yaml") for f in files]
    return out + [
        Case("check-simulation-disable", ["check", "--json", "--disable", "Dashboard", "--", "simulation.yaml"],
             "the check with a participant left out"),
        Case("check-simulation-manual", ["check", "--json", "--manual", "Dashboard", "--", "simulation.yaml"],
             "the check with a participant that the user starts"),
        Case("check-simulation-scenario", ["check", "--json", "--scenario", "scenarios/throttle.yaml", "--",
                                           "simulation.yaml"], "the check with a scenario"),
        Case("check-simulation-no-logger", ["check", "--json", "--disable", "Logger", "--", "simulation.yaml"],
             "the check without Logger"),
        Case("check-simulation-scenario-fails", ["check", "--json", "--scenario", "scenarios/fails.yaml", "--",
                                                 "simulation.yaml"], "the check with a scenario that fails"),
        Case("check-forever-disable", ["check", "--json", "--disable", "Dashboard", "--", "forever.yaml"],
             "the check of forever.yaml with a participant left out"),
        Case("check-forever-no-logger", ["check", "--json", "--disable", "Logger", "--", "forever.yaml"],
             "the check of forever.yaml without Logger"),
        Case("check-forever-disable-both", ["check", "--json", "--disable", "Dashboard", "--disable", "Logger", "--",
                                            "forever.yaml"], "the check of forever.yaml with two participants left out"),
        Case("check-forever-scenario", ["check", "--json", "--scenario", "scenarios/throttle.yaml", "--",
                                        "forever.yaml"], "the check of forever.yaml with a scenario"),
        Case("check-forever-scenario-no-logger", ["check", "--json", "--scenario", "scenarios/throttle.yaml",
                                                  "--disable", "Logger", "--", "forever.yaml"],
             "the check of forever.yaml with a scenario, without Logger"),
        Case("check-simulation-scenario-paused", ["check", "--json", "--scenario", "scenarios/throttle.yaml",
                                                  "--start-paused", "--", "simulation.yaml"],
             "the check of a request file with a scenario and start_paused (the launcher refuses the two together)",
             request={"scenario": "scenarios/throttle.yaml", "start_paused": True}),
        Case("check-variants", ["check", "--json", "--", "variants.yaml"],
             "the check of a file with variants, none chosen (the launcher says to choose one)"),
        Case("check-variants-small", ["check", "--json", "--variant", "small", "--", "variants.yaml"],
             "the check of a variant"),
        Case("check-variants-quiet", ["check", "--json", "--variant", "quiet", "--", "variants.yaml"],
             "the check of a variant that leaves Logger out"),
    ]


def runs() -> list[Case]:
    to_the_end = ["simulation", "bad-lines", "bad-types", "check-error", "control-trouble", "future-events",
                  "large-fields", "large-result", "launcher-crash", "log-dropped", "loose-fields", "orphan",
                  "finding-flood", "flood", "result-then-wait", "participant-crash"]
    out = [run_case(f"run-{f}", f"{f}.yaml", f"a run of {f}.yaml to its end, nothing written to the launcher")
           for f in to_the_end]
    stop = ("send", STOP)
    out += [
        run_case("run-manual", "manual.yaml", "Dashboard is started by hand when the launcher says so",
                 ("until", event("participant", state="manual", name="Dashboard")), ("sleep", 0.3),
                 ("start", "Dashboard")),
        run_case("run-variants-small", "variants.yaml", "a run of a variant", extra=("--variant", "small")),
        run_case("run-simulation-disable", "simulation.yaml", "a run without Logger", extra=("--disable", "Logger")),
        run_case("run-simulation-speed", "simulation.yaml", "a run without Logger at speed 2",
                 extra=("--disable", "Logger", "--speed", "2")),
        run_case("run-scenario-fails", "simulation.yaml", "a scenario whose second step fails",
                 extra=("--scenario", "scenarios/fails.yaml")),
        run_case("run-scenario-passes", "simulation.yaml", "a scenario that passes",
                 extra=("--scenario", "scenarios/throttle.yaml")),
        # As the GUI server starts a scenario run with a launcher that has the feature timeline
        run_case("run-scenario-fails-timeline", "simulation.yaml",
                 "a scenario whose second step fails, with its timeline file",
                 extra=("--scenario", "scenarios/fails.yaml", "--timeline", "<timeline>")),
        run_case("run-scenario-passes-timeline", "simulation.yaml", "a scenario that passes, with its timeline file",
                 extra=("--scenario", "scenarios/throttle.yaml", "--timeline", "<timeline>")),
        run_case("run-simulation-paused", "simulation.yaml", "started paused, then resumed",
                 ("until", paused(0)), ("send", RESUME), extra=("--start-paused",)),
        run_case("run-manual-stop", "simulation.yaml", "a participant that the user starts never joins: stopped",
                 ("until", log("to join (manual)")), ("sleep", 0.3), stop, extra=("--manual", "Dashboard")),
        run_case("run-network-manual-stop", "network.yaml",
                 "the registry on all addresses: Dashboard, to be started by the user with "
                 "VEHICLE_SIL_LISTEN_ON_NETWORK=1, never joins: stopped", ("until", log("to join (manual)")), ("sleep", 0.3), stop),
        run_case("forever-stop", "forever.yaml", "stopped while it runs", RUNNING, ("until", running_time(5)), stop,
                 default=True),
        run_case("forever-stop-twice", "forever.yaml", "two stops at once", RUNNING, ("until", running_time(5)),
                 stop, stop),
        run_case("forever-end-of-input", "forever.yaml", "the end of standard input while it runs", RUNNING,
                 ("until", running_time(5)), ("close",)),
        run_case("forever-step-refused", "forever.yaml", "a step while it runs is refused, then a stop", RUNNING,
                 ("until", running_time(3)), ("send", step(10 ** 12)), ("until", command("step")), stop),
        run_case("forever-force", "forever.yaml", "two stops at once, without Logger", RUNNING,
                 ("until", running_time(5)), stop, stop, extra=("--disable", "Logger")),
        run_case("deaf", "deaf.yaml", "stopped while it runs (the fault deaf never reads the stop)", RUNNING,
                 ("until", running_time(5)), stop),
        run_case("jitter", "jitter.yaml", "stopped while it runs at speed 1", RUNNING, ("until", running_time(13)),
                 stop),
        run_case("late", "late.yaml", "stopped while it runs at speed 1", RUNNING, ("until", running_time(13)), stop),
        run_case("ignore-stop-twice", "ignore-stop.yaml", "Logger does not end when asked: the second stop kills it",
                 RUNNING, ("until", running_time(5)), stop, ("until", event("participant", state="stop_requested",
                                                                           name="Logger")), ("sleep", 0.5), stop,
                 default=True),
        run_case("ignore-stop-end-of-input", "ignore-stop.yaml",
                 "the end of standard input: the launcher waits for Logger (stop_timeout_ms 600000)",
                 RUNNING, ("until", running_time(5)), ("close",),
                 ("until", event("participant", state="stop_requested", name="Logger")), ("sleep", 2), ("hang",)),
    ]
    out += [
        run_case("forever-step-10", "forever.yaml", "a step of 10 ms while it runs is refused, then a stop", RUNNING,
                 ("until", running_time(3)), ("send", step(10)), ("until", command("step")), stop),
        run_case("run-simulation-no-dashboard", "simulation.yaml", "a run without Dashboard",
                 extra=("--disable", "Dashboard")),
        run_case("forever-no-logger-stop", "forever.yaml", "without Logger, stopped while it runs", RUNNING,
                 ("until", running_time(3)), stop, extra=("--disable", "Logger")),
        run_case("forever-no-logger-paused-stop", "forever.yaml", "without Logger, started paused, stopped",
                 ("until", paused(0)), ("sleep", 0.3), stop, extra=("--disable", "Logger", "--start-paused")),
        run_case("forever-paused-stop", "forever.yaml", "started paused, stopped", ("until", paused(0)),
                 ("sleep", 0.3), stop, extra=("--start-paused",)),
        run_case("forever-paused-commands", "forever.yaml",
                 "started paused: step 25, resume, pause, speed 2, stop", ("until", paused(0)), ("send", step(25)),
                 ("until", paused(25)), ("send", RESUME), ("until", going()), ("send", PAUSE),
                 ("until", lambda events: command("pause")(events) and paused()(events)),
                 ("send", speed(2)), ("until", paused(speed=2)), stop, extra=("--start-paused",)),
    ]
    paused_at_start = ("--disable", "Logger", "--speed", "max", "--start-paused")
    for value, small in ((2, 10), (1, 100)):
        out.append(run_case(
            f"time-commands-speed-{value}", "forever.yaml",
            f"started paused: step 25, speed {value}, resume, step {small} (refused while it runs), pause, stop",
            ("until", paused(0)), ("send", step(25)), ("until", paused(25)), ("send", speed(value)),
            ("until", paused(speed=value)), ("send", RESUME), ("until", going()),
            ("send", step(small)), ("until", command("step", 2)), ("send", PAUSE),
            ("until", lambda events: command("pause")(events) and paused()(events)), stop,
            extra=paused_at_start))
    control = CREDENTIALS
    out += [
        run_case("control-paused-stop", "forever.yaml", "with the control server, started paused, stopped",
                 STANDS, ("sleep", 0.3), stop, extra=(*control, "--start-paused")),
        run_case("control-paused-resume", "forever.yaml", "with the control server, started paused, resumed, stopped",
                 STANDS, ("send", RESUME), ("until", running_time(3)), stop, extra=(*control, "--start-paused")),
        run_case("control-paused-step-200", "forever.yaml", "with the control server: a step of 200 ms, stopped",
                 STANDS, ("send", step(200)), ("until", paused(200)), stop, extra=(*control, "--start-paused")),
        run_case("control-paused-step-100", "forever.yaml", "with the control server: a step of 100 ms, stopped",
                 STANDS, ("send", step(100)), ("until", paused(100)), stop, extra=(*control, "--start-paused")),
        run_case("control-paused-steps", "forever.yaml", "with the control server: two steps of 100 ms, stopped",
                 STANDS, ("send", step(100)), ("until", paused(100)), ("send", step(100)), ("until", paused(200)),
                 stop, extra=(*control, "--start-paused")),
        run_case("control-no-logger-step-200", "forever.yaml",
                 "with the control server, without Logger: a step of 200 ms, stopped", STANDS, ("send", step(200)),
                 ("until", paused(200)), stop, extra=(*control, "--disable", "Logger", "--start-paused")),
        run_case("control-stop", "forever.yaml", "with the control server, stopped while it runs", RUNNING,
                 ("until", running_time(3)), stop, extra=control),
        run_case("control-no-logger-stop", "forever.yaml", "with the control server, without Logger, stopped",
                 RUNNING, ("until", running_time(3)), stop, extra=(*control, "--disable", "Logger")),
        run_case("control-max-engine", "forever.yaml",
                 "with the control server at speed max, Engine alone, for 2.5 s of time events, stopped", RUNNING,
                 ("until", running_time(25)), stop,
                 extra=(*control, "--disable", "Dashboard", "--disable", "Logger", "--speed", "max")),
        run_case("control-max-stop", "forever.yaml", "with the control server at speed max, stopped", RUNNING,
                 ("until", running_time(3)), stop, extra=(*control, "--speed", "max")),
        run_case("control-force", "forever.yaml", "with the control server, two stops at once", RUNNING,
                 ("until", running_time(5)), stop, stop, extra=control),
        run_case("control-paused-resume-force", "forever.yaml",
                 "with the control server, started paused, resumed, two stops at once", STANDS, ("send", RESUME),
                 ("until", running_time(5)), stop, stop, extra=(*control, "--start-paused")),
        run_case("control-simulation-resume", "simulation.yaml",
                 "with the control server, started paused, resumed, to the end", STANDS, ("send", RESUME),
                 extra=(*control, "--start-paused")),
        run_case("control-simulation-step-200", "simulation.yaml",
                 "with the control server, started paused: a step of 200 ms, then resumed to the end (the example "
                 "client variables.mjs)", STANDS, ("send", step(200)), ("until", paused(200)), ("send", RESUME),
                 extra=(*control, "--start-paused")),
        run_case("control-simulation-step-500", "simulation.yaml",
                 "with the control server, started paused: a step of 500 ms, then resumed to the end", STANDS,
                 ("send", step(500)), ("until", paused(500)), ("send", RESUME), extra=(*control, "--start-paused")),
        run_case("control-simulation-stop-finite", "simulation.yaml",
                 "with the control server, a finite paused run force-stopped before the first step", STANDS,
                 ("sleep", 0.3), stop, stop, extra=(*control, "--stop-at-ms", "1000", "--start-paused")),
        run_case("control-simulation-stop", "simulation.yaml",
                 "with the control server, started paused, stopped before the first step", STANDS, ("sleep", 0.3),
                 stop, extra=(*control, "--start-paused")),
        run_case("control-scenario-slow", "forever.yaml",
                 "a scenario at speed 0.001 with the control server (only to watch): its second step runs until a stop",
                 ("until", event("scenario_step", index=2, status="running")), ("sleep", 0.3), stop,
                 extra=("--scenario", "scenarios/throttle.yaml", *control, "--speed", "0.001"), default=True),
        run_case("control-scenario-slow-force", "forever.yaml",
                 "a scenario at speed 0.001 with the control server: two stops at once",
                 ("until", event("scenario_step", index=2, status="running")), ("sleep", 0.3), stop, stop,
                 extra=("--scenario", "scenarios/throttle.yaml", *control, "--speed", "0.001")),
        run_case("control-scenario-slow-no-logger", "forever.yaml",
                 "a scenario at speed 0.001 with the control server, without Logger: stopped",
                 ("until", event("scenario_step", index=2, status="running")), ("sleep", 0.3), stop,
                 extra=("--scenario", "scenarios/throttle.yaml", *control, "--disable", "Logger", "--speed", "0.001")),
        run_case("control-scenario-slow-time", "forever.yaml",
                 "a scenario at speed 0.001 with the control server: the time commands of standard input still work "
                 "(pause, a step of 10 ms, speed 0.002, resume), then a stop",
                 ("until", event("scenario_step", index=2, status="running")), ("send", PAUSE),
                 ("until", lambda events: command("pause")(events) and paused()(events)),
                 ("send", step(10)), ("until", command("step")), ("send", speed(0.002)),
                 ("until", command("speed")), ("send", RESUME), ("until", command("resume")), stop,
                 extra=("--scenario", "scenarios/throttle.yaml", *control, "--speed", "0.001")),
        run_case("control-scenario-passes", "simulation.yaml", "a scenario that passes, with the control server",
                 extra=("--scenario", "scenarios/throttle.yaml", *control)),
        run_case("control-scenario-fails", "simulation.yaml", "a scenario that fails, with the control server",
                 extra=("--scenario", "scenarios/fails.yaml", *control)),
        run_case("control-simulation-force", "simulation.yaml",
                 "with the control server, started paused, stopped without waiting before the first step (two stops "
                 "at once: the example client variables.mjs when a write is refused)", STANDS, ("sleep", 0.3), stop,
                 stop, extra=(*control, "--start-paused")),
        run_case("control-simulation-paused", "simulation.yaml",
                 "with the control server, started paused: a pause (it stands already), a resume, to the end",
                 STANDS, ("send", PAUSE), ("until", command("pause")), ("send", RESUME),
                 extra=(*control, "--start-paused")),
    ]
    finite = {f"run-{name}" for name in to_the_end if name != "participant-crash"} | {
        "run-manual", "run-variants-small", "run-simulation-disable", "run-simulation-speed",
        "run-simulation-paused", "run-simulation-no-dashboard", "control-simulation-resume",
        "control-simulation-step-200", "control-simulation-step-500", "control-simulation-paused",
    }
    for case in out:
        if case.name in finite:
            finite_run(case, 300 if case.name == "run-result-then-wait" else 1000)
    return out


def connects() -> list[Case]:
    """Connecting the Ethernet network of connect.yaml to the fake pcap library's adapter fake0, as the GUI server
    asks: the list of that one network, the checks (nothing connected: the one that names the networks; none;
    connected; connected with the time policy free) and a run with it connected, to its end; a network that the
    file lacks (listed: a usage error; in a request file's connect: a note); a file with variants."""
    target = ("--connect", "ETH_EXT=fake0")
    unused = ("--connect", "CAN1=physical:1")
    out = [
        Case("connections-connect", ["connections", "--json", "--network", "ETH_EXT", "--", "connect.yaml"],
             "where ETH_EXT can be connected (packet: the fake pcap library's adapters; tap: none but lo)",
             tools=True, own_network=True),
        Case("check-connect-none", ["check", "--json", "--", "connect.yaml"], "the check of connect.yaml, nothing "
             "connected"),
        Case("check-connect", ["check", "--json", *target, "--", "connect.yaml"],
             "the check with ETH_EXT connected to fake0: the time is locked to real time", tools=True),
        Case("check-connect-stop", ["check", "--json", "--stop-at-ms", "1000", *target, "--", "connect.yaml"],
             "the finite check with ETH_EXT connected to fake0", tools=True),
        Case("check-connect-sil-kit-only", ["check", "--json", "--connect", "ETH_EXT=none", "--", "connect.yaml"],
             "the check with ETH_EXT kept on SIL Kit only (none)"),
        Case("check-connect-free", ["check", "--json", "--time-policy", "free", *target, "--", "connect.yaml"],
             "the check with ETH_EXT connected to fake0 and the time policy free: the time is not locked", tools=True),
        Case("run-connect", [*RUN, *target, "--", "connect.yaml"],
             "a run with ETH_EXT connected to fake0, at real time, to its end", tools=True),
        Case("check-connect-can", ["check", "--json", "--connect", "J1939_CAN=fake0", "--", "connect.yaml"],
             "the check with J1939_CAN connected to fake0 (on Linux the SocketCAN bridge, which is not among the "
             "tools)", tools=True),
        Case("connections-unknown-network", ["connections", "--json", "--network", "NOPE", "--", "connect.yaml"],
             "the list of a network that connect.yaml lacks: a usage error"),
        Case("connections-variants", ["connections", "--json", "--network", "J1939_CAN", "--", "variants.yaml"],
             "where J1939_CAN of a file with variants can be connected (read without a variant; on Linux the "
             "SocketCAN bridge, which is not among the tools)", tools=True),
        Case("check-connect-unused", ["check", "--json", *unused, "--", "connect.yaml"],
             "the check of a request whose connect has a network that connect.yaml lacks: the note "
             "connect-network-unused", request={"connect": {"CAN1": "physical:1"}}),
        Case("run-connect-unused", [*RUN, *unused, "--", "connect.yaml"],
             "a run of a request whose connect has a network that connect.yaml lacks, to its end",
             request={"connect": {"CAN1": "physical:1"}}),
    ]
    for case in out:
        if case.name in {"run-connect", "run-connect-unused"}:
            finite_run(case, 1000)
    return out


def gui_timing_cases() -> list[Case]:
    """The standard screen starts at speed 1 and supplies its own finite run limit."""
    timed = ("--speed", "1", "--stop-at-ms", "1000")
    target = ("--connect", "ETH_EXT=fake0")
    paused = {"speed": 1, "stop_at_ms": 1000, "start_paused": True}
    return [
        Case("check-simulation-speed1", ["check", "--json", "--speed", "1", "--", "simulation.yaml"],
             "the standard GUI checks simulation.yaml at its initial speed"),
        Case("check-simulation-speed1-stop", ["check", "--json", *timed, "--", "simulation.yaml"],
             "the standard GUI checks its finite simulation run"),
        Case("check-simulation-speed1-stop-paused", ["check", "--json", *timed, "--start-paused", "--", "simulation.yaml"],
             "the standard GUI checks a finite run started paused", request=paused),
        Case("check-simulation-disable-speed1-stop",
             ["check", "--json", "--disable", "Dashboard", *timed, "--", "simulation.yaml"],
             "the standard GUI checks a finite run without Dashboard"),
        Case("check-simulation-no-logger-speed2-stop",
             ["check", "--json", "--disable", "Logger", "--speed", "2", "--stop-at-ms", "1000", "--", "simulation.yaml"],
             "a finite run without Logger, checked at speed 2"),
        run_case("run-simulation-speed1-stop", "simulation.yaml", "the standard GUI runs to the requested stop",
                 extra=timed),
        run_case("run-simulation-disable-speed1-stop", "simulation.yaml",
                 "the standard GUI runs without Dashboard to the requested stop",
                 extra=("--disable", "Dashboard", *timed)),
        run_case("control-simulation-speed1-stop-resume", "simulation.yaml",
                 "the standard GUI resumes a finite run started paused", STANDS, ("send", RESUME),
                 extra=(*CREDENTIALS, *timed, "--start-paused")),
        Case("check-manual-speed1", ["check", "--json", "--speed", "1", "--", "manual.yaml"],
             "the standard GUI checks manual.yaml at its initial speed"),
        Case("check-manual-speed1-stop", ["check", "--json", *timed, "--", "manual.yaml"],
             "the standard GUI checks a finite run with a manually started participant"),
        run_case("run-manual-speed1-stop", "manual.yaml", "Dashboard joins a finite run by hand",
                 ("until", event("participant", state="manual", name="Dashboard")), ("sleep", 0.3),
                 ("start", "Dashboard"), extra=timed),
        Case("check-connect-speed1", ["check", "--json", "--speed", "1", "--", "connect.yaml"],
             "the standard GUI checks connect.yaml at its initial speed"),
        Case("check-forever-max", ["check", "--json", "--speed", "max", "--", "forever.yaml"],
             "the Workbench checks an unbounded run at maximum speed"),
        Case("check-connect-speed1-stop", ["check", "--json", *timed, "--", "connect.yaml"],
             "the standard GUI checks its finite connected run"),
        Case("check-connect-speed1-stop-connected", ["check", "--json", *timed, *target, "--", "connect.yaml"],
             "the standard GUI checks its finite run with ETH_EXT connected", tools=True),
        Case("run-connect-speed1-stop", [*RUN, *timed, *target, "--", "connect.yaml"],
             "the standard GUI runs ETH_EXT connected at speed 1 until its requested stop", tools=True),
    ]


def gui_speed_checks() -> list[Case]:
    """Check the same configurations at the standard screen's initial speed."""
    out = []
    for case in checks():
        # The simulation case is listed explicitly above. start_paused has to be sent in a
        # request, and is recorded separately where the screen uses it.
        if case.name in {"check-simulation", "check-manual"} or case.request is not None or "--start-paused" in case.argv:
            continue
        split = case.argv.index("--")
        out.append(Case(case.name + "-speed1", [*case.argv[:split], "--speed", "1", *case.argv[split:]],
                        case.about + ", at the standard GUI's initial speed", tools=case.tools))
    return out


def finite_api_cases() -> list[Case]:
    """Explicit stop times for server tests that previously used the configuration's limit."""
    names = {
        "check-simulation", "check-simulation-no-logger", "check-variants-small",
        "check-flood", "check-finding-flood", "check-launcher-crash", "check-bad-lines", "check-bad-types",
        "check-future-events", "check-control-trouble", "check-large-fields", "check-large-result",
        "check-log-dropped", "check-loose-fields", "check-manual", "check-orphan",
        "check-result-then-wait", "run-simulation", "run-simulation-disable", "run-simulation-speed",
        "run-variants-small", "run-flood", "run-finding-flood", "run-launcher-crash", "run-bad-lines", "run-bad-types",
        "run-future-events", "run-control-trouble", "run-large-fields", "run-large-result",
        "run-log-dropped", "run-loose-fields", "run-manual", "run-orphan", "run-result-then-wait",
    }
    out = []
    for case in [*checks(), *runs()]:
        if case.name not in names:
            continue
        split = case.argv.index("--")
        # This configuration previously stopped at 300 ms; the others stopped at 1000 ms.
        stop = "300" if case.argv[-1] == "result-then-wait.yaml" else "1000"
        argv = case.argv if "--stop-at-ms" in case.argv else [*case.argv[:split], "--stop-at-ms", stop, *case.argv[split:]]
        out.append(Case(case.name + "-finite", list(argv),
                        case.about + ", with an explicit stop time", list(case.script), case.default,
                        tools=case.tools))
    return out


def gui_finite_cases() -> list[Case]:
    """The finite fixtures also used through the standard screen at speed 1."""
    names = {"check-control-trouble-finite", "run-control-trouble-finite", "check-flood-finite", "run-flood-finite",
             "check-result-then-wait-finite", "run-result-then-wait-finite"}
    out = []
    for case in finite_api_cases():
        if case.name not in names:
            continue
        at = case.argv.index("--stop-at-ms")
        out.append(Case(case.name + "-speed1", [*case.argv[:at], "--speed", "1", *case.argv[at:]],
                        case.about + ", at the standard GUI's initial speed", list(case.script), case.default,
                        tools=case.tools))
    return out


def gui_speed_runs() -> list[Case]:
    """Interactive unbounded runs at the standard screen's initial speed."""
    names = {"forever-stop", "forever-stop-twice", "forever-end-of-input", "forever-step-refused",
             "forever-force", "forever-step-10", "forever-no-logger-stop", "forever-no-logger-paused-stop",
             "forever-paused-stop", "ignore-stop-twice", "ignore-stop-end-of-input", "run-manual-stop",
             "run-network-manual-stop", "run-participant-crash", "run-scenario-fails", "run-scenario-passes",
             "run-scenario-fails-timeline", "run-scenario-passes-timeline", "control-scenario-fails",
             "control-scenario-passes", "control-paused-resume", "control-paused-stop", "control-stop", "deaf", "jitter", "late"}
    out = []
    for case in runs():
        if case.name not in names:
            continue
        at = case.argv.index("--start-paused") if "--start-paused" in case.argv else case.argv.index("--")
        out.append(Case(case.name + "-speed1", [*case.argv[:at], "--speed", "1", *case.argv[at:]],
                        case.about + ", at the standard GUI's initial speed", list(case.script), case.default,
                        tools=case.tools))
    out.append(run_case(
        "forever-paused-commands-speed1", "forever.yaml",
        "at speed 1: started paused, step 25, resume, pause, speed 2, stop",
        ("until", paused(0)), ("send", step(25)), ("until", paused(25)), ("send", RESUME),
        ("until", going()), ("send", PAUSE),
        ("until", lambda events: command("pause")(events) and paused()(events)),
        ("send", speed(2)), ("until", paused(speed=2)), ("send", STOP),
        extra=("--speed", "1", "--start-paused")))
    out.append(run_case("run-launcher-crash-speed1", "launcher-crash.yaml",
                        "at speed 1, the replay fault ends the launcher while it runs",
                        RUNNING, ("hang",), extra=("--speed", "1")))
    for case in checks():
        if case.name not in {"check-forever", "check-forever-no-logger"}:
            continue
        at = case.argv.index("--")
        disabled = [case.argv[i + 1] for i, arg in enumerate(case.argv[:-1]) if arg == "--disable"]
        request = {"speed": 1, "start_paused": True}
        if disabled:
            request["disable"] = disabled
        out.append(Case(case.name + "-speed1-paused",
                        [*case.argv[:at], "--speed", "1", "--start-paused", *case.argv[at:]],
                        case.about + ", at speed 1 and started paused", request=request))
    return out


def paused_api_checks() -> list[Case]:
    """Check's paused intent is part of the request, even without a scenario."""
    out = []
    for case in checks():
        if case.name not in {"check-simulation", "check-forever", "check-forever-no-logger"}:
            continue
        at = case.argv.index("--")
        disabled = [case.argv[i + 1] for i, arg in enumerate(case.argv[:-1]) if arg == "--disable"]
        request = {"start_paused": True}
        if disabled:
            request["disable"] = disabled
        out.append(Case(case.name + "-paused", [*case.argv[:at], "--start-paused", *case.argv[at:]],
                        case.about + ", with a paused start", request=request))
    out.extend([
        Case("check-simulation-paused-finite", ["check", "--json", "--stop-at-ms", "1000", "--start-paused", "--", "simulation.yaml"],
             "a finite simulation checked with a paused start", request={"stop_at_ms": 1000, "start_paused": True}),
        Case("check-simulation-no-dashboard-finite", ["check", "--json", "--disable", "Dashboard", "--stop-at-ms", "1000", "--", "simulation.yaml"],
             "a finite simulation checked without Dashboard"),
        Case("check-connect-unused-finite", ["check", "--json", "--stop-at-ms", "1000", "--connect", "CAN1=physical:1", "--", "connect.yaml"],
             "a finite check with an unused connection", request={"stop_at_ms": 1000, "connect": {"CAN1": "physical:1"}}),
        Case("check-forever-scenario-slow", ["check", "--json", "--scenario", "scenarios/throttle.yaml", "--speed", "0.001", "--", "forever.yaml"],
             "a slow scenario checked before a control run"),
        Case("check-forever-disable-both-max", ["check", "--json", "--disable", "Dashboard", "--disable", "Logger", "--speed", "max", "--", "forever.yaml"],
             "a maximum-speed check with Engine alone"),
        Case("check-forever-no-logger-max-paused",
             ["check", "--json", "--disable", "Logger", "--speed", "max", "--start-paused", "--", "forever.yaml"],
             "a paused run without Logger, checked at maximum speed",
             request={"disable": ["Logger"], "speed": "max", "start_paused": True}),
        Case("check-forever-scenario-no-logger-slow",
             ["check", "--json", "--scenario", "scenarios/throttle.yaml", "--disable", "Logger",
              "--speed", "0.001", "--", "forever.yaml"],
             "a slow scenario without Logger"),
    ])
    return out


CASES: list[Case] = [Case("version", ["version", "--json"], "version --json"), *checks(), *runs(), *connects(),
                     *gui_timing_cases(), *gui_speed_checks(), *finite_api_cases(), *gui_finite_cases(), *gui_speed_runs(),
                     *paused_api_checks()]


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
