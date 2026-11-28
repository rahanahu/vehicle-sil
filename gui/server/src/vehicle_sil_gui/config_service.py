"""Configuration files, check/save/describe/connections queue, and connect-target TOCTOU guard.

Owns:
- `ConfigRoot`, `Descriptions`, and `_checks` (tracked short-lived check/probe subprocesses)
- `_check_lock` and `_check_waiting` (serialized check/save/connections queue)
- `_names`, `_networks`, and `_targets` (bounded caches and TOCTOU connect-target allowlist)

Does not import `RunManager`, `WsGateway`, or `server` (strict acyclic DAG). Cross-service
notifications (`on_config_saved`, `ensure_usable`, `not_closing`, `facts`) are injected.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
from collections.abc import Awaitable, Callable
from pathlib import Path
from typing import TypeVar

from pydantic import ValidationError

from . import control, dashboard, process
from .allowed import Facts, allowed_save
from .config_save import (check_text, check_unchanged, decide, existing_file, new_file, replace_file, save_request,
                          stamp_of)
from .contracts import (CHECK_FIELDS, CheckFileRequest, CheckTextRequest, ConnectionsRequest, DescribeRequest, RunRequest,
                        format_validation_error, model_allowed_fields, validate_model_defer_file)
from .descriptions import Descriptions
from .launcher import (DESCRIPTIONS_NAME, FEATURE_RECORD_AFTER_FAIL, FEATURE_RECORD_EVENTS,
                       FEATURE_RECORD_SIGNALS, FEATURE_RUN_STOP_AT, FEATURE_SCENARIO_CASES,
                       FEATURE_SCENARIO_HANDOVER, NO_CONNECTION, InvalidRequest, Launcher,
                       cached_check_command, check_command, connections_command, describe_command, hide_request,
                       save_check_command, text_check_command, with_participant_timeouts, write_request_file)
from .paths import ConfigFile, ConfigRoot, PathRejected
from .refusal import Refused
from .scenarios import list_scenarios, relative_to_config, resolve_scenario

MAX_BODY = 64 * 1024
# The text of a configuration file: `text` of api/check and of POST api/config (as UTF-8), the file of GET api/config
MAX_TEXT = 1024 * 1024
# The body of api/check and of POST api/config: a text of MAX_TEXT in JSON however it is escaped (\u0001 takes 6 bytes
# for 1 byte of UTF-8, the most), and the rest of the body.
MAX_TEXT_BODY = 6 * MAX_TEXT + MAX_BODY
CHECK_TIMEOUT_S = 120
USAGE_EXIT_CODE = 4  # vehicle_sil_launch: the command line is wrong
MAX_CHECK_WAITING = 4  # api/check requests waiting for the one that runs
MAX_NAMES_CACHED = 256
USABLE_SHOWN = 5  # the usable targets that a target-not-listed message names; the rest are counted
_MAX_CONFIG_HASH_BYTES = 16 * 1024 * 1024

T = TypeVar("T")
CheckRunner = Callable[[list[str], Path, Path | None, bytes | None], Awaitable[tuple[int, dict]]]


def remember(cache: dict, key: object, value: object) -> None:
    """Puts the value in a cache of at most MAX_NAMES_CACHED entries, as the newest (the oldest goes first)."""
    cache.pop(key, None)
    cache[key] = value
    while len(cache) > MAX_NAMES_CACHED:
        del cache[next(iter(cache))]


def usable_text(usable: tuple[str, ...]) -> str:
    """The usable targets of a list, for a message: the first few in the list's order, and how many more."""
    if not usable:
        return "it has no usable target"
    shown = ", ".join(json.dumps(v, ensure_ascii=False) for v in usable[:USABLE_SHOWN])
    more = len(usable) - USABLE_SHOWN
    return f"the usable ones: {shown}" + (f" and {more} more" if more > 0 else "")


def network_names(result: dict) -> list[str]:
    """The networks of a check result (none when the check could not say: a usage error, a file not read)."""
    networks = result.get("networks")
    return [n["name"] for n in networks if isinstance(n, dict) and isinstance(n.get("name"), str)] \
        if isinstance(networks, list) else []


def _needs_live_describe(result: dict) -> bool:
    """True when saved --descriptions left any describable participant undescribed or could not be read."""
    participants = result.get("participants")
    if isinstance(participants, list) and any(
        isinstance(p, dict)
        and p.get("status") == "not_described"
        and p.get("describe") is not False
        and p.get("enabled") is not False
        and not p.get("manual")
        for p in participants
    ):
        return True
    findings = result.get("findings")
    return isinstance(findings, list) and any(
        isinstance(f, dict) and f.get("code") == "descriptions-not-used" for f in findings
    )


def file_sha256(path: Path) -> str | None:
    try:
        with path.open("rb") as f:
            h = hashlib.sha256()
            total = 0
            while chunk := f.read(65536):
                total += len(chunk)
                if total > _MAX_CONFIG_HASH_BYTES:
                    return None
                h.update(chunk)
            return h.hexdigest()
    except OSError:
        return None


def read_at_most(path: Path, size: int) -> bytes:
    with open(path, "rb") as f:
        return f.read(size)


def unreadable(relative: str, e: OSError) -> Exception:
    """The refusal of a configuration file that the OS did not let the server read."""
    if isinstance(e, FileNotFoundError):
        return PathRejected("file-not-found", f"{relative!r} is not there any more: choose one that GET api/configs "
                                              "lists")
    return Refused(422, "config-unreadable", f"{relative} cannot be read ({e.strerror or e}): check that this user "
                                             "can read it")


def utf8_text(relative: str, data: bytes) -> str:
    """The bytes of a configuration file as text; Refused (422 config-unreadable) when they are not UTF-8 text (a
    UTF-16 file is not a configuration file: ConfigRoot.configuration finds no marker in it)."""
    if b"\0" in data:
        why = f"it has a NUL byte at byte {data.index(0)}"
    else:
        try:
            return data.decode("utf-8")
        except UnicodeDecodeError as e:
            why = f"byte {e.start} is not UTF-8"
    raise Refused(422, "config-unreadable", f"{relative} is not UTF-8 text ({why}): save it as UTF-8 without NUL "
                                            "characters, the encoding that the launcher reads")


def refusal_of(e: PathRejected | InvalidRequest) -> Refused:
    """Translates path or request validation errors into a uniform HTTP `Refused`."""
    if isinstance(e, PathRejected):
        return Refused(404 if e.code == "file-not-found" else 400, e.code, str(e))
    return Refused(400 if e.code != "launcher-unusable" else 503, e.code, str(e))


def make_request_file(intent: dict) -> Path:
    """The request file of a check or a run; Refused (500 request-file) when no file can be made."""
    try:
        return write_request_file(intent)
    except OSError as e:
        raise Refused(500, "request-file", f"the file that carries the request to the launcher could not be "
                                           f"made in the temporary directory ({e}): free space there, or set "
                                           "TMPDIR to a directory that this user can write") from None


def _check_known_participants(names: list[str] | None, known: list[str]) -> list[str]:
    """Ensures validated participant names exist in `known`; raises Refused (400 unknown-participant) otherwise."""
    if not names:
        return []
    unknown = [n for n in names if n not in known]
    if unknown:
        raise Refused(
            400,
            "unknown-participant",
            f"no participant named {', '.join(repr(n) for n in unknown)} in this file: the "
            f"participants are {', '.join(known) or 'none'}",
        )
    return list(names)


def _noop() -> None:
    return None


class ConfigService:
    """Workspace configuration files, serialized check/save/connections queue, and connect-target TOCTOU guard."""

    def __init__(
        self,
        root: Path | ConfigRoot,
        launcher: Launcher,
        *,
        panel_key: str = "",
        participant_start_timeout_ms: int = 30000,
        participant_stop_timeout_ms: int = 10000,
        check_timeout_s: float = CHECK_TIMEOUT_S,
        command_changes_allowed: bool = False,
        check_runner: CheckRunner | None = None,
        not_closing: Callable[[], None] = _noop,
        ensure_usable: Callable[[], Awaitable[None]] | None = None,
        facts: Callable[[], Facts] = Facts,
        on_config_saved: Callable[[str], Awaitable[None]] | None = None,
    ):
        self.root = root if isinstance(root, ConfigRoot) else ConfigRoot(root)
        self.launcher = launcher
        self._panel_key = panel_key
        self._participant_start_timeout_ms = participant_start_timeout_ms
        self._participant_stop_timeout_ms = participant_stop_timeout_ms
        self._check_timeout_s = check_timeout_s
        self._command_changes_allowed = command_changes_allowed
        self._check_runner = check_runner
        self._not_closing = not_closing
        self._ensure_usable_cb = ensure_usable
        self._facts = facts
        self._on_config_saved = on_config_saved
        self.descriptions = Descriptions()
        self._checks: set[asyncio.subprocess.Process] = set()  # checks and launcher version/check probes
        self.launcher.track = self._checks
        self._check_lock = asyncio.Lock()
        self._check_waiting = 0
        self._names: dict[tuple[str, str | None], tuple[tuple, list[str]]] = {}
        self._networks: dict[tuple[str, str | None], tuple[tuple, list[str]]] = {}
        # Of each (file, network), the targets that its last list (POST api/connections) gave as usable, with the
        # file's stamp and SHA-256 then: the only targets that a check or a run passes on (see validate_connect_targets).
        self._targets: dict[tuple[str, str], tuple[tuple, str | None, tuple[str, ...]]] = {}

    @property
    def checks(self) -> set[asyncio.subprocess.Process]:
        return self._checks

    def bind_runtime_hooks(
        self,
        *,
        not_closing: Callable[[], None],
        ensure_usable: Callable[[], Awaitable[None]],
        facts: Callable[[], Facts],
        on_config_saved: Callable[[str], Awaitable[None]],
    ) -> None:
        """Wires server lifecycle hooks without introducing a ConfigService -> RunManager import."""
        self._not_closing = not_closing
        self._ensure_usable_cb = ensure_usable
        self._facts = facts
        self._on_config_saved = on_config_saved

    async def _ensure_usable(self) -> None:
        if self._ensure_usable_cb is not None:
            await self._ensure_usable_cb()
            return
        await self.launcher.probe()
        if not self.launcher.usable:
            raise Refused(503, "launcher-unusable",
                          "; ".join(p["message"] for p in self.launcher.problems) or "the launcher is not usable")

    # ------------------------------------------------------------ workspace & read endpoints

    def resolve(self, file_param: object) -> ConfigFile:
        try:
            return self.root.resolve(file_param)
        except PathRejected as e:
            raise refusal_of(e) from None

    def resolve_scenario(self, config: ConfigFile, wanted: object) -> str | None:
        try:
            return resolve_scenario(self.root, config, wanted)
        except PathRejected as e:
            raise refusal_of(e) from None

    async def list_configs(self) -> dict:
        files, truncated = await asyncio.to_thread(self.root.list)
        return {"root": str(self.root.root), "files": files, "truncated": truncated}

    async def get_config(self, file_param: object) -> dict:
        """The text of a configuration file, with the SHA-256 of its bytes (stamp). The text is the bytes read as
        UTF-8, as they are (a BOM and the line ends too)."""
        config = await self._configuration(file_param)
        try:
            data = await asyncio.to_thread(read_at_most, config.absolute, MAX_TEXT + 1)
        except OSError as e:
            err = unreadable(config.relative, e)
            raise refusal_of(err) if isinstance(err, PathRejected) else err from None
        if len(data) > MAX_TEXT:
            raise Refused(413, "config-too-large",
                          f"{config.relative} is larger than {MAX_TEXT} bytes (1 MiB), the most that the GUI "
                          "server reads: edit it in a text editor")
        text = utf8_text(config.relative, data)
        return {"file": config.relative, "text": text, "stamp": hashlib.sha256(data).hexdigest()}

    async def dashboard(self, file_param: object) -> dict:
        config = self.resolve(file_param)
        result = await asyncio.to_thread(dashboard.read_dashboard, self.root, config, f"panels/{self._panel_key}/")
        return result.to_json(config.relative)

    async def scenarios(self, file_param: object) -> dict:
        """The scenario files of a configuration (the .yaml and .yml files in `scenarios` next to it)."""
        config = self.resolve(file_param)
        directory, found, files, truncated = await asyncio.to_thread(list_scenarios, self.root, config)
        return {"file": config.relative, "directory": directory, "found": found,
                "scenarios": files, "truncated": truncated}

    async def read_panel_module(self, path_str: str) -> bytes | None:
        """Reads a validated panel JavaScript module under root, or None if missing/invalid/too large."""
        try:
            path = await asyncio.to_thread(dashboard.module_path, self.root, path_str)
            body = await asyncio.to_thread(read_at_most, path, dashboard.MAX_MODULE_BYTES + 1)
        except (PathRejected, OSError):
            return None
        return body if len(body) <= dashboard.MAX_MODULE_BYTES else None

    # ------------------------------------------------------------ save, check, describe, connections

    def check_save_allowed(self) -> None:
        self._not_closing()
        save = allowed_save(self._facts())
        if not save["ok"]:
            raise Refused(403, save["reason"], save["message"])

    async def save_config(self, body: dict) -> dict:
        """Saves the text of a configuration file (config_save.py): an existing configuration file with the stamp the
        screen read, or a new file (stamp null) only with --allow-command-changes."""
        self.check_save_allowed()
        try:
            relative, data, stamp = save_request(body, MAX_TEXT)
            name = "/".join(self.root.parts(relative))
        except (PathRejected, InvalidRequest) as e:
            raise refusal_of(e) from None
        if stamp is None and not self._command_changes_allowed:
            raise Refused(403, "commands-changed",
                          f"{name} would be a new file (stamp is null), so there is no saved file to compare "
                          "its commands with; the GUI server makes new files only when it was started with "
                          "--allow-command-changes: create the file in a text editor, or start the GUI with "
                          "--allow-command-changes")
        config = await self._save_target(relative, stamp)  # refused here before it waits in the queue
        check_text(config.relative, data)
        await self._ensure_usable()
        await self._in_check_queue(lambda: self._save_locked(config, data, stamp))
        return {"file": config.relative, "stamp": stamp_of(data)}

    async def _save_locked(self, config: ConfigFile, data: bytes, stamp: str | None) -> None:
        again = await self._save_target(config.relative, stamp)
        if again.absolute != config.absolute:
            raise Refused(409, "config-changed", f"{config.relative} leads to another file than when the save was "
                                                 "sent (a symbolic link changed): read it again and save again")
        await self._unchanged(config, stamp)
        descriptions = await asyncio.to_thread(self.descriptions.path_of, (config.relative, None))
        request = make_request_file({})
        real = config.absolute.relative_to(self.root.root).as_posix()  # what --compare-commands reads: no link
        try:
            argv = save_check_command(self.launcher, config.relative, request, descriptions,
                                      real if stamp is not None else None)
            _, result = await self._run_launcher_check(argv, request, descriptions, data)
        finally:
            control.remove(request)
        decide(config.relative, result, self._command_changes_allowed)
        await self._unchanged(config, stamp)
        try:
            await asyncio.to_thread(replace_file, config.absolute, data, stamp is None)
        except FileExistsError:
            raise Refused(409, "config-changed", f"{config.relative} was made by another program while the save "
                                                 "waited: read it (GET api/config) and save it with its stamp, or "
                                                 "choose another name") from None
        except OSError as e:
            raise Refused(409, "config-busy",
                          f"{config.relative} could not be replaced ({e.strerror or e}): another program may have it "
                          "open, or this user may not write it or its directory; close that program or fix the "
                          "permissions, then save again") from None
        self.descriptions.forget(config.relative)
        if self._on_config_saved is not None:
            await self._on_config_saved(config.relative)
        for kept in (self._names, self._networks, self._targets):
            for key in [k for k in kept if k[0] == config.relative]:
                del kept[key]

    async def check(self, body: dict) -> dict:
        if "text" in body:
            return await self._check_text(body)
        try:
            req, _ = validate_model_defer_file(CheckFileRequest, body)
        except ValidationError as exc:
            code, message = format_validation_error(exc, endpoint="api/check", allowed_fields=CHECK_FIELDS)
            raise Refused(400, code, message) from None
        config = self.resolve(body.get("file"))
        await self._ensure_usable()
        scenario = self.resolve_scenario(config, body.get("scenario"))
        intent, disable, manual = await self.build_intent(config, req, scenario)
        code, result = await self._in_check_queue(lambda: self._check_and_guard(config.relative, intent))
        return {"file": config.relative, "disable": disable, "manual": manual,
                "scenario": scenario, "exit_code": code, "check": result}

    async def _check_text(self, body: dict) -> dict:
        """api/check with text: checks unsaved text using saved --describe output without starting processes."""
        try:
            req, _ = validate_model_defer_file(CheckTextRequest, body)
        except ValidationError as exc:
            code, message = format_validation_error(
                exc, endpoint="api/check", allowed_fields=model_allowed_fields(CheckTextRequest),
                extra_with_text=True,
            )
            raise Refused(400, code, message) from None
        config = await self._configuration(body.get("file"))
        data = req.text.encode("utf-8")
        if len(data) > MAX_TEXT:
            raise Refused(413, "text-too-large",
                          f"text is {len(data)} bytes as UTF-8, more than the {MAX_TEXT} (1 MiB) that the GUI server "
                          "checks: save the file and check it without text")
        await self._ensure_usable()
        intent = {"variant": req.variant} if req.variant else {}
        if req.strict:
            intent["strict"] = True
        code, result = await self._in_check_queue(lambda: self._text_check_locked(config.relative, intent, data))
        return {"file": config.relative, "disable": [], "manual": [], "scenario": None,
                "exit_code": code, "check": result}

    async def describe(self, req: DescribeRequest, raw_file: object) -> dict:
        """Describes the executables of the saved file again, saving their --describe output."""
        config = await self._configuration(raw_file)
        await self._ensure_usable()
        code, result = await self._in_check_queue(lambda: self._describe_locked(config.relative, req.variant))
        return {"file": config.relative, "variant": req.variant, "exit_code": code, "check": result}

    async def connections(self, req: ConnectionsRequest, raw_file: object) -> dict:
        """Lists usable external connection targets for one network of the file."""
        network = req.network
        config = self.resolve(raw_file)
        await self._ensure_usable()
        # Asking for a new list ends the last one at once: no run starts with it while this one waits or runs
        self._targets.pop((config.relative, network), None)
        code, result = await self._in_check_queue(lambda: self._list_targets(config.relative, network))
        if code == USAGE_EXIT_CODE:  # the only usage error of this command line: a network the file lacks
            findings = [f for f in result.get("findings", []) if isinstance(f, dict)]
            said = "; ".join(f["message"] for f in findings if isinstance(f.get("message"), str))
            raise Refused(400, "unknown-network",
                          f"{config.relative} has no network {network!r} to list (the launcher says: "
                          f"{said or 'usage error'}): give the name of a can or ethernet network of the file, "
                          "as networks[].name of its check", {"network": network, "findings": findings})
        return {"file": config.relative, "network": network, "exit_code": code, "connections": result}

    # ------------------------------------------------------------ collaborator API for RunManager

    def stamp(self, relative: str) -> tuple:
        try:
            st = self.root.resolve(relative).absolute.stat()
        except (PathRejected, OSError):
            raise Refused(404, "file-not-found", f"{relative!r} is not there any more: choose one that GET "
                                                 "api/configs lists") from None
        return (st.st_mtime_ns, st.st_size)

    def cached_names(self, relative: str, variant: str | None, current_stamp: tuple) -> list[str] | None:
        cached = self._names.get((relative, variant))
        return cached[1] if cached and cached[0] == current_stamp else None

    def cached_networks(self, relative: str, variant: str | None, current_stamp: tuple) -> list[str] | None:
        cached = self._networks.get((relative, variant))
        return cached[1] if cached and cached[0] == current_stamp else None

    def remember_names(self, relative: str, variant: str | None, current_stamp: tuple, names: list[str]) -> None:
        remember(self._names, (relative, variant), (current_stamp, names))

    async def participant_names(self, relative: str, variant: str | None = None) -> list[str]:
        """The participant names of the file (of its variant), from the last check of it, or from a new check."""
        cached = self.cached_names(relative, variant, self.stamp(relative))
        if cached is not None:
            return cached
        await self._in_check_queue(
            lambda: self._check_locked(relative, {"variant": variant} if variant else {}, write_cache=False)
        )
        return self._names.get((relative, variant), ((), []))[1]

    async def build_intent(
        self,
        config: ConfigFile,
        req: RunRequest,
        scenario: str | None,
        *,
        known_participants: list[str] | None = None,
        fetch_names: bool = True,
    ) -> tuple[dict, list[str], list[str]]:
        """Builds the request file's keys: (intent, disable, manual), validating dynamic features & participants."""
        if (req.stop_at_ms is not None or req.late_tolerance_ms is not None) \
                and FEATURE_RUN_STOP_AT not in self.launcher.features:
            raise Refused(409, "launcher-unsupported",
                          "the launcher does not support the requested run timing: install one with run-stop-at")
        if req.case is not None and FEATURE_SCENARIO_CASES not in self.launcher.features:
            raise Refused(409, "launcher-unsupported",
                          "the launcher does not support scenario cases in requests: install one with scenario-cases")
        record = list(dict.fromkeys(req.record)) if req.record else []
        if record and FEATURE_RECORD_SIGNALS not in self.launcher.features:
            raise Refused(409, "launcher-unsupported",
                          "the launcher does not support extra recorded signals in requests: install one with "
                          "record-signals")
        if req.record_after_fail_ms is not None and FEATURE_RECORD_AFTER_FAIL not in self.launcher.features:
            raise Refused(409, "launcher-unsupported",
                          "the launcher does not support post-failure tail recording in requests: install one with "
                          "record-after-fail")
        if req.record_events is True and FEATURE_RECORD_EVENTS not in self.launcher.features:
            raise Refused(409, "launcher-unsupported",
                          "the launcher does not support virtual-time ECU event recording in requests: install one "
                          "with record-events")
        if req.after_scenario == "pause":
            if FEATURE_SCENARIO_HANDOVER not in self.launcher.features:
                raise Refused(409, "launcher-unsupported",
                              "the launcher cannot hand a scenario run over to manual control: install one with "
                              "scenario-handover")
            if scenario is None:
                raise Refused(400, "invalid-field",
                              "after_scenario 'pause' needs a scenario: choose a scenario, or omit after_scenario")
            if req.stop_at_ms is not None:
                raise Refused(400, "invalid-field",
                              "after_scenario 'pause' cannot be used with stop_at_ms: remove stop_at_ms, or use "
                              "after_scenario 'stop'")
        after_scenario = (
            "pause" if req.after_scenario == "pause"
            else "stop" if (req.after_scenario == "stop" and FEATURE_SCENARIO_HANDOVER in self.launcher.features)
            else None
        )
        speed = float(req.speed) if isinstance(req.speed, (int, float)) else req.speed
        wanted, wanted_manual = req.disable, req.manual
        if wanted or wanted_manual:
            if fetch_names:
                known = await self.participant_names(config.relative, req.variant)
                disable = _check_known_participants(wanted, known)
                manual = _check_known_participants(wanted_manual, known)
            elif known_participants is not None:
                disable = _check_known_participants(wanted, known_participants)
                manual = _check_known_participants(wanted_manual, known_participants)
            else:
                disable, manual = list(wanted or ()), list(wanted_manual or ())
        else:
            disable, manual = [], []
        intent = {"variant": req.variant, "params": req.params or None,
                  "scenario": relative_to_config(config, scenario) if scenario is not None else None,
                  "case": req.case,
                  "after_scenario": after_scenario,
                  "disable": disable or None, "enable": list(req.enable) if req.enable else None,
                  "manual": manual or None, "record": record or None,
                  "record_after_fail_ms": req.record_after_fail_ms,
                  "record_events": True if req.record_events else None, "strict": req.strict or None, "speed": speed,
                  "start_paused": req.start_paused or None, "connect": req.connect or None,
                  "time_policy": req.time_policy, "stop_at_ms": req.stop_at_ms,
                  "late_tolerance_ms": req.late_tolerance_ms}
        return {k: v for k, v in intent.items() if v is not None}, disable, manual

    def validate_connect_targets(
        self,
        relative: str,
        connect: dict[str, str],
        networks: list[str] | None,
        checked_stamp: tuple | None = None,
        checked_sha256: str | None = None,
    ) -> None:
        """Enforces the TOCTOU connect-target allowlist populated by POST api/connections."""
        external = [(network, target) for network, target in connect.items() if target != NO_CONNECTION]
        if not external:
            return
        current_stamp = self.stamp(relative)
        need_sha256 = checked_sha256 is not None or any(
            (listed := self._targets.get((relative, network))) is not None and listed[1] is not None
            for network, _ in external
        )
        current_sha256 = file_sha256(self.resolve(relative).absolute) if need_sha256 else None
        for network, target in external:
            if (
                (checked_stamp is not None and checked_stamp != current_stamp)
                or (checked_sha256 is not None and current_sha256 != checked_sha256)
            ):
                raise Refused(400, "target-not-listed",
                              f"{relative} has changed since this run was checked, so the networks that it connects "
                              "may be others: start the run again", {"network": network, "target": target})
            if networks is not None and network not in networks:
                continue
            listed = self._targets.get((relative, network))
            if (
                listed is None
                or listed[0] != current_stamp
                or (listed[1] is not None and current_sha256 != listed[1])
            ):
                why = (f"there is no list of where {network!r} of {relative} can be connected" if listed is None else
                       f"{relative} has changed since {network!r} was listed")
            elif target not in listed[2]:
                why = (f"the last list of {network!r} of {relative} does not have {target!r} as usable "
                       f"({usable_text(listed[2])})")
            else:
                continue
            raise Refused(400, "target-not-listed",
                          f"{why}: ask POST api/connections {{\"file\": {json.dumps(relative)}, \"network\": "
                          f"{json.dumps(network)}}} and give a value that it lists with usable: true, exactly as "
                          "written there, or \"none\" to keep the network on SIL Kit only",
                          {"network": network, "target": target})

    def cancel_checks(self) -> None:
        for proc in list(self._checks):
            process.kill_tree(proc)

    async def cleanup(self) -> None:
        await asyncio.to_thread(self.descriptions.remove_all)

    # ------------------------------------------------------------ internal file & check-queue helpers

    async def _save_target(self, relative: object, stamp: str | None) -> ConfigFile:
        try:
            return await asyncio.to_thread(new_file if stamp is None else existing_file, self.root, relative)
        except PathRejected as e:
            raise refusal_of(e) from None
        except OSError as e:
            err = unreadable(str(relative), e)
            raise refusal_of(err) if isinstance(err, PathRejected) else err from None

    async def _unchanged(self, config: ConfigFile, stamp: str | None) -> None:
        try:
            await asyncio.to_thread(check_unchanged, config, stamp, MAX_TEXT)
        except OSError as e:
            err = unreadable(config.relative, e)
            raise refusal_of(err) if isinstance(err, PathRejected) else err from None

    async def _configuration(self, relative: object) -> ConfigFile:
        try:
            return await asyncio.to_thread(self.root.configuration, relative)
        except PathRejected as e:
            raise refusal_of(e) from None
        except OSError as e:
            err = unreadable(str(relative), e)
            raise refusal_of(err) if isinstance(err, PathRejected) else err from None

    async def _check_and_guard(self, relative: str, intent: dict) -> tuple[int, dict]:
        code, result = await self._check_locked(relative, intent)
        self.validate_connect_targets(relative, intent.get("connect", {}), network_names(result))
        return code, result

    async def _in_check_queue(self, work: Callable[[], Awaitable[T]]) -> T:
        """One check (or list, or save) at a time: the others wait (MAX_CHECK_WAITING of them, check_timeout_s each)."""
        self._not_closing()
        if self._check_waiting >= MAX_CHECK_WAITING:
            raise Refused(503, "check-busy", f"{MAX_CHECK_WAITING} checks are already waiting for the one that "
                                             "runs: wait for their answers, then check again")
        self._check_waiting += 1
        try:
            async with asyncio.timeout(self._check_timeout_s):
                await self._check_lock.acquire()
        except TimeoutError:
            raise Refused(503, "check-busy", f"another check has run for more than {self._check_timeout_s:g} s: "
                                             "try again when it has ended") from None
        finally:
            self._check_waiting -= 1
        try:
            self._not_closing()
            return await work()
        finally:
            self._check_lock.release()

    async def _check_locked(self, relative: str, intent: dict, *, write_cache: bool = True) -> tuple[int, dict]:
        stamp = self.stamp(relative)
        key = (relative, intent.get("variant"))
        has_connect = bool(intent.get("connect"))
        subset_only = bool(intent.get("disable") or intent.get("enable") or intent.get("manual"))
        request = make_request_file(intent)
        try:
            if not has_connect and self.descriptions.made_from(key) == stamp:
                desc_path = await asyncio.to_thread(self.descriptions.path_of, key)
                code, result = await self._run_check(
                    relative, intent, request, stamp, descriptions=desc_path, write_descriptions=False,
                )
                if not _needs_live_describe(result):
                    return code, result
            if write_cache and not subset_only and not has_connect:
                plain_intent = not any(k != "variant" for k in intent)
                desc_path = await asyncio.to_thread(self.descriptions.path_of, key)
                try:
                    code, result = await self._run_check(
                        relative, intent, request, stamp, descriptions=desc_path, write_descriptions=True,
                    )
                except Refused as e:
                    if e.code != "shutting-down":
                        if plain_intent:
                            self.descriptions.made(key, stamp, False)
                        else:
                            self.descriptions.forget_key(key)
                    raise
                usable = result.get("result") not in ("usage", "unreadable")
                if usable or plain_intent:
                    self.descriptions.made(key, stamp, usable)
                else:
                    self.descriptions.forget_key(key)
                return code, result
            return await self._run_check(relative, intent, request, stamp)
        finally:
            control.remove(request)

    async def _describe_locked(self, relative: str, variant: str | None) -> tuple[int, dict]:
        key = (relative, variant)
        stamp = self.stamp(relative)
        path = await asyncio.to_thread(self.descriptions.path_of, key)
        intent = {"variant": variant} if variant else {}
        request = make_request_file(intent)
        try:
            code, result = await self._run_check(
                relative, intent, request, stamp, descriptions=path, write_descriptions=True,
            )
        except Refused as e:
            if e.code != "shutting-down":
                self.descriptions.made(key, stamp, False)
            raise
        finally:
            control.remove(request)
        self.descriptions.made(key, stamp, result.get("result") not in ("usage", "unreadable"))
        return code, result

    async def _text_check_locked(self, relative: str, intent: dict, text: bytes) -> tuple[int, dict]:
        key = (relative, intent.get("variant"))
        if self.descriptions.made_from(key) != self.stamp(relative):
            try:
                await self._describe_locked(*key)
            except Refused as e:  # the text is checked with no descriptions; api/describe says what went wrong
                if e.code == "shutting-down":
                    raise
        path = await asyncio.to_thread(self.descriptions.path_of, key)
        request = make_request_file(intent)
        try:
            return await self._run_launcher_check(
                text_check_command(self.launcher, relative, request, path), request, path, text,
            )
        finally:
            control.remove(request)

    async def _run_check(
        self,
        relative: str,
        intent: dict,
        request: Path,
        stamp: tuple,
        descriptions: Path | None = None,
        *,
        write_descriptions: bool = True,
    ) -> tuple[int, dict]:
        if descriptions is None:
            argv = check_command(self.launcher, relative, request)
        elif write_descriptions:
            argv = describe_command(self.launcher, relative, request, descriptions)
        else:
            argv = cached_check_command(self.launcher, relative, request, descriptions)
        code, result = await self._run_launcher_check(argv, request, descriptions)
        names = [p["name"] for p in result["participants"] if isinstance(p, dict) and isinstance(p.get("name"), str)]
        if names or result.get("result") not in ("usage",):
            remember(self._names, (relative, intent.get("variant")), (stamp, names))
        if result.get("result") == "passed":
            remember(self._networks, (relative, intent.get("variant")), (stamp, network_names(result)))
        return code, result

    async def _run_launcher_check(
        self,
        argv: list[str],
        request: Path,
        descriptions: Path | None = None,
        text: bytes | None = None,
    ) -> tuple[int, dict]:
        if self._check_runner is not None:
            return await self._check_runner(argv, request, descriptions, text)
        return await self._launcher_check(argv, request, descriptions, text)

    def _participant_timeouts(self, argv: list[str]) -> list[str]:
        return with_participant_timeouts(argv, self._participant_start_timeout_ms,
                                         self._participant_stop_timeout_ms)

    async def _launcher_check(
        self,
        argv: list[str],
        request: Path,
        descriptions: Path | None = None,
        text: bytes | None = None,
    ) -> tuple[int, dict]:
        try:
            code, out, err = await process.run_to_end(
                self._participant_timeouts(argv), cwd=self.root.root, timeout_s=self._check_timeout_s,
                track=self._checks, stdin=text,
            )
        except TimeoutError:
            raise Refused(504, "check-timeout", f"the check did not end in {self._check_timeout_s:g} s (it was "
                                                "stopped): an ECU's --describe may hang") from None
        except OSError as e:
            raise Refused(502, "launcher-start", f"cannot start {self.launcher.path}: {e}") from None
        self._not_closing()

        def hidden(value: object) -> object:
            value = hide_request(value, request)
            return value if descriptions is None else hide_request(value, descriptions, DESCRIPTIONS_NAME)
        try:
            result = hidden(json.loads(out))
        except (ValueError, RecursionError):
            result = None
        if not isinstance(result, dict) or not isinstance(result.get("participants"), list):
            tail = hidden(err.decode("utf-8", "replace")).strip().splitlines()[-10:]
            raise Refused(502, "launcher-output", f"check --json exited with code {code} and did not write the "
                                                  "check object", tail)
        return code, result

    async def _list_targets(self, relative: str, network: str) -> tuple[int, dict]:
        stamp = self.stamp(relative)
        sha256 = file_sha256(self.resolve(relative).absolute)
        self._targets.pop((relative, network), None)
        try:
            code, out, err = await process.run_to_end(
                connections_command(self.launcher, relative, network),
                cwd=self.root.root, timeout_s=self._check_timeout_s, track=self._checks,
            )
        except TimeoutError:
            raise Refused(504, "check-timeout", f"the list of where {network!r} can be connected did not end in "
                                                f"{self._check_timeout_s:g} s (it was stopped): a bridge's list may "
                                                "hang") from None
        except OSError as e:
            raise Refused(502, "launcher-start", f"cannot start {self.launcher.path}: {e}") from None
        self._not_closing()
        try:
            result = json.loads(out)
        except (ValueError, RecursionError):
            result = None
        if not isinstance(result, dict) or not isinstance(result.get("networks"), list):
            tail = err.decode("utf-8", "replace").strip().splitlines()[-10:]
            raise Refused(502, "launcher-output", f"connections --json exited with code {code} and did not write "
                                                  "the connections object", tail)
        usable = tuple(dict.fromkeys(
            t["value"] for n in result["networks"]
            if isinstance(n, dict) and n.get("name") == network and isinstance(n.get("list"), dict)
            and isinstance(n["list"].get("targets"), list)
            for t in n["list"]["targets"]
            if isinstance(t, dict) and t.get("usable") is True and isinstance(t.get("value"), str)))
        if code != USAGE_EXIT_CODE:  # no such network: nothing to remember
            remember(self._targets, (relative, network), (stamp, sha256, usable))
        return code, result
