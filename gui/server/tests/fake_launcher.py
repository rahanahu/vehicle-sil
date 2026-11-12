#!/usr/bin/env python3
"""A stand-in for vehicle_sil_launch that speaks its machine interface, for the GUI server's tests and the screen's e2e.

It is not a simulation: it reads the participant names of a configuration file and plays a scenario, writing
the same lines as `vehicle_sil_launch run --events` (docs/simulation-config.md, 「機械から使う」: the same events,
keys, order and values, a heartbeat after a second without a line, command events with source and message,
interrupted_by "stop command" / "end of input" / "output closed", hello and result also for a usage error or a
file it cannot read) and reading `{"command":"stop"}` lines on standard input. test_real_launcher.py compares
what it writes with what the real launcher writes. Standard library only.

    fake_launcher.py version [--json]
    fake_launcher.py check [--json] [--strict] [--disable <name>]... [--enable <name>]... [--] <file>
    fake_launcher.py run [--events] [--json] [--stdin-commands] [--strict] [--disable <name>]... [--enable <name>]...
                         [--speed <number|max>] [--start-paused] [--] <file>

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

Other environment variables: FAKE_LAUNCHER_TICK_MS (wall time of one 100 ms step at speed 1, default 100),
FAKE_LAUNCHER_FEATURES (the features of `version --json`, comma-separated, default
events,stdin-commands,check-variables as the real launcher; add speed for --speed), FAKE_LAUNCHER_CHECK_DELAY_MS
(how long `check` takes, default 0), FAKE_LAUNCHER_HEARTBEAT_MS (a heartbeat after this long without a line,
default 1000; 0: none).

Like the real launcher, it writes `time` only when running starts (0) and when stop_at_ms is reached; the log
lines of the participants come every step.

Each started participant is a real sleeping process, a child of this one, so that a test can see whether
stopping left processes behind. The configuration is read with a small line parser, not a YAML library:
top-level `stop_at_ms:`, and under `participants:` the keys name, command, describe, required, enabled, step_ms.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

VERSION = "0.1.0"
SCENARIOS = ("pass", "check-error", "participant-crash", "forever", "ignore-stop", "launcher-crash",
             "bad-lines", "future-events", "flood", "finding-flood", "bad-types", "orphan", "check-garbage",
             "non-finite", "result-then-wait", "log-dropped", "silent", "large-fields", "loose-fields",
             "large-result")
USAGE = next(p for p in __doc__.split("\n\n") if p.lstrip().startswith("fake_launcher.py version"))
STEP_MS = 100

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
              "registry_uri": None, "participants": [], "networks": []}
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
            if section in ("stop_at_ms", "start_timeout_ms", "stop_timeout_ms", "registry_uri"):
                config[section] = scalar(value)
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
    for p in config["participants"]:
        entry = {"name": p["name"], "line": p["line"], "command": p["command"], "describe": p["describe"],
                 "required": p["required"], "enabled": p["enabled"], "step_ms": p["step_ms"]}
        if not p["enabled"]:
            entry.update(status="disabled", reported=None, variables=None)
            findings.append(finding("note", "participant-disabled", file, p["line"], p["name"], None,
                                    f"participant '{p['name']}': disabled (enabled: false, or --disable): it is "
                                    "neither checked nor started, and the other findings are about the simulation "
                                    "without it"))
        elif not p["describe"]:
            entry.update(status="not_checked", reported=None, variables=None)
            findings.append(finding("note", "not-checked", file, p["line"], p["name"], None,
                                    f"participant '{p['name']}': describe is false, so it was not checked"))
        else:
            step = p["step_ms"] or 10
            entry.update(status="described", reported={
                "vehicle_sil": VERSION, "name": p["name"], "step_ns": step * 1_000_000,
                "networks": [{"name": n["name"], "kind": n.get("kind"), "use": "raw", "nodes": []}
                             for n in networks]}, variables=variables(p["name"]))
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
                if rest[i] != "max" and not re.fullmatch(r"\d+(\.\d+)?", rest[i]):
                    raise Usage(f"--speed {rest[i]}: write a positive number or max")
                args["speed"] = rest[i]
            else:
                args[a[2:]].append(rest[i])
        elif a.startswith("-"):
            raise Usage(f"unknown argument '{a}'")
        else:
            args["files"].append(a)
        i += 1
    if args.get("json") and args.get("events"):
        raise Usage("--json and --events cannot be used together")
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


def read_commands(ev: Events, stops: Stops) -> None:
    """The standard input of --stdin-commands: a command event at once for every line (and for the end of the
    input when it is the first stop request), before what the command does."""
    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        try:
            command = json.loads(line)
        except ValueError:
            command = None
        name = command.get("command") if isinstance(command, dict) else None
        name = name if isinstance(name, str) else None
        if name == "stop":
            with ev.lock:  # the command event before anything the stop makes happen
                stops.add("stop command")
                ev.emit("command", command="stop", source="stdin", accepted=True, message=None)
        elif not isinstance(command, dict):
            ev.emit("command", command=None, source="stdin", accepted=False,
                    message="not a JSON object: write one object per line, such as {\"command\":\"stop\"}")
        else:
            ev.emit("command", command=name, source="stdin", accepted=False,
                    message=f"unknown command {name!r}: the commands are stop" if name is not None
                    else "no \"command\": write {\"command\":\"stop\"}")
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


def run(args: dict, file: str, config: dict, scenario: str, ev: Events, stops: Stops) -> int:
    if scenario == "ignore-stop":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    strict = bool(args.get("strict"))
    result_check = check(file, config, scenario, strict)
    ev.emit("check", check=result_check)
    participants = []
    for p in config["participants"]:
        participants.append({"name": p["name"], "required": p["required"], "enabled": p["enabled"],
                             "ended": "not_started", "exit_code": None, "signal": None, "left_processes": False,
                             "log": None})
    run_findings: list[dict] = []

    def result(outcome: str, simulation, interrupted_by=None) -> dict:
        everything = result_check["findings"] + run_findings
        return {"vehicle_sil_run": 1, "vehicle_sil": VERSION, "file": file, "result": outcome, "strict": strict,
                "errors": sum(f["severity"] == "error" for f in everything),
                "warnings": sum(f["severity"] == "warning" for f in everything),
                "notes": sum(f["severity"] == "note" for f in everything),
                "simulation": simulation, "interrupted_by": interrupted_by, "participants": participants,
                "findings": run_findings, "check": result_check}

    def finish(outcome: str, simulation, code: int, interrupted_by=None) -> int:
        ev.emit("phase", phase="ended")
        r = result(outcome, simulation, interrupted_by)
        ev.emit("result", result=r)
        if args.get("json"):
            print(json.dumps(r, indent=2, ensure_ascii=False))
        return code

    def say(line: str) -> None:
        ev.emit("log", source="vehicle_sil_launch", line=line)

    def note(f: dict) -> None:
        run_findings.append(f)
        ev.emit("finding", **f)

    def interrupted(started: bool) -> None:
        by = stops.by or "stop command"
        if started:
            say(f"interrupted ({by}): the simulation was stopped")
        note(finding("note", "interrupted", file, None, None, None,
                     f"interrupted ({by}): the launcher stopped the simulation and the participants" if started
                     else f"interrupted ({by}): the launcher stopped the participants before the simulation started"))

    if result_check["result"] != "passed":
        participants.clear()  # nothing was started: the result lists no participant
        return finish("check_failed", None, 1)
    if stops.count:  # a stop during the check takes effect after it: nothing is started
        interrupted(False)
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
    ev.emit("phase", phase="running")
    say("simulation running")
    ev.emit("time", sim_ms=0)

    speed = args.get("speed")
    tick_s = int(os.environ.get("FAKE_LAUNCHER_TICK_MS", "100")) / 1000
    if speed == "max":
        tick_s = 0
    elif speed is not None:
        tick_s /= float(speed)
    stop_at = None if scenario in ("forever", "ignore-stop") else config["stop_at_ms"]
    sim_ms, step, asked = 0, 0, False

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

    while True:
        # A stop decides what this step does.
        if stops.count and scenario == "ignore-stop" and not asked:
            asked = True  # the participants do not end: the launcher asks them to stop, and waits
            interrupted(True)
            ev.emit("phase", phase="stopping")
            for name in running:
                say(f"asking {name} to stop (SIGTERM)")
                ev.emit("participant", name=name, state="stop_requested")
        if stops.count and (scenario != "ignore-stop" or stops.count >= 2):
            if not asked:
                interrupted(True)
                ev.emit("phase", phase="stopping")
            end_participants("killed" if scenario == "ignore-stop" else "ended")
            return finish("interrupted", "interrupted", 130, stops.by)

        time.sleep(tick_s)
        step += 1
        sim_ms += STEP_MS
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
            note(f)
            ev.emit("phase", phase="stopping")
            end_participants("ended")
            return finish("failed", "aborted", 1)
        if stop_at is not None and sim_ms >= stop_at:
            ev.emit("time", sim_ms=sim_ms)
            say("the simulation ended")
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
            "errors": 1, "warnings": 0, "notes": 0, "simulation": None, "interrupted_by": None, "participants": [],
            "findings": [], "check": checked or unreadable_check(file, outcome, code, message, strict)}


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
            features = os.environ.get("FAKE_LAUNCHER_FEATURES", "events,stdin-commands,check-variables")
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
    if args["command"] == "run":
        ev.emit("phase", phase="checking")
        if args.get("stdin_commands"):  # read from here on, as the real launcher does once the file is read
            threading.Thread(target=read_commands, args=(ev, stops), daemon=True).start()
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
    return run(args, file, config, scenario, ev, stops)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
