"""Finding vehicle_sil_launch, asking what it can do, and the only command lines this server gives it."""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import process

LAUNCHER_NAME = "vehicle_sil_launch"
ENV_VARIABLE = "VEHICLE_SIL_LAUNCH"
REQUIRED_FEATURES = ("events", "stdin-commands")
VERSION_TIMEOUT_S = 10
# The words of vehicle_sil_launch that this server uses, in one place (docs/simulation-config.md, 「等速で動かす」
# and 「機械から使う」): the range of a speed (the same for --speed and the speed command), the most milliseconds
# one step command may ask for, the time commands, and the features that say which of them a launcher has.
SPEED_MIN = 0.000001
SPEED_MAX = 1000000.0
STEP_MS_MAX = 10 ** 12
TIME_COMMANDS = ("pause", "resume", "step", "speed")
FEATURE_SPEED = "speed"
FEATURE_START_PAUSED = "start-paused"
FEATURE_TIME_COMMANDS = "time-commands"
# The control links (the launcher's side of the ECUs' variables): the event that says, once per started participant
# before the simulation runs, where its link stands ({"name", "state", "variables" | "version"}). The states
# (connected, no_hello, version_mismatch, not_offered, none) are passed on as they are: the server does not read
# them. The summary keeps the event's fields in the participant's entry, under the same name as the event.
FEATURE_CONTROL_LINKS = "control-links"
CONTROL_EVENT = "control_link"
CONTROL_SUMMARY_FIELD = "control_link"


class LauncherNotFound(Exception):
    pass


@dataclass
class Launcher:
    path: Path
    usable: bool = False
    problems: list[dict] = field(default_factory=list)
    version: dict | None = None
    _stamp: tuple | None = None
    track: set | None = field(default=None, repr=False)  # where a running `version --json` is put (see process)
    _lock: asyncio.Lock = field(default_factory=asyncio.Lock, repr=False)

    @property
    def prefix(self) -> list[str]:
        """The program to start. A .py file (the fake launcher of the tests) runs with this Python."""
        if self.path.suffix == ".py":
            return [sys.executable, str(self.path)]
        return [str(self.path)]

    @property
    def features(self) -> list[str]:
        f = (self.version or {}).get("features")
        return [x for x in f if isinstance(x, str)] if isinstance(f, list) else []

    def info(self) -> dict:
        return {"path": str(self.path), "usable": self.usable, "problems": self.problems, "version": self.version}

    async def probe(self) -> None:
        """Runs `version --json` (again only when the file has changed) and decides whether it is usable.
        A call made while another one runs waits for its answer."""
        async with self._lock:
            await self._probe()

    async def _probe(self) -> None:
        try:
            st = self.path.stat()
            stamp = (st.st_mtime_ns, st.st_size)
        except OSError as e:
            self._set(None, [{"code": "launcher-missing",
                              "message": f"{self.path} cannot be read any more ({e.strerror}): build it again, "
                                         "or start the GUI with --launcher <path>"}])
            self._stamp = None
            return
        if stamp == self._stamp:
            return
        self._stamp = stamp  # under the lock: no one sees the new stamp before the answer below
        try:
            code, out, err = await process.run_to_end(self.prefix + ["version", "--json"], cwd=None,
                                                      timeout_s=VERSION_TIMEOUT_S, track=self.track)
        except (OSError, TimeoutError) as e:
            what = "did not answer in time" if isinstance(e, TimeoutError) else f"cannot be started ({e})"
            self._set(None, [{"code": "launcher-version",
                              "message": f"{self.path} version --json {what}: check that it is "
                                         f"{LAUNCHER_NAME} and that it runs"}])
            return
        try:
            version = json.loads(out)
        except ValueError:
            version = None
        if code != 0 or not isinstance(version, dict) or version.get("vehicle_sil_launch") != 1:
            last = err.decode("utf-8", "replace").strip().splitlines()[-1:] or [""]
            self._set(None, [{"code": "launcher-version",
                              "message": f"{self.path} version --json did not write the version object "
                                         f"(exit code {code}{': ' + last[0] if last[0] else ''}): this "
                                         f"{LAUNCHER_NAME} is older than this GUI; build a newer one"}])
            return
        self.version = version
        missing = [f for f in REQUIRED_FEATURES if f not in self.features]
        self._set(version, [{"code": "missing-feature",
                             "message": f"{self.path} has no '{f}' (its features: "
                                        f"{', '.join(self.features) or 'none'}): this {LAUNCHER_NAME} is older "
                                        "than this GUI; build a newer one"} for f in missing])

    def _set(self, version: dict | None, problems: list[dict]) -> None:
        self.version = version
        self.problems = problems
        self.usable = not problems


def find(explicit: str | None) -> Path:
    """--launcher, then the environment variable, then PATH. Raises LauncherNotFound with what to do."""
    for given, origin in ((explicit, "--launcher"), (os.environ.get(ENV_VARIABLE), ENV_VARIABLE)):
        if given:
            path = Path(given).expanduser()
            if not path.is_file():
                raise LauncherNotFound(f"{origin} {given}: no such file: give the path of {LAUNCHER_NAME}")
            return path.resolve()
    found = shutil.which(LAUNCHER_NAME)
    if not found:
        raise LauncherNotFound(f"{LAUNCHER_NAME} is not on PATH: give its path with --launcher <path> or "
                               f"{ENV_VARIABLE}=<path> (it is built in build/<preset>/tools/launch/)")
    path = Path(found).resolve()
    if path.parent == Path.cwd().resolve():
        # Windows looks in the current directory before PATH (and a "." on PATH does it anywhere): a folder
        # received from someone could start its own program here.
        raise LauncherNotFound(f"{LAUNCHER_NAME} was found in the current directory ({path}), not taken from "
                               "there: give its path with --launcher <path> if it is the one to use")
    return path


# ---------------------------------------------------------------- the command lines


class InvalidRequest(ValueError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def check_speed(speed: object) -> float | str | None:
    if speed is None or speed == "max":
        return speed
    # Compared before any conversion: an integer of 400 digits is out of range, not an overflow.
    if isinstance(speed, bool) or not isinstance(speed, (int, float)) or not SPEED_MIN <= speed <= SPEED_MAX:
        raise InvalidRequest("invalid-field", f"speed must be null, \"max\" or a number from {SPEED_MIN:f} to "
                                              f"{SPEED_MAX:.0f} (1 is real time)")
    return float(speed)


def check_disable(disable: object, known: list[str]) -> list[str]:
    """The names to give --disable: each must be a participant of the check result of the file."""
    if disable is None:
        return []
    if not isinstance(disable, list) or not all(isinstance(n, str) for n in disable):
        raise InvalidRequest("invalid-field", "disable must be a list of participant names")
    unknown = [n for n in disable if n not in known]
    if unknown:
        raise InvalidRequest("unknown-participant",
                             f"no participant named {', '.join(repr(n) for n in unknown)} in this file: the "
                             f"participants are {', '.join(known) or 'none'}")
    if len(set(disable)) != len(disable):
        raise InvalidRequest("invalid-field", "disable names a participant twice")
    return list(disable)


def speed_argument(speed: float | str) -> str:
    """A speed as --speed and the speed command take it: max, or the shortest decimal that is the number (2, 0.5,
    1e-06), never rounded (f"{x:g}" would make 1.2345678 into 1.23457)."""
    if speed == "max":
        return "max"
    text = repr(float(speed))
    return text[:-2] if text.endswith(".0") else text


def check_time_command(body: dict) -> dict:
    """The body of POST api/time as the command line it becomes: {"command": "pause" | "resume"},
    {"command": "step", "ms": <1 to 10^12>} or {"command": "speed", "value": <a speed or "max">}. Other keys
    are refused (they would be ignored by the launcher, and a typo should not pass silently)."""
    name = body.get("command")
    if name not in TIME_COMMANDS:
        raise InvalidRequest("invalid-field", f"command must be one of {', '.join(TIME_COMMANDS)} (to stop, use "
                                              "POST api/stop)")
    allowed = {"command", *({"step": ("ms",), "speed": ("value",)}.get(name, ()))}
    extra = sorted(k for k in body if k not in allowed)
    if extra:
        raise InvalidRequest("invalid-field", f"{name} takes no {', '.join(extra)}: send "
                                              f"{_time_example(name)}")
    if name == "step":
        ms = body.get("ms")
        if isinstance(ms, bool) or not isinstance(ms, int) or not 1 <= ms <= STEP_MS_MAX:
            raise InvalidRequest("invalid-field", f"step needs ms, a whole number of milliseconds of virtual time "
                                                  f"from 1 to {STEP_MS_MAX}: send {_time_example(name)}")
        return {"command": name, "ms": ms}
    if name == "speed":
        value = body.get("value")
        if value is None:
            raise InvalidRequest("invalid-field", f"speed needs value: send {_time_example(name)}")
        return {"command": name, "value": "max" if value == "max" else check_speed(value)}
    return {"command": name}


def _time_example(name: str) -> str:
    return {"step": '{"command":"step","ms":100}', "speed": '{"command":"speed","value":1}'}.get(
        name, f'{{"command":"{name}"}}')


def command_line(command: dict) -> bytes:
    """The line written to the launcher's standard input for a command that check_time_command accepted."""
    if command["command"] == "speed":
        value = command["value"]
        text = '"max"' if value == "max" else speed_argument(value)
        return f'{{"command":"speed","value":{text}}}\n'.encode()
    return (json.dumps(command, separators=(",", ":")) + "\n").encode()


def check_command(launcher: Launcher, file: str, disable: list[str]) -> list[str]:
    argv = launcher.prefix + ["check", "--json"]
    for name in disable:
        argv += ["--disable", name]
    return argv + ["--", file]


def run_command(launcher: Launcher, file: str, disable: list[str], speed: float | str | None,
                start_paused: bool, control_credentials: Path | None = None) -> list[str]:
    """control_credentials: start the launcher's control server (the ECUs' variables) on a free port of
    127.0.0.1, with the credentials of that file (a token is never an argument: arguments are in the hello
    event and in the list of processes)."""
    argv = launcher.prefix + ["run", "--events", "--stdin-commands"]
    if control_credentials is not None:
        argv += ["--control", "0", "--control-credentials", str(control_credentials)]
    for name in disable:
        argv += ["--disable", name]
    if speed is not None:
        argv += ["--speed", speed_argument(speed)]
    if start_paused:
        argv += ["--start-paused"]
    return argv + ["--", file]
