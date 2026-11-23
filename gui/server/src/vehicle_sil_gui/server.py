"""The HTTP and WebSocket server: the API, the checks of every request, the launcher as a child process.

Concurrency: everything runs on one event loop, so state changes only between awaits. Each handler decides
and records what it does (a run starting, a stop requested) before its first await, and checks again after
each await whether the server has started to end. Every launcher started is kept in `_runs` until it has
exited and its output is read, so the end of the server stops all of them, whatever raced.
"""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import hmac
import itertools
import json
import logging
import os
import secrets
import time
from collections.abc import Awaitable, Callable
from typing import TypeVar
from dataclasses import dataclass, field
from pathlib import Path

from aiohttp import WSCloseCode, WSMsgType, web
import yaml

from . import __version__, control, dashboard, process
from .allowed import Facts, Numbered, allowed as allowed_of, allowed_save, allowed_start
from .hub import Client, Hub, Outgoing, encode
from .launcher import (CONNECTIONS_FIELDS, DESCRIBE_FIELDS, DESCRIPTIONS_NAME, NO_CONNECTION, REQUEST_FIELDS,
                       FEATURE_RUN_STOP_AT, FEATURE_TIMELINE, SPEED_MAX, SPEED_MIN, STEP_MS_MAX, TEXT_FIELDS,
                       InvalidRequest, Launcher,
                       check_command, check_connect, check_disable, check_enable, check_fields, check_manual,
                       check_network, check_params, check_run_millis, check_speed, check_strict, check_name_list, check_time_command, check_time_policy,
                       check_variant, connections_command, describe_command, hide_request,
                       hide_request_in_line, save_check_command, text_check_command,
                       with_participant_timeouts, write_request_file)
from . import client_files
from .config_save import (check_text, check_unchanged, decide, existing_file, new_file, replace_file, save_request,
                          stamp_of)
from .descriptions import Descriptions
from .paths import ConfigFile, ConfigRoot, PathRejected
from .recording import Recording
from .refusal import Refused
from .runtime import LauncherRuntime, OneShotRuntime, RunHandle, RunSpec, ServeRuntime
from .scenarios import list_scenarios, relative_to_config, resolve_scenario
from .session import SILENT_AFTER_S, Limits, Session, check_timing
from .source import Source
from .static import MAX_FILE_BYTES, NOT_BUILT, StaticFiles, TooLarge
from .timelines import (KEPT as TIMELINES_KEPT, PAGE_FILES, Timelines, api_path, find_page_dir, read_page_file,
                        written_whole)
from .variables import AT_MS_MAX, Limits as VariableLimits, Rejected, Variables, Watch, check_watch

PROTOCOL = 3  # vehicle_sil_gui: the version of the shapes in gui/protocol
MAX_BODY = 64 * 1024
# The text of a configuration file: `text` of api/check and of POST api/config (as UTF-8), the file of GET api/config
MAX_TEXT = 1024 * 1024
# The body of api/check and of POST api/config: a text of MAX_TEXT in JSON however it is escaped (\u0001 takes 6 bytes
# for 1 byte of UTF-8, the most), and the rest of the body.
MAX_TEXT_BODY = 6 * MAX_TEXT + MAX_BODY
MAX_WS_MESSAGE = 256 * 1024  # a watch that names many variables
MAX_SOCKETS = 32  # authenticated WebSocket connections
MAX_UNAUTHENTICATED = 16  # connections waiting for their auth; when full, the oldest is closed for a new one
AUTH_TIMEOUT_S = 5
WS_CLOSE_TIMEOUT_S = 2  # waiting for the screen's answer to a close
CHECK_TIMEOUT_S = 120
USAGE_EXIT_CODE = 4  # vehicle_sil_launch: the command line is wrong
MAX_CHECK_WAITING = 4  # api/check requests waiting for the one that runs
MAX_NAMES_CACHED = 256
MAX_LINE = 16 * 1024 * 1024  # one line of the launcher's events (a check with many variables is long)
STDIN_DRAIN_S = 2  # writing a stop command to a launcher that does not read its input
ESCALATE_WAIT_S = 5  # at the server's end, after asking the group to stop, before killing it
START_WAIT_S = 5  # at the server's end, for a launcher being started to be registered
VARIABLES_CLOSE_S = 3  # at a run's end, for the connection to its control server to close
SOCKETS_CLOSE_S = 3  # at the server's end, for the WebSocket connections to close
# The longest the server's end takes after --stop-timeout (see cli.py): the waits above, the output read
# after the launcher's exit, and aiohttp's own end (twice its shutdown_timeout).
RUNNER_SHUTDOWN_S = 1.0
END_BUDGET_S = (START_WAIT_S + ESCALATE_WAIT_S + process.KILL_WAIT_S + process.OUTPUT_GRACE_S + 1
                + VARIABLES_CLOSE_S + SOCKETS_CLOSE_S + 2 * RUNNER_SHUTDOWN_S)

CSP = "; ".join([
    "default-src 'self'",
    "script-src 'self'",
    "style-src 'self' 'unsafe-inline'",  # Angular puts component styles in <style>
    "img-src 'self' data:",
    "font-src 'self'",
    "connect-src 'self'",
    "object-src 'none'",
    "base-uri 'self'",
    "form-action 'none'",
    "frame-ancestors 'none'",
])
COMMON_HEADERS = {
    "Content-Security-Policy": CSP,
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "no-referrer",
    "X-Frame-Options": "DENY",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Resource-Policy": "same-origin",
}

log = logging.getLogger("vehicle_sil_gui")
T = TypeVar("T")


def remember(cache: dict, key: object, value: object) -> None:
    """Puts the value in a cache of at most MAX_NAMES_CACHED entries, as the newest (the oldest goes first)."""
    cache.pop(key, None)
    cache[key] = value
    while len(cache) > MAX_NAMES_CACHED:
        del cache[next(iter(cache))]


USABLE_SHOWN = 5  # the usable targets that a target-not-listed message names; the rest are counted


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


def error(status: int, code: str, message: str, detail: object = None) -> web.Response:
    return web.json_response({"error": {"code": code, "message": message, "detail": detail}}, status=status)


def same_secret(given: str, token: str) -> bool:
    """Constant time. surrogatepass: whatever the client sent (a header that is not UTF-8, a lone surrogate in
    JSON) is compared, never an error."""
    return hmac.compare_digest(given.encode("utf-8", "surrogatepass"), token.encode("utf-8", "surrogatepass"))


def page_load(request: web.Request) -> bool:
    """Whether the browser loads a page (an address typed, a link, a reload), not a fetch or a script: only then
    is a route of the screen answered with its index.html. Sec-Fetch-Mode says it; a browser without it is asked
    by its Accept, which has text/html for a page and not for a fetch of JSON."""
    mode = request.headers.get("Sec-Fetch-Mode")
    if mode is not None:
        return mode == "navigate"
    return "text/html" in request.headers.get("Accept", "")


def count_or_none(value: object) -> int | None:
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else None


def _read_at_most(path: Path, size: int) -> bytes:
    with open(path, "rb") as f:
        return f.read(size)


def _unreadable(relative: str, e: OSError) -> Exception:
    """The refusal of a configuration file that the OS did not let the server read."""
    if isinstance(e, FileNotFoundError):
        return PathRejected("file-not-found", f"{relative!r} is not there any more: choose one that GET api/configs "
                                              "lists")
    return Refused(422, "config-unreadable", f"{relative} cannot be read ({e.strerror or e}): check that this user "
                                             "can read it")


def _utf8_text(relative: str, data: bytes) -> str:
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



@dataclass
class Options:
    root: Path
    launcher: Launcher
    port: int
    token: str
    initial_file: str | None = None
    dev_origin: str | None = None
    web_dir: Path | None = None
    stop_timeout_s: float | None = None
    participant_start_timeout_ms: int = 30000
    participant_stop_timeout_ms: int = 10000
    check_timeout_s: float = CHECK_TIMEOUT_S
    silent_after_s: float = SILENT_AFTER_S
    limits: Limits = field(default_factory=Limits)
    variables: bool = True  # start the launcher's control server and read the ECUs' variables from it
    variable_limits: VariableLimits = field(default_factory=VariableLimits)
    # (address, token) -> the source of a run's variables; None: the launcher's control server over gRPC
    # (control.GrpcSource). The tests put a stand-in here.
    source: Callable[[str, str], Source] | None = None
    save: bool = True  # POST api/config saves a text (not --no-save)
    # It also saves a text that changes the commands that run, and makes new files (--allow-command-changes)
    command_changes_allowed: bool = False
    runtime: LauncherRuntime | None = None


@dataclass(eq=False)
class Run:
    """One active or completed run and the session it feeds."""
    session: Session
    handle: RunHandle
    variables: Variables
    silence_task: asyncio.Task | None = None
    features: tuple[str, ...] = ()  # of the launcher that runs it (version --json when it started)
    credentials: Path | None = None  # the control server's credentials file, until the launcher has read it
    request: Path | None = None  # the request file (what the browser chose); its name is hidden from the output
    request_gone: bool = False  # the file is removed (when the launcher has ended)
    token: str | None = None  # of that file
    recording: Recording = field(default_factory=Recording)  # the applied writes made through this server
    timeline: Path | None = None  # the file the launcher writes the scenario's timeline to (--timeline)
    preflight_timing: dict = field(default_factory=dict)


class _PreStartGate:
    """Buffers runtime callbacks until the launcher's initial `check` settles on pass or fail."""

    def __init__(self, config_relative: str, request_path: Path | None):
        self.config_relative = config_relative
        self.request_path = request_path
        self.live = False
        self.discard = False
        self.decision = asyncio.Event()
        self.cleanup_done = asyncio.Event()
        self.passed_check: dict | None = None
        self.failed_check: dict | None = None
        self.buffer: list[tuple] = []
        self.cleanup_called = False

    def pass_check(self, check: dict | None = None) -> None:
        if not self.decision.is_set():
            self.passed_check = check
            self.decision.set()

    def fail_check(self, check: dict) -> None:
        if not self.decision.is_set():
            self.failed_check = check
            self.decision.set()

    def inspect_line(self, line: str | None) -> None:
        if self.decision.is_set() or line is None or not line.strip():
            return
        text = hide_request_in_line(line, self.request_path) if self.request_path is not None else line
        try:
            ev = json.loads(text)
        except (ValueError, RecursionError):
            return
        if not isinstance(ev, dict):
            return
        kind = ev.get("event")
        if kind == "check" and isinstance(ev.get("check"), dict):
            chk = ev["check"]
            if chk.get("result") == "passed":
                self.pass_check(chk)
            else:
                self.fail_check(chk)
        elif kind == "result" and isinstance(ev.get("result"), dict):
            res = ev["result"]
            chk = res.get("check")
            if isinstance(chk, dict) and chk.get("result") != "passed":
                self.fail_check(chk)
            elif res.get("result") in ("check_failed", "usage"):
                self.fail_check({
                    "result": res["result"],
                    "errors": res.get("errors") if isinstance(res.get("errors"), int) else 1,
                    "findings": res.get("findings") if isinstance(res.get("findings"), list) and res.get("findings") else [{
                        "severity": "error",
                        "code": res["result"],
                        "file": self.config_relative,
                        "line": None,
                        "participant": None,
                        "network": None,
                        "message": res.get("message") or "check failed",
                    }],
                })
            else:
                self.pass_check(chk if isinstance(chk, dict) else None)
        elif (kind == "phase" and ev.get("phase") not in ("checking", "ended")) or kind in (
            "registry", "control_server", "participant", "time"
        ):
            self.pass_check(None)


def _read_config_metadata(path: Path, variant: str | None) -> tuple[tuple[int, int], list[str] | None, list[str] | None]:
    """Reads file stamp plus participant and network names from the same open file descriptor."""
    with path.open("rb") as f:
        data = f.read(MAX_TEXT + 1)
        st = os.fstat(f.fileno())
    stamp = (st.st_mtime_ns, st.st_size)
    if len(data) > MAX_TEXT:
        return stamp, None, None
    try:
        doc = yaml.safe_load(data.decode("utf-8"))
    except (UnicodeDecodeError, yaml.YAMLError):
        return stamp, None, None
    if not isinstance(doc, dict):
        return stamp, None, None

    participants: list[str] | None = None
    raw_participants = doc.get("participants")
    if isinstance(raw_participants, list):
        names: list[str] = [
            entry["name"]
            for entry in raw_participants
            if isinstance(entry, dict) and isinstance(entry.get("name"), str)
        ]
        if variant is not None and isinstance(doc.get("variants"), dict):
            if variant not in doc["variants"]:
                names = []
                participants = None
            else:
                var_cfg = doc["variants"].get(variant)
                if isinstance(var_cfg, dict) and isinstance(var_cfg.get("participants"), dict):
                    for pname in var_cfg["participants"]:
                        if isinstance(pname, str) and pname not in names:
                            names.append(pname)
                participants = names
        else:
            participants = names

    networks: list[str] | None = None
    raw_networks = doc.get("networks")
    if isinstance(raw_networks, list):
        networks = [
            entry["name"]
            for entry in raw_networks
            if isinstance(entry, dict) and isinstance(entry.get("name"), str)
        ]
    return stamp, participants, networks


class _RunSink:
    """Routes runtime lifecycle callbacks for one run to GuiServer, Session, Variables, and Hub."""

    def __init__(self, server: GuiServer, run: Run, *, gate: _PreStartGate | None = None):
        self._server = server
        self._run = run
        self._gate = gate

    def on_stdout_line(self, line: str | None) -> None:
        gate = self._gate
        if gate is not None and not gate.live:
            if gate.discard:
                return
            gate.buffer.append(("stdout", line))
            gate.inspect_line(line)
            return
        run, session = self._run, self._run.session
        if line is None:
            messages = session.problem("line-too-long", f"the launcher wrote a line longer than "
                                       f"{MAX_LINE} bytes: it is skipped", None)
            if session.check is None and not session.check_timing and run.preflight_timing:
                # Check and run use the same intent, including its stop time.
                session.check_timing = run.preflight_timing
                messages.append(session.message())
            self._server.hub.broadcast(messages)
        elif line.strip():
            text = hide_request_in_line(line, run.request) if run.request is not None else line
            self._server.relay(session, text, run)

    def on_stderr_line(self, line: str) -> None:
        if not line:
            return
        gate = self._gate
        if gate is not None and not gate.live:
            if not gate.discard:
                gate.buffer.append(("stderr", line))
            return
        run = self._run
        text = hide_request(line, run.request) if run.request is not None else line
        self._server.hub.broadcast(run.session.stderr_line(text))

    def on_problem(self, code: str, message: str, detail: object = None) -> None:
        gate = self._gate
        if gate is not None and not gate.live:
            if not gate.discard:
                gate.buffer.append(("problem", code, message, detail))
            return
        self._server.hub.broadcast(self._run.session.problem(code, message, detail))

    def on_process_exited(self, returncode: int | None) -> None:
        gate = self._gate
        if gate is not None and not gate.live:
            if not gate.discard:
                gate.buffer.append(("exit", returncode))
            return
        run = self._run
        if run.silence_task is not None:
            run.silence_task.cancel()
        # Recorded now (api/session and a stop see it), sent with the end: a summary sent before the
        # launcher's last lines are read would be older than the events that follow it.
        run.session.launcher_exited(returncode)
        run.request_gone = control.remove(run.request)  # the launcher has ended: it needs the file no more

    async def on_run_finished(self, *, output_cut: bool = False, launcher_alive: bool = False) -> None:
        gate = self._gate
        if gate is not None and not gate.live:
            if gate.discard:
                return
            gate.pass_check(None)
            gate.buffer.append(("finish", output_cut, launcher_alive))
            return
        run = self._run
        if run.silence_task is not None:
            run.silence_task.cancel()
        if not run.request_gone:
            run.request_gone = control.remove(run.request)
        await self._server._close_variables(run)  # before the end: the summary sent with it has their last state
        await self._server._keep_timeline(run)  # before the end too: the summary says where the file is
        self._server.hub.broadcast(run.session.ended(output_cut=output_cut, launcher_alive=launcher_alive))

    async def on_run_cleanup(self) -> None:
        gate = self._gate
        if gate is not None and not gate.live:
            if not gate.discard:
                gate.cleanup_called = True
                gate.cleanup_done.set()
                return
            await self._cleanup_run()
            gate.cleanup_done.set()
            return
        await self._cleanup_run()

    async def _cleanup_run(self) -> None:
        run = self._run
        if run.silence_task is not None:
            run.silence_task.cancel()
            await asyncio.gather(run.silence_task, return_exceptions=True)
            run.silence_task = None
        self._server._remove_credentials(run, last=True)
        if not run.request_gone and not control.remove(run.request):
            log.warning("the request file %s of run %s could not be removed: remove it by hand", run.request,
                        run.session.id)
        if not run.variables.closed:  # cancelled on the way: the connection is still closed
            await asyncio.shield(self._server._close_variables(run))
        self._server._runs.discard(run)


@dataclass(eq=False)
class Connection:
    """An authenticated screen: its queue and its watches (by the id the screen gave them)."""
    client: Client
    watches: dict[int, Watch] = field(default_factory=dict)


class GuiServer:
    def __init__(self, options: Options):
        self.o = options
        self.root = ConfigRoot(options.root)
        self.launcher = options.launcher
        self.runtime: LauncherRuntime = options.runtime or ServeRuntime(
            self.launcher, cwd=self.root.root,
            participant_start_timeout_ms=options.participant_start_timeout_ms,
            participant_stop_timeout_ms=options.participant_stop_timeout_ms,
            max_line=MAX_LINE, escalate_wait_s=ESCALATE_WAIT_S,
            finish_timeout_s=process.OUTPUT_GRACE_S + 1 + VARIABLES_CLOSE_S,
            on_status=self._on_runtime_status,
        )
        self.runtime.sync_launcher()
        self.instance = secrets.token_hex(8)
        # The panel modules are served under panels/<key>/ (a module cannot be imported with a header): this key
        # is not the token, gives only the reading of panel modules, and is told only with GET api/dashboard.
        self.panel_key = secrets.token_urlsafe(18)
        self.static = StaticFiles(options.web_dir)
        self.timelines = Timelines()
        self.descriptions = Descriptions()
        self.timeline_page = find_page_dir()
        self.client_dir = client_files.find_dir()
        self._client_warned = False  # the files that vsil/ serves are missing: said once on stderr, at the first request
        self.current: Run | None = None  # the last run started (the one the screens see)
        self._runs: set[Run] = set()  # every launcher not yet followed to its end
        self._starting = False
        self._started = asyncio.Event()  # set when no start is in progress
        self._started.set()
        self._next_session_id = 1
        self.numbered = Numbered()  # the numbering of `allowed` that every place that carries it shares
        self._check_lock = asyncio.Lock()
        self._check_waiting = 0
        self._checks: set[asyncio.subprocess.Process] = set()  # the checks and the launcher's version probes
        self.launcher.track = self._checks
        self._names: dict[tuple[str, str | None], tuple[tuple, list[str]]] = {}
        # Of each (file, network), the targets that its last list (POST api/connections) gave as usable, with the
        # file's stamp then: the only targets that a check or a run passes on (see _check_targets).
        self._targets: dict[tuple[str, str], tuple[tuple, tuple[str, ...]]] = {}
        self._sockets: dict[web.WebSocketResponse, web.BaseRequest] = {}
        self._unauthenticated: dict[web.WebSocketResponse, None] = {}  # oldest first
        self._authenticated: set[web.WebSocketResponse] = set()
        self._evicting: set[asyncio.Task] = set()
        self._stop_on_start: bool | None = None  # api/stop while a run starts: its force, applied once started
        self._hurry = asyncio.Event()
        self._closing = False
        self._watching: dict[Watch, Client] = {}  # every watch of every connection, with its connection's queue
        self._ticker: asyncio.Task | None = None
        self._tick_now = asyncio.Event()
        self._library: control.Library | None = None
        self._no_library: str | None = None  # why the gRPC library cannot be used, once known
        self._library_lock = asyncio.Lock()
        self.hub = Hub(self.snapshot)
        port = options.port
        self.hosts = {f"127.0.0.1:{port}"}
        self.origins = {f"http://127.0.0.1:{port}"}
        if options.dev_origin:
            self.origins.add(options.dev_origin)
            self.hosts.add(options.dev_origin.split("://", 1)[1])

    @property
    def session(self) -> Session | None:
        return self.current.session if self.current else None

    def facts(self) -> Facts:
        """What `allowed` needs of the server itself (the launcher is as the last probe found it)."""
        usable = self.launcher.usable
        return Facts(closing=self._closing, starting=self._starting, launcher_usable=usable,
                     launcher_message="" if usable else "; ".join(p["message"] for p in self.launcher.problems),
                     save=self.o.save, command_changes_allowed=self.o.command_changes_allowed)

    def allowed(self) -> dict:
        """The `allowed` of the run the screens see; with no run, what the server accepts (start)."""
        return self.session.allowed() if self.session else self.numbered.stamp(
            allowed_of(self.facts(), False, False, "none"))

    def _on_runtime_status(self, status: dict) -> None:
        self.hub.broadcast([{"type": "runtime", "runtime": status}])

    def _push_allowed(self) -> None:
        """Tells the screens an `allowed` that changed with no `session` message to carry it (a start begins or
        fails, the server's end begins): a small message of its own. Nothing when it has not changed."""
        answer = self.allowed()
        if answer["rev"] > self.hub.told_allowed_rev:  # not yet sent to every screen, whoever made the number
            self.hub.broadcast([{"type": "allowed", "session": self.session.id if self.session else None,
                                 "allowed": answer}])

    # ------------------------------------------------------------ the application

    def app(self) -> web.Application:
        # MAX_BODY for every body (see _body), but those of api/check and POST api/config
        app = web.Application(client_max_size=MAX_TEXT_BODY, middlewares=[self._headers, self._guard])
        app.router.add_get("/api/app", self.get_app)
        app.router.add_get("/api/configs", self.get_configs)
        app.router.add_get("/api/config", self.get_config)
        app.router.add_post("/api/config", self.post_config)
        app.router.add_post("/api/check", self.post_check)
        app.router.add_post("/api/describe", self.post_describe)
        app.router.add_post("/api/connections", self.post_connections)
        app.router.add_post("/api/prepare", self.post_prepare)
        app.router.add_post("/api/run", self.post_run)
        app.router.add_post("/api/stop", self.post_stop)
        app.router.add_post("/api/stop-session", self.post_stop_session)
        app.router.add_post("/api/time", self.post_time)
        app.router.add_post("/api/write", self.post_write)
        app.router.add_get("/api/session", self.get_session)
        app.router.add_get("/api/dashboard", self.get_dashboard)
        app.router.add_get("/api/scenarios", self.get_scenarios)
        app.router.add_get("/api/recording", self.get_recording)
        app.router.add_get("/api/timeline/{session}", self.get_timeline)
        app.router.add_get("/ws", self.websocket)
        app.router.add_route("*", "/api/{rest:.*}", self.api_not_found)
        app.router.add_get("/panels/{key}/{path:.*}", self.get_panel)
        app.router.add_get("/timeline", self.get_timeline_page)
        app.router.add_get("/timeline/{name:.*}", self.get_timeline_page)
        app.router.add_get("/" + client_files.PATH.rstrip("/"), self.get_client_file)
        app.router.add_get(f"/{client_files.PATH}{{name:.*}}", self.get_client_file)
        app.router.add_get("/{path:.*}", self.get_static)
        return app

    @web.middleware
    async def _headers(self, request: web.Request, handler) -> web.StreamResponse:
        try:
            response = await handler(request)
        except web.HTTPException as e:  # the router's 404 and 405, aiohttp's 413
            code = {404: "not-found", 405: "method-not-allowed", 413: "too-large"}.get(e.status, "http-error")
            response = error(e.status, code, f"{request.method} {request.path}: {e.reason}")
        except Refused as e:
            response = error(e.status, e.code, str(e), e.detail)
        except Exception:
            log.exception("internal error on %s %s", request.method, request.path)
            response = error(500, "internal", "the GUI server failed on this request (a bug): its standard error "
                                              "has the details")
        if not isinstance(response, web.WebSocketResponse) or not response.prepared:
            for k, v in COMMON_HEADERS.items():
                response.headers.setdefault(k, v)
            if request.path.startswith("/api/") or request.path == "/ws":
                response.headers["Cache-Control"] = "no-store"
            else:
                response.headers.setdefault("Cache-Control", "no-cache")
        return response

    @web.middleware
    async def _guard(self, request: web.Request, handler) -> web.StreamResponse:
        """Host (against DNS rebinding), Origin (against other pages), the token (for the API)."""
        host = request.headers.get("Host", "").lower()
        if host not in self.hosts:
            return error(403, "forbidden-host", f"this server answers only to Host {' or '.join(sorted(self.hosts))}"
                                                ": open the URL the server printed")
        origin = request.headers.get("Origin")
        needs_origin = request.method not in ("GET", "HEAD") or request.path == "/ws"
        if (origin is not None or needs_origin) and origin not in self.origins:
            return error(403, "forbidden-origin",
                         f"Origin {origin or '(none)'} is not this server: requests that change something, and "
                         "WebSocket connections, are accepted only from the server's own page")
        if request.method == "OPTIONS":
            return error(403, "cors-not-allowed", "this server does not answer cross-origin requests")
        if request.path.startswith("/api/"):
            if not self._token_ok(request.headers.get("Authorization", "")):
                response = error(401, "unauthorized",
                                 "missing or wrong token: send Authorization: Bearer <token>, the token in the URL "
                                 "the server printed (#token=...)")
                response.headers["WWW-Authenticate"] = "Bearer"
                return response
            # A body must be JSON (a form or text/plain post is what another page could send without asking);
            # no body at all (a bare POST api/stop) needs no Content-Type.
            if request.method == "POST" and request.body_exists and request.content_type != "application/json":
                return error(415, "unsupported-media-type", "send the body as JSON (Content-Type: application/json)")
        return await handler(request)

    def _token_ok(self, value: str) -> bool:
        scheme, _, given = value.partition(" ")
        return scheme.lower() == "bearer" and same_secret(given.strip(), self.o.token)

    async def _body(self, request: web.Request, empty_ok: bool = False, max_size: int = MAX_BODY) -> dict:
        """The JSON object of the body; one larger than max_size bytes is refused (413 too-large)."""
        if empty_ok and not request.body_exists:
            return {}
        try:
            body = await request.clone(client_max_size=max_size).json()
        except (ValueError, UnicodeDecodeError, RecursionError):
            raise InvalidRequest("invalid-json", "the body is not JSON") from None
        if not isinstance(body, dict):
            raise InvalidRequest("invalid-json", "the body must be a JSON object")
        return body

    def _not_closing(self) -> None:
        if self._closing:
            raise Refused(503, "shutting-down", "the GUI server is stopping: start it again to go on")

    @staticmethod
    def _refusal(e: Exception) -> web.Response:
        if isinstance(e, PathRejected):
            return error(404 if e.code == "file-not-found" else 400, e.code, str(e))
        if isinstance(e, InvalidRequest):
            return error(400 if e.code != "launcher-unusable" else 503, e.code, str(e))
        assert isinstance(e, (Refused, Rejected))
        return error(e.status, e.code, str(e), e.detail)

    # ------------------------------------------------------------ API

    async def get_app(self, request: web.Request) -> web.Response:
        await self.launcher.probe()
        self.runtime.sync_launcher()
        self._push_allowed()  # the probe may have made the launcher usable or not
        why_not = await self._no_variables()
        v = self.o.variable_limits
        return web.json_response({
            "vehicle_sil_gui": PROTOCOL, "version": __version__, "instance": self.instance,
            "root": str(self.root.root), "initial_file": self.o.initial_file,
            "launcher": self.launcher.info(),
            "variables": {"available": why_not is None, "reason": why_not},
            "limits": {"log_lines_per_source": self.o.limits.log_lines_per_source,
                       "check_timeout_s": self.o.check_timeout_s, "speed_min": SPEED_MIN, "speed_max": SPEED_MAX,
                       "step_ms_max": STEP_MS_MAX,
                       "watches": v.watches, "watch_variables": v.watch_variables,
                       "watch_period_ms_min": v.period_ms_min, "watch_period_ms_max": v.period_ms_max,
                       "writes_per_request": v.writes_per_request, "write_at_ms_max": AT_MS_MAX,
                       "write_timeout_s": v.write_timeout_s},
        })

    async def _no_variables(self) -> str | None:
        """Why a run started now would have no variables (None: it would have them). The launcher is probed."""
        if not self.o.variables:
            return "the GUI server was started without variables"
        if control.FEATURE not in self.launcher.features:
            return "this launcher was built without a control server, so the variables of the ECUs cannot be read"
        if self.o.source is not None:
            return None
        async with self._library_lock:
            if self._library is None and self._no_library is None:
                try:
                    self._library = await asyncio.to_thread(control.load)  # imports gRPC: not on the loop
                except control.Unavailable as e:
                    self._no_library = str(e)
        return self._no_library

    async def get_configs(self, request: web.Request) -> web.Response:
        files, truncated = await asyncio.to_thread(self.root.list)
        return web.json_response({"root": str(self.root.root), "files": files, "truncated": truncated})

    async def get_config(self, request: web.Request) -> web.Response:
        """The text of a configuration file, with the SHA-256 of its bytes (stamp). The text is the bytes read as
        UTF-8, as they are (a BOM and the line ends too)."""
        try:
            config = await self._configuration(request.query.get("file"))
            try:
                data = await asyncio.to_thread(_read_at_most, config.absolute, MAX_TEXT + 1)
            except OSError as e:
                raise _unreadable(config.relative, e) from None
            if len(data) > MAX_TEXT:
                raise Refused(413, "config-too-large",
                              f"{config.relative} is larger than {MAX_TEXT} bytes (1 MiB), the most that the GUI "
                              "server reads: edit it in a text editor")
            text = _utf8_text(config.relative, data)
        except (PathRejected, Refused) as e:
            return self._refusal(e)
        return web.json_response({"file": config.relative, "text": text,
                                  "stamp": hashlib.sha256(data).hexdigest()})

    async def post_config(self, request: web.Request) -> web.Response:
        """Saves the text of a configuration file (config_save.py): an existing configuration file with the stamp the
        screen read, or a new file (stamp null) only with --allow-command-changes. The launcher's check of the text
        says whether it can be read, and whether it runs the same commands as the saved file (only then is it saved
        by default); this server compares no YAML."""
        try:
            self._not_closing()
            save = allowed_save(self.facts())
            if not save["ok"]:
                raise Refused(403, save["reason"], save["message"])
            body = await self._body(request, max_size=MAX_TEXT_BODY)
            relative, data, stamp = save_request(body, MAX_TEXT)
            name = "/".join(self.root.parts(relative))
            if stamp is None and not self.o.command_changes_allowed:
                raise Refused(403, "commands-changed",
                              f"{name} would be a new file (stamp is null), so there is no saved file to compare "
                              "its commands with; the GUI server makes new files only when it was started with "
                              "--allow-command-changes: create the file in a text editor, or start the GUI with "
                              "--allow-command-changes")
            config = await self._save_target(relative, stamp)  # refused here before it waits in the queue
            check_text(config.relative, data)
            await self._usable()
            await self._in_check_queue(lambda: self._save_locked(config, data, stamp))
        except (PathRejected, InvalidRequest, Refused) as e:
            return self._refusal(e)
        return web.json_response({"file": config.relative, "stamp": stamp_of(data)})

    async def _save_locked(self, config: ConfigFile, data: bytes, stamp: str | None) -> None:
        """In one turn of the queue of the checks: the file is as the screen read it; the launcher checks the text as
        that file's with the saved descriptions (no process starts) and compares its commands with the saved file;
        the file is looked at once more (it may have changed meanwhile: a guard against most races, not all), then
        replaced. The file is resolved once more here, in the queue: the launcher compares with that real file and the
        server writes that real file (a link that changed while the save waited leads to 409 config-changed). What the
        server kept of the old content is dropped."""
        again = await self._save_target(config.relative, stamp)
        if again.absolute != config.absolute:
            raise Refused(409, "config-changed", f"{config.relative} leads to another file than when the save was "
                                                 "sent (a symbolic link changed): read it again and save again")
        await self._unchanged(config, stamp)
        descriptions = await asyncio.to_thread(self.descriptions.path_of, (config.relative, None))
        request = self._make_request({})
        real = config.absolute.relative_to(self.root.root).as_posix()  # what --compare-commands reads: no link
        try:
            argv = save_check_command(self.launcher, config.relative, request, descriptions,
                                      real if stamp is not None else None)
            _, result = await self._launcher_check(argv, request, descriptions, data)
        finally:
            control.remove(request)
        decide(config.relative, result, self.o.command_changes_allowed)
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
        await self.runtime.release_config(config.relative)
        for kept in (self._names, self._targets):
            for key in [k for k in kept if k[0] == config.relative]:
                del kept[key]

    async def _save_target(self, relative: object, stamp: str | None) -> ConfigFile:
        """The file a save writes: a new file (stamp None) or an existing configuration file (config_save.py)."""
        try:
            return await asyncio.to_thread(new_file if stamp is None else existing_file, self.root, relative)
        except PathRejected:
            raise
        except OSError as e:
            raise _unreadable(str(relative), e) from None

    async def _unchanged(self, config: ConfigFile, stamp: str | None) -> None:
        try:
            await asyncio.to_thread(check_unchanged, config, stamp, MAX_TEXT)
        except OSError as e:
            raise _unreadable(config.relative, e) from None

    async def _configuration(self, relative: object) -> ConfigFile:
        """The configuration file whose content is read or checked for the screen (ConfigRoot.configuration)."""
        try:
            return await asyncio.to_thread(self.root.configuration, relative)
        except PathRejected:
            raise
        except OSError as e:
            raise _unreadable(str(relative), e) from None

    async def get_session(self, request: web.Request) -> web.Response:
        # The same encoding as the WebSocket: strict JSON (never a bare NaN or Infinity), or a 500.
        return web.Response(text=encode({"instance": self.instance, "session": self.session.summary() if self.session else None,
                                         "allowed": self.allowed(), "runtime": self.runtime.status()}),
                            content_type="application/json")

    async def get_dashboard(self, request: web.Request) -> web.Response:
        try:
            config = self.root.resolve(request.query.get("file"))
        except PathRejected as e:
            return self._refusal(e)
        result = await asyncio.to_thread(dashboard.read_dashboard, self.root, config, f"panels/{self.panel_key}/")
        return web.json_response(result.to_json(config.relative))

    async def get_scenarios(self, request: web.Request) -> web.Response:
        """The scenario files of a configuration (the .yaml and .yml files in `scenarios` next to it)."""
        try:
            config = self.root.resolve(request.query.get("file"))
        except PathRejected as e:
            return self._refusal(e)
        directory, found, files, truncated = await asyncio.to_thread(list_scenarios, self.root, config)
        return web.json_response({"file": config.relative, "directory": directory, "found": found,
                                  "scenarios": files, "truncated": truncated})

    async def get_recording(self, request: web.Request) -> web.Response:
        """The applied writes of the current run (running, or the last one) as a scenario's YAML. The server
        writes no file: the screen gives the text to the browser to save."""
        run = self.current
        if run is None:
            return web.json_response({"recording": None})
        enums = {(peer, name): entry["enum"] for peer, p in run.variables.peers.items()
                 for name, entry in p.index.items() if entry.get("enum")}
        sim_ms = (run.session.time or {}).get("sim_ms")  # how far the run has gone (its last time event)
        run_ms = sim_ms if isinstance(sim_ms, int) and not isinstance(sim_ms, bool) else None
        return web.json_response({"recording": run.recording.to_json(run.session.id, run.session.file,
                                                                     run.session.state, enums, run_ms)})

    async def get_timeline(self, request: web.Request) -> web.StreamResponse:
        """The timeline file of a scenario run (one of the last runs that wrote one), as the launcher wrote it."""
        text = request.match_info["session"]
        session_id = int(text) if text.isascii() and text.isdigit() and len(text) <= 18 else None
        path = await asyncio.to_thread(self.timelines.file_of, session_id) if session_id is not None else None
        if path is None:
            return error(404, "no-timeline",
                         f"run {text} has no timeline file on this server: a scenario run started from this GUI "
                         "server records one when the launcher has the feature 'timeline', and it can be read once "
                         f"the run has ended; the server keeps the files of the last {TIMELINES_KEPT} such runs "
                         "until it stops", {"session_id": session_id})
        path, name = path
        return web.FileResponse(path, headers={"Content-Type": "application/json",
                                               "Content-Disposition": f'inline; filename="{name}"'})

    async def get_timeline_page(self, request: web.Request) -> web.StreamResponse:
        """The page that draws a timeline file (gui/timeline), under timeline/; no token (as the screen's files):
        it reads the token as every screen of this server does, and sends it with its request for the file."""
        if "name" not in request.match_info:
            # Keeps ?src= and ?lang=: /timeline?src=... must open the same file as /timeline/?src=...
            query = request.query_string
            return web.Response(status=308, headers={"Location": "/timeline/" + (f"?{query}" if query else "")})
        found = PAGE_FILES.get(request.match_info["name"])
        if found is None:
            return error(404, "not-found", f"GET {request.path}: the timeline page has no such file")
        if self.timeline_page is None:
            return error(404, "not-found", "the timeline page (gui/timeline) is not in this installation of the GUI "
                                           "server: install the GUI from the tools archive, or run it from the "
                                           "repository")
        body = await asyncio.to_thread(read_page_file, self.timeline_page, found[0])
        if body is None:
            return error(404, "not-found", f"GET {request.path}: {found[0]} of the timeline page cannot be read")
        return web.Response(body=body, headers={"Content-Type": found[1]})

    async def get_client_file(self, request: web.Request) -> web.StreamResponse:
        """The client library and the parts (client_files.FILES), under vsil/, for a screen of your own; no token
        (as the timeline page). Only the names of the table are served: no path is built from the request."""
        found = client_files.FILES.get(request.match_info.get("name", ""))
        if found is None:
            return error(404, "not-found", f"GET {request.path}: the GUI server serves only "
                                           + " and ".join(f"/{client_files.PATH}{n}" for n in client_files.FILES))
        if self.client_dir is None:  # not built when the server started: looked for again (the build may be done now)
            self.client_dir = await asyncio.to_thread(client_files.find_dir)
        if self.client_dir is None:
            self._warn_client_files(client_files.missing_message(where=True))
            return error(404, "not-found", client_files.missing_message())
        body = await asyncio.to_thread(read_page_file, self.client_dir, request.match_info["name"])
        if body is None:
            message = (f"GET {request.path}: {request.match_info['name']} cannot be read: build it again with "
                       f"`{client_files.BUILD_COMMAND}` in gui/web (in the tools archive: unpack it again)")
            self._warn_client_files(message)
            return error(404, "not-found", message)
        return web.Response(body=body, headers={"Content-Type": found})

    def _warn_client_files(self, message: str) -> None:
        if not self._client_warned:  # once: the next requests get the 404 only
            self._client_warned = True
            log.warning("%s", message)

    def _scenario(self, config: ConfigFile, body: dict) -> str | None:
        """The scenario of a check or run request as the listing has it (root-relative; None: none)."""
        return resolve_scenario(self.root, config, body.get("scenario"))

    async def _intent(self, config: ConfigFile, body: dict,
                      scenario: str | None, *,
                      known_participants: list[str] | None = None,
                      fetch_names: bool = True) -> tuple[dict, list[str], list[str]]:
        """The request file's keys of a check or run request: (intent, disable, manual). The shapes are checked
        here; what the values mean for the file (is there such a variant, can these be used together) is the
        launcher's to say. For api/check, unknown disable/manual names are checked against the launcher's known
        participants; for api/run, YAML metadata validates them without spawning a pre-check subprocess."""
        variant = check_variant(body.get("variant"))
        params = check_params(body.get("params"))
        enable = check_enable(body.get("enable"))
        strict = check_strict(body.get("strict"))
        speed = check_speed(body.get("speed"))
        start_paused = body.get("start_paused", False)
        if not isinstance(start_paused, bool):
            raise InvalidRequest("invalid-field", "start_paused must be true or false")
        stop_at_ms = check_run_millis(body, "stop_at_ms", 1)
        late_tolerance_ms = check_run_millis(body, "late_tolerance_ms", 0)
        if (stop_at_ms is not None or late_tolerance_ms is not None) and FEATURE_RUN_STOP_AT not in self.launcher.features:
            raise Refused(409, "launcher-unsupported",
                          "the launcher does not support the requested run timing: install one with run-stop-at")
        connect = check_connect(body.get("connect"))
        time_policy = check_time_policy(body.get("time_policy"))
        wanted, wanted_manual = body.get("disable"), body.get("manual")
        if wanted or wanted_manual:
            if fetch_names:
                known = await self.participant_names(config.relative, variant)
                disable, manual = check_disable(wanted, known), check_manual(wanted_manual, known)
            elif known_participants is not None:
                disable = check_disable(wanted, known_participants)
                manual = check_manual(wanted_manual, known_participants)
            else:
                disable = check_name_list("disable", wanted)
                manual = check_name_list("manual", wanted_manual)
        else:
            disable, manual = [], []
        intent = {"variant": variant, "params": params or None,
                  "scenario": relative_to_config(config, scenario) if scenario is not None else None,
                  "disable": disable or None, "enable": enable or None, "manual": manual or None,
                  "strict": strict or None, "speed": speed, "start_paused": start_paused or None,
                  "connect": connect or None, "time_policy": time_policy, "stop_at_ms": stop_at_ms,
                  "late_tolerance_ms": late_tolerance_ms}
        return {k: v for k, v in intent.items() if v is not None}, disable, manual

    def _check_targets(self, relative: str, connect: dict[str, str], networks: list[str] | None,
                       checked_stamp: tuple | None = None) -> None:
        """The guard between the browser and the bridges' devices: a target other than "none" of a network of the
        configuration (or any target if networks could not be read) is passed on only when the last list of that
        network of that file (POST api/connections), made from the file as it is now, gave it as usable, exactly
        so written. Refused 400 target-not-listed otherwise. checked_stamp (a run): the file's stamp before its
        metadata read; a file changed since may have that network now, so every target other than "none" is refused."""
        for network, target in connect.items():
            if target == NO_CONNECTION:
                continue
            if checked_stamp is not None and checked_stamp != self._stamp(relative):
                raise Refused(400, "target-not-listed",
                              f"{relative} has changed since this run was checked, so the networks that it connects "
                              "may be others: start the run again", {"network": network, "target": target})
            if networks is not None and network not in networks:
                continue
            listed = self._targets.get((relative, network))
            if listed is None or listed[0] != self._stamp(relative):
                why = (f"there is no list of where {network!r} of {relative} can be connected" if listed is None else
                       f"{relative} has changed since {network!r} was listed")
            elif target not in listed[1]:
                why = (f"the last list of {network!r} of {relative} does not have {target!r} as usable "
                       f"({usable_text(listed[1])})")
            else:
                continue
            raise Refused(400, "target-not-listed",
                          f"{why}: ask POST api/connections {{\"file\": {json.dumps(relative)}, \"network\": "
                          f"{json.dumps(network)}}} and give a value that it lists with usable: true, exactly as "
                          "written there, or \"none\" to keep the network on SIL Kit only",
                          {"network": network, "target": target})

    async def api_not_found(self, request: web.Request) -> web.Response:
        return error(404, "not-found", f"{request.method} {request.path}: no such API")

    async def post_check(self, request: web.Request) -> web.Response:
        try:
            self._not_closing()
            body = await self._body(request, max_size=MAX_TEXT_BODY)
            if "text" in body:
                return await self._check_text(body)
            check_fields("api/check", body, (*REQUEST_FIELDS, "text"))
            config = self.root.resolve(body.get("file"))
            await self._usable()
            scenario = self._scenario(config, body)
            intent, disable, manual = await self._intent(config, body, scenario)
            code, result = await self._in_check_queue(lambda: self._check_and_guard(config.relative, intent))
        except (PathRejected, InvalidRequest, Refused) as e:
            return self._refusal(e)
        return web.json_response({"file": config.relative, "disable": disable, "manual": manual,
                                  "scenario": scenario, "exit_code": code, "check": result})

    async def _check_text(self, body: dict) -> web.Response:
        """api/check with text: the check of that text as the file's (its relative paths are from the file's
        directory), which starts no process: the launcher takes each executable's --describe output from the
        descriptions that the check of the saved file wrote. The names and the targets that the server keeps of the
        file (participant_names, _check_targets) are of the saved file: this check neither reads nor changes them."""
        others = sorted(k for k in body if k not in TEXT_FIELDS)
        if others:
            raise InvalidRequest("invalid-field", f"api/check takes no {', '.join(repr(k) for k in others)} together "
                                                  "with text: a check of unsaved text checks the configuration only; "
                                                  f"the fields it takes with text are {', '.join(TEXT_FIELDS)}")
        check_fields("api/check", body, TEXT_FIELDS)
        config = await self._configuration(body.get("file"))
        text = body["text"]
        if not isinstance(text, str):
            raise InvalidRequest("invalid-field", "text must be the text of the configuration file (a string), or "
                                                  "left out to check the saved file")
        try:
            data = text.encode("utf-8")
        except UnicodeEncodeError:
            raise InvalidRequest("invalid-field", "text is not valid Unicode (it has a lone surrogate such as "
                                                  "\\ud800): send the text as UTF-8") from None
        if len(data) > MAX_TEXT:
            raise Refused(413, "text-too-large",
                          f"text is {len(data)} bytes as UTF-8, more than the {MAX_TEXT} (1 MiB) that the GUI server "
                          "checks: save the file and check it without text")
        variant, strict = check_variant(body.get("variant")), check_strict(body.get("strict"))
        await self._usable()
        intent = {"variant": variant} if variant else {}
        if strict:
            intent["strict"] = True
        code, result = await self._in_check_queue(lambda: self._text_check_locked(config.relative, intent, data))
        return web.json_response({"file": config.relative, "disable": [], "manual": [], "scenario": None,
                                  "exit_code": code, "check": result})

    async def post_describe(self, request: web.Request) -> web.Response:
        """Describes the executables of the saved file again (a check of it that saves their --describe output), for
        the next checks of its text (api/check with text)."""
        try:
            self._not_closing()
            body = await self._body(request)
            check_fields("api/describe", body, DESCRIBE_FIELDS)
            config = await self._configuration(body.get("file"))
            variant = check_variant(body.get("variant"))
            await self._usable()
            code, result = await self._in_check_queue(lambda: self._describe_locked(config.relative, variant))
        except (PathRejected, InvalidRequest, Refused) as e:
            return self._refusal(e)
        return web.json_response({"file": config.relative, "variant": variant, "exit_code": code,
                                  "check": result})

    async def post_connections(self, request: web.Request) -> web.Response:
        """Where one network of the file can be connected on this computer (connections --json --network), in the
        queue of the checks. The targets that it lists as usable are the ones that a check or a run may name."""
        try:
            self._not_closing()
            body = await self._body(request)
            check_fields("api/connections", body, CONNECTIONS_FIELDS)
            network = check_network(body.get("network"))
            config = self.root.resolve(body.get("file"))
            await self._usable()
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
        except (PathRejected, InvalidRequest, Refused) as e:
            return self._refusal(e)
        return web.json_response({"file": config.relative, "network": network, "exit_code": code,
                                  "connections": result})

    async def post_prepare(self, request: web.Request) -> web.Response:
        """Prepares the persistent runtime for a configuration when eligible."""
        try:
            self._not_closing()
            if self._starting or (self.session is not None and self.session.state == "running"):
                raise Refused(409, "already-running", "a simulation is running: stop it before preparing another")
            body = await self._body(request)
            check_fields("api/prepare", body, ("file", "variant", "disable", "enable", "manual"))
            config = self.root.resolve(body.get("file"))
            variant = check_variant(body.get("variant"))
            disable = tuple(check_name_list("disable", body.get("disable")))
            enable = tuple(check_name_list("enable", body.get("enable")))
            manual = tuple(check_name_list("manual", body.get("manual")))
            await self._usable()
            why_not = await self._no_variables()
            status = await self.runtime.prepare(
                config.relative,
                variant=variant,
                disable=disable,
                enable=enable,
                manual=manual,
                want_control=why_not is None,
            )
        except (PathRejected, InvalidRequest, Refused) as e:
            return self._refusal(e)
        return web.json_response({"instance": self.instance, "runtime": status})

    async def post_run(self, request: web.Request) -> web.Response:
        try:
            self._not_closing()
            body = await self._body(request)
            check_fields("api/run", body, REQUEST_FIELDS)
            self._not_closing()
            # The rules of a start are `allowed_start`'s (the same as the `allowed.start` the screens see).
            running = self.session is not None and self.session.state == "running"
            start = allowed_start(self.facts(), running)
            if start["reason"] in ("already-running", "starting"):  # the others are answered below, in order
                raise Refused(409, start["reason"], start["message"])
        except (InvalidRequest, Refused) as e:
            return self._refusal(e)
        # From here until the launcher is registered (or the start fails), no other run may start.
        self._starting = True
        self._started.clear()
        self._stop_on_start = None
        self._push_allowed()
        try:
            return await self._start(body)
        except (PathRejected, InvalidRequest, Refused) as e:
            return self._refusal(e)
        finally:
            self._starting = False
            self._stop_on_start = None
            self._started.set()
            self._push_allowed()

    async def _start(self, body: dict) -> web.Response:
        config = self.root.resolve(body.get("file"))
        await self._usable()
        scenario = self._scenario(config, body)
        variant = check_variant(body.get("variant"))
        try:
            checked_stamp, yaml_participants, yaml_networks = await asyncio.to_thread(
                _read_config_metadata, config.absolute, variant,
            )
        except OSError as e:
            raise _unreadable(config.relative, e) from None
        intent, disable, manual = await self._intent(
            config, body, scenario, known_participants=yaml_participants, fetch_names=False,
        )
        speed, start_paused = intent.get("speed"), intent.get("start_paused", False)
        why_not = await self._no_variables()
        self._not_closing()
        credentials = token = None
        if why_not is None:
            making = asyncio.ensure_future(asyncio.to_thread(control.credentials_file))
            try:
                credentials, token = await asyncio.shield(making)
            except asyncio.CancelledError:  # the request was ended meanwhile: the file is still removed
                making.add_done_callback(lambda f: f.exception() is None and control.remove(f.result()[0]))
                raise
            except OSError as e:
                why_not = f"the file with the control server's credentials could not be made ({e})"
        # A scenario run records its timeline when the launcher can (the file is named here, the launcher makes it)
        timeline = await asyncio.to_thread(self.timelines.new_file) \
            if scenario is not None and FEATURE_TIMELINE in self.launcher.features else None
        try:
            # The last await is above: from here to the launcher's start no other request runs, so the targets are
            # compared with the lists and the file as they are when the launcher is started.
            self._check_targets(config.relative, intent.get("connect", {}), yaml_networks, checked_stamp)
            request = self._make_request(intent)
        except BaseException:
            control.remove(credentials)
            control.remove(timeline)
            raise
        try:
            handle = await self.runtime.start_run(RunSpec(
                config=config.relative,
                request=request,
                credentials=credentials,
                timeline=timeline,
                variant=intent.get("variant"),
                disable=tuple(disable),
                enable=tuple(intent.get("enable", ())),
                manual=tuple(manual),
            ))
        except BaseException:
            control.remove(credentials)
            control.remove(request)
            control.remove(timeline)
            raise
        run_credentials = credentials
        run_token = token
        if handle.control_token is not None:
            control.remove(run_credentials)
            run_credentials = None
            run_token = handle.control_token
        session = Session(self._next_session_id, config.relative, disable, speed, start_paused,
                          limits=self.o.limits, launcher_pid=handle.pid, launcher_mode=handle.mode,
                          scenario=scenario, manual=manual)
        session.facts = self.facts
        session.numbered = self.numbered
        preflight_timing = {k: intent[k] for k in ("stop_at_ms", "late_tolerance_ms") if k in intent}
        gate = _PreStartGate(config.relative, request) if getattr(handle, "waits_for_check", True) else None

        def emit_variables(messages: list[dict]) -> None:
            if gate is not None and gate.discard:
                return
            self._tell_variables(messages)

        variables = Variables(session.id, emit_variables, limits=self.o.variable_limits, secret=run_token,
                              ending=lambda: session.phase == "stopping" or session.result is not None
                              or not session.launcher_running)
        if why_not is None:
            variables.expect()
        else:
            variables.set_off(why_not)
        session.variables = variables.summary
        recording = Recording() if why_not is None and scenario is None else Recording(
            available=False, reason="this run plays a scenario: its writes are the scenario's own" if scenario
            is not None else f"this run has no variables to write: {why_not}")
        run = Run(session, handle, variables, features=tuple(self.launcher.features), credentials=run_credentials,
                  token=run_token, recording=recording, timeline=timeline, request=request,
                  preflight_timing=preflight_timing)
        self._runs.add(run)
        sink = _RunSink(self, run, gate=gate)
        handle.start(sink)
        if gate is not None:
            wait_cap = 5.0 if self.o.stop_timeout_s is None else self.o.stop_timeout_s
            try:
                async with asyncio.timeout(self.o.check_timeout_s):
                    await gate.decision.wait()
            except BaseException as exc:
                gate.discard = True
                try:
                    handle.send_stop(2)
                    if not gate.cleanup_done.is_set():
                        with contextlib.suppress(TimeoutError, Exception):
                            async with asyncio.timeout(min(1.0, wait_cap)):
                                await gate.cleanup_done.wait()
                    if not gate.cleanup_done.is_set():
                        abort = getattr(handle, "abort", None)
                        if abort is not None:
                            abort()
                        with contextlib.suppress(TimeoutError, Exception):
                            async with asyncio.timeout(process.KILL_WAIT_S + 1.0):
                                await gate.cleanup_done.wait()
                finally:
                    await asyncio.shield(self._discard_failed_start(run))
                if isinstance(exc, TimeoutError):
                    raise Refused(504, "check-timeout",
                                  f"the check did not end in {self.o.check_timeout_s:g} s (it was stopped): "
                                  "an ECU's --describe may hang") from None
                raise
            if gate.failed_check is not None:
                gate.discard = True
                try:
                    if not gate.cleanup_done.is_set():
                        with contextlib.suppress(TimeoutError, Exception):
                            async with asyncio.timeout(min(5.0, wait_cap)):
                                await gate.cleanup_done.wait()
                    if not gate.cleanup_done.is_set():
                        abort = getattr(handle, "abort", None)
                        if abort is not None:
                            abort()
                        with contextlib.suppress(TimeoutError, Exception):
                            async with asyncio.timeout(process.KILL_WAIT_S + 1.0):
                                await gate.cleanup_done.wait()
                finally:
                    await asyncio.shield(self._discard_failed_start(run))
                self._not_closing()
                checked = hide_request(gate.failed_check, request)
                raise Refused(422, "check-failed", self._check_failed_message(config.relative, checked),
                              {"result": checked.get("result"), "findings": checked.get("findings", [])})
            if self._closing:
                gate.discard = True
                await asyncio.shield(self._discard_failed_start(run))
                raise Refused(503, "shutting-down", "the GUI server is stopping: the run that was starting is stopped")
            if isinstance(gate.passed_check, dict):
                names = [p["name"] for p in gate.passed_check.get("participants", [])
                         if isinstance(p, dict) and isinstance(p.get("name"), str)]
                if names or gate.passed_check.get("result") not in ("usage",):
                    remember(self._names, (config.relative, intent.get("variant")), (checked_stamp, names))

        self._next_session_id += 1
        self.current = run
        run.silence_task = asyncio.create_task(self._watch_silence(session))
        if self._stop_on_start is not None:  # api/stop came while it was starting: it applies now
            self._write_stop(run, self._stop_on_start)
        # The session messages do not carry the variables: a new run's are told right after it.
        self.hub.broadcast([session.message()])
        self._tell_variables([variables.message()])
        initial_summary = session.summary()
        if gate is not None:
            gate.live = True
            while gate.buffer:
                item = gate.buffer.pop(0)
                kind = item[0]
                if kind == "stdout":
                    sink.on_stdout_line(item[1])
                elif kind == "stderr":
                    sink.on_stderr_line(item[1])
                elif kind == "problem":
                    sink.on_problem(item[1], item[2], item[3])
                elif kind == "exit":
                    sink.on_process_exited(item[1])
                elif kind == "finish":
                    await sink.on_run_finished(output_cut=item[1], launcher_alive=item[2])
            if gate.cleanup_called:
                await sink.on_run_cleanup()
        if self._closing:  # the end began while the launcher was starting: it is stopped with the others
            raise Refused(503, "shutting-down", "the GUI server is stopping: the run that was starting is stopped")
        return web.json_response({"instance": self.instance, "session": initial_summary})

    async def _discard_failed_start(self, run: Run) -> None:
        self._remove_credentials(run)
        if not run.request_gone:
            run.request_gone = control.remove(run.request)
        control.remove(run.timeline)
        run.timeline = None
        if not run.variables.closed:
            await self._close_variables(run)
        self._runs.discard(run)

    async def post_stop_session(self, request: web.Request) -> web.Response:
        """Stop one identified run, never a run that replaced it."""
        return await self.post_stop(request, targeted=True)

    async def post_stop(self, request: web.Request, *, targeted: bool = False) -> web.Response:
        try:
            body = await self._body(request, empty_ok=True)
            if targeted:
                check_fields("api/stop-session", body, ("force", "session", "instance"))
            else:  # a field this endpoint does not take must not be ignored while the run is stopped
                check_fields("api/stop", body, ("force",))
        except InvalidRequest as e:
            return self._refusal(e)
        force = body.get("force", False)
        if not isinstance(force, bool):
            return error(400, "invalid-field", "force must be true or false")
        target = body.get("session") if targeted else None
        if targeted and (isinstance(target, bool) or not isinstance(target, int) or target < 1):
            return error(400, "invalid-field", "session must be a positive integer")
        if targeted and (not isinstance(body.get("instance"), str) or not body["instance"]):
            return error(400, "invalid-field", "instance must be the server instance from the run or session answer")
        # Check before setting _stop_on_start or writing to a launcher: a delayed request must
        # never stop a different run, including one which is still starting.
        if target is not None and (body["instance"] != self.instance or self._starting or self.current is None
                                   or self.current.session.id != target):
            return error(409, "session-changed", "the run has changed: no stop was sent")
        if self._starting and not self._closing:  # applied as soon as the launcher has started (in _start)
            self._stop_on_start = force or bool(self._stop_on_start)
        run = self.current
        if run is None:
            return web.json_response({"session": None})
        session = run.session
        if self._closing or self._starting:  # the server's end is stopping it, or it is the run before
            return web.json_response({"session": session.summary()})
        if self._write_stop(run, force):  # decided, recorded and written before any await
            await run.handle.drain(STDIN_DRAIN_S)
        return web.json_response({"session": session.summary()})

    async def post_time(self, request: web.Request) -> web.Response:
        """A time command (pause, resume, step, run_until, speed) to the launcher of the run that is running. Written
        to its standard input and not waited for: whether the launcher took it comes as its command event (a launcher
        that stands still must not hold this request)."""
        try:
            self._not_closing()
            body = await self._body(request)
            targeted = "session" in body or "instance" in body
            session = body.pop("session", None)
            instance = body.pop("instance", None)
            if targeted:
                if type(session) is not int or session < 1:
                    raise InvalidRequest("invalid-field", "session must be a positive integer")
                if not isinstance(instance, str) or not instance:
                    raise InvalidRequest("invalid-field", "instance must be the server instance from the run or session answer")
            command = check_time_command(body)
        except (InvalidRequest, Refused) as e:
            return self._refusal(e)
        run = self.current
        # Compare the target after reading the request and before writing anything to the launcher.
        if targeted and (instance != self.instance or self._starting or run is None or session != run.session.id):
            return error(409, "session-changed", "the run has changed: no time command was sent")
        if run is None or self._starting or run.session.state != "running" or not run.session.launcher_running \
                or not run.handle.can_send():
            return error(409, "not-running", "no simulation is running: time commands go to the run that is "
                                             "running")
        said = self._launcher_refuses(run, command["command"])
        if said is not None and said[0] == "not-running":  # the run is over (a late command could change nothing)
            return error(409, "not-running", said[1])
        gone = error(409, "not-running", "the launcher's input is closed (it is ending): the command was not sent")
        if not run.handle.send_time(command):  # written before any await: in the order the requests came
            return gone
        if command["command"] in ("resume", "step"):
            run.variables.unsettle()  # it runs on: the values are not called paused until they are read again
        if not await run.handle.drain(STDIN_DRAIN_S):
            return gone
        # Not the summary: what the command does comes later, as the launcher's command and time events.
        return web.json_response({"session_id": run.session.id, "command": command})

    @staticmethod
    def _launcher_refuses(run: Run, name: str) -> tuple[str, str] | None:
        """(reason, message) when the launcher's last `state` says it does not accept `name` (an entry of its
        `allowed`); None when it accepts it, or has not said."""
        state = run.session.launcher_state
        item = state["allowed"].get(name) if state else None
        if not isinstance(item, dict) or item.get("ok") is not False or not isinstance(item.get("reason"), str):
            return None
        message = item.get("message")
        return item["reason"], message if isinstance(message, str) else f"the launcher does not accept {name} now"

    def _tell_variables(self, messages: list[dict]) -> None:
        """What a run's variables send to the screens. When their state or schema is told (a new run, ready, the
        end), every watch is due at once, whatever its period: the first and the last values do not wait for it."""
        self.hub.broadcast(messages)
        for m in messages:  # the record of the applied writes looks at the operations as the screens do
            if m.get("type") == "operation":
                for run in self._runs:
                    if run.session.id == m.get("session"):
                        run.recording.on_operation(m)
        if any(m.get("type") == "variables" for m in messages) and self._watching:
            now = time.monotonic()
            for watch in self._watching:
                watch.due = now
            self._tick_now.set()

    async def post_write(self, request: web.Request) -> web.Response:
        """Writes variables of the run that is running: {"session": <the run's id>, "writes": {"<peer>":
        {"<variable>": <value>}}, "at_ms", "late"}. The session names the run the write is meant for: a write for
        another run is refused (session-changed), so that a write sent while one run ends and the next begins is
        not made in the next. Checked against the schema here, then given to the launcher's control server; the
        answer is the operation's number, and what became of it comes to every screen as `operation` messages."""
        try:
            self._not_closing()
            body = await self._body(request)
            meant = body.get("session") if isinstance(body, dict) else None
            if isinstance(meant, bool) or not isinstance(meant, int):
                raise Refused(400, "invalid-field", "a write needs session: the id of the run it is meant for (the "
                                                    "id of the summary of GET api/session, or of hello)")
            run = self.current
            if run is None or self._starting or run.session.state != "running" or not run.session.launcher_running:
                raise Refused(409, "not-running", "no simulation is running: variables are written to the run that "
                                                  "is running")
            if meant != run.session.id:
                raise Refused(409, "session-changed", f"the write is meant for the run {meant}, but the run that is "
                                                      f"running is {run.session.id}: nothing was written; look at "
                                                      "the variables of this run before writing to it")
            # Whether the run accepts a write is the launcher's rule (its `state`: `allowed.write`, for example
            # scenario-running); this server does not repeat it. `no-control-server` is answered below (unsupported).
            said = self._launcher_refuses(run, "write")
            if said is None and run.session.launcher_state is None and run.session.scenario is not None:
                # The launcher's first `state` has not come yet: the control server would refuse the write, but
                # with another answer than this one, so the scenario's rule is applied until the state comes.
                said = ("scenario-running", f"the run {run.session.id} plays the scenario {run.session.scenario}: "
                                            "variables cannot be written while it runs (the scenario must give the "
                                            "same result every time); read them, or start a run without a scenario "
                                            "to write")
            if said is not None and said[0] != "no-control-server":
                raise Refused(409, said[0], said[1])
            if control.FEATURE not in run.features:
                raise Refused(400, "unsupported", "the launcher of this run was built without a control server: "
                                                  "its ECUs' variables cannot be written")
            writes, at_ns, late = run.variables.check_write(body)
            seq = await run.variables.write(writes, at_ns, late)
            run.recording.wrote(seq, writes, run.variables.operations.get(seq))
        except (InvalidRequest, Refused, Rejected) as e:
            return self._refusal(e)
        return web.json_response({"session_id": run.session.id, "operation": seq})

    def _write_stop(self, run: Run, force: bool) -> bool:
        """Records a stop request and writes its commands to the launcher's input (no await); whether something
        was written that may need draining."""
        session = run.session
        commands = session.request_stop(force)
        if not commands:
            return False
        delivered = run.handle.send_stop(commands)
        messages = []
        if not delivered:
            messages = session.problem("stop-not-delivered", "the stop could not be written to the launcher's "
                                                             "input (it is closed): the launcher may be ending; "
                                                             "if it is not, stop the GUI server", None)
        messages.append(session.message())
        self.hub.broadcast(messages)
        return delivered

    async def _usable(self) -> None:
        await self.launcher.probe()
        self.runtime.sync_launcher()
        if not self.launcher.usable:
            raise InvalidRequest("launcher-unusable",
                                 "; ".join(p["message"] for p in self.launcher.problems) or "the launcher is not usable")

    @staticmethod
    def _check_failed_message(relative: str, result: dict) -> str:
        errors = result.get("errors")
        what = f"{errors} error(s)" if isinstance(errors, int) and errors else f"the result {result.get('result')!r}"
        return (f"the check of {relative} found {what}, so no run was started: fix what detail.findings lists "
                "(the same findings as POST api/check), then start again")

    async def participant_names(self, relative: str, variant: str | None = None) -> list[str]:
        """The participant names of the file (of its variant), from the last check of it, or from a new check
        (no --disable) when the file has changed since."""
        cached = self._names.get((relative, variant))
        if cached and cached[0] == self._stamp(relative):
            return cached[1]
        await self._check(relative, {"variant": variant} if variant else {})
        return self._names.get((relative, variant), ((), []))[1]

    def _stamp(self, relative: str) -> tuple:
        try:
            st = self.root.resolve(relative).absolute.stat()
        except OSError:
            raise PathRejected("file-not-found", f"{relative!r} is not there any more: choose one that GET "
                                                 "api/configs lists") from None
        return (st.st_mtime_ns, st.st_size)

    async def _check(self, relative: str, intent: dict) -> tuple[int, dict]:
        """check --json of the file with the intent (the keys of a request file), in the queue of the checks."""
        return await self._in_check_queue(lambda: self._check_locked(relative, intent))

    async def _check_and_guard(self, relative: str, intent: dict) -> tuple[int, dict]:
        """api/check in the queue: the check, then the guard of its connect against that check's networks."""
        code, result = await self._check_locked(relative, intent)
        self._check_targets(relative, intent.get("connect", {}), network_names(result))
        return code, result

    async def _in_check_queue(self, work: Callable[[], Awaitable[T]]) -> T:
        """One check (or list, or save) at a time: the others wait (MAX_CHECK_WAITING of them, check_timeout_s each)."""
        self._not_closing()
        if self._check_waiting >= MAX_CHECK_WAITING:
            raise Refused(503, "check-busy", f"{MAX_CHECK_WAITING} checks are already waiting for the one that "
                                             "runs: wait for their answers, then check again")
        self._check_waiting += 1
        try:
            async with asyncio.timeout(self.o.check_timeout_s):
                await self._check_lock.acquire()
        except TimeoutError:
            raise Refused(503, "check-busy", f"another check has run for more than {self.o.check_timeout_s:g} s: "
                                             "try again when it has ended") from None
        finally:
            self._check_waiting -= 1
        try:
            self._not_closing()
            return await work()
        finally:
            self._check_lock.release()

    async def _check_locked(self, relative: str, intent: dict) -> tuple[int, dict]:
        stamp = self._stamp(relative)
        request = self._make_request(intent)  # a few lines, and removed here whatever comes of the check
        try:
            return await self._run_check(relative, intent, request, stamp)
        finally:
            control.remove(request)

    async def _describe_locked(self, relative: str, variant: str | None) -> tuple[int, dict]:
        """The check of the saved file (with its variant only) that saves each executable's --describe output in the
        descriptions of (file, variant), for the checks of its text. A check that could not read the file (or its
        variant) leaves no descriptions: the text is then checked with none. So does a check that ended in an error
        (check-timeout, launcher-output, launcher-start), which is raised: it is not run again for the text before
        the file changes or api/describe asks for it."""
        key = (relative, variant)
        stamp = self._stamp(relative)
        path = await asyncio.to_thread(self.descriptions.path_of, key)
        intent = {"variant": variant} if variant else {}
        request = self._make_request(intent)
        try:
            code, result = await self._run_check(relative, intent, request, stamp, descriptions=path)
        except Refused as e:
            if e.code != "shutting-down":
                self.descriptions.made(key, stamp, False)
            raise
        finally:
            control.remove(request)
        self.descriptions.made(key, stamp, result.get("result") not in ("usage", "unreadable"))
        return code, result

    async def _text_check_locked(self, relative: str, intent: dict, text: bytes) -> tuple[int, dict]:
        """The check of a text as the file's, with the descriptions of (file, variant): made first by a check of the
        saved file when there are none, or the file has changed since they were made. In the one turn of the queue
        of the checks."""
        key = (relative, intent.get("variant"))
        if self.descriptions.made_from(key) != self._stamp(relative):
            try:
                await self._describe_locked(*key)
            except Refused as e:  # the text is checked with no descriptions; api/describe says what went wrong
                if e.code == "shutting-down":
                    raise
        path = await asyncio.to_thread(self.descriptions.path_of, key)
        request = self._make_request(intent)
        try:
            return await self._launcher_check(text_check_command(self.launcher, relative, request, path), request,
                                              path, text)
        finally:
            control.remove(request)

    @staticmethod
    def _make_request(intent: dict) -> Path:
        """The request file of a check or a run; Refused (500 request-file) when no file can be made."""
        try:
            return write_request_file(intent)
        except OSError as e:
            raise Refused(500, "request-file", f"the file that carries the request to the launcher could not be "
                                               f"made in the temporary directory ({e}): free space there, or set "
                                               "TMPDIR to a directory that this user can write") from None

    async def _run_check(self, relative: str, intent: dict, request: Path, stamp: tuple,
                         descriptions: Path | None = None) -> tuple[int, dict]:
        """A check of the saved file, whose participant names are kept (participant_names). descriptions: the check
        also saves the --describe output there (describe_command)."""
        argv = (check_command(self.launcher, relative, request) if descriptions is None else
                describe_command(self.launcher, relative, request, descriptions))
        code, result = await self._launcher_check(argv, request, descriptions)
        names = [p["name"] for p in result["participants"] if isinstance(p, dict) and isinstance(p.get("name"), str)]
        if names or result.get("result") not in ("usage",):
            remember(self._names, (relative, intent.get("variant")), (stamp, names))
        return code, result

    def _participant_timeouts(self, argv: list[str]) -> list[str]:
        return with_participant_timeouts(argv, self.o.participant_start_timeout_ms,
                                         self.o.participant_stop_timeout_ms)

    async def _launcher_check(self, argv: list[str], request: Path, descriptions: Path | None = None,
                              text: bytes | None = None) -> tuple[int, dict]:
        """check --json with this command line (text: on its standard input); the check object, with the names of
        this server's own files (the request, the saved descriptions) hidden."""
        try:
            code, out, err = await process.run_to_end(self._participant_timeouts(argv), cwd=self.root.root, timeout_s=self.o.check_timeout_s,
                                                      track=self._checks, stdin=text)
        except TimeoutError:
            raise Refused(504, "check-timeout", f"the check did not end in {self.o.check_timeout_s:g} s (it was "
                                                "stopped): an ECU's --describe may hang") from None
        except OSError as e:
            raise Refused(502, "launcher-start", f"cannot start {self.launcher.path}: {e}") from None
        self._not_closing()  # stopped by the server's end: not a launcher that misbehaved

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
        """connections --json of one network (in the queue of the checks). Its usable targets replace those of the
        last list of (file, network); a list that fails leaves none."""
        stamp = self._stamp(relative)
        self._targets.pop((relative, network), None)
        try:
            code, out, err = await process.run_to_end(connections_command(self.launcher, relative, network),
                                                      cwd=self.root.root, timeout_s=self.o.check_timeout_s,
                                                      track=self._checks)
        except TimeoutError:
            raise Refused(504, "check-timeout", f"the list of where {network!r} can be connected did not end in "
                                                f"{self.o.check_timeout_s:g} s (it was stopped): a bridge's list may "
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
            remember(self._targets, (relative, network), (stamp, usable))
        return code, result

    # ------------------------------------------------------------ the launcher's run

    async def _keep_timeline(self, run: Run) -> None:
        """Keeps the run's timeline file if the launcher wrote a whole one: a result that does not report
        timeline-write (a file written only in part), and a file that is not empty."""
        if run.timeline is None:
            return
        whole = written_whole(run.session.result)
        if await asyncio.to_thread(self.timelines.finish, run.session.id, run.timeline, whole,
                                   run.session.scenario or ""):
            run.session.timeline_url = api_path(run.session.id)

    def _remove_credentials(self, run: Run, last: bool = False) -> None:
        """Removes the run's credentials file. One that cannot be removed now (another program holds it open) is
        tried again at the run's end, and then said."""
        if control.remove(run.credentials):
            run.credentials = None
        elif last:
            log.warning("the credentials file %s of run %s could not be removed: remove it by hand (its token is "
                        "of no use after the run)", run.credentials, run.session.id)

    async def _close_variables(self, run: Run) -> None:
        self._remove_credentials(run)
        try:
            async with asyncio.timeout(VARIABLES_CLOSE_S):
                await run.variables.close()
        except TimeoutError:
            log.warning("the connection to the control server of run %s did not close in time", run.session.id)

    def _control_started(self, run: Run, event: dict) -> None:
        """The launcher's control server listens (its control_server event): connects to it. It has read the
        credentials by now, so their file is removed."""
        self._remove_credentials(run)
        variables = run.variables
        if variables.state != "connecting" or variables.source is not None or variables.closed:
            return  # a run without variables, or a second event
        try:
            address = control.check_address(event.get("address"))
            if event.get("tls") is True:
                raise ValueError("the launcher's control server asks for TLS, which this GUI server did not ask for")
            if self.o.source is not None:
                source = self.o.source(address, run.token or "")
            else:
                source = control.GrpcSource(self._library, address, run.token, run_id=run.handle.run_id)
        except Exception as e:
            variables.fail(f"{e}: the variables of this run are not read")
            return
        variables.connect(source)

    async def _watch_silence(self, session: Session) -> None:
        """Tells the screens when the launcher has written nothing for silent_after_s (it may hang)."""
        while True:
            await asyncio.sleep(min(1.0, self.o.silent_after_s / 4))
            self.hub.broadcast(session.check_silence(time.monotonic(), self.o.silent_after_s))

    def relay(self, session: Session, line: str, run: Run | None = None) -> None:
        """One line of the launcher's events: into the summary and to the screens. Nothing that goes wrong with
        one line stops the reading: the line becomes a bad-event problem, and the next line is read."""
        try:
            messages = session.feed_line(line)
            sent = self.hub.broadcast(messages, session.log_key)
            if run is not None:
                for m in messages:
                    if m["type"] != "event":
                        continue
                    if m["event"]["event"] == control.EVENT:
                        self._control_started(run, m["event"])
                    elif m["event"]["event"] == "phase" and m["event"].get("phase") == "stopping":
                        run.variables.read_last()  # the simulation is over: its control server closes soon
            if sent:
                return
            error: str = "a message for the screens could not be made of it"
        except Exception as e:  # a shape the checks did not foresee
            log.exception("reading an event")
            error = type(e).__name__
        self.hub.broadcast(session.problem("bad-event", f"the server could not pass on an event ({error}): it is "
                                                        "ignored", session._cut(line)))

    # ------------------------------------------------------------ the server's end

    async def shutdown(self) -> None:
        """Refuses new runs and checks, stops every launcher (end of input, then --stop-timeout, then SIGTERM /
        Ctrl+Break, then a kill of the tree), kills the checks, and closes every WebSocket. When a stop
        timeout is set, the wait is bounded (END_BUDGET_S after --stop-timeout). A second call
        (a second Ctrl+C) skips the waiting."""
        if self._closing:
            self._hurry.set()
            return
        self._closing = True
        self._push_allowed()
        for proc in list(self._checks):
            process.kill_tree(proc)
        try:
            async with asyncio.timeout(START_WAIT_S):
                await self._started.wait()
        except TimeoutError:
            log.warning("a run was still starting when the server stopped")
        await self.runtime.shutdown(self._hurry, self.o.stop_timeout_s)
        for proc in list(self._checks):  # one that was starting when the end began
            process.kill_tree(proc)
        for pid in self.runtime.active_pids():
            log.warning("the launcher (pid %s) could not be stopped: stop it by hand", pid)
        self.hub.close_all()
        if self._ticker is not None:
            self._ticker.cancel()
        await self._close_sockets()
        await asyncio.to_thread(self.timelines.remove_all)
        await asyncio.to_thread(self.descriptions.remove_all)

    def kill_all(self) -> None:
        """The last resort when the end takes longer than it may: every launcher's and check's tree."""
        self.runtime.kill_all()
        for proc in list(self._checks):
            process.kill_tree(proc)

    async def _close_sockets(self) -> None:
        sockets = dict(self._sockets)
        if not sockets:
            return
        closing = [asyncio.create_task(ws.close(code=WSCloseCode.GOING_AWAY, message=b"server stopping"))
                   for ws in sockets]
        await asyncio.wait(closing, timeout=SOCKETS_CLOSE_S)
        for task in closing:
            task.cancel()
        await asyncio.gather(*closing, return_exceptions=True)
        for ws, request in sockets.items():
            if not ws.closed and request.transport is not None:
                request.transport.abort()

    # ------------------------------------------------------------ WebSocket

    def snapshot(self, session_id: int | None, from_seq: int | None) -> list[dict]:
        s = self.session
        hello = {"type": "hello", "vehicle_sil_gui": PROTOCOL, "version": __version__, "instance": self.instance,
                 "session": s.summary() if s else None, "allowed": self.allowed(), "replay": None,
                 "runtime": self.runtime.status()}
        if s is None:
            return [hello, {"type": "synced", "session": None, "last_seq": None}]
        start = from_seq if session_id == s.id else None
        hello["replay"] = {"from_seq": start}
        dropped, events = s.replay(start)
        return [hello, *([dropped] if dropped else []), *events,
                {"type": "synced", "session": s.id, "last_seq": s.last_seq}]

    async def websocket(self, request: web.Request) -> web.StreamResponse:
        if self._closing:
            return error(503, "shutting-down", "the GUI server is stopping")
        if len(self._authenticated) >= MAX_SOCKETS:
            return error(503, "too-many-connections", "too many screens are connected: close some tabs")
        if len(self._unauthenticated) >= MAX_UNAUTHENTICATED:
            # The oldest connection that has not sent its auth makes room: a process without the token that
            # opens connections cannot keep the screen out, only make it try again.
            oldest = next(iter(self._unauthenticated))
            del self._unauthenticated[oldest]
            task = asyncio.create_task(self._make_room(oldest))
            self._evicting.add(task)
            task.add_done_callback(self._evicting.discard)
        ws = web.WebSocketResponse(max_msg_size=MAX_WS_MESSAGE, heartbeat=30, timeout=WS_CLOSE_TIMEOUT_S)
        self._sockets[ws] = request  # counted from here, before the handshake's first await
        self._unauthenticated[ws] = None
        try:
            await ws.prepare(request)
            await self._talk(ws)
        finally:
            self._sockets.pop(ws, None)
            self._unauthenticated.pop(ws, None)
            self._authenticated.discard(ws)
        return ws

    @staticmethod
    async def _make_room(ws: web.WebSocketResponse) -> None:
        try:
            await ws.close(code=WSCloseCode.TRY_AGAIN_LATER, message=b"too-many-unauthenticated")
        except Exception:  # not prepared yet: _talk closes it when it is
            pass

    async def _talk(self, ws: web.WebSocketResponse) -> None:
        if ws not in self._unauthenticated:  # it made room for a newer one during its handshake
            await ws.close(code=WSCloseCode.TRY_AGAIN_LATER, message=b"too-many-unauthenticated")
            return
        try:
            async with asyncio.timeout(AUTH_TIMEOUT_S):
                msg = await ws.receive()
        except TimeoutError:
            await ws.close(code=4401, message=b"auth-timeout")
            return
        if self._closing or ws.closed or ws not in self._unauthenticated or \
                msg.type in (WSMsgType.CLOSE, WSMsgType.CLOSING, WSMsgType.CLOSED, WSMsgType.ERROR):
            return
        auth = None
        if msg.type == WSMsgType.TEXT:
            try:
                auth = json.loads(msg.data)
            except (ValueError, RecursionError):
                pass
        if not (isinstance(auth, dict) and auth.get("type") == "auth" and isinstance(auth.get("token"), str)
                and same_secret(auth["token"], self.o.token)):
            await ws.send_json({"type": "error", "session": None, "code": "unauthorized",
                                "message": "the first message must be {\"type\":\"auth\",\"token\":...} with the "
                                           "token of the URL the server printed", "detail": None})
            await ws.close(code=4401, message=b"unauthorized")
            return
        del self._unauthenticated[ws]
        if len(self._authenticated) >= MAX_SOCKETS:  # others authenticated since this one's handshake
            await ws.close(code=WSCloseCode.TRY_AGAIN_LATER, message=b"too-many-connections")
            return
        self._authenticated.add(ws)
        client = self.hub.client(ws.send_str)
        # A screen that keeps the check from hello and the check event can ask for summaries without it.
        client.without_check = auth.get("session_without_check") is True
        connection = Connection(client)
        sender = asyncio.create_task(self.hub.serve(client, count_or_none(auth.get("session")),
                                                    count_or_none(auth.get("from_seq"))))
        reader = asyncio.create_task(self._listen(ws, connection))
        try:
            await asyncio.wait({sender, reader}, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for watch in connection.watches.values():
                self._unwatch(watch)
            connection.watches.clear()
            for task in (sender, reader):
                task.cancel()
            await asyncio.gather(sender, reader, return_exceptions=True)
        await ws.close(code=WSCloseCode.GOING_AWAY if self._closing else WSCloseCode.OK)

    async def _listen(self, ws: web.WebSocketResponse, connection: Connection) -> None:
        """Reads what the screen sends after auth (watch, unwatch), until it closes. A message that is wrong gets
        an `error` for this screen only, and the connection goes on; a type not known is ignored."""
        async for msg in ws:
            if msg.type in (WSMsgType.ERROR, WSMsgType.CLOSE):
                return
            if msg.type != WSMsgType.TEXT:
                continue
            try:
                self._from_screen(connection, msg.data)
            except Rejected as e:
                connection.client.put(Outgoing({"type": "error", "session": None, "code": e.code,
                                                "message": str(e), "detail": e.detail}))

    def _from_screen(self, connection: Connection, text: str) -> None:
        try:
            message = json.loads(text)
        except (ValueError, RecursionError):
            message = None
        if not isinstance(message, dict) or not isinstance(message.get("type"), str):
            raise Rejected(400, "invalid-message", 'a message must be a JSON object with "type" (watch, unwatch): '
                                                   "it is ignored")
        limits = self.o.variable_limits
        if message["type"] == "watch":
            id, wanted, period_s = check_watch(message, limits)
            old = connection.watches.pop(id, None)  # the same id again: the watch is replaced
            if old is not None:
                self._unwatch(old)
            elif len(connection.watches) >= limits.watches:
                raise Rejected(400, "too-many-watches", f"a connection has at most {limits.watches} watches: end "
                                                        'one ({"type":"unwatch","id":...}) or put the variables '
                                                        "into one watch", {"id": id})
            client = connection.client
            watch = Watch(id, wanted, period_s, time.monotonic(),  # due at once: the last values
                          lambda w: self._values_text(w, client))
            connection.watches[id] = watch
            self._watching[watch] = connection.client
            if self._ticker is None or self._ticker.done():
                self._ticker = asyncio.create_task(self._tick())
            self._tick_now.set()
        elif message["type"] == "unwatch":
            watch = connection.watches.pop(message.get("id"), None) if count_or_none(message.get("id")) is not None \
                else None
            if watch is not None:
                self._unwatch(watch)

    def _unwatch(self, watch: Watch) -> None:
        watch.cancelled = True
        client = self._watching.pop(watch, None)
        if client is not None:
            client.drop_deferred(watch)  # it does not wait in the queue of a screen that does not read

    def _values_text(self, watch: Watch, client: Client) -> str | None:
        """The `values` message of a watch, made when its connection sends it. None while the screen has not
        been sent the run's `variables` yet (they are behind this in its queue): the values of a run come after
        the run is told; the watch is put into the queue again at its next period."""
        run = self.current
        if run is None or not run.variables.has_values:
            return None
        if client.variables_of != run.session.id:
            watch.due = time.monotonic()  # again, now behind the run's `variables` in the queue
            self._tick_now.set()
            return None
        return encode(run.variables.values_message(watch))

    async def _tick(self) -> None:
        """For the watches whose period has passed: reads their peers' values from the run's control server (one
        call for all of them, one read at a time), then puts each watch whose values or time have changed since
        it was last sent into its connection's queue.
        Reading takes at most half of the time (the next read waits as long as the last one took), however many
        peers are watched however often. Ends when there is no watch."""
        read_again = 0.0
        while self._watching and not self._closing:
            self._tick_now.clear()
            now = time.monotonic()
            try:
                due = [(watch, client) for watch, client in self._watching.items() if watch.due <= now]
                for watch, _ in due:
                    watch.due = max(watch.due + watch.period_s, now + watch.period_s / 2)
                run = self.current
                if due and run is not None and run.variables.state == "ready" and now >= read_again:
                    names = {name for watch, _ in due for name in run.variables.peer_names(watch)}
                    if names:
                        await run.variables.refresh(names)  # ends within the read's timeout; never raises
                        read_again = time.monotonic() + (time.monotonic() - now)
                run = self.current
                variables = run.variables if run is not None and run.variables.has_values else None
                for watch, client in due:
                    if variables is not None and not watch.cancelled and variables.stamp(watch) != watch.sent:
                        client.put_deferred(watch)
            except Exception:  # a bug must not end the values of every screen
                log.exception("sending the values of the watches")
            wake = min([now + 1.0, *(watch.due for watch in self._watching)])
            try:
                async with asyncio.timeout(max(wake - time.monotonic(), 0.001)):
                    await self._tick_now.wait()
            except TimeoutError:
                pass

    # ------------------------------------------------------------ the screen

    async def get_panel(self, request: web.Request) -> web.StreamResponse:
        """A panel module (dashboard.module_path): only with the panel key, as JavaScript, never sniffed."""
        not_found = web.Response(status=404, text="not found\n", content_type="text/plain")
        if not same_secret(request.match_info["key"], self.panel_key):
            return not_found
        try:
            path = await asyncio.to_thread(dashboard.module_path, self.root, request.match_info["path"])
            body = await asyncio.to_thread(_read_at_most, path, dashboard.MAX_MODULE_BYTES + 1)
        except (PathRejected, OSError):
            return not_found
        if len(body) > dashboard.MAX_MODULE_BYTES:
            return not_found
        return web.Response(body=body, headers={"Content-Type": "text/javascript; charset=utf-8",
                                                "Cache-Control": "no-store"})

    async def get_static(self, request: web.Request) -> web.StreamResponse:
        not_found = web.Response(status=404, text="not found\n", content_type="text/plain")
        if request.headers.get("Service-Worker") == "script":
            # A service worker would outlive this server: one started later on the same port would show the old
            # screen, and that screen would be given the new token.
            return web.Response(status=404, text="the GUI server does not serve service workers\n",
                                content_type="text/plain")
        path, media, status = self.static.lookup(request.path, page_load(request))
        if status != 200:
            return not_found
        if path is None:
            return web.Response(text=NOT_BUILT, content_type="text/html", charset="utf-8")
        # The file itself, read here: not FileResponse, which would also serve a neighbouring .gz or .br.
        try:
            body = await asyncio.to_thread(self.static.read, path)
        except TooLarge:
            message = (f"{request.path}: larger than {MAX_FILE_BYTES // (1024 * 1024)} MiB, which the GUI server "
                       "does not serve: make the build split it (or leave its source maps out)")
            log.warning("%s", message)
            return web.Response(status=404, text=message + "\n", content_type="text/plain")
        if body is None:
            return not_found
        return web.Response(body=body, headers={"Content-Type": media})
