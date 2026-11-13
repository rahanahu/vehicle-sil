#!/usr/bin/env python3
"""A stand-in for vehicle_sil_launch that plays back what the real one wrote, for the GUI server's tests and the
screen's e2e.

    replay_launcher.py <the arguments the GUI server gives vehicle_sil_launch>

It does not simulate anything. recordings/ holds what the real launcher wrote for the cases that the tests use
(record_launcher.py makes them, and says how to make them again when the launcher changes): its standard output
and error, its exit code, and where a command was written to its standard input. This program finds the
recording of the command line it is given and writes the same lines in the same order.

Which recording: the one whose command line is the same, where the configuration file and the scenario file are
compared by their text (SHA-256 of the text with LF line ends), not by their names, and the token file of
--control-credentials is left out. Several may match (the same command line with other commands written to the
launcher): see below; REPLAY_CASE=<case> plays only that one. When none matches, it writes what to do on standard
error and exits with code 2.

How it plays:
- seq is counted again and t_ms is the time since this program started; heartbeat comes after REPLAY_HEARTBEAT_MS
  (default 1000; 0: none) without a line, from hello to the result, as the real launcher writes it.
- The time between lines is the recorded one (in steps of 100 ms) scaled by REPLAY_TICK_MS/100 (default 100: as
  recorded; the GUI server's tests use 10).
- A recording that ends while the launcher waited on (hang) ends the same way: it waits until a signal.
- SIGTERM ends it where it is, with the processes it started (the real launcher stops the run and writes its
  result; no test needs that).
- Of several recordings of one command line, it plays the one that record_launcher.py marks as the default (else
  the one with the fewest commands; of those, the one whose first comes latest); when a command comes that another one has next, it goes on with that one from as far as it
  had come, and after a command with the one that has the fewest more (it stays with the one it plays when that
  is as good). The virtual
  time goes on from where it was: the other recording's times are moved on by the difference.
- Where the recording wrote a command, it waits for one from standard input (or for its end), and goes on when it
  is the same command. Meanwhile, when the simulation was running there, it runs on as the launcher would: a time
  event every 100 ms (scaled), the virtual time moved on by the recording's last move (nothing else is made up;
  REPLAY_RUN_ON=0: it stands still instead, so that what is read meanwhile does not depend on timing). Another command ends the replay with code 3 and a line on standard error that says which
  was expected. A command that comes early hurries the lines before it. Commands after the last recorded one are
  not answered.
- The recording's marks are put back: <root> (the directory it runs in), <file> and <scenario> (the arguments),
  "<pid>" (its own pid in hello; for a started participant the pid of a sleeping child process that it starts in
  its place and ends when the participant ends, so that a test can see whether stopping left processes behind),
  <control> (the address of the control server below).
- The variables of the participants (a check's participants[].variables, the count of a control_link) are those
  of the stand-in control server (fake_control.variables), because the values come from it: with --control it
  starts that stand-in (fake_control.py, gRPC) for the participants whose control link is connected, with the token
  of the --control-credentials file, and moves its time as the time events say.

Faults (what the real launcher does not do on purpose, to see how the GUI server takes it): a line
`# replay-fault: <name>` in the configuration file, or REPLAY_FAULT=<name>. "The first time event" below is the
first one after the phase running, "the next" the one after it.

  bad-lines          after the first time event: a line that is not JSON, a JSON array, an unknown event, a gap
                     in seq and a log event with an unknown field
  bad-types          after the first time event: known events whose fields have the wrong types
  non-finite         after the first time event: a time and a log event with a number that is valid JSON but not
                     finite (1e999, -1e999)
  loose-fields       after the first time event: known events with fields missing or of other types where the GUI
                     server does not check them
  future-events      hello says vehicle_sil_events 2
  flood              REPLAY_FLOOD (default 5000) log lines after the first time event
  finding-flood      the same number of finding events
  large-result       the same number of notes that the result lists too
  log-dropped        a log_dropped event after the next time event
  large-fields       the first started participant's command is longer than 4 KiB
  launcher-crash     at the third time event of the run (or before a command written after the first): a line on
                     standard error and exit code 70, without a result
  orphan             after the first time event, a process in a session of its own that keeps the standard output
                     and error open for REPLAY_ORPHAN_S seconds (default 30); a log line says its pid
  check-garbage      check --json writes a line that is not JSON
  result-then-wait   after the result it waits for a command or the end of standard input before it exits
  silent             at the next time event nothing at all, not even a heartbeat, for REPLAY_SILENT_S seconds
                     (default 8)
  ignore-stop        SIGTERM is ignored (a launcher that is stuck): only a kill ends it from outside
  deaf               it never reads standard input (on Linux its pipe is made as small as it can be)
  late               (a run with a speed) the next time event is REPLAY_LATE_MS (default 200) late: a time event
                     with late_ms, the counts of a late step after it, the result's timing and the warning
                     behind-real-time
  jitter             (a run with a speed) every other time event is 3 ms late, within the tolerance
  manual-stay        the participants that the user started (manual) and that joined do not leave at the end: the
                     launcher's log line that says so (its wording, from the launcher's source) before the phase
                     ended (a held process cannot be made to stay reliably when recording)
  control-trouble    of the participants whose control link is connected, the first's is no_hello, the second's
                     version_mismatch (warnings at the end); the participant after them in the file gets
                     not_offered (no control in its check, a note in the check)

A scenario run with the control server (--scenario with --control, the feature scenario-control) that no recording
has is played from the recording without --control, with the control_server event added; the control server
then refuses every write, as the real one does while a scenario runs. This stands in for launchers that are
recorded before they have the feature; a recording with it is played instead once there is one.

Other environment variables: REPLAY_FEATURES (the features of version --json, comma-separated, to stand in for an
older launcher; default: the recorded ones without control-server, a launcher built without the control server,
so that only the tests that ask for it need gRPC), REPLAY_CHECK_DELAY_MS (how long check takes), REPLAY_CONTROL
(what goes wrong with the control server: wrong-token, unreachable, bad-address, closes: it ends after the third
time event), REPLAY_RECORDINGS (another directory of recordings, with fake_control.py next to it). Standard library
only, except fake_control (grpcio) for --control. A --control-credentials file that others can read ends it with
code 3 (the real launcher's rule for the token file).
"""

from __future__ import annotations

import hashlib
import json
import os
import queue
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECORDINGS = Path(os.environ.get("REPLAY_RECORDINGS") or HERE / "recordings")
sys.path.insert(0, str(RECORDINGS.parent))  # fake_control.py, next to recordings/ (also for a copy elsewhere)
FORMAT = 1
STEP_MS = 100  # the stand-in control server runs a step every 100 ms of virtual time
REGISTRY = "silkit://127.0.0.1:8500"  # what record_launcher.py writes for the registry's address
FAULTS = ("bad-lines", "bad-types", "non-finite", "loose-fields", "future-events", "flood", "finding-flood",
          "large-result", "log-dropped", "large-fields", "launcher-crash", "orphan", "check-garbage",
          "result-then-wait", "silent", "ignore-stop", "deaf", "late", "jitter", "control-trouble", "manual-stay")
RECORDER = "gui/server/tests/record_launcher.py"
SIGNAL = "<SIGTERM>"  # in the queue of the commands: SIGTERM came


# ---------------------------------------------------------------- choosing a recording


def text_sha256(path: str | None) -> str | None:
    """The SHA-256 of a file's text with LF line ends (None: no such file)."""
    if path is None:
        return None
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


def shape(argv: list[str]) -> tuple[list[str], str | None, str | None]:
    """The command line with the files as marks, the configuration file, the scenario file."""
    out, file, scenario = [], None, None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--" and i + 1 < len(argv):
            file = argv[i + 1]
            out += ["--", "<file>", *argv[i + 2:]]
            break
        if a in ("--scenario", "--control-credentials") and i + 1 < len(argv):
            if a == "--scenario":
                scenario = argv[i + 1]
            out += [a, "<scenario>" if a == "--scenario" else "<credentials>"]
            i += 2
            continue
        out.append(a)
        i += 1
    return out, file, scenario


def read_recording(path: Path) -> tuple[dict, list[dict]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    head = json.loads(lines[0])
    if head.get("vehicle_sil_recording") != FORMAT:
        raise ValueError(f"{path}: not a recording of format {FORMAT}")
    return head, [json.loads(line) for line in lines[1:] if line.strip()]


def choose(argv: list[str]) -> list[tuple[dict, list[dict]]] | str:
    """The recordings of this command line (REPLAY_CASE: only that one), or what is wrong (a message for standard
    error)."""
    wanted, file, scenario = shape(argv)
    config_sha, scenario_sha = text_sha256(file), text_sha256(scenario)
    same_line, matches = [], []
    for path in sorted(RECORDINGS.glob("*.jsonl")):
        head, entries = read_recording(path)
        if head["shape"] != wanted:
            continue
        if head.get("config") == file and head.get("scenario") == scenario:
            same_line.append(head["case"])
        if head.get("config_sha256") == config_sha and head.get("scenario_sha256") == scenario_sha:
            matches.append((head, entries))
    line = " ".join(argv)
    case = os.environ.get("REPLAY_CASE")
    if case and matches:
        chosen = [m for m in matches if m[0]["case"] == case]
        if chosen:
            return chosen
        return (f"replay_launcher: REPLAY_CASE={case} is not a recording of `{line}`: those are "
                f"{', '.join(m[0]['case'] for m in matches)}")
    if matches:
        return matches
    if same_line:
        return (f"replay_launcher: the recordings of `{line}` ({', '.join(same_line)}) were made from another text "
                f"of {file}{' or ' + scenario if scenario else ''}: the file has changed since they were made; "
                f"record them again with {RECORDER} --launcher <vehicle_sil_launch>")
    return (f"replay_launcher: no recording of `{line}`: add the case to CASES in {RECORDER} and record it "
            "with --launcher <vehicle_sil_launch>")


def fault_of(file: str | None) -> str | None:
    """REPLAY_FAULT, else the configuration's `# replay-fault:` line; none: REPLAY_FAULT=none."""
    fault = os.environ.get("REPLAY_FAULT")
    if fault == "none":
        return None
    if not fault and file:
        try:
            for line in Path(file).read_text(encoding="utf-8").splitlines():
                if line.startswith("# replay-fault:"):
                    fault = line.split(":", 1)[1].strip()
                    break
        except OSError:
            pass
    return fault or None


# ---------------------------------------------------------------- faults: the entries changed


def events_at(entries: list[dict], kind: str) -> list[int]:
    return [i for i, e in enumerate(entries) if e.get("out", {}).get("event") == kind]


def running_times(entries: list[dict]) -> list[int]:
    """The indexes of the time events after the phase running."""
    start = next((i for i in events_at(entries, "phase") if entries[i]["out"]["phase"] == "running"), None)
    return [i for i in events_at(entries, "time") if start is not None and i > start]


def started(entries: list[dict]) -> list[str]:
    return [entries[i]["out"]["name"] for i in events_at(entries, "participant")
            if entries[i]["out"].get("state") == "started"]


def note_finding(code: str, message: str) -> dict:
    return {"severity": "note", "code": code, "file": "<file>", "line": None, "participant": None, "network": None,
            "message": message}


def apply_fault(fault: str, entries: list[dict], head: dict) -> list[dict]:
    times = running_times(entries)
    first = times[0] + 1 if times else len(entries)
    after_next = times[1] + 1 if len(times) > 1 else first
    names = started(entries) or ["vehicle_sil_launch"]
    flood = int(os.environ.get("REPLAY_FLOOD", "5000"))

    def insert(at: int, *added: dict) -> list[dict]:
        return entries[:at] + list(added) + entries[at:]

    if fault == "bad-lines":
        return insert(first, {"raw": "this line is not JSON"}, {"raw": "[1, 2, 3]"},
                      {"out": {"event": "mystery", "detail": "an event this reader does not know"}},
                      {"seq_gap": 1},
                      {"out": {"event": "log", "source": names[0], "line": "after a gap", "colour": "blue"}})
    if fault == "bad-types":
        return insert(first, *({"out": {"event": kind, **fields}} for kind, fields in (
            ("check", {"check": {"participants": 5}}), ("participant", {"name": 5}),
            ("participant", {"name": "Engine", "state": ["x"]}), ("log", {"source": None}),
            ("time", {"sim_ms": "100"}), ("phase", {"phase": {}}), ("result", {"result": []}),
            ("command", {"command": "stop", "accepted": "yes"}), ("finding", {"severity": 1}))))
    if fault == "non-finite":
        return insert(first, {"raw_event": '"event":"time","sim_ms":50,"actual_speed":1e999'},
                      {"raw_event": '"event":"log","source":"vehicle_sil_launch","line":"x","level":-1e999'})
    if fault == "loose-fields":
        return insert(first, {"out": {"event": "finding", "severity": "note",
                                      "message": "a finding with only a severity and a message"}},
                      {"out": {"event": "registry", "uri": None}},
                      {"out": {"event": "participant", "name": names[0], "state": "joined", "signal": 9,
                               "message": 5}},
                      {"raw_event": '"event":"log","source":"vehicle_sil_launch","line":"a log event without t_ms"',
                       "no_t_ms": True})
    if fault == "future-events":
        for e in entries:
            if e.get("out", {}).get("event") == "hello":
                e["out"]["vehicle_sil_events"] = 2
        return entries
    if fault == "flood":
        return insert(first, {"out": {"event": "log", "source": names[0], "line": "flood line {n}"}, "repeat": flood})
    if fault in ("finding-flood", "large-result"):
        entries = insert(first, {"out": {"event": "finding", **note_finding("flood", "flood finding {n}")},
                                 "repeat": flood})
        if fault == "large-result":
            for i in events_at(entries, "result"):
                result = entries[i]["out"]["result"]
                result["findings"] = [note_finding("flood", f"flood finding {n}") for n in range(flood)] + \
                    result["findings"]
                result["notes"] += flood
        return entries
    if fault == "log-dropped":
        return insert(after_next, {"out": {"event": "log_dropped", "count": 125,
                                           "sources": {names[0]: 120, "vehicle_sil_launch": 5}}})
    if fault == "large-fields":
        for i in events_at(entries, "participant"):
            if entries[i]["out"].get("state") == "started":
                entries[i]["out"]["command"].append("--padding=" + "x" * 5000)
                break
        return entries
    if fault == "launcher-crash":  # at the third time event of the run, or before a command written after it
        at = times[min(2, len(times) - 1)] if times else len(entries)
        at = min([at] + [i for i, e in enumerate(entries) if "send" in e and times and i > times[0]])
        return entries[:at] + [{"err": "replay_launcher: dying on purpose (fault launcher-crash)"}, {"exit": 70}]
    if fault == "orphan":
        return insert(first, {"orphan": True})
    if fault == "result-then-wait":
        at = events_at(entries, "result")
        return insert(at[-1] + 1, {"wait_input": True}) if at else entries
    if fault == "silent":
        return insert(after_next, {"silent": True})
    if fault in ("late", "jitter"):
        return lateness(fault, entries, times)
    if fault == "control-trouble":
        return control_trouble(entries)
    if fault == "manual-stay":
        manual = {entries[i]["out"]["name"] for i in events_at(entries, "participant")
                  if entries[i]["out"].get("state") == "manual"}
        joined = [entries[i]["out"]["name"] for i in events_at(entries, "participant")
                  if entries[i]["out"].get("state") == "joined" and entries[i]["out"]["name"] in manual]
        checked = next((entries[i]["out"]["check"] for i in events_at(entries, "check")), {})
        timeout = checked.get("stop_timeout_ms") or 10000
        ended = [i for i in events_at(entries, "phase") if entries[i]["out"]["phase"] == "ended"]
        return insert(ended[-1] if ended else len(entries), *(
            {"out": {"event": "log", "source": "vehicle_sil_launch",
                     "line": f"{name} (started by you, manual) has not left the simulation {timeout} ms after it "
                             "ended: the launcher cannot end it, so end it yourself"}} for name in joined))
    return entries  # check-garbage, ignore-stop, deaf: in the program


def lateness(fault: str, entries: list[dict], times: list[int]) -> list[dict]:
    """late: the next time event comes REPLAY_LATE_MS late and is counted (the warning behind-real-time at the
    end); jitter: every other one 3 ms late, within the tolerance."""
    late = int(os.environ.get("REPLAY_LATE_MS", "200"))
    counts = {"late_steps": 0, "max_late_ms": 0, "slipped_ms": 0}
    out = []
    for i, e in enumerate(entries):
        ev = e.get("out", {})
        if ev.get("event") == "time" and i in times:
            n = times.index(i)
            if fault == "jitter":
                ev["late_ms"] = 3 if n % 2 else 0
            elif n == 1:
                counts = {"late_steps": 1, "max_late_ms": late, "slipped_ms": late}
                out.append({"out": {**ev, **counts, "late_ms": late}})
            if fault == "late":
                ev.update(counts)
        if ev.get("event") == "result" and fault == "late" and isinstance(ev["result"].get("timing"), dict):
            ev["result"]["timing"].update(counts)
            warning = {"severity": "warning", "code": "behind-real-time", "file": "<file>", "line": None,
                       "participant": None, "network": None,
                       "message": f"1 of {ev['result']['timing'].get('paced_steps')} paced steps were more than "
                                  f"{ev['result']['timing'].get('late_tolerance_ms')} ms late (the latest by {late} "
                                  f"ms), so the simulation came {late} ms behind real time in all: lower the "
                                  "speed, or find what is slow"}
            ev["result"]["findings"] = [warning, *ev["result"]["findings"]]
            ev["result"]["warnings"] += 1
            # with the findings of the simulation's end, before the others (warnings before notes)
            at = next((j for j in range(times[-1] + 1 if times else 0, len(out))
                       if out[j].get("out", {}).get("event") in ("finding", "phase")), len(out))
            out.insert(at, {"out": {"event": "finding", **warning}})
        out.append(e)
    return out


def control_trouble(entries: list[dict]) -> list[dict]:
    """The first three participants that describe themselves: no_hello, version_mismatch, not_offered."""
    connected = [e["out"]["name"] for e in entries if e.get("out", {}).get("event") == "control_link"
                 and e["out"].get("state") == "connected"][:2]
    states = dict(zip(connected, ("no_hello", "version_mismatch")))
    for e in entries:
        ev = e.get("out", {})
        if ev.get("event") == "check" and len(connected) == 2:
            names = [p["name"] for p in ev["check"]["participants"]]
            after = names[names.index(connected[1]) + 1:] if connected[1] in names else []
            if after:
                states[after[0]] = "not_offered"
            break
    if not states:
        return entries
    warnings = []
    out = []
    for e in entries:
        ev = e.get("out", {})
        for checked in checks_in(ev):
            for p in checked.get("participants", []):
                if states.get(p["name"]) == "not_offered":
                    p["control"] = None
                    checked.setdefault("findings", []).append(dict(
                        note_finding("control-link-not-offered",
                                     f"participant '{p['name']}': it describes variables, but its control agent "
                                     "is not offered, so its variables cannot be read or written"),
                        line=p.get("line"), participant=p["name"]))
                    checked["notes"] = checked.get("notes", 0) + 1
        if ev.get("event") == "control_link" and ev.get("name") in states:
            state = states[ev["name"]]
            ev.pop("variables", None)
            ev["state"] = state
            if state == "version_mismatch":
                ev["version"] = 2
            if state in ("no_hello", "version_mismatch"):
                message = (f"participant '{ev['name']}': it describes variables (--describe), but its control "
                           "agent did not say hello before the simulation started, so its variables cannot be "
                           "read or written in this run" if state == "no_hello" else
                           f"participant '{ev['name']}': its control agent speaks version 2 of the control "
                           "messages and this launcher version 1, so its variables cannot be read or written in "
                           "this run")
                warnings.append({"severity": "warning",
                                 "code": "control-link-no-hello" if state == "no_hello" else "control-link-version",
                                 "file": "<file>", "line": None, "participant": ev["name"], "network": None,
                                 "message": message})
        if ev.get("event") == "result" and isinstance(ev.get("result"), dict):
            r = ev["result"]
            for p in r.get("participants", []):
                if p["name"] in states and isinstance(p.get("control_link"), dict):
                    state = states[p["name"]]
                    p["control_link"] = {"state": state, "variables": None,
                                         "version": 2 if state == "version_mismatch" else None,
                                         "late_messages": p["control_link"].get("late_messages", 0)}
            r["findings"] = warnings + r["findings"]
            r["warnings"] += len(warnings)
            out += [{"out": {"event": "finding", **w}} for w in warnings]
        out.append(e)
    return out


def checks_in(value) -> list[dict]:
    """The check objects in an event or a printed object (a check event, a result's check, check --json)."""
    found = []
    if isinstance(value, dict):
        if value.get("vehicle_sil_check") is not None:
            found.append(value)
        for v in value.values():
            found += checks_in(v)
    return found


# ---------------------------------------------------------------- playing


class Take:
    """One recording, its fault applied: where its commands were written, and how far it had come there."""

    def __init__(self, head: dict, entries: list[dict]):
        self.head = head
        self.case = head["case"]
        self.entries = entries
        self.sends = [i for i, e in enumerate(entries) if "send" in e]
        # The milestones written before each entry: every event but log lines (time events included), so that two
        # recordings of one command line can be lined up
        self.milestones = []
        n = 0
        for e in entries:
            self.milestones.append(n)
            if e.get("out", {}).get("event") not in (None, "log", "log_dropped"):
                n += 1
        self.last_time = max((i for i, e in enumerate(entries) if e.get("out", {}).get("event") == "time"),
                             default=-1)

    def command(self, k: int) -> str | None:
        return self.entries[self.sends[k]]["send"]


class Player:
    """Plays one of the recordings of a command line. They differ in the commands written to the launcher: it
    starts with the one that has none (else the one whose first command comes latest), and when a command comes
    that another recording has next, it goes on with that one, from as far as it had come."""

    def __init__(self, argv: list[str], takes: list[Take], fault: str | None):
        self.argv = argv
        self.takes = takes
        self.take = min(takes, key=lambda t: (not t.head.get("default"), len(t.sends),
                                              -(t.milestones[t.sends[0]] if t.sends else 0)))
        self.entries = self.take.entries
        self.case = self.take.case
        self.i = 0  # the next entry
        self.consumed: list[str | None] = []  # the commands taken so far
        self.accepted: str | None = None  # a command taken before the send entry it answers is reached
        self.has_accepted = False
        self.fault = fault
        _, self.file, self.scenario = shape(argv)
        self.root = os.getcwd()
        self.start = time.monotonic()
        self.seq = 0
        self.lock = threading.RLock()
        self.last_write = self.start
        self.hello = False
        self.sealed = False
        self.muted_until = 0.0
        self.inputs: queue.Queue = queue.Queue()
        self.tick = int(os.environ.get("REPLAY_TICK_MS", "100")) / 100
        self.sleepers: dict[str, subprocess.Popen] = {}
        self.control = None
        self.control_ms = 0  # the next step of the stand-in control server
        self.last_sim = 0  # the virtual time of the last time event written
        self.last_time: dict | None = None  # that event
        self.time_step = 0  # the last move of the virtual time between two time events
        self.phase = None
        self.shift = 0  # added to the virtual times of the recording played (see go_on_with)
        self.time_events = 0
        self.peers = self._connected()

    # ---- writing

    def _line(self, line: str, stream=None) -> None:
        stream = stream or sys.stdout
        try:
            stream.write(line + "\n")
            stream.flush()
        except (BrokenPipeError, OSError):
            self.finish(141)  # the reader is gone
        if stream is sys.stdout:
            self.last_write = time.monotonic()

    def t_ms(self) -> int:
        return int((time.monotonic() - self.start) * 1000)

    def emit(self, event: dict) -> None:
        with self.lock:
            while time.monotonic() < self.muted_until:
                time.sleep(0.01)
            kind = event["event"]
            rest = {k: v for k, v in event.items() if k != "event"}
            self._line(json.dumps({"event": kind, "seq": self.seq, "t_ms": self.t_ms(), **rest},
                                  ensure_ascii=False, separators=(",", ":")))
            self.seq += 1
            if kind == "hello":
                self.hello = True
            if kind == "result" and isinstance(event.get("result"), dict):
                self.sealed = True

    def heartbeat(self, every_s: float) -> None:
        while True:
            time.sleep(min(0.05, every_s / 4))
            with self.lock:
                now = time.monotonic()
                if self.hello and not self.sealed and now >= self.muted_until and now - self.last_write >= every_s:
                    self.emit({"event": "heartbeat"})

    # ---- the marks

    def marks(self, value, key=None):
        if isinstance(value, dict):
            return {k: self.marks(v, k) for k, v in value.items()}
        if isinstance(value, list):
            return [self.marks(v) for v in value]
        if isinstance(value, str):
            value = value.replace("<root>", self.root).replace("<file>", self.file or "")
            value = value.replace("<scenario>", self.scenario or "")
            if self.control is not None:
                value = value.replace("<control>", self.control_address)
        return value

    def _connected(self) -> list[str]:
        return [e["out"]["name"] for e in self.entries
                if e.get("out", {}).get("event") == "control_link" and e["out"].get("state") == "connected"]

    def model(self, value) -> None:
        """The variables of the stand-in control server in place of the recorded ones."""
        import fake_control
        for checked in checks_in(value):
            for p in checked.get("participants", []):
                if isinstance(p.get("variables"), list):
                    p["variables"] = fake_control.variables(p["name"])
        if isinstance(value, dict):
            if value.get("event") == "control_link" and isinstance(value.get("variables"), int):
                value["variables"] = len(fake_control.variables(value["name"]))
            result = value.get("result")
            if isinstance(result, dict):
                for p in result.get("participants", []):
                    link = p.get("control_link")
                    if isinstance(link, dict) and isinstance(link.get("variables"), int):
                        link["variables"] = len(fake_control.variables(p["name"]))

    # ---- input

    def read_input(self) -> None:
        for raw in sys.stdin:
            line = raw.strip()
            if line:
                self.inputs.put(line)
        self.inputs.put(None)

    def since(self, take: Take, i: int, k: int) -> int:
        """The milestones of take before entry i, counted from its command k-1 (from the start when k is 0)."""
        i = min(i, len(take.milestones) - 1)
        return take.milestones[i] - (take.milestones[take.sends[k - 1]] if k else 0) if take.milestones else 0

    def switch(self, got: str | None, back: bool) -> bool:
        """Goes on with a recording whose next command is got, from as far as this one has come since the last
        command (back: or from its command, when it came earlier there). False when none has it next."""
        k = len(self.consumed)
        fits = [t for t in self.takes if len(t.sends) > k and
                all(same_command(c, t.command(j)) for j, c in enumerate(self.consumed)) and
                same_command(got, t.command(k))]
        here = self.since(self.take, self.i, k)
        later = [t for t in fits if self.since(t, t.sends[k], k) >= here]
        if later:
            take = min(later, key=lambda t: self.since(t, t.sends[k], k))
        elif back and fits:
            take = max(fits, key=lambda t: self.since(t, t.sends[k], k))
        else:
            return False
        i = take.sends[k - 1] + 1 if k else 0
        while i < take.sends[k] and self.since(take, i, k) < here:
            i += 1
        self.go_on_with(take, i)
        self.accepted, self.has_accepted = got, True
        return True

    def go_on_with(self, take: Take, i: int) -> None:
        """Goes on with another recording from entry i; its virtual time goes on from where this one's is (its
        times are moved on by the difference, never back)."""
        before = max((e["out"]["sim_ms"] for e in take.entries[:i] if e.get("out", {}).get("event") == "time"
                      and isinstance(e["out"].get("sim_ms"), int)), default=0)
        self.shift = max(0, self.last_sim - before)
        self.take, self.entries, self.case = take, take.entries, take.case
        self.i = i

    def settle(self) -> None:
        """After a command: of the recordings that had the same commands so far, the one that has no more (else
        the one whose next comes latest) goes on, from right after that command."""
        k = len(self.consumed)
        same = [t for t in self.takes if len(t.sends) >= k and
                all(same_command(c, t.command(j)) for j, c in enumerate(self.consumed))]
        def key(t: Take) -> tuple:
            return len(t.sends), -(self.since(t, t.sends[k], k) if len(t.sends) > k else 0)
        take = min(same, key=key)
        if take is not self.take and key(take) != key(self.take):
            self.go_on_with(take, take.sends[k - 1] + 1)

    def wait_input(self, expected) -> None:
        """At a command of the recording: the one that came first, or the next from standard input."""
        if self.has_accepted:
            got, self.has_accepted = self.accepted, False
        else:
            got = self.next_input()
        if got == SIGNAL:
            self.finish(143)
        if same_command(got, expected):
            self.consumed.append(got)
            self.settle()
            return
        if self.switch(got, back=True):
            return
        shown = "the end of standard input" if expected is None else expected
        sys.stderr.write(f"replay_launcher: the recording {self.case} has {shown} written to the launcher here, "
                         f"but it got {'the end of standard input' if got is None else got}, and no recording of "
                         f"this command line has that next: record a case with the commands that the test sends "
                         f"({RECORDER})\n")
        sys.stderr.flush()
        self.finish(3)

    def early_input(self) -> None:
        """A command came while no command of this recording is ahead: another recording may have it."""
        if self.has_accepted or self.inputs.empty() or any(s >= self.i for s in self.take.sends):
            return
        got = self.inputs.get()
        if got == SIGNAL:
            self.finish(143)
        self.switch(got, back=False)  # else it is not answered, as after the last command of a recording

    def next_input(self):
        """The next line of standard input (None: its end); meanwhile the simulation runs on (run_on)."""
        while True:
            try:
                return self.inputs.get(timeout=max(0.01, 0.1 * self.tick))
            except queue.Empty:
                self.run_on()

    def run_on(self) -> None:
        """While it waits for a command where the simulation was running (the phase running, the last time event
        not standing still), it runs on as the launcher would: a time event every 100 ms (scaled), the virtual time
        moved on by the last move between two time events of the recording. The recording's later times move on
        by as much."""
        last = self.last_time
        if self.phase != "running" or last is None or last.get("paused") is not False or not self.time_step or \
                os.environ.get("REPLAY_RUN_ON") == "0":
            return
        self.shift += self.time_step
        event = {**last, "sim_ms": self.last_sim + self.time_step}
        self.last_sim, self.last_time = event["sim_ms"], event
        self.emit(event)
        self.control_time(event, last=False)

    def pace(self, ms: int) -> None:
        """The recorded time between two lines, scaled; a command that has come hurries it."""
        end = time.monotonic() + ms * self.tick / 1000
        while (left := end - time.monotonic()) > 0 and self.inputs.empty() and not self.has_accepted:
            time.sleep(min(left, 0.01))

    # ---- the participants and the control server

    def sleeper(self, name: str) -> int:
        proc = subprocess.Popen([sys.executable, "-c", "import time\nwhile True: time.sleep(60)"],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.sleepers[name] = proc
        return proc.pid

    def end_sleeper(self, name) -> None:
        proc = self.sleepers.pop(name, None) if isinstance(name, str) else None
        if proc is not None:
            proc.kill()
            proc.wait()

    def start_control(self, event: dict) -> None:
        import fake_control
        trouble = os.environ.get("REPLAY_CONTROL", "")
        token, role = None, "operator"
        if "--control-credentials" in self.argv:
            path = self.argv[self.argv.index("--control-credentials") + 1]
            mode = os.stat(path).st_mode & 0o777
            if os.name == "posix" and mode & 0o077:  # the real launcher's rule for the token file
                sys.stderr.write(f"replay_launcher: --control-credentials {path} can be read by others "
                                 f"(mode {mode:o}): make it readable only by its owner (chmod 600)\n")
                self.finish(3)
            lines = [ln.split() for ln in Path(path).read_text().splitlines()
                     if ln.strip() and not ln.lstrip().startswith("#")]
            role, token = lines[0][0], lines[0][2]
        peers = {name: fake_control.variables(name) for name in self.peers}
        self.control = fake_control.FakeControl(peers, token + "-other" if trouble == "wrong-token" and token
                                                else token, role=role)
        self.control_address = self.control.start()
        if trouble == "unreachable":
            self.control.stop()
            self.control.server = None
        if trouble == "bad-address":
            self.control_address = "control.example:8510"
        if self.take.head.get("scenario_control") or "--scenario" in self.argv:
            self.control.refuse_writes = "a scenario runs: variables cannot be written by a control client"
        self.control.set_time(0, "--start-paused" in self.argv, running=False)

    def control_time(self, event: dict, last: bool) -> None:
        """The stand-in's steps up to the time of a time event (the step that begins there too, unless the
        simulation ends there), then its time."""
        if self.control is None or not isinstance(event.get("sim_ms"), int):
            return
        sim = event["sim_ms"]
        while self.control_ms < sim or (self.control_ms == sim and not last):
            self.control.step(self.control_ms)
            self.control_ms += STEP_MS
        self.control.set_time(sim, bool(event.get("paused")))
        self.time_events += 1
        if self.time_events == 3 and os.environ.get("REPLAY_CONTROL") == "closes":
            self.control.stop()
            self.control.server = None

    def control_end(self) -> None:
        if self.control is not None and self.control.server is not None:
            self.control.set_time(self.last_sim, False, running=False, ended=True)
            self.control.stop()
            self.control.server = None

    # ---- the whole

    def finish(self, code: int):
        for name in list(self.sleepers):
            self.end_sleeper(name)
        try:
            sys.stdout.flush()
        except OSError:
            pass
        os._exit(code)

    def play(self) -> None:
        if self.fault == "ignore-stop":
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
        else:  # the real launcher stops the run on SIGTERM; here it ends, with what it started, when it reads it
            signal.signal(signal.SIGTERM, lambda *_: self.inputs.put(SIGNAL))
        heartbeat_ms = int(os.environ.get("REPLAY_HEARTBEAT_MS", "1000"))
        if heartbeat_ms > 0:
            threading.Thread(target=self.heartbeat, args=(heartbeat_ms / 1000,), daemon=True).start()
        if self.fault == "deaf":
            try:  # the smallest pipe (Linux): what is written to it fills it soon
                import fcntl
                fcntl.fcntl(0, fcntl.F_SETPIPE_SZ, 4096)
            except (ImportError, AttributeError, OSError):
                pass
        elif "--stdin-commands" in self.argv:
            threading.Thread(target=self.read_input, daemon=True).start()
        if self.argv[:1] == ["check"]:
            time.sleep(int(os.environ.get("REPLAY_CHECK_DELAY_MS", "0")) / 1000)
        while self.i < len(self.entries):
            self.early_input()
            i = self.i
            entry = self.entries[i]
            self.i += 1
            if "gap_ms" in entry:
                self.pace(entry["gap_ms"])
            elif "out" in entry:
                event = self.marks(entry["out"])
                self.moved(event)
                if event.get("event") == "phase" and event.get("phase") == "ended":
                    self.control_end()  # closed before the run's end is written, as the launcher does
                self.model(event)
                for n in range(entry.get("repeat", 1)):
                    self.emit(self.event(event, n) if "repeat" in entry else event)
            elif "print" in entry:
                printed = self.marks(entry["print"])
                self.model(printed)
                if self.fault == "check-garbage" and printed.get("vehicle_sil_check") is not None:
                    self._line("this is not the check object")
                elif isinstance(printed, dict) and printed.get("vehicle_sil_launch") is not None:
                    wanted = os.environ.get("REPLAY_FEATURES")
                    printed["features"] = [f for f in printed["features"] if f != "control-server"] \
                        if wanted is None else [f for f in wanted.split(",") if f]
                    self._line(json.dumps(printed, indent=2, ensure_ascii=False))
                else:
                    self._line(json.dumps(printed, indent=2, ensure_ascii=False))
            elif "text" in entry:
                self._line(self.marks(entry["text"]))
            elif "err" in entry:
                self._line(self.marks(entry["err"]), sys.stderr)
            elif "raw" in entry:
                self._line(entry["raw"])
            elif "raw_event" in entry:
                with self.lock:
                    stamp = "" if entry.get("no_t_ms") else ',"t_ms":0'
                    self._line(f'{{{entry["raw_event"]},"seq":{self.seq}{stamp}}}')
                    self.seq += 1
            elif "seq_gap" in entry:
                with self.lock:
                    self.seq += entry["seq_gap"]
            elif "send" in entry:
                if self.fault == "deaf":  # it never reads the command: it runs on until a signal
                    while self.next_input() != SIGNAL:
                        pass
                    self.finish(143)
                self.wait_input(entry["send"])
            elif "silent" in entry:
                self.muted_until = time.monotonic() + float(os.environ.get("REPLAY_SILENT_S", "8"))
            elif "orphan" in entry:
                hold_s = float(os.environ.get("REPLAY_ORPHAN_S", "30"))
                orphan = subprocess.Popen(
                    [sys.executable, "-c", f"import time; time.sleep({hold_s})"],
                    stdin=subprocess.DEVNULL, start_new_session=True)
                self.emit({"event": "log", "source": "vehicle_sil_launch",
                           "line": f"left a process behind: pid {orphan.pid}"})
            elif "wait_input" in entry:
                if not self.has_accepted:
                    self.next_input()
            elif "hang" in entry:  # the recording ends while the launcher waits on: so does this, until a signal
                while self.next_input() != SIGNAL:
                    pass
                self.finish(143)
            elif "exit" in entry:
                self.finish(entry["exit"])
            if "out" in entry:
                self.side_effects(event, last=i == self.take.last_time)
        self.finish(0)

    def moved(self, event: dict) -> None:
        """The virtual time moved on after a change of recording (time events, the result's timing)."""
        if event.get("event") == "time" and isinstance(event.get("sim_ms"), int):
            event["sim_ms"] += self.shift
            if self.last_time is not None and event["sim_ms"] > self.last_sim and event.get("paused") is False:
                self.time_step = event["sim_ms"] - self.last_sim
            self.last_sim, self.last_time = event["sim_ms"], dict(event)
        result = event.get("result") if event.get("event") == "result" else None
        if self.shift and isinstance(result, dict) and isinstance(result.get("timing"), dict) and \
                isinstance(result["timing"].get("sim_ms"), int):
            result["timing"]["sim_ms"] += self.shift

    def event(self, event: dict, n: int) -> dict:
        return {k: (v.replace("{n}", str(n)) if isinstance(v, str) else v) for k, v in event.items()}

    def side_effects(self, recorded: dict, last: bool) -> None:
        """What a recorded event means for the processes and the control server (after it is written)."""
        kind = recorded.get("event")
        if kind == "control_server" and "--control" in self.argv:
            pass  # started before it was written (see emit_control)
        elif kind == "time":
            self.control_time(recorded, last)
        elif kind == "participant" and recorded.get("state") in ("ended", "killed"):
            self.end_sleeper(recorded.get("name"))
        elif kind == "phase":
            self.phase = recorded.get("phase")
            if self.phase == "stopping" and self.control is not None and self.control.server is not None:
                self.control.set_time(self.last_sim, False, running=False, ended=True)  # the simulation is over


def same_command(got: str | None, expected: str | None) -> bool:
    if got is None or expected is None:
        return got is expected
    try:
        return json.loads(got) == json.loads(expected)
    except ValueError:
        return got == expected


class Recorded(Player):
    """The pids and the control server's address are known only when the event is written."""

    def emit(self, event: dict) -> None:
        if event.get("event") == "participant" and event.get("state") == "started" and event.get("pid") == "<pid>":
            event = {**event, "pid": self.sleeper(event["name"])}
        elif event.get("event") == "hello" and event.get("pid") == "<pid>":
            event = {**event, "pid": os.getpid(), "file": self.file, "arguments": self.argv}
        elif event.get("event") == "control_server" and "--control" in self.argv and self.control is None:
            self.start_control(event)
            event = {**event, "address": self.control_address}
        super().emit(event)


def without_control(argv: list[str]) -> list[str]:
    out, i = [], 0
    while i < len(argv):
        if argv[i] in ("--control", "--control-credentials") and i + 1 < len(argv) and "--" not in argv[:i]:
            i += 2
            continue
        out.append(argv[i])
        i += 1
    return out


def scenario_control(chosen: list[tuple[dict, list[dict]]]) -> list[tuple[dict, list[dict]]]:
    """The stand-in for the feature scenario-control (the control server during a scenario), for as long as no
    recording has it: a recording of the scenario without --control, with the control_server event before the
    registry's (the control server reads as usual and refuses every write while the scenario runs)."""
    out = []
    for head, entries in chosen:
        at = events_at(entries, "registry")
        event = {"out": {"event": "control_server", "address": "<control>", "tls": False, "credentials": True}}
        out.append(({**head, "scenario_control": True}, entries[:at[0]] + [event] + entries[at[0]:] if at else entries))
    return out


def main(argv: list[str]) -> int:
    chosen = choose(argv)
    if isinstance(chosen, str) and "--scenario" in argv and "--control" in argv:
        fallback = choose(without_control(argv))
        if not isinstance(fallback, str):
            chosen = scenario_control(fallback)
    if isinstance(chosen, str):
        sys.stderr.write(chosen + "\n")
        return 2
    _, file, _ = shape(argv)
    fault = fault_of(file)
    if fault is not None and fault not in FAULTS:
        sys.stderr.write(f"replay_launcher: unknown fault '{fault}': the faults are {', '.join(FAULTS)}\n")
        return 2
    takes = [Take(head, apply_fault(fault, entries, head) if fault else entries) for head, entries in chosen]
    Recorded(argv, takes, fault).play()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
