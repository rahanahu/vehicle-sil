"""Finding vehicle_sil_launch, asking what it can do, and the only command lines this server gives it."""

from __future__ import annotations

import asyncio
import ipaddress
import json
import os
import re
import shutil
import sys
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from vehicle_sil_control.protocol import (
    NO_CONNECTION,
    SPEED_MAX,
    SPEED_MIN,
    STEP_MS_MAX,
    InvalidRequest,
    command_line,
    request_text,
    serve_prepare_command,
    serve_run_command,
    speed_argument,
    stop_command_line,
    write_request_file,
)

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
# Shared numeric bounds (SPEED_MIN, SPEED_MAX, STEP_MS_MAX) are imported from vehicle_sil_control.protocol
# and compared against C++ launcher constants by tests/launch/test_shared_values.cpp.
# The control links (the launcher's side of the ECUs' variables): the event that says, once per started participant
# before the simulation runs, where its link stands ({"name", "state", "variables" | "version"}). The states
# (connected, no_hello, version_mismatch, not_offered, none) are passed on as they are: the server does not read
# them. The summary keeps the event's fields in the participant's entry, under the same name as the event.
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
FEATURE_SCENARIO_HANDOVER = "scenario-handover"
FEATURE_SCENARIO_CASES = "scenario-cases"
FEATURE_EXTERNAL_REGISTRY = "external-registry"
FEATURE_RECORD_SIGNALS = "record-signals"
FEATURE_RECORD_AFTER_FAIL = "record-after-fail"


def external_registry_uri_error(uri: str) -> str | None:
    """Validates `--external-registry <uri>`: `silkit://<loopback-host>:<port>` (port 1..65535, no path)."""
    fmt_err = (
        "--external-registry must be silkit://<loopback-host>:<port> with a port from 1 to 65535 and no path "
        f"(e.g. silkit://127.0.0.1:8500 or silkit://localhost:8500), not '{uri}'"
    )
    if not uri.startswith("silkit://"):
        return fmt_err
    rest = uri[len("silkit://"):]
    if any(ord(c) <= 0x20 or ord(c) == 0x7F or c in "/?#" for c in rest):
        return fmt_err
    is_v6 = False
    if rest.startswith("["):
        close = rest.find("]")
        if close <= 1 or close + 2 > len(rest) or rest[close + 1] != ":":
            return fmt_err
        host = rest[1:close]
        if "%" in host:
            return fmt_err
        port_part = rest[close + 2:]
        is_v6 = True
    else:
        colon = rest.find(":")
        if colon <= 0 or rest.find(":", colon + 1) != -1:
            return fmt_err
        host = rest[:colon]
        if "[" in host or "]" in host:
            return fmt_err
        port_part = rest[colon + 1:]
    if not port_part or port_part[0] == "0" or not port_part.isascii() or not port_part.isdigit():
        return fmt_err
    port = int(port_part)
    if not 1 <= port <= 65535:
        return fmt_err
    loopback = False
    if is_v6:
        try:
            addr6 = ipaddress.IPv6Address(host)
            mapped = addr6.ipv4_mapped
            loopback = addr6.is_loopback or (mapped is not None and mapped.is_loopback)
        except ValueError:
            return fmt_err
    else:
        try:
            loopback = ipaddress.IPv4Address(host).is_loopback
        except ValueError:
            trimmed = host[:-1] if host.endswith(".") else host
            loopback = trimmed.lower() == "localhost"
    if not loopback:
        return (
            "--external-registry must name a loopback address on this computer "
            f"(e.g. silkit://127.0.0.1:8500 or silkit://localhost:8500), not '{uri}'"
        )
    return None


class LauncherNotFound(Exception):
    pass


@dataclass
class Launcher:
    path: Path
    usable: bool = False
    problems: list[dict] = field(default_factory=list)
    version: dict | None = None
    extra_features: tuple[str, ...] = ()
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
        required = (*REQUIRED_FEATURES, *(f for f in self.extra_features if f not in REQUIRED_FEATURES))
        missing = [f for f in required if f not in self.features]
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


def check_command(launcher: Launcher, file: str, request: Path) -> list[str]:
    """request: the file that write_request_file made (everything that the browser chose is in it)."""
    return launcher.prefix + ["check", "--json", "--request", str(request), "--", file]


def file_only_check_command(
    launcher: Launcher,
    file: str,
    *,
    variant: str | None = None,
    disable: Sequence[str] = (),
    enable: Sequence[str] = (),
    manual: Sequence[str] = (),
) -> list[str]:
    """Builds the `vehicle_sil_launch check --file-only --json` invocation."""
    argv = launcher.prefix + ["check", "--file-only", "--json"]
    if variant is not None:
        argv += ["--variant", variant]
    for name in disable:
        argv += ["--disable", name]
    for name in enable:
        argv += ["--enable", name]
    for name in manual:
        argv += ["--manual", name]
    return argv + ["--", file]


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


def serve_command(launcher: Launcher, file: str, control_credentials: Path | None = None,
                  *, once: bool = False, external_registry_uri: str | None = None) -> list[str]:
    """Builds the `vehicle_sil_launch serve` (or `serve --once`) invocation."""
    argv = launcher.prefix + ["serve"]
    if once:
        argv.append("--once")
    if external_registry_uri is not None:
        argv += ["--external-registry", external_registry_uri]
    if control_credentials is not None:
        argv += ["--control", "0", "--control-credentials", str(control_credentials)]
    return argv + ["--", file]


def with_participant_timeouts(argv: list[str], start_timeout_ms: int, stop_timeout_ms: int) -> list[str]:
    """Inserts non-default participant start/stop timeout options before the trailing `-- <file>`."""
    options: list[str] = []
    if start_timeout_ms != 30000:
        options += ["--participant-start-timeout-ms", str(start_timeout_ms)]
    if stop_timeout_ms != 10000:
        options += ["--participant-stop-timeout-ms", str(stop_timeout_ms)]
    return argv[:-2] + options + argv[-2:]


REQUEST_NAME = "the request"
DESCRIPTIONS_NAME = "the saved descriptions"


def hide_request(value: object, path: Path, said: str = REQUEST_NAME) -> object:
    """The launcher's words with the name of this server's own request file (a temporary file that nobody can open)
    said as "the request": in every text, "<file>, line N" too, and the `line` of a finding that was about the file.
    value: a text, or what json.loads made of the launcher's output. said: another name for another such file (the
    saved descriptions)."""
    name = str(path)
    pattern = re.compile(re.escape(name) + r"(?:, line \d+)?")
    if isinstance(value, str):
        return pattern.sub(said, value)
    if not isinstance(value, (list, dict)):
        return value

    root: list[object] | dict[object, object] = [] if isinstance(value, list) else {}
    stack: list[tuple[list | dict, list | dict]] = [(value, root)]
    while stack:
        src, dst = stack.pop()
        if isinstance(src, list):
            assert isinstance(dst, list)
            for item in src:
                if isinstance(item, str):
                    dst.append(pattern.sub(said, item))
                elif isinstance(item, list):
                    child_list: list[object] = []
                    dst.append(child_list)
                    stack.append((item, child_list))
                elif isinstance(item, dict):
                    child_dict: dict[object, object] = {}
                    dst.append(child_dict)
                    stack.append((item, child_dict))
                else:
                    dst.append(item)
        else:
            assert isinstance(src, dict) and isinstance(dst, dict)
            hide_line = src.get("file") == name
            for k, item in src.items():
                if hide_line and k == "line":
                    dst[k] = None
                elif isinstance(item, str):
                    dst[k] = pattern.sub(said, item)
                elif isinstance(item, list):
                    child_list = []
                    dst[k] = child_list
                    stack.append((item, child_list))
                elif isinstance(item, dict):
                    child_dict = {}
                    dst[k] = child_dict
                    stack.append((item, child_dict))
                else:
                    dst[k] = item
    return root


def _contains_text(value: object, needle: str) -> bool:
    stack: list[object] = [value]
    while stack:
        cur = stack.pop()
        if isinstance(cur, str):
            if needle in cur:
                return True
        elif isinstance(cur, list):
            stack.extend(cur)
        elif isinstance(cur, dict):
            stack.extend(cur.values())
    return False


def hide_request_in_event(event: dict, path: Path, said: str = REQUEST_NAME) -> dict:
    """One parsed launcher event, with hide_request applied when it names the request file."""
    if not _contains_text(event, str(path)):
        return event
    hidden = hide_request(event, path, said)
    assert isinstance(hidden, dict)
    return hidden
