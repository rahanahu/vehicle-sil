#!/usr/bin/env python3
"""A stand-in for vehicle_sil_launch that speaks its machine interface, for the GUI server's tests and the screen's e2e.

It is not a simulation: it reads the participant names of a configuration file and plays a scenario, writing
the same kinds of lines as `vehicle_sil_launch run --events` (one JSON object per line on standard output) and
reading `{"command":"stop"}` lines on standard input. Standard library only.

    fake_launcher.py version [--json]
    fake_launcher.py check [--json] [--strict] [--disable <name>]... [--enable <name>]... [--] <file>
    fake_launcher.py run [--events] [--json] [--stdin-commands] [--strict] [--disable <name>]... [--enable <name>]...
                         [--speed <number|max>] [--start-paused] [--] <file>

The scenario comes from FAKE_LAUNCHER_SCENARIO, else from a line `# fake-launcher: <scenario>` in the file,
else `pass`:

  pass               checks, starts every enabled participant, runs to stop_at_ms (or until stopped), passes
  check-error        the check finds an error, so nothing is started (result check_failed, exit code 1)
  participant-crash  the last required participant exits with code 3 on the third step (result failed, exit code 1)
  forever            runs until a stop command (or the end of standard input), whatever stop_at_ms says
  ignore-stop        like forever, but the first stop only waits; the second stop kills the participants; it
                     also ignores SIGTERM (a launcher that is stuck), so only a kill ends it from outside
  launcher-crash     dies on the third step without writing a result (exit code 70)
  bad-lines          writes a line that is not JSON, a JSON array, an unknown event, an unknown field and a gap
                     in seq on the first step, then runs like pass
  future-events      like pass, but hello says vehicle_sil_events 2
  flood              writes FAKE_LAUNCHER_FLOOD (default 5000) log lines at once when running starts, then passes
  finding-flood      like flood, with finding events instead of log lines
  bad-types          writes known events whose fields have the wrong types on the first step, then passes
  orphan             like pass, but before its result it starts a process in a session of its own that keeps
                     this launcher's standard output and error open for FAKE_LAUNCHER_ORPHAN_S seconds (default
                     30); a log line says its pid
  check-garbage      `check --json` writes a line that is not JSON (exit code 0)

Other environment variables: FAKE_LAUNCHER_TICK_MS (wall time of one 100 ms step at speed 1, default 100),
FAKE_LAUNCHER_FEATURES (the features of `version --json`, comma-separated, default events,stdin-commands,speed),
FAKE_LAUNCHER_CHECK_DELAY_MS (how long `check` takes, default 0).

Each started participant is a real sleeping process, a child of this one, so that a test can see whether
stopping left processes behind. The configuration is read with a small line parser, not a YAML library:
top-level `stop_at_ms:`, and under `participants:` the keys name, command, describe, required, enabled, step_ms.
"""

from __future__ import annotations

import json
import os
import queue
import re
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

VERSION = "0.1.0"
SCENARIOS = ("pass", "check-error", "participant-crash", "forever", "ignore-stop", "launcher-crash",
             "bad-lines", "future-events", "flood", "finding-flood", "bad-types", "orphan", "check-garbage")
USAGE = __doc__.split("\n\n")[1]
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
    def __init__(self, enabled: bool):
        self.enabled = enabled
        self.seq = 0
        self.start = time.monotonic()
        self.closed = False

    def emit(self, event: str, **fields) -> None:
        self.write({"event": event, "seq": self.seq, "t_ms": int((time.monotonic() - self.start) * 1000), **fields})
        self.seq += 1

    def raw(self, line: str) -> None:
        if self.enabled and not self.closed:
            self._write_line(line)

    def write(self, obj: dict) -> None:
        if self.enabled and not self.closed:
            self._write_line(json.dumps(obj, ensure_ascii=False, separators=(",", ":")))

    def _write_line(self, line: str) -> None:
        try:
            sys.stdout.write(line + "\n")
            sys.stdout.flush()
        except (BrokenPipeError, OSError):
            self.closed = True  # the reader is gone: stop, as a stop command would
            try:
                sys.stdout = open(os.devnull, "w")
            except OSError:
                pass


def read_commands(lines: queue.Queue) -> None:
    for line in sys.stdin:
        lines.put(line)
    lines.put(None)


def sleeper() -> subprocess.Popen:
    return subprocess.Popen([sys.executable, "-c", "import time\nwhile True: time.sleep(60)"],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run(args: dict, file: str, config: dict, scenario: str) -> int:
    if scenario == "ignore-stop":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    ev = Events(bool(args.get("events")))
    strict = bool(args.get("strict"))
    ev.emit("hello", vehicle_sil_events=2 if scenario == "future-events" else 1, vehicle_sil=VERSION,
            pid=os.getpid(), file=file, arguments=sys.argv[1:])
    ev.emit("phase", phase="checking")
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

    if result_check["result"] != "passed":
        return finish("check_failed", None, 1)

    commands: queue.Queue = queue.Queue()
    if args.get("stdin_commands"):
        threading.Thread(target=read_commands, args=(commands,), daemon=True).start()

    for p in participants:
        if not p["enabled"]:
            ev.emit("participant", name=p["name"], state="disabled")
    ev.emit("phase", phase="starting")
    ev.emit("registry", uri="silkit://127.0.0.1:8500")
    ev.emit("log", source="vehicle_sil_launch", line="registry at silkit://127.0.0.1:8500")
    running = {}
    by_name = {p["name"]: p for p in config["participants"]}
    for p in participants:
        if p["enabled"]:
            proc = sleeper()
            running[p["name"]] = proc
            ev.emit("participant", name=p["name"], state="started", pid=proc.pid, command=by_name[p["name"]]["command"],
                    log=None)
    ev.emit("phase", phase="waiting")
    for name in running:
        ev.emit("participant", name=name, state="joined")
    ev.emit("phase", phase="running")

    speed = args.get("speed")
    tick_s = int(os.environ.get("FAKE_LAUNCHER_TICK_MS", "100")) / 1000
    if speed == "max":
        tick_s = 0
    elif speed is not None:
        tick_s /= float(speed)
    stop_at = None if scenario in ("forever", "ignore-stop") else config["stop_at_ms"]
    sim_ms, step, stops, stdin_ended, said_waiting = 0, 0, 0, False, False

    def end_participants(how: str) -> None:
        for name, proc in running.items():
            proc.kill()
            proc.wait()
            entry = next(p for p in participants if p["name"] == name)
            if entry["ended"] != "not_started":
                continue
            if how == "killed":
                entry.update(ended="killed")
                ev.emit("participant", name=name, state="killed")
            else:
                entry.update(ended="exited", exit_code=0)
                ev.emit("participant", name=name, state="ended", exit_code=0, signal=None)

    while True:
        # Commands first: a stop decides what this step does.
        while True:
            try:
                line = commands.get_nowait()
            except queue.Empty:
                break
            if line is None:
                if not stdin_ended and stops == 0:
                    stops = 1
                stdin_ended = True
                continue
            try:
                command = json.loads(line)
            except ValueError:
                command = None
            if not isinstance(command, dict) or command.get("command") != "stop":
                ev.emit("command", command=command.get("command") if isinstance(command, dict) else None,
                        accepted=False, message="unknown command: the commands are {\"command\":\"stop\"}")
                continue
            stops += 1
            ev.emit("command", command="stop", accepted=True)
        if ev.closed and stops == 0:
            stops = 1
        if stops and scenario != "ignore-stop" or stops >= 2:
            ev.emit("phase", phase="stopping")
            if stops >= 2:
                end_participants("killed")
            else:
                end_participants("ended")
            run_findings.append(finding("note", "interrupted", file, None, None, None,
                                        "the simulation was interrupted (stop command)"))
            return finish("interrupted", "interrupted", 130, "stop")
        if stops == 1 and scenario == "ignore-stop" and not said_waiting:
            said_waiting = True
            ev.emit("log", source="vehicle_sil_launch",
                    line="stopping: waiting for the participants to end (stop again to not wait)")

        time.sleep(tick_s)
        step += 1
        sim_ms += STEP_MS
        if scenario == "bad-lines" and step == 1:
            ev.raw("this line is not JSON")
            ev.raw("[1, 2, 3]")
            ev.emit("mystery", detail="an event this reader does not know")
            ev.seq += 1  # a gap
            ev.emit("log", source=next(iter(running), "vehicle_sil_launch"), line="after a gap", colour="blue")
        if scenario == "flood" and step == 1:
            for n in range(int(os.environ.get("FAKE_LAUNCHER_FLOOD", "5000"))):
                ev.emit("log", source=next(iter(running)), line=f"flood line {n}")
        if scenario == "finding-flood" and step == 1:
            for n in range(int(os.environ.get("FAKE_LAUNCHER_FLOOD", "5000"))):
                ev.emit("finding", **finding("note", "flood", file, None, None, None, f"flood finding {n}"))
        if scenario == "bad-types" and step == 1:
            for kind, fields in (("check", {"check": {"participants": 5}}), ("participant", {"name": 5}),
                                 ("participant", {"name": "Engine", "state": ["x"]}), ("log", {"source": None}),
                                 ("time", {"sim_ms": "100"}), ("phase", {"phase": {}}), ("result", {"result": []}),
                                 ("command", {"command": "stop", "accepted": "yes"}), ("finding", {"severity": 1})):
                ev.emit(kind, **fields)
        if scenario == "orphan" and step == 1:
            orphan = subprocess.Popen(
                [sys.executable, "-c", f"import time; time.sleep({float(os.environ.get('FAKE_LAUNCHER_ORPHAN_S', '30'))})"],
                stdin=subprocess.DEVNULL, start_new_session=True)
            ev.emit("log", source="vehicle_sil_launch", line=f"left a process behind: pid {orphan.pid}")
        if scenario == "launcher-crash" and step == 3:
            sys.stderr.write("fake_launcher: dying on purpose (scenario launcher-crash)\n")
            sys.stderr.flush()
            os._exit(70)
        for name in running:
            ev.emit("log", source=name, line=f"step {step}: t = {sim_ms} ms")
        ev.emit("time", sim_ms=sim_ms)
        if scenario == "participant-crash" and step == 3:
            name = [p["name"] for p in participants if p["enabled"] and p["required"]][-1]
            running[name].kill()
            running[name].wait()
            entry = next(p for p in participants if p["name"] == name)
            entry.update(ended="exited", exit_code=3)
            ev.emit("log", source=name, line="error: something went wrong, exiting with code 3")
            ev.emit("participant", name=name, state="ended", exit_code=3, signal=None)
            f = finding("error", "participant-failed", file, by_name[name]["line"], name, None,
                        f"participant '{name}' failed while the simulation was running (exit code 3), "
                        "so the simulation was aborted: see its log")
            run_findings.append(f)
            ev.emit("finding", **f)
            ev.emit("phase", phase="stopping")
            end_participants("ended")
            return finish("failed", "aborted", 1)
        if stop_at is not None and sim_ms >= stop_at:
            ev.emit("phase", phase="stopping")
            end_participants("ended")
            return finish("passed", "reached_stop_at", 0)


# ---------------------------------------------------------------- main


def main(argv: list[str]) -> int:
    json_wanted = "--json" in argv
    try:
        args = parse(argv)
    except Usage as e:
        if json_wanted and argv and argv[0] == "check":
            print(json.dumps(unreadable_check(None, "usage", "usage", str(e), False), indent=2))
        sys.stderr.write(f"error: {e}\n{USAGE}\n")
        return 4
    if args["command"] == "version":
        if args.get("json"):
            features = os.environ.get("FAKE_LAUNCHER_FEATURES", "events,stdin-commands,speed")
            print(json.dumps({"vehicle_sil_launch": 1, "vehicle_sil": VERSION,
                              "formats": {"check": 1, "run": 1, "events": 1, "describe": 1},
                              "features": [f for f in features.split(",") if f]}))
        else:
            print(f"vehicle_sil_launch {VERSION}")
        return 0
    file = args["files"][0]
    strict = bool(args.get("strict"))
    try:
        config = read_config(Path(file))
    except (OSError, ValueError) as e:
        if args.get("json"):
            print(json.dumps(unreadable_check(file, "unreadable", "file", f"{file}: cannot read it: {e}", strict),
                             indent=2))
        sys.stderr.write(f"error: {file}: cannot read it: {e}\n")
        return 3
    scenario = os.environ.get("FAKE_LAUNCHER_SCENARIO") or config["scenario"] or "pass"
    if scenario not in SCENARIOS:
        sys.stderr.write(f"error: unknown scenario '{scenario}': the scenarios are {', '.join(SCENARIOS)}\n")
        return 4
    names = [p["name"] for p in config["participants"]]
    unknown = [n for n in args["disable"] + args["enable"] if n not in names]
    if unknown:
        message = (f"no participant named {', '.join(repr(n) for n in unknown)} in {file}: "
                   f"the participants are {', '.join(names)}")
        if args["command"] == "check" and args.get("json"):
            print(json.dumps(unreadable_check(file, "usage", "usage", message, strict), indent=2))
        sys.stderr.write(f"error: {message}\n")
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
    return run(args, file, config, scenario)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
