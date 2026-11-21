"""Finding vehicle_sil_launch, asking what it can do, and the only command lines this server gives it."""

from __future__ import annotations

import asyncio
import json
import math
import os
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from . import __version__, process

LAUNCHER_NAME = "vehicle_sil_launch"
ENV_VARIABLE = "VEHICLE_SIL_LAUNCH"
# `request`: run --request and check --request read the run's intent from a file (schema/run-request.schema.json), which
# is how this server gives the launcher everything that the browser chose. `time-commands`, `stdin-run-until` and
# `state`: the time commands (with their `id`) that the server writes to the launcher's input, and the `state` event
# that says what it accepts now. `speed` and `start-paused`: the run's speed and its start paused; `scenario` and
# `manual-participants`: the scenario runs and the manual participants.
# Every launcher of the same version has all of them, so the server does not ask per run; a launcher without one is refused when the server starts (and when the launcher is replaced).
REQUIRED_FEATURES = ("events", "stdin-commands", "request", "time-commands", "stdin-run-until", "state",
                     "speed", "start-paused", "scenario", "manual-participants")
VERSION_TIMEOUT_S = 10
# The words of vehicle_sil_launch that this server uses, in one place ("Running at a set speed (`speed`)" in docs/simulation-run.md,
# and docs/simulation-events.md): the range of a speed (the same for speed and the speed command), the most milliseconds
# one step command may ask for, the time commands, and the features that say which of them a launcher has. The
# launcher's tests (launch.the_gui_takes_the_launchers_limits) compare the numbers with its own.
SPEED_MIN = 0.000001
SPEED_MAX = 1000000.0
STEP_MS_MAX = 10 ** 12
TIME_COMMANDS = ("pause", "resume", "step", "run_until", "speed")
MAX_ID = 2 ** 53  # the largest id of a time command (an integer that JavaScript reads exactly)
# The control links (the launcher's side of the ECUs' variables): the event that says, once per started participant
# before the simulation runs, where its link stands ({"name", "state", "variables" | "version"}). The states
# (connected, no_hello, version_mismatch, not_offered, none) are passed on as they are: the server does not read
# them. The summary keeps the event's fields in the participant's entry, under the same name as the event.
FEATURE_CONTROL_LINKS = "control-links"
CONTROL_EVENT = "control_link"
CONTROL_SUMMARY_FIELD = "control_link"
# Scenarios (docs/scenario.md): run --scenario plays one and ends the run with its outcome; check --scenario checks it.
# A launcher with a control server (`control-server`) also has it during a scenario (read, time commands; it refuses
# writes): the two features are always printed together.
SCENARIO_STEP_EVENT = "scenario_step"
# A scenario run's timeline ("Recording a scenario run on a timeline" in docs/simulation-run.md): run --scenario
# --timeline <file> writes what the scenario wrote and saw to that file after the run.
FEATURE_TIMELINE = "timeline"
FEATURE_RUN_STOP_AT = "run-stop-at"


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
        """The program to start. A .py file (the stand-in launcher of the tests) runs with this Python."""
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
                                         f"{LAUNCHER_NAME} is older than this GUI"}])
            return
        self.version = version
        theirs = version.get("vehicle_sil")
        if theirs != __version__:  # in full, pre-release included
            self._set(version, [{"code": "launcher-version-mismatch",
                                 "message": f"this {LAUNCHER_NAME} ({self.path}) is vehicle-sil {theirs}, this GUI is "
                                            f"{__version__}: use the {LAUNCHER_NAME} of the same version"}])
            return
        missing = [f for f in REQUIRED_FEATURES if f not in self.features]
        self._set(version, [{"code": "missing-feature",
                             "message": f"{self.path} has no '{f}' (its features: "
                                        f"{', '.join(self.features) or 'none'}): this {LAUNCHER_NAME} is not "
                                        f"the version of this GUI; use the {LAUNCHER_NAME} of the same "
                                        "vehicle-sil version"} for f in missing])

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


# What the browser may send: `file` (the configuration file, which this server resolves) and the keys of the request
# file (schema/run-request.schema.json), with the names and value forms of that file. The keys of the launcher's side
# (control, timeline, log_dir, events, stdin) are not in the list: this server decides them.
REQUEST_VERSION_KEY = "vehicle_sil_run_request"
INTENT_FIELDS = ("scenario", "variant", "params", "disable", "enable", "manual", "strict", "connect", "time_policy")
# `$schema` is the editor's key of a request file: read and dropped, as the launcher does.
REQUEST_FIELDS = ("file", "$schema", REQUEST_VERSION_KEY, *INTENT_FIELDS, "speed", "start_paused", "stop_at_ms", "late_tolerance_ms")
# POST api/check with `text` (the unsaved text of the file, checked as that file): the configuration only, so of the
# intent only what chooses which configuration is checked. A scenario, the participants to leave out or start by
# hand, the connections and the time are choices of a run, which a run makes from the saved file.
TEXT_FIELDS = ("file", "$schema", REQUEST_VERSION_KEY, "variant", "strict", "text")
# POST api/describe: the file and its variant, whose executables are described again.
DESCRIBE_FIELDS = ("file", "variant")
# POST api/connections: the configuration file and the one network to list (the one the user opened).
CONNECTIONS_FIELDS = ("file", "network")
# The target of `connect` that keeps a network on SIL Kit only: it needs no list.
NO_CONNECTION = "none"
# The launcher's features `connect`, `connections` and `time-policy` are not in REQUIRED_FEATURES: every launcher with
# `request` has them (the launcher's features are only added, and these came before `request`, on every OS).
# `timeline` is the only feature of the always-printed ones that the server still asks per run (see FEATURE_TIMELINE).


def check_fields(endpoint: str, body: dict, allowed: tuple[str, ...]) -> None:
    """Refuses a body with a field the endpoint does not take (a field that is ignored, misspelled or not
    supported, would leave the sender believing it had an effect), a field that is null (the request file has no
    null: a field that is not wanted is left out) and a request format that is not 1."""
    unknown = sorted(k for k in body if k not in allowed)
    if unknown:
        raise InvalidRequest("invalid-field", f"{endpoint} takes no {', '.join(repr(k) for k in unknown)}: "
                                              f"the fields it takes are {', '.join(allowed)}")
    nulls = sorted(k for k, v in body.items() if v is None and k != "file")
    if nulls:
        raise InvalidRequest("invalid-field", f"{endpoint} takes no null: leave out {', '.join(repr(k) for k in nulls)} "
                                              "to choose nothing")
    if "$schema" in body and not isinstance(body["$schema"], str):
        raise InvalidRequest("invalid-field", "$schema must be a text (the path or URL of the schema), or left out")
    version = body.get(REQUEST_VERSION_KEY)
    if REQUEST_VERSION_KEY in body and (isinstance(version, bool) or version != 1):
        raise InvalidRequest("invalid-field", f"{REQUEST_VERSION_KEY} must be 1 (the format of the request file) or "
                                              "left out")


def check_variant(variant: object) -> str | None:
    """The variant of the request (None: none). Whether the file has it, the launcher's check says."""
    if variant is None:
        return None
    if not isinstance(variant, str) or not variant:
        raise InvalidRequest("invalid-field", "variant must be the name of a variant of the file (a non-empty "
                                              "string), or left out")
    return variant


def check_params(params: object) -> dict:
    """The params of the request: {name: value} of the scenario's parameters (a string, a number or true/false).
    Whether the scenario has them, and what a name or a value may be, the launcher says."""
    if params is None:
        return {}
    if not isinstance(params, dict):
        raise InvalidRequest("invalid-field", 'params must be an object of parameter name to value, e.g. '
                                              '{"idle_rpm": 650}')
    for name, value in params.items():
        ok = isinstance(value, (str, bool, int)) or (isinstance(value, float) and math.isfinite(value))
        if not ok:
            raise InvalidRequest("invalid-field", f"params {name!r} must be a string, a number or true or false")
    return dict(params)


def check_strict(strict: object) -> bool:
    if strict is None:
        return False
    if not isinstance(strict, bool):
        raise InvalidRequest("invalid-field", "strict must be true or false")
    return strict


def check_connect(connect: object) -> dict[str, str]:
    """The connect of the request: {network: target}. Whether the file has such a network and the target is one that
    the launcher takes, the launcher says; whether this server passes the target of a network of the file on, the
    lists of POST api/connections say (GuiServer._check_targets)."""
    if connect is None:
        return {}
    if not isinstance(connect, dict) or not all(isinstance(n, str) and n and isinstance(t, str)
                                                for n, t in connect.items()):
        raise InvalidRequest("invalid-field", 'connect must be an object of network name to target, e.g. '
                                              '{"ETH_EXT": "enp3s0"}: a target that POST api/connections lists, or '
                                              '"none" for SIL Kit only')
    return dict(connect)


def check_time_policy(policy: object) -> str | None:
    """The time_policy of the request. Which values there are, and what they do, the launcher says."""
    if policy is None:
        return None
    if not isinstance(policy, str) or not policy:
        raise InvalidRequest("invalid-field", 'time_policy must be the name of a time policy that the launcher '
                                              'takes (a string, e.g. "free"), or left out')
    return policy


def check_network(network: object) -> str:
    """The network of POST api/connections: a name (which ones the file has, its last check says)."""
    if not isinstance(network, str) or not network:
        raise InvalidRequest("invalid-field", 'network must be the name of a network of the file (a non-empty '
                                              'string), e.g. {"file": "simulation.yaml", "network": "ETH_EXT"}')
    return network


def check_run_millis(body: dict, key: str, minimum: int) -> int | None:
    if key not in body:
        return None
    value = body[key]
    if type(value) is not int or not minimum <= value <= STEP_MS_MAX:
        raise InvalidRequest("invalid-field", f"{key} must be an integer from {minimum} to {STEP_MS_MAX}")
    return value


def check_speed(speed: object) -> float | str | None:
    if speed is None or speed == "max":
        return speed
    # Compared before any conversion: an integer of 400 digits is out of range, not an overflow.
    if isinstance(speed, bool) or not isinstance(speed, (int, float)) or not SPEED_MIN <= speed <= SPEED_MAX:
        raise InvalidRequest("invalid-field", f"speed must be \"max\" or a number from {SPEED_MIN:f} to "
                                              f"{SPEED_MAX:.0f} (1 is real time)")
    return float(speed)


def check_disable(disable: object, known: list[str]) -> list[str]:
    """The names of the request's disable: each must be a participant of the check result of the file."""
    return _check_names("disable", disable, known)


def check_manual(manual: object, known: list[str]) -> list[str]:
    """The names of the request's manual (the user starts them): participants of the check result of the file.
    (A name that is also in disable is the launcher's to refuse.)"""
    return _check_names("manual", manual, known)


def check_enable(enable: object) -> list[str]:
    """The names of the request's enable (taken in although the file disables them): which names the file has is
    the launcher's to say."""
    return _check_names("enable", enable, None)


def _check_names(key: str, value: object, known: list[str] | None) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(isinstance(n, str) and n for n in value):
        raise InvalidRequest("invalid-field", f"{key} must be a list of participant names")
    unknown = [n for n in value if known is not None and n not in known]
    if unknown:
        raise InvalidRequest("unknown-participant",
                             f"no participant named {', '.join(repr(n) for n in unknown)} in this file: the "
                             f"participants are {', '.join(known or []) or 'none'}")
    if len(set(value)) != len(value):
        raise InvalidRequest("invalid-field", f"{key} names a participant twice")
    return list(value)


def speed_argument(speed: float | str) -> str:
    """A speed as --speed and the speed command take it: max, or the shortest decimal that is the number (2, 0.5,
    1e-06), never rounded (f"{x:g}" would make 1.2345678 into 1.23457)."""
    if speed == "max":
        return "max"
    text = repr(float(speed))
    return text[:-2] if text.endswith(".0") else text


def check_time_command(body: dict) -> dict:
    """The command of POST api/time after its run target has been removed: {"command": "pause" | "resume"},
    {"command": "step", "ms": <1 to 10^12>}, {"command": "run_until", "at_ms": <0 to 10^12>} or
    {"command": "speed", "value": <a speed or "max">}, each with an
    optional "id" (a whole number from 0 to 2^53: the launcher's `command` event answering it has the same id).
    Other keys are refused (they would be ignored by the launcher, and a typo should not pass silently)."""
    name = body.get("command")
    if name not in TIME_COMMANDS:
        raise InvalidRequest("invalid-field", f"command must be one of {', '.join(TIME_COMMANDS)} (to stop, use "
                                              "POST api/stop)")
    allowed = {"command", "id", *({"step": ("ms",), "run_until": ("at_ms",), "speed": ("value",)}.get(name, ()))}
    extra = sorted(k for k in body if k not in allowed)
    if extra:
        raise InvalidRequest("invalid-field", f"{name} takes no {', '.join(extra)}: send "
                                              f"{_time_example(name)}")
    request = {}
    if "id" in body:
        number = body["id"]
        if isinstance(number, bool) or not isinstance(number, int) or not 0 <= number <= MAX_ID:
            raise InvalidRequest("invalid-field", f"id must be a whole number from 0 to {MAX_ID} (the launcher's "
                                                  "answer carries it back): leave it out, or send "
                                                  f"{_time_example(name, 7)}")
        request["id"] = number
    if name == "step":
        ms = body.get("ms")
        if isinstance(ms, bool) or not isinstance(ms, int) or not 1 <= ms <= STEP_MS_MAX:
            raise InvalidRequest("invalid-field", f"step needs ms, a whole number of milliseconds of virtual time "
                                                  f"from 1 to {STEP_MS_MAX}: send {_time_example(name)}")
        return {"command": name, "ms": ms, **request}
    if name == "run_until":
        at_ms = body.get("at_ms")
        if isinstance(at_ms, bool) or not isinstance(at_ms, int) or not 0 <= at_ms <= STEP_MS_MAX:
            raise InvalidRequest("invalid-field", f"run_until needs at_ms, the virtual time to stand still at, a whole "
                                                  f"number of milliseconds from 0 to {STEP_MS_MAX}: send "
                                                  f"{_time_example(name)}")
        return {"command": name, "at_ms": at_ms, **request}
    if name == "speed":
        value = body.get("value")
        if value is None:
            raise InvalidRequest("invalid-field", f"speed needs value: send {_time_example(name)}")
        return {"command": name, "value": "max" if value == "max" else check_speed(value), **request}
    return {"command": name, **request}


def _time_example(name: str, id: int | None = None) -> str:
    text = {"step": '{"command":"step","ms":100}', "run_until": '{"command":"run_until","at_ms":5000}',
            "speed": '{"command":"speed","value":1}'}.get(name, f'{{"command":"{name}"}}')
    return text if id is None else f'{text[:-1]},"id":{id}}}'



def command_line(command: dict) -> bytes:
    """The line written to the launcher's standard input for a command that check_time_command accepted."""
    if command["command"] == "speed":
        value = command["value"]
        text = '"max"' if value == "max" else speed_argument(value)
        number = f',"id":{command["id"]}' if "id" in command else ""
        return f'{{"command":"speed","value":{text}{number}}}\n'.encode()
    return (json.dumps(command, separators=(",", ":")) + "\n").encode()


def request_text(intent: dict) -> str:
    """The text of the request file for an intent that the check_* functions accepted (the keys of
    schema/run-request.schema.json): the version first, then the intent's keys as given."""
    return json.dumps({REQUEST_VERSION_KEY: 1, **intent}, ensure_ascii=False, indent=2) + "\n"


def write_request_file(intent: dict) -> Path:
    """A new request file for this run or check, readable by this user only (POSIX: mode 0600, made so; Windows:
    the user's temporary directory, with the access rights of that directory). The caller removes it
    (control.remove) when the launcher is done with it."""
    try:
        data = request_text(intent).encode("utf-8")
    except UnicodeEncodeError:
        raise InvalidRequest("invalid-field", "a text of the request is not valid Unicode (it has a lone surrogate "
                                              "such as \\ud800): send the text as UTF-8") from None
    fd, name = tempfile.mkstemp(prefix="vehicle-sil-gui-request-", suffix=".json")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
    except BaseException:
        Path(name).unlink(missing_ok=True)
        raise
    return Path(name)


def check_command(launcher: Launcher, file: str, request: Path) -> list[str]:
    """request: the file that write_request_file made (everything that the browser chose is in it)."""
    return launcher.prefix + ["check", "--json", "--request", str(request), "--", file]


def text_check_command(launcher: Launcher, file: str, request: Path, descriptions: Path) -> list[str]:
    """The check of a text that the caller writes to the launcher's standard input, as if it were the file's (the
    paths in it are from the file's directory). It starts no process: each participant's description is the one
    saved in `descriptions` for its command and executable as they are now (a file that is not there: none)."""
    return launcher.prefix + ["check", "--json", "--text-from-stdin", "--descriptions", str(descriptions),
                              "--request", str(request), "--", file]


def save_check_command(launcher: Launcher, file: str, request: Path, descriptions: Path,
                       compare: str | None) -> list[str]:
    """The check of a text that POST api/config would save as `file`: text_check_command's, which starts no process,
    and with compare also check --compare-commands <compare> (the saved file, its links followed, from the root as the
    working directory), which says in `commands` whether the text runs the same commands. A new file has nothing to
    compare with (None)."""
    argv = text_check_command(launcher, file, request, descriptions)
    return argv[:-2] + ["--compare-commands", compare, "--", file] if compare is not None else argv


def describe_command(launcher: Launcher, file: str, request: Path, descriptions: Path) -> list[str]:
    """The check of the file as it is saved, which starts each executable with --describe, as every check does, and
    saves their output in `descriptions` for text_check_command."""
    return launcher.prefix + ["check", "--json", "--write-descriptions", str(descriptions),
                              "--request", str(request), "--", file]


def connections_command(launcher: Launcher, file: str, network: str) -> list[str]:
    """Where one network of the file can be connected on this computer: only the bridge of that network is asked
    (no other one is started, and so no Npcap prompt comes for a network that the user did not open)."""
    return launcher.prefix + ["connections", "--json", "--network", network, "--", file]


def run_command(launcher: Launcher, file: str, request: Path, control_credentials: Path | None = None,
                timeline: Path | None = None) -> list[str]:
    """request: as check_command's. control_credentials: start the launcher's control server (the ECUs' variables)
    on a free port of 127.0.0.1, with the credentials of that file (a token is never an argument: arguments are in
    the hello event and in the list of processes). timeline: the file that the launcher writes the scenario's
    timeline to (only with a scenario in the request, and a launcher with FEATURE_TIMELINE)."""
    argv = launcher.prefix + ["run", "--events", "--stdin-commands"]
    if timeline is not None:
        argv += ["--timeline", str(timeline)]
    if control_credentials is not None:
        argv += ["--control", "0", "--control-credentials", str(control_credentials)]
    return argv + ["--request", str(request), "--", file]


REQUEST_NAME = "the request"
DESCRIPTIONS_NAME = "the saved descriptions"


def hide_request(value: object, path: Path, said: str = REQUEST_NAME) -> object:
    """The launcher's words with the name of this server's own request file (a temporary file that nobody can open)
    said as "the request": in every text, "<file>, line N" too, and the `line` of a finding that was about the file.
    value: a text, or what json.loads made of the launcher's output. said: another name for another such file (the
    saved descriptions)."""
    name = str(path)
    if isinstance(value, str):
        return re.sub(re.escape(name) + r"(?:, line \d+)?", said, value)
    if isinstance(value, list):
        return [hide_request(v, path, said) for v in value]
    if isinstance(value, dict):
        out = {k: hide_request(v, path, said) for k, v in value.items()}
        if value.get("file") == name and "line" in out:
            out["line"] = None
        return out
    return value


def hide_request_in_line(line: str, path: Path) -> str:
    """One line of the launcher's events, with hide_request applied when it names the request file."""
    name = str(path)
    if name not in line and json.dumps(name)[1:-1] not in line:
        return line
    try:
        return json.dumps(hide_request(json.loads(line), path), ensure_ascii=False, separators=(",", ":"))
    except (ValueError, RecursionError):
        return hide_request(line, path)
