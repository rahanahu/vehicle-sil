#!/usr/bin/env python3
"""Records what the real vehicle_sil_launch writes, for replay_launcher.py (the stand-in of the GUI server's tests
and of the screen's e2e).

    record_launcher.py --launcher <vehicle_sil_launch> [--examples <dir>] [--out <dir>] [<case>...]
    record_launcher.py --launcher <vehicle_sil_launch> --compare [<case>...]

Each case of CASES below runs the real launcher in a copy of configs/ (with engine/ and dashboard/ linked to the
ecu_main example's executables, which are built next to the launcher: build/<preset>/examples/ecu_main, or
--examples), writes the commands of its script to the launcher's standard input when its output says so, and
writes what came back to recordings/<case>.jsonl (--out: another directory). Without cases, every case.

What changes from run to run is replaced with a fixed mark: the copy's directory (<root>), the pids ("<pid>"), the
registry's port (silkit://127.0.0.1:8500), the control server's address (<control>), times (no seq, no t_ms, the
time between lines in steps of 100 ms), how fast it went (actual_speed 1.5, wall_ms 1000, paused_ms 500 when it
stood still) and lateness (0). A recording that still has a path of this computer, the user's name or the control
token in it is not written (the case fails with what was found).

--compare records into a temporary directory and compares with recordings/: the events in order (without time,
log and heartbeat, whose number depends on timing), their keys and the JSON types of their values, the commands,
the exit code, and what check --json and version --json print. It says which case differs and how; record again
when the change is meant (then `git diff gui/server/tests/recordings` shows what changed for the GUI).

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
STOP = '{"command":"stop"}'
PAUSE, RESUME = '{"command":"pause"}', '{"command":"resume"}'
TIMEOUT_S = 60


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


def run_case(name: str, file: str, about: str, *script, extra: tuple = (), default: bool = False) -> Case:
    return Case(name, [*RUN, *extra, "--", file], about, list(script), default)


CREDENTIALS = ("--control", "0", "--control-credentials", "<credentials>")


# ---------------------------------------------------------------- recording


class Recorder:
    def __init__(self, launcher: Path, examples: Path, root: Path):
        self.launcher = launcher
        self.examples = examples
        self.root = root
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

    def record(self, case: Case) -> tuple[dict, list[dict]]:
        argv = [str(self.root / ".control-credentials") if a == "<credentials>" else a for a in case.argv]
        proc = subprocess.Popen([str(self.launcher), *argv], cwd=self.root, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
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
        return self.normalize(case, lines, code)

    # ---- the marks

    def normalize(self, case: Case, lines: list[tuple[float, str, str]], code: int) -> tuple[dict, list[dict]]:
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
            for path in sorted({str(self.root.resolve()), str(self.root), str(self.examples.resolve()),
                                str(self.examples)}, key=len, reverse=True):
                s = s.replace(path, "<root>")
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
                if "actual_speed" in out and out["actual_speed"] is not None:
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
                if any(f.get("code") == "behind-real-time" for f in [e] + (e.get("result") or {}).get("findings", [])
                       if isinstance(f, dict)):
                    raise RuntimeError(f"{case.name}: the run came behind real time (the machine was busy): record "
                                       "it again on a machine that is not busy")
                e = {k: v for k, v in e.items() if k not in ("seq", "t_ms")}
                entries.append({"out": marks(e)})
        if printed is not None:
            entries.insert(0, {"print": marks(printed)})
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

    def forbid(self, case: Case, head: dict, entries: list[dict]) -> None:
        text = json.dumps([head, entries], ensure_ascii=False)
        found = [what for what, needle in (
            ("the control token", self.token), ("the home directory", str(Path.home())),
            ("the temporary directory", tempfile.gettempdir() + "/"), ("the user's name", getpass.getuser()),
            ("the directory of the executables", str(self.examples))) if needle and needle in text]
        if re.search(r"/home/|/Users/|/tmp/claude", text):
            found.append("a path of this computer")
        if found:
            raise RuntimeError(f"{case.name}: the recording still has {', '.join(found)} in it: add a mark for it "
                               "in Recorder.normalize")


def shape_of(argv: list[str]) -> list[str]:
    import replay_launcher
    return replay_launcher.shape([a if a != "<credentials>" else "x" for a in argv])[0]


def text_sha256(path: Path) -> str:
    import replay_launcher
    return replay_launcher.text_sha256(str(path))


def write(path: Path, head: dict, entries: list[dict]) -> None:
    path.write_text("\n".join(json.dumps(x, ensure_ascii=False) for x in [head, *entries]) + "\n", encoding="utf-8",
                    newline="\n")


# ---------------------------------------------------------------- comparing


NOT_IN_ORDER = {"time", "log", "heartbeat", "log_dropped"}


def types(value):
    """The keys and JSON types of a value, as a comparable tree (lists: the types of their first item)."""
    if isinstance(value, dict):
        return {k: types(v) for k, v in value.items()}
    if isinstance(value, list):
        return [types(value[0])] if value else []
    return "null" if value is None else type(value).__name__.replace("int", "number").replace("float", "number")


def outline(entries: list[dict]) -> list:
    out = []
    for e in entries:
        if "out" in e and e["out"].get("event") not in NOT_IN_ORDER:
            out.append(("event", e["out"]["event"], e["out"].get("state") or e["out"].get("phase"),
                        json.dumps(types(e["out"]), sort_keys=True)))
        elif "send" in e or "exit" in e:
            out.append(tuple(e.items())[0])
        elif "print" in e:
            out.append(("print", json.dumps(types(e["print"]), sort_keys=True)))
    return out


def compare(name: str, tracked: Path, fresh: Path) -> list[str]:
    import replay_launcher
    if not tracked.is_file():
        return [f"{name}: recordings/{name}.jsonl is missing: record it"]
    _, a = replay_launcher.read_recording(tracked)
    _, b = replay_launcher.read_recording(fresh)
    left, right = outline(a), outline(b)
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
        print(f"record_launcher.py: no case {', '.join(unknown)}: the cases are in record_cases.py",
              file=sys.stderr)
        return 2
    chosen = [c for c in every if not args.cases or c.name in args.cases]
    failed = []
    with tempfile.TemporaryDirectory(prefix="record-launcher-") as tmp:
        root = Path(tmp) / "root"
        root.mkdir()
        recorder = Recorder(args.launcher.resolve(), examples.resolve(), root)
        recorder.prepare()
        out = Path(tmp) / "fresh" if args.compare else args.out
        out.mkdir(parents=True, exist_ok=True)
        for case in chosen:
            try:
                head, entries = recorder.record(case)
            except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
                failed.append(f"{case.name}: {e}")
                continue
            write(out / f"{case.name}.jsonl", head, entries)
            if args.compare:
                failed += compare(case.name, RECORDINGS / f"{case.name}.jsonl", out / f"{case.name}.jsonl")
    for line in failed:
        print(line, file=sys.stderr)
    print(f"record_launcher: {len(chosen) - len(failed)} of {len(chosen)} cases "
          f"{'as recorded' if args.compare else 'recorded'}" + (f" (into {args.out})" if not args.compare else ""))
    return 1 if failed else 0


def paused(sim_ms: int | None = None, **fields) -> Callable[[list[dict]], bool]:
    """A time event that stands still (at sim_ms, or anywhere), and has these fields."""
    def seen(events: list[dict]) -> bool:
        return any(e.get("event") == "time" and e.get("paused") is True and
                   (sim_ms is None or e.get("sim_ms") == sim_ms) and all(e.get(k) == v for k, v in fields.items())
                   for e in events)
    return seen


def last_time(**fields) -> Callable[[list[dict]], bool]:
    """The last time event has these fields."""
    def seen(events: list[dict]) -> bool:
        times = [e for e in events if e.get("event") == "time"]
        return bool(times) and all(times[-1].get(k) == v for k, v in fields.items())
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
             "participant-crash", "result-then-wait"]
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
        run_case("run-simulation-disable", "simulation.yaml", "a run without Logger", extra=("--disable", "Logger")),
        run_case("run-simulation-speed", "simulation.yaml", "a run without Logger at speed 2",
                 extra=("--disable", "Logger", "--speed", "2")),
        run_case("run-scenario-fails", "simulation.yaml", "a scenario whose second step fails",
                 extra=("--scenario", "scenarios/fails.yaml")),
        run_case("run-scenario-passes", "simulation.yaml", "a scenario that passes",
                 extra=("--scenario", "scenarios/throttle.yaml")),
        run_case("run-simulation-paused", "simulation.yaml", "started paused, then resumed",
                 ("until", paused(0)), ("send", RESUME), extra=("--start-paused",)),
        run_case("run-manual-stop", "simulation.yaml", "a participant that the user starts never joins: stopped",
                 ("until", log("to join (manual)")), ("sleep", 0.3), stop, extra=("--manual", "Dashboard")),
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
                 ("until", paused(30)), ("send", RESUME), ("until", last_time(paused=False)), ("send", PAUSE),
                 ("until", lambda events: command("pause")(events) and last_time(paused=True)(events)),
                 ("send", speed(2)), ("until", paused(speed=2)), stop, extra=("--start-paused",)),
    ]
    paused_at_start = ("--disable", "Logger", "--speed", "max", "--start-paused")
    for value, small in ((2, 10), (1, 100)):
        out.append(run_case(
            f"time-commands-speed-{value}", "forever.yaml",
            f"started paused: step 25, speed {value}, resume, step {small} (refused while it runs), pause, stop",
            ("until", paused(0)), ("send", step(25)), ("until", paused(30)), ("send", speed(value)),
            ("until", paused(speed=value)), ("send", RESUME), ("until", last_time(paused=False)),
            ("send", step(small)), ("until", command("step", 2)), ("send", PAUSE),
            ("until", lambda events: command("pause")(events) and last_time(paused=True)(events)), stop,
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
        run_case("control-scenario-passes", "simulation.yaml", "a scenario that passes, with the control server",
                 extra=("--scenario", "scenarios/throttle.yaml", *control)),
        run_case("control-scenario-fails", "simulation.yaml", "a scenario that fails, with the control server",
                 extra=("--scenario", "scenarios/fails.yaml", *control)),
        run_case("control-simulation-paused", "simulation.yaml",
                 "with the control server, started paused: a pause (it stands already), a resume, to the end",
                 STANDS, ("send", PAUSE), ("until", command("pause")), ("send", RESUME),
                 extra=(*control, "--start-paused")),
    ]
    return out


CASES: list[Case] = [Case("version", ["version", "--json"], "version --json"), *checks(), *runs()]


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
