"""The HTTP and WebSocket server: the API, the checks of every request, the launcher as a child process.

Concurrency: everything runs on one event loop, so state changes only between awaits. Each handler decides
and records what it does (a run starting, a stop requested) before its first await, and checks again after
each await whether the server has started to end. Every launcher started is kept in `_runs` until it has
exited and its output is read, so the end of the server stops all of them, whatever raced.
"""

from __future__ import annotations

import asyncio
import hmac
import itertools
import json
import logging
import secrets
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from aiohttp import WSCloseCode, WSMsgType, web

from . import __version__, control, process
from .hub import Client, Hub, Outgoing, encode
from .launcher import (FEATURE_SPEED, FEATURE_START_PAUSED, FEATURE_TIME_COMMANDS, SPEED_MAX, SPEED_MIN, STEP_MS_MAX,
                       InvalidRequest, Launcher, check_command, check_disable, check_speed, check_time_command,
                       command_line, run_command)
from .paths import ConfigRoot, PathRejected
from .session import SILENT_AFTER_S, Limits, Session
from .source import Source
from .static import NOT_BUILT, StaticFiles
from .variables import AT_MS_MAX, Limits as VariableLimits, Rejected, Variables, Watch, check_watch

PROTOCOL = 1  # vehicle_sil_gui: the version of the shapes in gui/protocol
MAX_BODY = 64 * 1024
MAX_WS_MESSAGE = 256 * 1024  # a watch that names many variables
MAX_SOCKETS = 32  # authenticated WebSocket connections
MAX_UNAUTHENTICATED = 16  # connections waiting for their auth; when full, the oldest is closed for a new one
AUTH_TIMEOUT_S = 5
WS_CLOSE_TIMEOUT_S = 2  # waiting for the screen's answer to a close
CHECK_TIMEOUT_S = 120
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


def error(status: int, code: str, message: str, detail: object = None) -> web.Response:
    return web.json_response({"error": {"code": code, "message": message, "detail": detail}}, status=status)


def same_secret(given: str, token: str) -> bool:
    """Constant time. surrogatepass: whatever the client sent (a header that is not UTF-8, a lone surrogate in
    JSON) is compared, never an error."""
    return hmac.compare_digest(given.encode("utf-8", "surrogatepass"), token.encode("utf-8", "surrogatepass"))


def count_or_none(value: object) -> int | None:
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else None


class Refused(Exception):
    """A request that is refused with an HTTP error (status, code, message, detail)."""

    def __init__(self, status: int, code: str, message: str, detail: object = None):
        super().__init__(message)
        self.status, self.code, self.detail = status, code, detail



@dataclass
class Options:
    root: Path
    launcher: Launcher
    port: int
    token: str
    initial_file: str | None = None
    dev_origin: str | None = None
    web_dir: Path | None = None
    stop_timeout_s: float = 10.0
    check_timeout_s: float = CHECK_TIMEOUT_S
    silent_after_s: float = SILENT_AFTER_S
    limits: Limits = field(default_factory=Limits)
    variables: bool = True  # start the launcher's control server and read the ECUs' variables from it
    variable_limits: VariableLimits = field(default_factory=VariableLimits)
    # (address, token) -> the source of a run's variables; None: the launcher's control server over gRPC
    # (control.GrpcSource). The tests put a stand-in here.
    source: Callable[[str, str], Source] | None = None


@dataclass(eq=False)
class Run:
    """One launcher process and the session it feeds, from its start until its output is read."""
    session: Session
    proc: asyncio.subprocess.Process
    variables: Variables
    task: asyncio.Task | None = None
    features: tuple[str, ...] = ()  # of the launcher that runs it (version --json when it started)
    credentials: Path | None = None  # the control server's credentials file, until the launcher has read it
    token: str | None = None  # of that file


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
        self.instance = secrets.token_hex(8)
        self.static = StaticFiles(options.web_dir)
        self.current: Run | None = None  # the last run started (the one the screens see)
        self._runs: set[Run] = set()  # every launcher not yet followed to its end
        self._starting = False
        self._started = asyncio.Event()  # set when no start is in progress
        self._started.set()
        self._ids = itertools.count(1)
        self._check_lock = asyncio.Lock()
        self._check_waiting = 0
        self._checks: set[asyncio.subprocess.Process] = set()  # the checks and the launcher's version probes
        self.launcher.track = self._checks
        self._names: dict[str, tuple[tuple, list[str]]] = {}
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

    # ------------------------------------------------------------ the application

    def app(self) -> web.Application:
        app = web.Application(client_max_size=MAX_BODY, middlewares=[self._headers, self._guard])
        app.router.add_get("/api/app", self.get_app)
        app.router.add_get("/api/configs", self.get_configs)
        app.router.add_post("/api/check", self.post_check)
        app.router.add_post("/api/run", self.post_run)
        app.router.add_post("/api/stop", self.post_stop)
        app.router.add_post("/api/time", self.post_time)
        app.router.add_post("/api/write", self.post_write)
        app.router.add_get("/api/session", self.get_session)
        app.router.add_get("/ws", self.websocket)
        app.router.add_route("*", "/api/{rest:.*}", self.api_not_found)
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

    async def _body(self, request: web.Request, empty_ok: bool = False) -> dict:
        if empty_ok and not request.body_exists:
            return {}
        try:
            body = await request.json()
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
            return "the GUI server was started with --no-variables"
        if control.FEATURE not in self.launcher.features:
            return (f"this vehicle_sil_launch has no '{control.FEATURE}' (its features: "
                    f"{', '.join(self.launcher.features) or 'none'}): build it with "
                    "-DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON (docs/control-api.md)")
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

    async def get_session(self, request: web.Request) -> web.Response:
        # The same encoding as the WebSocket: strict JSON (never a bare NaN or Infinity), or a 500.
        return web.Response(text=encode({"session": self.session.summary() if self.session else None}),
                            content_type="application/json")

    async def api_not_found(self, request: web.Request) -> web.Response:
        return error(404, "not-found", f"{request.method} {request.path}: no such API")

    async def post_check(self, request: web.Request) -> web.Response:
        try:
            self._not_closing()
            body = await self._body(request)
            config = self.root.resolve(body.get("file"))
            await self._usable()
            wanted = body.get("disable")
            known = await self.participant_names(config.relative) if wanted else []
            disable = check_disable(wanted, known)
            code, result = await self._check(config.relative, disable)
        except (PathRejected, InvalidRequest, Refused) as e:
            return self._refusal(e)
        return web.json_response({"file": config.relative, "disable": disable, "exit_code": code, "check": result})

    async def post_run(self, request: web.Request) -> web.Response:
        try:
            self._not_closing()
            body = await self._body(request)
            self._not_closing()
            if self._starting or (self.session is not None and self.session.state == "running"):
                raise Refused(409, "already-running", "a simulation is starting or running: stop it (POST api/stop) "
                                                      "and wait for it to end before starting another")
        except (InvalidRequest, Refused) as e:
            return self._refusal(e)
        # From here until the launcher is registered (or the start fails), no other run may start.
        self._starting = True
        self._started.clear()
        self._stop_on_start = None
        try:
            return await self._start(body)
        except (PathRejected, InvalidRequest, Refused) as e:
            return self._refusal(e)
        finally:
            self._starting = False
            self._stop_on_start = None
            self._started.set()

    async def _start(self, body: dict) -> web.Response:
        config = self.root.resolve(body.get("file"))
        await self._usable()
        speed = check_speed(body.get("speed"))
        start_paused = body.get("start_paused", False)
        if not isinstance(start_paused, bool):
            raise InvalidRequest("invalid-field", "start_paused must be true or false")
        for wanted, feature, default in ((speed is not None, FEATURE_SPEED, "speed null"),
                                         (start_paused, FEATURE_START_PAUSED, "start_paused false")):
            if wanted and feature not in self.launcher.features:
                raise InvalidRequest("unsupported", f"this vehicle_sil_launch has no '{feature}': leave {default}, "
                                                    "or build a newer launcher")
        known = await self.participant_names(config.relative)
        disable = check_disable(body.get("disable"), known)
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
        argv = run_command(self.launcher, config.relative, disable, speed, start_paused, credentials)
        try:
            proc = await process.start(argv, cwd=self.root.root)
        except BaseException as e:
            control.remove(credentials)
            if not isinstance(e, OSError):
                raise
            raise Refused(502, "launcher-start", f"cannot start {self.launcher.path}: {e}") from None
        # Registered in the same step as the start returns: the server's end sees every launcher.
        session = Session(next(self._ids), config.relative, disable, speed, start_paused, limits=self.o.limits,
                          launcher_pid=proc.pid)
        variables = Variables(session.id, self._tell_variables, limits=self.o.variable_limits, secret=token,
                              ending=lambda: session.phase == "stopping" or session.result is not None
                              or not session.launcher_running)
        if why_not is None:
            variables.expect()
        else:
            variables.set_off(why_not)
        session.variables = variables.summary
        run = Run(session, proc, variables, features=tuple(self.launcher.features), credentials=credentials,
                  token=token)
        self._runs.add(run)
        self.current = run
        run.task = asyncio.create_task(self._follow(run))
        if self._stop_on_start is not None:  # api/stop came while it was starting: it applies now
            self._write_stop(run, self._stop_on_start)
        # The session messages do not carry the variables: a new run's are told right after it.
        self.hub.broadcast([session.message()])
        self._tell_variables([variables.message()])
        if self._closing:  # the end began while the launcher was starting: it is stopped with the others
            raise Refused(503, "shutting-down", "the GUI server is stopping: the run that was starting is stopped")
        return web.json_response({"session": session.summary()})

    async def post_stop(self, request: web.Request) -> web.Response:
        try:
            body = await self._body(request, empty_ok=True)
        except InvalidRequest as e:
            return self._refusal(e)
        force = body.get("force", False)
        if not isinstance(force, bool):
            return error(400, "invalid-field", "force must be true or false")
        if self._starting and not self._closing:  # applied as soon as the launcher has started (in _start)
            self._stop_on_start = force or bool(self._stop_on_start)
        run = self.current
        if run is None:
            return web.json_response({"session": None})
        session, proc = run.session, run.proc
        if self._closing or self._starting:  # the server's end is stopping it, or it is the run before
            return web.json_response({"session": session.summary()})
        if self._write_stop(run, force):  # decided, recorded and written before any await
            try:
                async with asyncio.timeout(STDIN_DRAIN_S):
                    await proc.stdin.drain()
            except (TimeoutError, BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
                pass
        return web.json_response({"session": session.summary()})

    async def post_time(self, request: web.Request) -> web.Response:
        """A time command (pause, resume, step, speed) to the launcher of the run that is running. Written to its
        standard input and not waited for: whether the launcher took it comes as its command event (a launcher
        that stands still must not hold this request)."""
        try:
            self._not_closing()
            command = check_time_command(await self._body(request))
        except (InvalidRequest, Refused) as e:
            return self._refusal(e)
        run = self.current
        proc = run.proc if run else None
        if run is None or self._starting or run.session.state != "running" or not run.session.launcher_running \
                or proc.stdin is None or proc.stdin.is_closing():
            return error(409, "not-running", "no simulation is running: time commands go to the run that is "
                                             "running (start one with POST api/run, and wait for its start)")
        # What the launcher that runs can do, not the one on disk now (it may have been built again since).
        if FEATURE_TIME_COMMANDS not in run.features:
            return error(400, "unsupported", f"the vehicle_sil_launch of this run has no '{FEATURE_TIME_COMMANDS}': "
                                             "it cannot pause, resume, step or change the speed; build a newer "
                                             "launcher")
        gone = error(409, "not-running", "the launcher's input is closed (it is ending): the command was not sent")
        try:
            proc.stdin.write(command_line(command))  # written before any await: in the order the requests came
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            return gone
        if command["command"] in ("resume", "step"):
            run.variables.unsettle()  # it runs on: the values are not called paused until that is read again
        try:
            async with asyncio.timeout(STDIN_DRAIN_S):
                await proc.stdin.drain()
        except TimeoutError:
            pass  # the launcher does not read its input now: the command waits there, the request does not
        except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
            return gone
        # Not the summary: what the command does comes later, as the launcher's command and time events.
        return web.json_response({"session_id": run.session.id, "command": command})

    def _tell_variables(self, messages: list[dict]) -> None:
        """What a run's variables send to the screens. When their state or schema is told (a new run, ready, the
        end), every watch is due at once, whatever its period: the first and the last values do not wait for it."""
        self.hub.broadcast(messages)
        if any(m.get("type") == "variables" for m in messages) and self._watching:
            now = time.monotonic()
            for watch in self._watching:
                watch.due = now
            self._tick_now.set()

    async def post_write(self, request: web.Request) -> web.Response:
        """Writes variables of the run that is running: {"writes": {"<peer>": {"<variable>": <value>}}, "at_ms",
        "late"}. Checked against the schema here, then given to the launcher's control server; the answer is the
        operation's number, and what became of it comes to every screen as `operation` messages."""
        try:
            self._not_closing()
            body = await self._body(request)
            run = self.current
            if run is None or self._starting or run.session.state != "running" or not run.session.launcher_running:
                raise Refused(409, "not-running", "no simulation is running: variables are written to the run that "
                                                  "is running (start one with POST api/run)")
            if control.FEATURE not in run.features:
                raise Refused(400, "unsupported", f"the vehicle_sil_launch of this run has no '{control.FEATURE}': "
                                                  "its ECUs' variables cannot be written; build it with "
                                                  "-DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON")
            writes, at_ns, late = run.variables.check_write(body)
            seq = await run.variables.write(writes, at_ns, late)
        except (InvalidRequest, Refused, Rejected) as e:
            return self._refusal(e)
        return web.json_response({"session_id": run.session.id, "operation": seq})

    def _write_stop(self, run: Run, force: bool) -> bool:
        """Records a stop request and writes its commands to the launcher's input (no await); whether something
        was written that may need draining."""
        session, proc = run.session, run.proc
        commands = session.request_stop(force)
        if not commands:
            return False
        delivered = proc.stdin is not None and not proc.stdin.is_closing()
        if delivered:
            try:
                proc.stdin.write(b'{"command":"stop"}\n' * commands)
            except (BrokenPipeError, ConnectionResetError, RuntimeError, OSError):
                delivered = False
        messages = [session.message()]
        if not delivered:
            messages = session.problem("stop-not-delivered", "the stop could not be written to the launcher's "
                                                             "input (it is closed): the launcher may be ending; "
                                                             "if it is not, stop the GUI server", None) + messages
        self.hub.broadcast(messages)
        return delivered

    async def _usable(self) -> None:
        await self.launcher.probe()
        if not self.launcher.usable:
            raise InvalidRequest("launcher-unusable",
                                 "; ".join(p["message"] for p in self.launcher.problems) or "the launcher is not usable")

    async def participant_names(self, relative: str) -> list[str]:
        """The participant names of the file, from the last check of it, or from a new check (no --disable)
        when the file has changed since."""
        cached = self._names.get(relative)
        if cached and cached[0] == self._stamp(relative):
            return cached[1]
        await self._check(relative, [])
        return self._names.get(relative, ((), []))[1]

    def _stamp(self, relative: str) -> tuple:
        try:
            st = self.root.resolve(relative).absolute.stat()
        except OSError:
            raise PathRejected("file-not-found", f"{relative!r} is not there any more: choose one that GET "
                                                 "api/configs lists") from None
        return (st.st_mtime_ns, st.st_size)

    async def _check(self, relative: str, disable: list[str]) -> tuple[int, dict]:
        """check --json, one at a time: the others wait (MAX_CHECK_WAITING of them, check_timeout_s each)."""
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
            return await self._check_locked(relative, disable)
        finally:
            self._check_lock.release()

    async def _check_locked(self, relative: str, disable: list[str]) -> tuple[int, dict]:
        stamp = self._stamp(relative)
        try:
            code, out, err = await process.run_to_end(check_command(self.launcher, relative, disable),
                                                      cwd=self.root.root, timeout_s=self.o.check_timeout_s,
                                                      track=self._checks)
        except TimeoutError:
            raise Refused(504, "check-timeout", f"the check did not end in {self.o.check_timeout_s:g} s (it was "
                                                "stopped): an ECU's --describe may hang") from None
        except OSError as e:
            raise Refused(502, "launcher-start", f"cannot start {self.launcher.path}: {e}") from None
        self._not_closing()  # stopped by the server's end: not a launcher that misbehaved
        try:
            result = json.loads(out)
        except (ValueError, RecursionError):
            result = None
        if not isinstance(result, dict) or not isinstance(result.get("participants"), list):
            tail = err.decode("utf-8", "replace").strip().splitlines()[-10:]
            raise Refused(502, "launcher-output", f"check --json exited with code {code} and did not write the "
                                                  "check object", tail)
        names = [p["name"] for p in result["participants"] if isinstance(p, dict) and isinstance(p.get("name"), str)]
        if names or result.get("result") not in ("usage",):
            self._names.pop(relative, None)
            self._names[relative] = (stamp, names)
            while len(self._names) > MAX_NAMES_CACHED:
                del self._names[next(iter(self._names))]
        return code, result

    # ------------------------------------------------------------ the launcher's run

    async def _follow(self, run: Run) -> None:
        """Reads the launcher's output until it has exited (not until its output ends: what it started may keep
        the output open), then the rest for a short time."""
        session, proc = run.session, run.proc
        reader = asyncio.create_task(self._read_output(run))
        watch = asyncio.create_task(self._watch_silence(session))
        try:
            code = await proc.wait()
            watch.cancel()
            process.kill_tree(proc)  # what the launcher left in its group (POSIX)
            # Recorded now (api/session and a stop see it), sent with the end: a summary sent before the
            # launcher's last lines are read would be older than the events that follow it.
            session.launcher_exited(code)
            done, _ = await asyncio.wait({reader}, timeout=process.OUTPUT_GRACE_S)
            await self._close_variables(run)  # before the end: the summary sent with it has their last state
            self.hub.broadcast(session.ended(output_cut=not done))
        finally:
            watch.cancel()
            reader.cancel()
            await asyncio.gather(reader, watch, return_exceptions=True)
            process.close_pipes(proc)
            self._remove_credentials(run, last=True)
            if not run.variables.closed:  # cancelled on the way: the connection is still closed
                await asyncio.shield(self._close_variables(run))
            self._runs.discard(run)

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
                source = control.GrpcSource(self._library, address, run.token)
        except Exception as e:
            variables.fail(f"{e}: the variables of this run are not read")
            return
        variables.connect(source)

    async def _watch_silence(self, session: Session) -> None:
        """Tells the screens when the launcher has written nothing for silent_after_s (it may hang)."""
        while True:
            await asyncio.sleep(min(1.0, self.o.silent_after_s / 4))
            self.hub.broadcast(session.check_silence(time.monotonic(), self.o.silent_after_s))

    async def _read_output(self, run: Run) -> None:
        session, proc = run.session, run.proc

        async def stdout() -> None:
            async for line in process.lines(proc.stdout, MAX_LINE):
                if line is None:
                    self.hub.broadcast(session.problem("line-too-long", f"the launcher wrote a line longer than "
                                                       f"{MAX_LINE} bytes: it is skipped", None))
                elif line.strip():
                    self.relay(session, line, run)

        async def stderr() -> None:
            async for line in process.lines(proc.stderr, 64 * 1024):
                if line:
                    self.hub.broadcast(session.stderr_line(line))

        await asyncio.gather(stdout(), stderr(), return_exceptions=True)

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
        Ctrl+Break, then a kill of the tree), kills the checks, and closes every WebSocket. Each wait is
        bounded (END_BUDGET_S after --stop-timeout). A second call (a second Ctrl+C) skips the waiting."""
        if self._closing:
            self._hurry.set()
            return
        self._closing = True
        for proc in list(self._checks):
            process.kill_tree(proc)
        try:
            async with asyncio.timeout(START_WAIT_S):
                await self._started.wait()
        except TimeoutError:
            log.warning("a run was still starting when the server stopped")
        await asyncio.gather(*(self._stop_run(run) for run in list(self._runs)))
        for proc in list(self._checks):  # one that was starting when the end began
            process.kill_tree(proc)
        for run in list(self._runs):
            log.warning("the launcher (pid %s) could not be stopped: stop it by hand", run.proc.pid)
        self.hub.close_all()
        if self._ticker is not None:
            self._ticker.cancel()
        await self._close_sockets()

    def kill_all(self) -> None:
        """The last resort when the end takes longer than it may: every launcher's and check's tree."""
        for proc in [run.proc for run in self._runs] + list(self._checks):
            process.kill_tree(proc)

    async def _stop_run(self, run: Run) -> None:
        proc, session = run.proc, run.session
        if proc.returncode is None:
            if proc.stdin is not None:
                proc.stdin.close()
            if not await self._wait(proc, self.o.stop_timeout_s):
                self.hub.broadcast(session.problem("stop-escalated", "the launcher did not stop in "
                                                   f"{self.o.stop_timeout_s:g} s after its input closed: it was "
                                                   "asked to stop (SIGTERM / Ctrl+Break)", None))
                process.ask_to_stop(proc)
                if not await self._wait(proc, ESCALATE_WAIT_S):
                    process.kill_tree(proc)
                    await process.wait_bounded(proc, process.KILL_WAIT_S)
        if run.task is not None:
            await asyncio.wait({run.task}, timeout=process.OUTPUT_GRACE_S + 1 + VARIABLES_CLOSE_S)

    async def _wait(self, proc: asyncio.subprocess.Process, seconds: float) -> bool:
        hurry = asyncio.create_task(self._hurry.wait())
        done = asyncio.create_task(proc.wait())
        await asyncio.wait({hurry, done}, timeout=seconds, return_when=asyncio.FIRST_COMPLETED)
        hurry.cancel()
        if not done.done():
            done.cancel()
            return False
        return True

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
                 "session": s.summary() if s else None, "replay": None}
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
        """For the watches whose period has passed: reads their peers' values from the run's control server, then
        puts each watch whose values or time have changed since it was last sent into its connection's queue.
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

    async def get_static(self, request: web.Request) -> web.StreamResponse:
        path, media, status = self.static.lookup(request.path)
        if status != 200:
            return web.Response(status=404, text="not found\n", content_type="text/plain")
        if path is None:
            return web.Response(text=NOT_BUILT, content_type="text/html", charset="utf-8")
        # The file itself, read here: not FileResponse, which would also serve a neighbouring .gz or .br.
        try:
            body = await asyncio.to_thread(path.read_bytes)
        except OSError:
            return web.Response(status=404, text="not found\n", content_type="text/plain")
        return web.Response(body=body, headers={"Content-Type": media})
