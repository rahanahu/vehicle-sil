#!/usr/bin/env python3
"""A stand-in for vehicle_sil_launch that speaks its machine interface, for the GUI server's tests and the screen's e2e.

It is not a simulation: it reads the participant names of a configuration file and plays a scenario, writing
the same lines as `vehicle_sil_launch run --events` (docs/simulation-config.md, 「機械から使う」: the same events,
keys, order and values, a heartbeat after a second without a line, command events with source and message,
interrupted_by "stop command" / "end of input" / "output closed", hello and result also for a usage error or a
file it cannot read, time events and the result's timing) and reading the commands stop, pause, resume, step and
speed on standard input. test_real_launcher.py compares what it writes with what the real launcher writes.
Standard library only.

    fake_launcher.py version [--json]
    fake_launcher.py check [--json] [--strict] [--disable <name>]... [--enable <name>]... [--] <file>
    fake_launcher.py run [--events] [--json] [--stdin-commands [--start-paused]] [--strict] [--disable <name>]...
                         [--enable <name>]... [--speed <number|max>] [--] <file>

The scenario comes from FAKE_LAUNCHER_SCENARIO, else from a line `# fake-launcher: <scenario>` in the file,
else `pass`:

  pass               checks, starts every enabled participant, runs to stop_at_ms (or until stopped), passes
  check-error        the check finds an error, so nothing is started (result check_failed, exit code 1)
  participant-crash  the last required participant exits with code 3 on the third step: ended, then failed
                     (reason process_ended), the finding participant-failed (result failed, exit code 1)
  forever            runs until a stop command (or the end of standard input), whatever stop_at_ms says
  ignore-stop        like forever, but the participants do not end when stopped: the first stop asks them
                     to (stop_requested) and waits; the second stop kills them; it also ignores SIGTERM (a
                     launcher that is stuck), so only a kill ends it from outside
  launcher-crash     dies on the third step without writing a result (exit code 70)
  bad-lines          writes a line that is not JSON, a JSON array, an unknown event, an unknown field and a gap
                     in seq on the first step, then runs like pass
  future-events      like pass, but hello says vehicle_sil_events 2
  flood              writes FAKE_LAUNCHER_FLOOD (default 5000) log lines at once when running starts, then passes
  finding-flood      like flood, with finding events instead of log lines
  large-result       like finding-flood, but the findings are notes of the run that its result lists too (with
                     FAKE_LAUNCHER_FLOOD=40000 the result is larger than the 4 MiB the GUI server keeps)
  bad-types          writes known events whose fields have the wrong types on the first step, then passes
  orphan             like pass, but before its result it starts a process in a session of its own that keeps
                     this launcher's standard output and error open for FAKE_LAUNCHER_ORPHAN_S seconds (default
                     30); a log line says its pid
  check-garbage      `check --json` writes a line that is not JSON (exit code 0)
  non-finite         like pass, but on the first step writes a time event and a log event with a number that is
                     valid JSON but not finite (1e999, -1e999)
  result-then-wait   like pass, but after its result it keeps running (the participants have ended) until a stop
                     command or the end of standard input, then exits with the result's code
  log-dropped        like pass, with a log_dropped event on the second step (as a launcher whose reader was slow)
  silent             like pass, but on the second step it writes nothing at all, not even a heartbeat, for
                     FAKE_LAUNCHER_SILENT_S seconds (default 8): a launcher that hangs, then recovers
  large-fields       like pass, but the first participant is started with a command longer than 4 KiB (the
                     summary keeps the participant without it, and names it in omitted_fields)
  loose-fields       like pass, but on the first step writes known events whose fields that the GUI server does
                     not check are missing or of other types (the server passes them on as they are): a finding
                     with only severity and message, a registry with uri null, a participant event with a number
                     as its signal and its message, and a log event without t_ms
  late               like forever, but with a speed (a number) the third step starts FAKE_LAUNCHER_LATE_MS (default
                     200) late: a time event while it stands (late_ms), then the counts of a late step
                     (late_steps, max_late_ms, slipped_ms) and the warning behind-real-time at the end
  jitter             like forever, but with a speed (a number) every other step starts 3 ms late, within the
                     tolerance: late_ms comes and goes, no step is counted late (as the real launcher's time events
                     while a step's synchronisation takes a little longer than its real time)
  control-trouble    like pass, but of the participants that describe themselves, the first offers its variables and
                     does not say hello (no_hello, warning control-link-no-hello at the end), the second says hello in
                     another version of the messages (version_mismatch, warning control-link-version at the end) and
                     the third does not offer them (not_offered: no control in its check, note
                     control-link-not-offered in the check)
  deaf               like forever, but it never reads its standard input (on Linux its pipe is made as small as it
                     can be): a launcher that hangs while commands are written to it; only a signal ends it

Other environment variables: FAKE_LAUNCHER_TICK_MS (wall time of one 100 ms step at speed 1, and of every step
when no speed is given, default 100), FAKE_LAUNCHER_FEATURES (the features of `version --json`, comma-separated,
default the real launcher's: events,stdin-commands,check-variables,speed,start-paused,time-commands; leave some out
to stand in for an older launcher), FAKE_LAUNCHER_CHECK_DELAY_MS (how long `check` takes, default 0),
FAKE_LAUNCHER_HEARTBEAT_MS (a heartbeat after this long without a line, default 1000; 0: none).

Time: the virtual time goes on in steps of 100 ms (shorter to end at a step command's time or at stop_at_ms), and
every step writes a log line per participant. The speed is --speed, else the file's speed; without either the
fake still takes FAKE_LAUNCHER_TICK_MS per step (so that a run can be watched) and reports the speed "max", and
"max" takes no time. The controller's time step (time_step_ms, which a step command is rounded up to) is chosen
as the real launcher chooses it. A time event comes at the start (0), after every step that took time (the real
launcher writes one at most every 100 ms, so this is as often as it would), at once when the simulation stands
still or runs on, when the speed changes and when a step command has ended, and at the end; its fields are the
real launcher's. A pause stands still at once when the fake waits for the time of the next step, else after the
step it is in (as the real launcher at its next controller step). Lateness is counted only in the scenario late.

Each started participant is a real sleeping process, a child of this one, so that a test can see whether
stopping left processes behind. The configuration is read with a small line parser, not a YAML library:
top-level `stop_at_ms:`, and under `participants:` the keys name, command, describe, required, enabled, step_ms.
"""

from __future__ import annotations

import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path

VERSION = "0.1.0"
SCENARIOS = ("pass", "check-error", "participant-crash", "forever", "ignore-stop", "launcher-crash",
             "bad-lines", "future-events", "flood", "finding-flood", "bad-types", "orphan", "check-garbage",
             "non-finite", "result-then-wait", "log-dropped", "silent", "large-fields", "loose-fields",
             "large-result", "late", "jitter", "deaf", "control-trouble")
USAGE = next(p for p in __doc__.split("\n\n") if p.lstrip().startswith("fake_launcher.py version"))
STEP_MS = 100
FEATURES = "events,stdin-commands,check-variables,speed,start-paused,time-commands,control-links"
# The control links (the launcher's side of the ECUs' variables), all of their words: the event, its states, the
# keys of the check's and the result's participants[], what an ECU offers, and the codes of the findings
CONTROL_EVENT = "control_link"
CONTROL_CONNECTED, CONTROL_NO_HELLO, CONTROL_VERSION_MISMATCH, CONTROL_NOT_OFFERED, CONTROL_NONE = \
    "connected", "no_hello", "version_mismatch", "not_offered", "none"
CONTROL_RESULT_KEY = "control_link"  # the result's participants[]: {state, variables, version, late_messages}
CONTROL_CHECK_KEY = "control"  # the check's participants[]: what the ECU offers, or null
CONTROL_TRANSPORT, CONTROL_VERSION = "silkit", 1
CONTROL_NO_HELLO_CODE, CONTROL_VERSION_CODE = "control-link-no-hello", "control-link-version"  # warnings of the run
CONTROL_NOT_OFFERED_CODE = "control-link-not-offered"  # a note of the check


def control_plan(config: dict, scenario: str) -> dict[str, str]:
    """How the link of each enabled participant that describes itself will stand (scenario control-trouble: the
    first three in turn no_hello, version_mismatch, not_offered; else connected)."""
    trouble = [CONTROL_NO_HELLO, CONTROL_VERSION_MISMATCH, CONTROL_NOT_OFFERED] if scenario == "control-trouble" else []
    return {p["name"]: trouble.pop(0) if trouble else CONTROL_CONNECTED
            for p in config["participants"] if p["enabled"] and p["describe"]}


def control_link(state: str, variables: int | None = None, version: int | None = None) -> dict:
    """The result's control_link of a participant."""
    return {"state": state, "variables": variables, "version": version, "late_messages": 0}
SPEED_MIN, SPEED_MAX = 0.000001, 1000000.0
MAX_MILLIS = 10 ** 12
LATE_TOLERANCE_MS = 10
COMMANDS = ("stop", "pause", "resume", "step", "speed")
EXPECTED = 'write one JSON object per line, such as {"command":"stop"}'

# ---------------------------------------------------------------- configuration


def scalar(text: str):
    text = text.split(" #", 1)[0].strip()
    if text in ("true", "false"):
        return text == "true"
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    if text.startswith("[") and text.endswith("]"):
        return [scalar(p) for p in text[1:-1].split(",") if p.strip()]
    return text.strip("'\"")


def read_config(path: Path) -> dict:
    """{"scenario", "stop_at_ms", "participants": [...], "networks": [...]}; raises OSError, ValueError."""
    text = path.read_text(encoding="utf-8")
    config = {"scenario": None, "stop_at_ms": None, "start_timeout_ms": 30000, "stop_timeout_ms": 10000,
              "registry_uri": None, "speed": None, "time_step_ms": None, "late_tolerance_ms": None,
              "participants": [], "networks": []}
    if "vehicle_sil_simulation" not in text:
        raise ValueError("missing key 'vehicle_sil_simulation'")
    section = None
    for number, line in enumerate(text.splitlines(), start=1):
        if m := re.match(r"#\s*fake-launcher:\s*(\S+)", line):
            config["scenario"] = m.group(1)
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if not line[0].isspace():
            key, _, value = line.partition(":")
            section = key.strip()
            if section in ("stop_at_ms", "start_timeout_ms", "stop_timeout_ms", "registry_uri", "time_step_ms",
                           "late_tolerance_ms"):
                config[section] = scalar(value)
            elif section == "speed":
                speed = parse_speed(str(scalar(value)))
                if speed is None:
                    raise ValueError(f"speed must be a number from 0.000001 to 1000000 or max, not {value.strip()}")
                config["speed"] = speed
            continue
        if section not in ("participants", "networks"):
            continue
        if m := re.match(r"\s*-\s+(\w+):\s*(.*)$", line):
            config[section].append({"line": number})
        elif not (m := re.match(r"\s+(\w+):\s*(.*)$", line)) or not config[section]:
            continue
        config[section][-1][m.group(1)] = scalar(m.group(2))
    for p in config["participants"]:
        p.setdefault("command", [])
        p.setdefault("describe", True)
        p.setdefault("required", True)
        p.setdefault("enabled", True)
        p.setdefault("step_ms", None)
        p.setdefault("name", None)
    return config


def parse_speed(text: str) -> float | str | None:
    """A speed as the launcher reads it (a plain decimal number in range, or max); None when it is not one."""
    if text == "max":
        return "max"
    if not re.fullmatch(r"\d[\d.eE+-]*", text):
        return None
    try:
        value = float(text)
    except ValueError:
        return None
    return value if SPEED_MIN <= value <= SPEED_MAX else None


def number(value: float) -> float | int:
    """A number as the real launcher writes it (1, not 1.0)."""
    return int(value) if float(value).is_integer() else value


# ---------------------------------------------------------------- the check


def finding(severity, code, file, line, participant, network, message, detail=None) -> dict:
    return {"severity": severity, "code": code, "file": file, "line": line, "participant": participant,
            "network": network, "message": message, "detail": detail}


def variables(name: str) -> list[dict]:
    base = name.lower()
    return [
        {"name": f"{base}.demand", "type": "real", "causality": "input", "unit": "%", "min": 0, "max": 100,
         "step": None, "description": "requested level", "items": []},
        {"name": f"{base}.speed", "type": "integer", "causality": "output", "unit": "rpm", "min": None,
         "max": None, "step": None, "description": "", "items": []},
        {"name": f"{base}.mode", "type": "enum", "causality": "parameter", "unit": "", "min": None, "max": None,
         "step": None, "description": "operating mode",
         "items": [{"name": "off", "value": 0}, {"name": "on", "value": 1}]},
    ]


def check(file: str, config: dict, scenario: str, strict: bool) -> dict:
    networks = config["networks"]
    participants, findings = [], []
    enabled = [p for p in config["participants"] if p["enabled"]]
    plan = control_plan(config, scenario)
    for p in config["participants"]:
        entry = {"name": p["name"], "line": p["line"], "command": p["command"], "describe": p["describe"],
                 "required": p["required"], "enabled": p["enabled"], "step_ms": p["step_ms"]}
        if not p["enabled"]:
            entry.update(status="disabled", reported=None, variables=None, **{CONTROL_CHECK_KEY: None})
            findings.append(finding("note", "participant-disabled", file, p["line"], p["name"], None,
                                    f"participant '{p['name']}': disabled (enabled: false, or --disable): it is "
                                    "neither checked nor started, and the other findings are about the simulation "
                                    "without it"))
        elif not p["describe"]:
            entry.update(status="not_checked", reported=None, variables=None, **{CONTROL_CHECK_KEY: None})
            findings.append(finding("note", "not-checked", file, p["line"], p["name"], None,
                                    f"participant '{p['name']}': describe is false, so it was not checked"))
        else:
            step = p["step_ms"] or 10
            entry.update(status="described", reported={
                "vehicle_sil": VERSION, "name": p["name"], "step_ns": step * 1_000_000,
                "networks": [{"name": n["name"], "kind": n.get("kind"), "use": "raw", "nodes": []}
                             for n in networks]}, variables=variables(p["name"]))
            offered = plan.get(p["name"]) != CONTROL_NOT_OFFERED
            entry[CONTROL_CHECK_KEY] = {"transport": CONTROL_TRANSPORT, "wire_version": CONTROL_VERSION} \
                if offered else None
            if not offered:
                findings.append(finding("note", CONTROL_NOT_OFFERED_CODE, file, p["line"], p["name"], None,
                                        f"participant '{p['name']}': it has variables but does not offer them to "
                                        "the simulation (--no-control, or an executable too old to say): run cannot "
                                        "read or write them"))
        participants.append(entry)
    if scenario == "check-error":
        target = next((p for p in enabled if p["describe"]), enabled[0] if enabled else None)
        name = target["name"] if target else None
        findings.insert(0, finding("error", "step-mismatch", file, target["line"] if target else None, name, None,
                                   f"participant '{name}': step_ms is 20, but the executable's step is 10 ms: "
                                   "write step_ms: 10, or remove step_ms"))
    if not any(p["required"] for p in enabled):
        findings.insert(0, finding("error", "no-required-participant", file, None, None, None,
                                   "no enabled participant is required, so the simulation would wait for none: "
                                   "enable a participant, or set required: true on one"))
    if len(enabled) == 1:
        for n in networks:
            findings.append(finding("warning", "single-participant", file, n["line"], None, n["name"],
                                    f"network '{n['name']}': only {enabled[0]['name']} is on it, so what it sends "
                                    "there reaches no one: add the other participants of this network, or remove it"))
    order = {"error": 0, "warning": 1, "note": 2}
    findings.sort(key=lambda f: order[f["severity"]])
    counts = {s: sum(f["severity"] == s for f in findings) for s in order}
    failed = counts["error"] or (strict and counts["warning"])
    return {
        "vehicle_sil_check": 1, "vehicle_sil": VERSION, "file": file, "result": "failed" if failed else "passed",
        "strict": strict, "errors": counts["error"], "warnings": counts["warning"], "notes": counts["note"],
        "stop_at_ms": config["stop_at_ms"], "start_timeout_ms": config["start_timeout_ms"],
        "stop_timeout_ms": config["stop_timeout_ms"], "registry_uri": config["registry_uri"],
        "participants": participants,
        "networks": [{"name": n["name"], "kind": n.get("kind"), "line": n["line"],
                      "participants": [p["name"] for p in enabled]} for n in networks],
        "findings": findings,
    }


def unreadable_check(file: str | None, result: str, code: str, message: str, strict: bool) -> dict:
    return {"vehicle_sil_check": 1, "vehicle_sil": VERSION, "file": file, "result": result, "strict": strict,
            "errors": 1, "warnings": 0, "notes": 0, "stop_at_ms": None, "start_timeout_ms": None,
            "stop_timeout_ms": None, "registry_uri": None, "participants": [], "networks": [],
            "findings": [finding("error", code, file, None, None, None, message)]}


# ---------------------------------------------------------------- arguments


class Usage(Exception):
    pass


def parse(argv: list[str]) -> dict:
    if not argv:
        raise Usage("no command: write version, check or run")
    command, rest = argv[0], argv[1:]
    flags = {"version": {"--json"},
             "check": {"--json", "--strict"},
             "run": {"--json", "--events", "--stdin-commands", "--strict", "--start-paused"}}
    valued = {"version": set(), "check": {"--disable", "--enable"},
              "run": {"--disable", "--enable", "--speed"}}
    if command not in flags:
        raise Usage(f"unknown command '{command}': write version, check or run")
    args = {"command": command, "disable": [], "enable": [], "files": [], "speed": None}
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == "--":
            args["files"] += rest[i + 1:]
            break
        if a in flags[command]:
            args[a[2:].replace("-", "_")] = True
        elif a in valued[command]:
            if i + 1 >= len(rest):
                raise Usage(f"{a} needs a value")
            i += 1
            if a == "--speed":
                speed = parse_speed(rest[i])
                if speed is None:
                    raise Usage(f"--speed must be a number of simulation seconds per wall-clock second from 0.000001 "
                                f"to 1000000 (1: real time, 2: twice as fast) or max (as fast as possible), not "
                                f"'{rest[i]}'")
                args["speed"] = speed
            else:
                args[a[2:]].append(rest[i])
        elif a.startswith("-"):
            raise Usage(f"unknown argument '{a}'")
        else:
            args["files"].append(a)
        i += 1
    if args.get("json") and args.get("events"):
        raise Usage("--json and --events cannot be used together")
    if args.get("start_paused") and not args.get("stdin_commands"):
        raise Usage("--start-paused needs --stdin-commands (resume or step it with a command)")
    if command != "version" and len(args["files"]) != 1:
        raise Usage("write one configuration file")
    return args


# ---------------------------------------------------------------- run


class Events:
    """The event stream: seq and writing under one lock, a heartbeat after HEARTBEAT_S without a line (from hello
    to the result), nothing after the result. A reader that has gone away closes the stream; the run then stops
    as if told to (interrupted_by "output closed")."""

    def __init__(self, enabled: bool):
        self.enabled = enabled
        self.seq = 0
        self.start = time.monotonic()
        self.closed = False
        self.sealed = False
        self.muted_until = 0.0  # scenario silent: nothing at all is written before this time (no heartbeat either)
        self.last_write = self.start
        self.lock = threading.RLock()
        self.on_closed = lambda: None
        heartbeat_ms = int(os.environ.get("FAKE_LAUNCHER_HEARTBEAT_MS", "1000"))
        if enabled and heartbeat_ms > 0:
            threading.Thread(target=self._heartbeat, args=(heartbeat_ms / 1000,), daemon=True).start()

    def t_ms(self) -> int:
        return int((time.monotonic() - self.start) * 1000)

    def emit(self, event: str, **fields) -> None:
        with self.lock:
            self._wait_muted()
            self.write({"event": event, "seq": self.seq, "t_ms": self.t_ms(), **fields})
            self.seq += 1
            if event == "result" and isinstance(fields.get("result"), dict):
                self.sealed = True

    def raw(self, line: str) -> None:
        with self.lock:
            if self.enabled and not self.closed and not self.sealed:
                self._write_line(line)

    def write(self, obj: dict) -> None:
        if self.enabled and not self.closed and not self.sealed:
            self._write_line(json.dumps(obj, ensure_ascii=False, separators=(",", ":")))

    def _wait_muted(self) -> None:
        while time.monotonic() < self.muted_until:
            time.sleep(0.01)

    def _write_line(self, line: str) -> None:
        try:
            sys.stdout.write(line + "\n")
            sys.stdout.flush()
            self.last_write = time.monotonic()
        except (BrokenPipeError, OSError):
            self.closed = True  # the reader is gone: stop, as a stop command would
            try:
                sys.stdout = open(os.devnull, "w")
            except OSError:
                pass
            self.on_closed()

    def _heartbeat(self, every_s: float) -> None:
        while not self.sealed and not self.closed:
            time.sleep(min(0.05, every_s / 4))
            now = time.monotonic()
            if now < self.muted_until or now - self.last_write < every_s:
                continue
            with self.lock:
                if not self.sealed and time.monotonic() - self.last_write >= every_s:
                    self.emit("heartbeat")


class Stops:
    """The stop requests (stop commands, the end of standard input, a closed output): how many, and the first's
    source as interrupted_by says it."""

    def __init__(self):
        self.lock = threading.Lock()
        self.count = 0
        self.by: str | None = None

    def add(self, by: str, only_first: bool = False) -> bool:
        with self.lock:
            if only_first and self.count:
                return False
            self.count += 1
            self.by = self.by or by
            return True


class TimeGate:
    """The controller's time as the fake keeps it, shared by the run loop and the reader of the commands (one
    condition). The commands decide here, as the real launcher's do: pause stands still at the next step, resume
    runs on, step runs to a time and stands still again (only while paused), speed paces from the next step.
    Before the run starts, pause, resume and speed are kept for its start; after the simulation, none is taken."""

    def __init__(self, speed: float | str | None, paused: bool):
        self.cond = threading.Condition()
        self.speed = speed  # a number, "max", or None: no speed given (the fake's own pace, reported as "max")
        self.paused = paused  # as the commands see it: paused (or started paused) and not resumed
        self.target: int | None = 0 if paused else None  # stand still at this time (None and paused: at the next step)
        self.sim_ms = 0
        self.step_ms: int | None = None  # the controller's time step, once the simulation runs
        self.detached = False  # the simulation is over

    def attach(self, step_ms: int) -> None:
        with self.cond:
            self.step_ms = step_ms

    def detach(self) -> None:
        with self.cond:
            self.detached = True
            self.paused, self.target = False, None
            self.cond.notify_all()

    def refusal(self, name: str) -> str | None:
        if self.detached:
            return (f"the simulation is not running (it is over, or it did not start): {name} works from the start "
                    "of the run until the simulation ends")
        if name == "step" and self.step_ms is None:
            return ("the simulation has not been set up yet: step works once it stands still (a time event with "
                    "paused true), with --start-paused from the start")
        if name == "step" and not self.paused:
            return ('step works while the simulation is paused: send {"command":"pause"} first (or run with '
                    "--start-paused)")
        return None

    def apply(self, name: str, ms: int | None, speed) -> int | None:
        """Applies an accepted time command (under the condition); a step's milliseconds, rounded up to whole
        steps of the controller."""
        if name == "pause":
            self.paused = True
            self.target = None if self.step_ms is not None else 0
        elif name == "resume":
            self.paused, self.target = False, None
        elif name == "speed":
            self.speed = speed
        elif name == "step":
            ms = -(-ms // self.step_ms) * self.step_ms
            start = self.target if self.target is not None and self.target > self.sim_ms else self.sim_ms
            self.target = start + ms
        self.cond.notify_all()
        return ms


def read_command(line: str) -> tuple[str | None, dict, str | None]:
    """(the name or None, the command's values, why it is refused or None) of one line, as the real launcher reads
    it: step needs ms, a whole number; speed needs value, a number or "max"; other keys are ignored."""
    try:
        command = json.loads(line)
    except ValueError:
        return None, {}, f"the line is not valid JSON: {EXPECTED}"
    if not isinstance(command, dict):
        return None, {}, f"the line is not a JSON object: {EXPECTED}"
    name = command.get("command")
    if not isinstance(name, str):
        return None, {}, ('the object has no "command"' if name is None else '"command" is not a string') + \
            f": {EXPECTED}"
    if name == "step":
        ms = command.get("ms")
        if isinstance(ms, bool) or not isinstance(ms, int) or not 1 <= ms <= MAX_MILLIS:
            return name, {}, (f'step needs "ms", a whole number of milliseconds of virtual time from 1 to {MAX_MILLIS}'
                              ', such as {"command":"step","ms":100}')
        return name, {"ms": ms}, None
    if name == "speed":
        value = command.get("value")
        speed = "max" if value == "max" else parse_speed(repr(value)) \
            if isinstance(value, (int, float)) and not isinstance(value, bool) else None
        if speed is None:
            return name, {}, ('speed needs "value", a number of simulation seconds per wall-clock second from 0.000001 '
                              'to 1000000 or "max", such as {"command":"speed","value":1}')
        return name, {"speed": speed}, None
    if name not in COMMANDS:
        shown = name if len(name) <= 64 else name[:64] + "..."
        return name, {}, f"unknown command '{shown}': the commands are stop, pause, resume, step and speed"
    return name, {}, None


def settle_input(stops: Stops, limit_s: float = 1.0) -> None:
    """The real launcher's check runs every ECU's --describe, so what is already on standard input when it starts
    (the end of a closed input, early commands) has been read when it ends; the fake's check takes no time, so it
    waits here, up to limit_s, for the reader to take what is waiting (POSIX; elsewhere it does not wait)."""
    try:
        import select
        end = time.monotonic() + limit_s
        while time.monotonic() < end and not stops.count and select.select([sys.stdin], [], [], 0)[0]:
            time.sleep(0.01)
    except (ImportError, OSError, ValueError):
        pass


def read_commands(ev: Events, stops: Stops, gate: TimeGate) -> None:
    """The standard input of --stdin-commands: a command event at once for every line (and for the end of the
    input when it is the first stop request), before what the command does."""
    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        name, values, wrong = read_command(line)
        with ev.lock:  # the command event before anything the command makes happen
            if wrong is not None:
                ev.emit("command", command=name, source="stdin", accepted=False, message=wrong)
            elif name == "stop":
                stops.add("stop command")
                ev.emit("command", command="stop", source="stdin", accepted=True, message=None)
            else:
                with gate.cond:
                    refused = gate.refusal(name)
                    if refused is not None:
                        ev.emit("command", command=name, source="stdin", accepted=False, message=refused)
                        continue
                    ms = values.get("ms")
                    if name == "step":
                        ms = -(-ms // gate.step_ms) * gate.step_ms
                        ev.emit("command", command=name, source="stdin", accepted=True, message=None, ms=ms)
                    else:
                        ev.emit("command", command=name, source="stdin", accepted=True, message=None)
                    gate.apply(name, values.get("ms"), values.get("speed"))
    with ev.lock:
        if stops.add("end of input", only_first=True):
            ev.emit("command", command="stop", source="eof", accepted=True, message=None)


def sleeper() -> subprocess.Popen:
    return subprocess.Popen([sys.executable, "-c", "import time\nwhile True: time.sleep(60)"],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def started_command(file: str, p: dict, uri: str, config: dict) -> list[str]:
    """The command as the launcher starts it: the executable found (relative to the file's directory, or on PATH
    for a bare name), and for an ecu_main executable the arguments the launcher adds."""
    command = [str(c) for c in p["command"]] or ["?"]
    directory = os.path.dirname(os.path.abspath(file))
    exe = command[0]
    if os.path.isabs(exe):
        found = exe
    elif "/" in exe or "\\" in exe or os.path.exists(os.path.join(directory, exe)):
        found = os.path.join(directory, exe)
    else:
        found = shutil.which(exe) or os.path.join(directory, exe)
    args = [a.replace("{registry_uri}", uri).replace("{name}", p["name"]) for a in command[1:]]
    if p["describe"]:
        timeout = (config["start_timeout_ms"] or 30000) + (config["stop_timeout_ms"] or 10000) + 10000
        args += ["--name", p["name"], "--registry-uri", uri, "--start-timeout-ms", str(timeout)]
    return [found, *args]


def plan_time_step(config: dict, speed) -> dict:
    """The controller's time step and what it means for the ECUs, chosen as the real launcher chooses them (with a
    speed: the greatest common divisor of 10 ms, stop_at_ms and the ECUs' steps; 1 ms when a step is not known)."""
    enabled = [p for p in config["participants"] if p["enabled"]]
    known = all(p["describe"] for p in enabled)
    steps = [p["step_ms"] or 10 for p in enabled if p["describe"]]
    coarsest = math.gcd(10, config["stop_at_ms"]) if config["stop_at_ms"] else 10
    fine = (math.gcd(coarsest, *steps) if steps else coarsest) if known else 1
    paced = isinstance(speed, float)
    step = config["time_step_ms"] or (fine if paced else coarsest)
    delay = max((step - math.gcd(step, s) for s in steps), default=0) if known else None
    return {"time_step_ms": step, "ecu_step_ms": min(steps) if known and steps else None, "step_delay_ms": delay,
            "fine_step_ms": fine, "written": config["time_step_ms"] is not None}


def finding_order(f: dict) -> tuple:
    """The order of the real launcher's findings: errors, warnings, notes; then by line (none first); stable."""
    return {"error": 0, "warning": 1, "note": 2}.get(f["severity"], 0), f["line"] is not None, f["line"] or 0


def run(args: dict, file: str, config: dict, scenario: str, ev: Events, stops: Stops, gate: TimeGate) -> int:
    if scenario == "ignore-stop":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    strict = bool(args.get("strict"))
    result_check = check(file, config, scenario, strict)
    ev.emit("check", check=result_check)
    participants = []
    for p in config["participants"]:
        participants.append({"name": p["name"], "required": p["required"], "enabled": p["enabled"],
                             "ended": "not_started", "exit_code": None, "signal": None, "left_processes": False,
                             "log": None, CONTROL_RESULT_KEY: control_link(CONTROL_NONE)})
    run_findings: list[dict] = []
    timing: dict | None = None  # the result's timing, once the simulation has ended

    def result(outcome: str, simulation, interrupted_by=None) -> dict:
        everything = result_check["findings"] + run_findings
        return {"vehicle_sil_run": 1, "vehicle_sil": VERSION, "file": file, "result": outcome, "strict": strict,
                "errors": sum(f["severity"] == "error" for f in everything),
                "warnings": sum(f["severity"] == "warning" for f in everything),
                "notes": sum(f["severity"] == "note" for f in everything),
                "simulation": simulation, "interrupted_by": interrupted_by, "timing": timing,
                "participants": participants, "findings": sorted(run_findings, key=finding_order),
                "check": result_check}

    def finish(outcome: str, simulation, code: int, interrupted_by=None) -> int:
        gate.detach()
        ev.emit("phase", phase="ended")
        failed = outcome == "passed" and strict and any(f["severity"] == "warning" for f in run_findings)
        r = result("failed" if failed else outcome, simulation, interrupted_by)
        ev.emit("result", result=r)
        if args.get("json"):
            print(json.dumps(r, indent=2, ensure_ascii=False))
        return 1 if failed else code

    def say(line: str) -> None:
        ev.emit("log", source="vehicle_sil_launch", line=line)

    def note(*found: dict) -> None:
        """Findings that are known at the same time: written in the order of the result."""
        for f in sorted(found, key=finding_order):
            run_findings.append(f)
            ev.emit("finding", **f)

    def interrupted(started: bool) -> dict:
        by = stops.by or "stop command"
        if started:
            say(f"interrupted ({by}): the simulation was stopped")
        return finding("note", "interrupted", file, None, None, None,
                       f"interrupted ({by}): the launcher stopped the simulation and the participants" if started
                       else f"interrupted ({by}): the launcher stopped the participants before the simulation "
                            "started")

    if result_check["result"] != "passed":
        participants.clear()  # nothing was started: the result lists no participant
        return finish("check_failed", None, 1)
    if args.get("stdin_commands"):
        settle_input(stops)
    if stops.count:  # a stop during the check takes effect after it: nothing is started
        note(interrupted(False))
        return finish("interrupted", "not_started", 130, stops.by)

    for p in participants:
        if not p["enabled"]:
            ev.emit("participant", name=p["name"], state="disabled")
    ev.emit("phase", phase="starting")
    uri = "silkit://127.0.0.1:8500"  # the real launcher takes a free port; a fixed one keeps the examples stable
    ev.emit("registry", uri=uri)
    say(f"registry at {uri}")
    running = {}
    by_name = {p["name"]: p for p in config["participants"]}
    for p in participants:
        if p["enabled"]:
            command = started_command(file, by_name[p["name"]], uri, config)
            if scenario == "large-fields" and not running:
                command.append("--padding=" + "x" * 5000)
            say(f"starting {p['name']}: {' '.join(command)}")
            proc = sleeper()
            running[p["name"]] = proc
            ev.emit("participant", name=p["name"], state="started", pid=proc.pid, command=command, log=None)
        else:
            say(f"not starting {p['name']}: disabled")
    ev.emit("phase", phase="waiting")
    for name in running:
        if by_name[name]["describe"]:  # only participants with a lifecycle join
            ev.emit("participant", name=name, state="joined")
    # Where each started participant's control link stands, in the file's order, before the simulation runs
    plan = control_plan(config, scenario)
    for name in running:
        state = plan.get(name, CONTROL_NONE)
        extra = {"variables": len(variables(name))} if state == CONTROL_CONNECTED else \
            {"version": CONTROL_VERSION + 1} if state == CONTROL_VERSION_MISMATCH else {}
        next(q for q in participants if q["name"] == name)[CONTROL_RESULT_KEY] = control_link(state, **extra)
        ev.emit(CONTROL_EVENT, name=name, state=state, **extra)
    ev.emit("phase", phase="running")
    say("simulation running")

    with gate.cond:
        plan = plan_time_step(config, gate.speed)
    gate.attach(plan["time_step_ms"])
    tolerance = config["late_tolerance_ms"] or LATE_TOLERANCE_MS
    pace_s = int(os.environ.get("FAKE_LAUNCHER_TICK_MS", "100")) / 1000 / STEP_MS  # per ms of virtual time
    stop_at = None if scenario in ("forever", "ignore-stop", "late", "jitter", "deaf") else config["stop_at_ms"]
    began = time.monotonic()
    clock = {"sim": 0, "step": 0, "paced": 0, "late_steps": 0, "max_late": 0, "slipped": 0, "late": 0,
             "paused_s": 0.0, "standing_since": None, "over": False}
    shown: dict = {}  # the last time event's values
    samples: deque = deque()  # (wall, sim) while it runs at one speed: the actual speed of about the last second

    def seconds_per_ms(speed) -> float:
        return 0.0 if speed == "max" else pace_s / speed if isinstance(speed, float) else pace_s

    def note_time(paused: bool, speed, moved: bool = False) -> None:
        """A time event when it stands still or runs on, the speed changes, it stands still at another time, the
        lateness changes; or when it moved (moved: after a step that took time, and at the end)."""
        if clock["over"]:
            return
        now, sim = time.monotonic(), clock["sim"]
        lateness = (clock["late_steps"], clock["max_late"], clock["slipped"], clock["late"])
        if paused or paused != shown.get("paused") or speed != shown.get("speed"):
            samples.clear()
        if not paused:
            samples.append((now, sim))
            while len(samples) > 2 and now - samples[1][0] >= 1.0:
                samples.popleft()
        changed = not shown or paused != shown["paused"] or speed != shown["speed"] or \
            (paused and sim != shown["sim"]) or lateness != shown["lateness"]
        if not changed and not (moved and sim != shown["sim"]):
            return
        actual = None
        if len(samples) >= 2 and samples[-1][0] - samples[0][0] >= 0.2:
            actual = round((samples[-1][1] - samples[0][1]) / 1000 / (samples[-1][0] - samples[0][0]), 3)
        ev.emit("time", sim_ms=sim, speed=number(speed) if isinstance(speed, float) else "max", paused=paused,
                actual_speed=actual, late_steps=clock["late_steps"], max_late_ms=clock["max_late"],
                slipped_ms=clock["slipped"], late_ms=clock["late"])
        shown.update(sim=sim, paused=paused, speed=speed, lateness=lateness)

    def end_simulation() -> None:
        """The simulation is over: its timing, the last time event, no more time commands."""
        nonlocal timing
        if clock["over"]:
            return
        with gate.cond:
            speed = gate.speed
        if clock["standing_since"] is not None:
            clock["paused_s"] += time.monotonic() - clock["standing_since"]
            clock["standing_since"] = None
        note_time(False, speed, moved=True)
        clock["over"] = True
        gate.detach()
        wall = int((time.monotonic() - began) * 1000)
        paused_ms = int(clock["paused_s"] * 1000)
        ran = wall - paused_ms
        timing = {"speed": number(speed) if isinstance(speed, float) else "max",
                  "time_step_ms": plan["time_step_ms"], "ecu_step_ms": plan["ecu_step_ms"],
                  "step_delay_ms": plan["step_delay_ms"], "late_tolerance_ms": tolerance, "sim_ms": clock["sim"],
                  "wall_ms": wall, "actual_speed": round(clock["sim"] / ran, 3) if ran >= 1 else None,
                  "paced_steps": clock["paced"], "late_steps": clock["late_steps"], "max_late_ms": clock["max_late"],
                  "slipped_ms": clock["slipped"], "late_ms": clock["late"], "paused_ms": paused_ms}

    def timing_findings() -> list[dict]:
        """The findings decided when the simulation ends: the control links, then the time (with the conditions of
        the real launcher)."""
        out = []
        for p in participants:
            line = by_name[p["name"]]["line"]
            if p[CONTROL_RESULT_KEY]["state"] == CONTROL_NO_HELLO:
                out.append(finding("warning", CONTROL_NO_HELLO_CODE, file, line, p["name"], None,
                                   f"participant '{p['name']}': it describes variables (--describe), but its control "
                                   "agent did not say hello before the simulation started, so its variables cannot "
                                   "be read or written in this run"))
            elif p[CONTROL_RESULT_KEY]["state"] == CONTROL_VERSION_MISMATCH:
                out.append(finding("warning", CONTROL_VERSION_CODE, file, line, p["name"], None,
                                   f"participant '{p['name']}': its control agent speaks version "
                                   f"{CONTROL_VERSION + 1} of the control messages and this launcher version "
                                   f"{CONTROL_VERSION}, so its variables cannot be read or written in this run"))
        if clock["late_steps"]:
            out.append(finding("warning", "behind-real-time", file, None, None, None,
                               f"{clock['late_steps']} of {clock['paced']} paced steps were more than {tolerance} ms "
                               f"late (the latest by {clock['max_late']} ms), so the simulation came "
                               f"{clock['slipped']} ms behind real time in all: lower the speed, or find what is slow"))
        # A speed at the end, or steps paced before (a run started with a speed counts even if it never moved)
        if (timing["speed"] != "max" or clock["paced"] > 0) and plan["step_delay_ms"] and \
                plan["fine_step_ms"] < plan["time_step_ms"]:
            out.append(finding("warning", "coarse-time-step", file, None, None, None,
                               f"the controller's time step is {plan['time_step_ms']} ms, so an ECU step may run up "
                               f"to {plan['step_delay_ms']} ms late in real time: write time_step_ms: "
                               f"{plan['fine_step_ms']}"))
        return out

    def entry(name: str) -> dict:
        return next(p for p in participants if p["name"] == name)

    def end_participants(how: str) -> None:
        for name, proc in running.items():
            if entry(name)["ended"] != "not_started":
                continue
            proc.kill()
            proc.wait()
            if how == "killed":
                say(f"killed {name}")
                entry(name).update(ended="killed")
                ev.emit("participant", name=name, state="killed")
            else:
                say(f"{name} exited with code 0")
                entry(name).update(ended="exited", exit_code=0)
                ev.emit("participant", name=name, state="ended", exit_code=0, signal=None)

    with gate.cond:  # read under the condition, written outside it (the reader of the commands takes both)
        paused, speed = gate.paused, gate.speed
    note_time(paused, speed)
    asked = False
    while True:
        # A stop decides what this step does.
        if stops.count and scenario == "ignore-stop" and not asked:
            asked = True  # the participants do not end: the launcher asks them to stop, and waits
            end_simulation()
            note(interrupted(True), *timing_findings())
            ev.emit("phase", phase="stopping")
            for name in running:
                say(f"asking {name} to stop (SIGTERM)")
                ev.emit("participant", name=name, state="stop_requested")
        if stops.count and (scenario != "ignore-stop" or stops.count >= 2):
            if not asked:
                end_simulation()
                note(interrupted(True), *timing_findings())
                ev.emit("phase", phase="stopping")
            end_participants("killed" if scenario == "ignore-stop" else "ended")
            return finish("interrupted", "interrupted", 130, stops.by)

        with gate.cond:
            if gate.paused and gate.target is None:
                gate.target = clock["sim"]  # a pause stands still at the step it is at
            target, speed = gate.target, gate.speed
        if target is not None and clock["sim"] >= target:  # standing still: until a command or a stop
            if clock["standing_since"] is None:
                clock["standing_since"] = time.monotonic()
            note_time(True, speed)
            seen = stops.count
            with gate.cond:
                if (gate.target, gate.paused, gate.speed) == (target, True, speed) and stops.count == seen:
                    gate.cond.wait(0.05)
            continue
        if clock["standing_since"] is not None:
            clock["paused_s"] += time.monotonic() - clock["standing_since"]
            clock["standing_since"] = None
        advance = STEP_MS
        if target is not None:
            advance = min(advance, target - clock["sim"])
        if stop_at is not None:
            advance = min(advance, max(stop_at - clock["sim"], 1))
        if not (target is not None and speed == "max"):  # a step at max speed ends before anyone could see it
            note_time(False, speed)
        # The real time of the step: a pause or a stop meanwhile comes before the step.
        deadline = time.monotonic() + seconds_per_ms(speed) * advance
        seen, held = stops.count, False
        while (left := deadline - time.monotonic()) > 0 and stops.count == seen:
            with gate.cond:
                if gate.paused and gate.target is None:
                    held = True
                    break
                gate.cond.wait(min(left, 0.02))
        if held or stops.count != seen:
            continue

        clock["step"] += 1
        clock["sim"] += advance
        step, sim_ms = clock["step"], clock["sim"]
        with gate.cond:
            gate.sim_ms = sim_ms
        if isinstance(speed, float):
            clock["paced"] += advance // plan["time_step_ms"]
        if scenario == "jitter" and isinstance(speed, float):
            clock["late"] = 3 if step % 2 else 0  # within the tolerance: not a late step
        if scenario == "late" and step == 3 and isinstance(speed, float):
            late = int(os.environ.get("FAKE_LAUNCHER_LATE_MS", "200"))
            time.sleep(late / 1000)  # the next step starts this late: counted while it stands, and after
            clock.update(late=late, late_steps=clock["late_steps"] + (late > tolerance),
                         max_late=max(clock["max_late"], late), slipped=clock["slipped"] + late)
            note_time(False, speed)
            clock["late"] = 0
        if scenario == "bad-lines" and step == 1:
            ev.raw("this line is not JSON")
            ev.raw("[1, 2, 3]")
            ev.emit("mystery", detail="an event this reader does not know")
            with ev.lock:
                ev.seq += 1  # a gap
            ev.emit("log", source=next(iter(running), "vehicle_sil_launch"), line="after a gap", colour="blue")
        if scenario == "flood" and step == 1:
            for n in range(int(os.environ.get("FAKE_LAUNCHER_FLOOD", "5000"))):
                ev.emit("log", source=next(iter(running)), line=f"flood line {n}")
        if scenario == "finding-flood" and step == 1:
            for n in range(int(os.environ.get("FAKE_LAUNCHER_FLOOD", "5000"))):
                ev.emit("finding", **finding("note", "flood", file, None, None, None, f"flood finding {n}"))
        if scenario == "large-result" and step == 1:
            for n in range(int(os.environ.get("FAKE_LAUNCHER_FLOOD", "5000"))):
                note(finding("note", "flood", file, None, None, None, f"flood finding {n}"))
        if scenario == "log-dropped" and step == 2:
            first = next(iter(running))
            ev.emit("log_dropped", count=125, sources={first: 120, "vehicle_sil_launch": 5})
        if scenario == "silent" and step == 2:
            ev.muted_until = time.monotonic() + float(os.environ.get("FAKE_LAUNCHER_SILENT_S", "8"))
        if scenario == "bad-types" and step == 1:
            for kind, fields in (("check", {"check": {"participants": 5}}), ("participant", {"name": 5}),
                                 ("participant", {"name": "Engine", "state": ["x"]}), ("log", {"source": None}),
                                 ("time", {"sim_ms": "100"}), ("phase", {"phase": {}}), ("result", {"result": []}),
                                 ("command", {"command": "stop", "accepted": "yes"}), ("finding", {"severity": 1})):
                ev.emit(kind, **fields)
        if scenario == "loose-fields" and step == 1:
            first = next(iter(running))
            ev.emit("finding", severity="note", message="a finding with only a severity and a message")
            ev.emit("registry", uri=None)
            ev.emit("participant", name=first, state="joined", signal=9, message=5)
            with ev.lock:
                ev.raw(json.dumps({"event": "log", "seq": ev.seq, "source": "vehicle_sil_launch",
                                   "line": "a log event without t_ms"}, separators=(",", ":")))
                ev.seq += 1
        if scenario == "non-finite" and step == 1:
            for fields in ('"event":"time","sim_ms":50,"actual_speed":1e999', '"event":"log","source":"vehicle_sil_launch",'
                           '"line":"x","level":-1e999'):
                with ev.lock:
                    ev.raw(f'{{{fields},"seq":{ev.seq},"t_ms":0}}')
                    ev.seq += 1
        if scenario == "orphan" and step == 1:
            orphan = subprocess.Popen(
                [sys.executable, "-c", f"import time; time.sleep({float(os.environ.get('FAKE_LAUNCHER_ORPHAN_S', '30'))})"],
                stdin=subprocess.DEVNULL, start_new_session=True)
            say(f"left a process behind: pid {orphan.pid}")
        if scenario == "launcher-crash" and step == 3:
            sys.stderr.write("fake_launcher: dying on purpose (scenario launcher-crash)\n")
            sys.stderr.flush()
            os._exit(70)
        for name in running:
            if entry(name)["ended"] == "not_started":
                ev.emit("log", source=name, line=f"step {step}: t = {sim_ms} ms")
        reached = stop_at is not None and sim_ms >= stop_at
        if target is not None and sim_ms >= target and not reached:
            note_time(True, speed)  # a step command has ended: it stands still here
        elif speed != "max" and not reached:
            note_time(False, speed, moved=True)
        if scenario == "participant-crash" and step == 3:
            name = [p["name"] for p in participants if p["enabled"] and p["required"]][-1]
            ev.emit("log", source=name, line="error: something went wrong, exiting with code 3")
            running[name].kill()
            running[name].wait()
            entry(name).update(ended="exited", exit_code=3)
            say(f"{name} exited with code 3")
            ev.emit("participant", name=name, state="ended", exit_code=3, signal=None)
            f = finding("error", "participant-failed", file, by_name[name]["line"], name, None,
                        f"participant '{name}': it failed while the simulation was running, and the simulation "
                        "was aborted: see its log")
            ev.emit("participant", name=name, state="failed", reason="process_ended", message=f["message"])
            say(f"participant '{name}' failed; aborting the simulation")
            end_simulation()
            note(f, *timing_findings())
            ev.emit("phase", phase="stopping")
            end_participants("ended")
            return finish("failed", "aborted", 1)
        if reached:
            end_simulation()
            say("the simulation ended")
            note(*timing_findings())
            ev.emit("phase", phase="stopping")
            end_participants("ended")
            code = finish("passed", "reached_stop_at", 0)
            while scenario == "result-then-wait" and stops.count == 0:  # ended, but runs on until told to stop
                time.sleep(0.02)
            return code


# ---------------------------------------------------------------- main


def usage_result(file: str | None, outcome: str, code: str, message: str, strict: bool,
                 checked: dict | None = None) -> dict:
    return {"vehicle_sil_run": 1, "vehicle_sil": VERSION, "file": file, "result": outcome, "strict": strict,
            "errors": 1, "warnings": 0, "notes": 0, "simulation": None, "interrupted_by": None, "timing": None,
            "participants": [], "findings": [], "check": checked or unreadable_check(file, outcome, code, message,
                                                                                     strict)}


VALUED = {"--disable", "--enable", "--speed", "--log-dir", "--timeout-ms", "--describe-timeout-ms"}


def has_flag(argv: list[str], flag: str) -> bool:
    """Whether the flag is given as an option (not as the value of another option, not after --)."""
    i = 1
    while i < len(argv) and argv[i] != "--":
        if argv[i] == flag:
            return True
        i += 2 if argv[i] in VALUED else 1
    return False


def file_argument(argv: list[str]) -> str | None:
    """The configuration file named on the command line, as hello says it (None: none)."""
    valued = VALUED
    i = 1
    while i < len(argv):
        if argv[i] == "--":
            return argv[i + 1] if i + 1 < len(argv) else None
        if argv[i] in valued:
            i += 2
            continue
        if not argv[i].startswith("-"):
            return argv[i]
        i += 1
    return None


def peek_scenario(file: str | None) -> str | None:
    """The scenario line of the file, before the file is read as a configuration (hello comes first)."""
    try:
        m = re.search(r"^#\s*fake-launcher:\s*(\S+)", Path(file).read_text(encoding="utf-8"), re.MULTILINE)
    except (OSError, ValueError, TypeError):
        return None
    return m.group(1) if m else None


def main(argv: list[str]) -> int:
    json_wanted = has_flag(argv, "--json")
    events_wanted = bool(argv) and argv[0] == "run" and has_flag(argv, "--events") and not json_wanted
    ev = Events(events_wanted)
    stops = Stops()
    ev.on_closed = lambda: stops.add("output closed", only_first=True)
    file_arg = file_argument(argv)
    if events_wanted:
        future = (os.environ.get("FAKE_LAUNCHER_SCENARIO") or peek_scenario(file_arg)) == "future-events"
        ev.emit("hello", vehicle_sil_events=2 if future else 1,
                vehicle_sil=VERSION, pid=os.getpid(), file=file_arg, arguments=argv)
    try:
        args = parse(argv)
    except Usage as e:
        if json_wanted and argv and argv[0] == "check":
            print(json.dumps(unreadable_check(file_arg, "usage", "usage", str(e), False), indent=2))
        sys.stderr.write(f"error: {e}\n{USAGE}\n")
        ev.emit("result", result=usage_result(file_arg, "usage", "usage", str(e), False))
        return 4
    if args["command"] == "version":
        if args.get("json"):
            features = os.environ.get("FAKE_LAUNCHER_FEATURES", FEATURES)
            print(json.dumps({"vehicle_sil_launch": 1, "vehicle_sil": VERSION,
                              "formats": {"check": 1, "run": 1, "events": 1, "describe": 1},
                              "features": [f for f in features.split(",") if f]}, indent=2))
        else:
            print(f"vehicle_sil_launch {VERSION}")
        return 0
    file = args["files"][0]
    strict = bool(args.get("strict"))
    try:
        config = read_config(Path(file))
    except (OSError, ValueError) as e:
        message = f"{file}: cannot read it: {e}"
        unreadable = unreadable_check(file, "unreadable", "file", message, strict)
        if args.get("json"):
            print(json.dumps(unreadable if args["command"] == "check"
                             else usage_result(file, "unreadable", "file", message, strict, unreadable), indent=2))
        ev.emit("phase", phase="checking")
        ev.emit("check", check=unreadable)
        ev.emit("phase", phase="ended")
        ev.emit("result", result=usage_result(file, "unreadable", "file", message, strict, unreadable))
        if not ev.enabled:
            sys.stderr.write(f"error: {message}\n")
        return 3
    scenario = os.environ.get("FAKE_LAUNCHER_SCENARIO") or config["scenario"] or "pass"
    if scenario not in SCENARIOS:
        sys.stderr.write(f"error: unknown scenario '{scenario}': the scenarios are {', '.join(SCENARIOS)}\n")
        return 4
    gate = TimeGate(args["speed"] or config["speed"], bool(args.get("start_paused")))
    if args["command"] == "run":
        ev.emit("phase", phase="checking")
        if args.get("stdin_commands") and scenario != "deaf":  # read from here on, as the real launcher does
            threading.Thread(target=read_commands, args=(ev, stops, gate), daemon=True).start()
        elif scenario == "deaf":
            try:  # the smallest pipe (Linux): what is written to it fills it soon
                import fcntl
                fcntl.fcntl(0, fcntl.F_SETPIPE_SZ, 4096)
            except (ImportError, AttributeError, OSError):
                pass
    names = [p["name"] for p in config["participants"]]
    unknown = [n for n in args["disable"] + args["enable"] if n not in names]
    if unknown:
        message = (f"--disable {unknown[0]}: the file has no participant with this name; its participants are "
                   f"{', '.join(names)}")
        usage = unreadable_check(file, "usage", "usage", message, strict)
        if args.get("json"):
            print(json.dumps(usage, indent=2))
        sys.stderr.write(f"error: {message}\n{USAGE}\n")
        ev.emit("check", check=usage)
        ev.emit("phase", phase="ended")
        ev.emit("result", result=usage_result(file, "usage", "usage", message, strict, usage))
        return 4
    for p in config["participants"]:
        if p["name"] in args["disable"]:
            p["enabled"] = False
        if p["name"] in args["enable"]:
            p["enabled"] = True
    if args["command"] == "check":
        time.sleep(int(os.environ.get("FAKE_LAUNCHER_CHECK_DELAY_MS", "0")) / 1000)
        if scenario == "check-garbage":
            print("this is not the check object")
            return 0
        result = check(file, config, scenario, strict)
        if args.get("json"):
            print(json.dumps(result, indent=2, ensure_ascii=False))
        else:
            print(f"{file}: {result['result']}: {result['errors']} errors, {result['warnings']} warnings, "
                  f"{result['notes']} notes")
        return 0 if result["result"] == "passed" else 1
    return run(args, file, config, scenario, ev, stops, gate)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
