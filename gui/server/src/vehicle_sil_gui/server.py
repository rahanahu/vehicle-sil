"""Thin HTTP transport, security middleware, declarative RouteSpec table, and GuiServer composition root.

Concurrency: everything runs on one event loop, so state changes only between awaits. Domain state, locks,
and background tasks live in `ConfigService`, `RunManager`, and `WsGateway` with exclusive ownership; this
module owns only HTTP security middleware, body framing, static asset serving, and route wiring.
"""

from __future__ import annotations

import asyncio
import logging
import secrets
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any, Literal, TypeVar

from aiohttp import web
from pydantic import ValidationError

from . import client_files
from .config_service import CHECK_TIMEOUT_S, MAX_BODY, MAX_TEXT_BODY, CheckRunner, ConfigService
from .contracts import (AppResponse, CheckRequest, CheckResponse, ConfigGetResponse, ConfigSaveRequest,
                        ConfigSaveResponse, ConfigsResponse, ConnectionsRequest, ConnectionsResponse,
                        DashboardResponse, DescribeRequest, DescribeResponse, PrepareRequest,
                        PrepareResponse, REQUEST_FIELDS, RecordingResponse, RunRequest, RunResponse,
                        ScenariosResponse, SessionResponse, StopSessionRequest, StopSessionResponse,
                        TimeCommandAck, TimeCommandRequest, WriteRequest, WriteResponse,
                        error_payload, format_validation_error, model_allowed_fields, parse_json_object,
                        validate_model_defer_file)
from .refusal import Refused
from .run_manager import Limits, RunManager, SILENT_AFTER_S, VariableLimits
from .static import MAX_FILE_BYTES, NOT_BUILT, StaticFiles, TooLarge
from .timelines import PAGE_FILES, find_page_dir, read_page_file
from .ws_gateway import WsGateway, same_secret

if TYPE_CHECKING:
    from .launcher import Launcher
    from .runtime import LauncherRuntime
    from .source import Source

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


def error(status: int, code: str, message: str, detail: object = None) -> web.Response:
    return web.json_response(error_payload(code, message, detail), status=status)


def page_load(request: web.Request) -> bool:
    """Whether the browser loads a page (navigation), not a fetch or script: only then is a SPA route answered with
    index.html."""
    mode = request.headers.get("Sec-Fetch-Mode")
    if mode is not None:
        return mode == "navigate"
    return "text/html" in request.headers.get("Accept", "")


@dataclass(frozen=True)
class RouteSpec:
    """Declarative specification for one `/api/*` HTTP endpoint (shared by router wiring and offline OpenAPI)."""
    method: Literal["GET", "POST"]
    path: str
    handler: str
    operation_id: str
    summary: str
    tags: tuple[str, ...] = ()
    request_model: Any = None
    response_model: Any = None
    query_params: tuple[str, ...] = ()
    path_params: tuple[str, ...] = ()
    max_body: int = MAX_BODY
    request_body_required: bool = True
    error_statuses: tuple[int, ...] = ()
    response_media_type: str = "application/json"
    description: str = ""


API_ROUTES: tuple[RouteSpec, ...] = (
    RouteSpec("GET", "/api/app", "get_app", "getApp",
              "Get GUI server capabilities, launcher status, and limits",
              tags=("server",), response_model=AppResponse, error_statuses=(401, 403)),
    RouteSpec("GET", "/api/configs", "get_configs", "listConfigs",
              "List simulation configuration files under --root",
              tags=("configs",), response_model=ConfigsResponse, error_statuses=(401, 403)),
    RouteSpec("GET", "/api/config", "get_config", "getConfig",
              "Read the UTF-8 text and SHA-256 stamp of a configuration file",
              tags=("configs",), query_params=("file",), response_model=ConfigGetResponse,
              error_statuses=(400, 401, 403, 404, 413, 422)),
    RouteSpec("POST", "/api/config", "post_config", "saveConfig",
              "Check and atomically save a configuration file",
              tags=("configs",), request_model=ConfigSaveRequest, response_model=ConfigSaveResponse,
              max_body=MAX_TEXT_BODY,
              error_statuses=(400, 401, 403, 404, 409, 413, 415, 422, 500, 502, 503, 504)),
    RouteSpec("POST", "/api/check", "post_check", "checkConfig",
              "Validate a saved configuration file or unsaved configuration text",
              tags=("configs",), request_model=CheckRequest, response_model=CheckResponse, max_body=MAX_TEXT_BODY,
              error_statuses=(400, 401, 403, 404, 409, 413, 415, 422, 500, 502, 503, 504)),
    RouteSpec("POST", "/api/describe", "post_describe", "describeConfig",
              "Refresh cached ECU --describe metadata for a configuration file",
              tags=("configs",), request_model=DescribeRequest, response_model=DescribeResponse,
              error_statuses=(400, 401, 403, 404, 413, 415, 422, 500, 502, 503, 504)),
    RouteSpec("POST", "/api/connections", "post_connections", "listConnections",
              "List usable external bridge connection targets for a network",
              tags=("configs",), request_model=ConnectionsRequest, response_model=ConnectionsResponse,
              error_statuses=(400, 401, 403, 404, 413, 415, 502, 503, 504)),
    RouteSpec("POST", "/api/prepare", "post_prepare", "prepareRun",
              "Warm up the persistent runtime for a configuration when eligible",
              tags=("runs",), request_model=PrepareRequest, response_model=PrepareResponse,
              error_statuses=(400, 401, 403, 404, 409, 413, 415, 502, 503, 504),
              description=(
                  "Inspects the configuration and warms up the persistent `serve` runtime when eligible. "
                  "Returns HTTP 200 `{instance}` once preparation finishes; runtime status transitions are broadcast "
                  "on `/ws` as `runtime` messages."
              )),
    RouteSpec("POST", "/api/run", "post_run", "startRun",
              "Check and start a simulation run",
              tags=("runs",), request_model=RunRequest, response_model=RunResponse,
              error_statuses=(400, 401, 403, 404, 409, 413, 415, 422, 500, 502, 503, 504),
              description=(
                  "Checks the configuration and starts a simulation run. HTTP 200 returns `{instance, run_id}` as soon "
                  "as the run is handed off; lifecycle, logs, variables, and the final result arrive asynchronously on "
                  "`/ws` (see `schemas/ws-server.schema.json`)."
              )),
    RouteSpec("POST", "/api/stop-session", "post_stop_session", "stopSession",
              "Request a graceful or forced stop of an identified run",
              tags=("runs",), request_model=StopSessionRequest, response_model=StopSessionResponse,
              error_statuses=(400, 401, 403, 409, 413, 415),
              description=(
                  "Requests a graceful (`force: false`, default) or forced (`force: true`) stop of the identified run. "
                  "Requires both `run_id` and `instance` (mismatches return `409 session-changed`). Idempotent: returns "
                  "HTTP 200 `{instance, run_id}` even when the targeted run has already ended."
              )),
    RouteSpec("POST", "/api/time", "post_time", "sendTimeCommand",
              "Send a time command to the running simulation",
              tags=("control",), request_model=TimeCommandRequest, response_model=TimeCommandAck,
              error_statuses=(400, 401, 403, 409, 413, 415, 503),
              description=(
                  "Validates the command and writes one NDJSON line to the launcher's standard input, returning HTTP 200 "
                  "as soon as the command is forwarded. HTTP 200 acknowledges forwarding only; whether the launcher "
                  "accepted or refused the command arrives asynchronously on `/ws` as an `event` message with "
                  "`event.event == \"command\"` (correlated by the optional numeric `id`; see "
                  "`schemas/ws-command-event.schema.json`).\n\n"
                  "Targeting: Protocol v4 preserves omitting both `run_id` and `instance` unchanged for compatibility "
                  "(targeting whichever run is current). However, new clients, AI agents, and CI automation should "
                  "always send both `instance` and `run_id` so the server refuses with `409 session-changed` if another "
                  "run has started or the server restarted. Sending only one of `run_id` or `instance` returns "
                  "`400 invalid-field`."
              )),
    RouteSpec("POST", "/api/write", "post_write", "writeVariables",
              "Write ECU variables in the running simulation",
              tags=("control",), request_model=WriteRequest, response_model=WriteResponse,
              error_statuses=(400, 401, 403, 409, 413, 415, 502, 503, 504),
              description=(
                  "Validates variable names, writability, and value bounds against the active run's schema and forwards "
                  "the write to the launcher Control Server. HTTP 200 `{run_id, operation}` acknowledges submission only; "
                  "the asynchronous execution outcome arrives on `/ws` as `operation` messages. Returns `502 write-failed` "
                  "when the Control Server write RPC fails."
              )),
    RouteSpec("GET", "/api/session", "get_session", "getSession",
              "Get the current run summary, server availability, and runtime status",
              tags=("runs",), response_model=SessionResponse, error_statuses=(401, 403)),
    RouteSpec("GET", "/api/dashboard", "get_dashboard", "getDashboard",
              "Read the dashboard layout for a configuration file",
              tags=("configs",), query_params=("file",), response_model=DashboardResponse,
              error_statuses=(400, 401, 403, 404)),
    RouteSpec("GET", "/api/scenarios", "get_scenarios", "listScenarios",
              "List scenario files next to a configuration file",
              tags=("configs",), query_params=("file",), response_model=ScenariosResponse,
              error_statuses=(400, 401, 403, 404)),
    RouteSpec("GET", "/api/recording", "get_recording", "getRecording",
              "Export applied variable writes of the current run as scenario YAML",
              tags=("artifacts",), response_model=RecordingResponse, error_statuses=(401, 403)),
    RouteSpec("GET", "/api/timeline/{run_id}", "get_timeline", "getTimeline",
              "Read the recorded timeline JSON file of a completed scenario run",
              tags=("artifacts",), path_params=("run_id",), response_model=None,
              error_statuses=(401, 403, 404),
              description=(
                  "Streams the recorded scenario timeline JSON file for a completed scenario run (`schema/timeline.schema.json`)."
              )),
)
_ROUTE_MAX_BODY: dict[str, int] = {spec.handler: spec.max_body for spec in API_ROUTES}


def route_specs() -> tuple[RouteSpec, ...]:
    """Returns the declarative `/api/*` endpoint table (callable offline without instantiating `GuiServer`)."""
    return API_ROUTES


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
    auth: bool = True  # require token authentication on HTTP API and WebSocket (not --no-auth)
    check_runner: CheckRunner | None = None  # injectable stand-in for `check --json` subprocesses in tests


class GuiServer:
    """Composition root wiring HTTP transport, security middleware, `ConfigService`, `RunManager`, and `WsGateway`."""

    def __init__(self, options: Options):
        if options.auth and not options.token:
            raise ValueError("Options.token must be non-empty when auth is enabled")
        self.o = options
        self.instance = secrets.token_hex(8)
        # The panel modules are served under panels/<key>/ (a module cannot be imported with a header): this key
        # is not the token, gives only the reading of panel modules, and is told only with GET api/dashboard.
        self.panel_key = secrets.token_urlsafe(18)
        self.configs = ConfigService(
            options.root,
            options.launcher,
            panel_key=self.panel_key,
            participant_start_timeout_ms=options.participant_start_timeout_ms,
            participant_stop_timeout_ms=options.participant_stop_timeout_ms,
            check_timeout_s=options.check_timeout_s,
            command_changes_allowed=options.command_changes_allowed,
            check_runner=options.check_runner,
        )
        self.runs = RunManager(
            instance=self.instance,
            launcher=options.launcher,
            configs=self.configs,
            runtime=options.runtime,
            initial_file=options.initial_file,
            stop_timeout_s=options.stop_timeout_s,
            participant_start_timeout_ms=options.participant_start_timeout_ms,
            participant_stop_timeout_ms=options.participant_stop_timeout_ms,
            check_timeout_s=options.check_timeout_s,
            silent_after_s=options.silent_after_s,
            limits=options.limits,
            variables=options.variables,
            variable_limits=options.variable_limits,
            source=options.source,
            save=options.save,
            command_changes_allowed=options.command_changes_allowed,
            auth=options.auth,
        )
        self.configs.bind_runtime_hooks(
            not_closing=self.runs.not_closing,
            ensure_usable=self.runs.ensure_usable,
            facts=self.runs.facts,
            on_config_saved=self.runs.release_config,
        )
        self.ws = WsGateway(
            self.runs,
            auth=options.auth,
            token=options.token,
            variable_limits=options.variable_limits,
        )
        self.runs.bind_hub(self.ws.hub, self.ws.wake_watches)
        self.static = StaticFiles(options.web_dir)
        self.timeline_page = find_page_dir()
        self.client_dir = client_files.find_dir()
        self._client_warned = False  # the files that vsil/ serves are missing: said once on stderr, at the first request
        port = options.port
        self.hosts = {f"127.0.0.1:{port}"}
        self.origins = {f"http://127.0.0.1:{port}"}
        if options.dev_origin:
            self.origins.add(options.dev_origin)
            self.hosts.add(options.dev_origin.split("://", 1)[1])

    async def shutdown(self) -> None:
        """Orchestrates bounded shutdown across RunManager, ConfigService, and WsGateway in strict phase order."""
        if not self.runs.begin_shutdown():
            return
        self.configs.cancel_checks()
        await self.runs.shutdown_runtime()
        self.configs.cancel_checks()  # one that was starting when the end began
        self.runs.warn_active_pids()
        await self.ws.shutdown()
        await self.runs.cleanup_timelines()
        await self.configs.cleanup()

    def kill_all(self) -> None:
        """Last-resort process-tree kill when shutdown exceeds its budget."""
        self.runs.kill_all()
        self.configs.cancel_checks()

    # ------------------------------------------------------------ aiohttp application & middleware

    def app(self) -> web.Application:
        # MAX_BODY for every body (see _body), except routes whose RouteSpec declares a larger max_body
        app = web.Application(client_max_size=max(spec.max_body for spec in API_ROUTES),
                              middlewares=[self._headers, self._guard])
        for spec in API_ROUTES:
            bound = getattr(self, spec.handler)
            if spec.method == "GET":
                app.router.add_get(spec.path, bound)
            else:
                app.router.add_post(spec.path, bound)
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
            if self.o.auth and not self._token_ok(request.headers.get("Authorization", "")):
                response = error(401, "unauthorized",
                                 "missing or wrong token: send Authorization: Bearer <token>, the token in the URL "
                                 "the server printed (#token=...)")
                response.headers["WWW-Authenticate"] = "Bearer"
                return response
            if request.method == "POST" and request.body_exists and request.content_type != "application/json":
                return error(415, "unsupported-media-type", "send the body as JSON (Content-Type: application/json)")
        return await handler(request)

    def _token_ok(self, value: str) -> bool:
        scheme, _, given = value.partition(" ")
        return scheme.lower() == "bearer" and same_secret(given.strip(), self.o.token)

    async def _body(self, request: web.Request, *, max_size: int, empty_ok: bool = False) -> dict:
        if empty_ok and not request.body_exists:
            return {}
        raw = await request.clone(client_max_size=max_size).read()
        try:
            return parse_json_object(raw, allow_non_finite=True)
        except ValueError as e:
            raise Refused(400, "invalid-json", str(e)) from None

    @staticmethod
    def _validate(model: type[T], body: dict, *, endpoint: str,
                  allowed_fields: tuple[str, ...] | None = None,
                  defer_file: bool = False) -> T:
        fields = allowed_fields if allowed_fields is not None else model_allowed_fields(model)  # type: ignore[arg-type]
        try:
            if defer_file:
                return validate_model_defer_file(model, body)[0]  # type: ignore[arg-type,return-value]
            return model.model_validate(body)  # type: ignore[attr-defined]
        except ValidationError as exc:
            code, message = format_validation_error(exc, endpoint=endpoint, allowed_fields=fields)
            raise Refused(400, code, message) from None

    async def _body_model(self, request: web.Request, model: type[T], *, endpoint: str,
                          max_size: int, empty_ok: bool = False,
                          allowed_fields: tuple[str, ...] | None = None,
                          defer_file: bool = False) -> tuple[T, dict]:
        body = await self._body(request, max_size=max_size, empty_ok=empty_ok)
        return self._validate(model, body, endpoint=endpoint, allowed_fields=allowed_fields,
                              defer_file=defer_file), body

    # ------------------------------------------------------------ thin API route adapters

    async def get_app(self, request: web.Request) -> web.Response:
        return web.json_response(await self.runs.get_app())

    async def get_configs(self, request: web.Request) -> web.Response:
        return web.json_response(await self.configs.list_configs())

    async def get_config(self, request: web.Request) -> web.Response:
        return web.json_response(await self.configs.get_config(request.query.get("file")))

    async def post_config(self, request: web.Request) -> web.Response:
        self.configs.check_save_allowed()
        body = await self._body(request, max_size=_ROUTE_MAX_BODY["post_config"])
        return web.json_response(await self.configs.save_config(body))

    async def post_check(self, request: web.Request) -> web.Response:
        self.runs.not_closing()
        body = await self._body(request, max_size=_ROUTE_MAX_BODY["post_check"])
        return web.json_response(await self.configs.check(body))

    async def post_describe(self, request: web.Request) -> web.Response:
        self.runs.not_closing()
        req, body = await self._body_model(request, DescribeRequest, endpoint="api/describe",
                                           max_size=_ROUTE_MAX_BODY["post_describe"], defer_file=True)
        return web.json_response(await self.configs.describe(req, body.get("file")))

    async def post_connections(self, request: web.Request) -> web.Response:
        self.runs.not_closing()
        req, body = await self._body_model(request, ConnectionsRequest, endpoint="api/connections",
                                           max_size=_ROUTE_MAX_BODY["post_connections"], defer_file=True)
        return web.json_response(await self.configs.connections(req, body.get("file")))

    async def post_prepare(self, request: web.Request) -> web.Response:
        async with self.runs.prepare_guard():
            req, body = await self._body_model(request, PrepareRequest, endpoint="api/prepare",
                                               max_size=_ROUTE_MAX_BODY["post_prepare"], defer_file=True)
            return web.json_response(await self.runs.prepare(req, body.get("file")))

    async def post_run(self, request: web.Request) -> web.Response:
        self.runs.not_closing()
        req, body = await self._body_model(request, RunRequest, endpoint="api/run",
                                           max_size=_ROUTE_MAX_BODY["post_run"],
                                           allowed_fields=REQUEST_FIELDS, defer_file=True)
        return web.json_response(await self.runs.start_run(req, body))

    async def post_stop_session(self, request: web.Request) -> web.Response:
        req, _ = await self._body_model(request, StopSessionRequest, endpoint="api/stop-session",
                                        max_size=_ROUTE_MAX_BODY["post_stop_session"], empty_ok=True)
        return web.json_response(await self.runs.stop_session(req))

    async def post_time(self, request: web.Request) -> web.Response:
        self.runs.not_closing()
        body = await self._body(request, max_size=_ROUTE_MAX_BODY["post_time"])
        return web.json_response(await self.runs.send_time(body))

    async def post_write(self, request: web.Request) -> web.Response:
        self.runs.not_closing()
        body = await self._body(request, max_size=_ROUTE_MAX_BODY["post_write"])
        return web.json_response(await self.runs.write_variables(body))

    async def get_session(self, request: web.Request) -> web.Response:
        return web.Response(text=self.runs.get_session_payload(), content_type="application/json")

    async def get_dashboard(self, request: web.Request) -> web.Response:
        return web.json_response(await self.configs.dashboard(request.query.get("file")))

    async def get_scenarios(self, request: web.Request) -> web.Response:
        return web.json_response(await self.configs.scenarios(request.query.get("file")))

    async def get_recording(self, request: web.Request) -> web.Response:
        return web.json_response(self.runs.get_recording())

    async def get_timeline(self, request: web.Request) -> web.StreamResponse:
        path, name = await self.runs.get_timeline_file(request.match_info["run_id"])
        return web.FileResponse(path, headers={"Content-Type": "application/json",
                                               "Content-Disposition": f'inline; filename="{name}"'})

    async def api_not_found(self, request: web.Request) -> web.Response:
        return error(404, "not-found", f"{request.method} {request.path}: no such API")

    async def websocket(self, request: web.Request) -> web.StreamResponse:
        return await self.ws.handle_ws(request)

    # ------------------------------------------------------------ thin static, panel, timeline, client adapters

    async def get_panel(self, request: web.Request) -> web.StreamResponse:
        """A panel module (dashboard.module_path): only with the panel key, as JavaScript, never sniffed."""
        not_found = web.Response(status=404, text="not found\n", content_type="text/plain")
        if not same_secret(request.match_info["key"], self.panel_key):
            return not_found
        body = await self.configs.read_panel_module(request.match_info["path"])
        if body is None:
            return not_found
        return web.Response(body=body, headers={"Content-Type": "text/javascript; charset=utf-8",
                                                "Cache-Control": "no-store"})

    async def get_timeline_page(self, request: web.Request) -> web.StreamResponse:
        """The timeline viewer page (gui/timeline) under timeline/ (no token, as other screen assets)."""
        if "name" not in request.match_info:
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
        """The client library and elements (client_files.FILES) under vsil/ (no token)."""
        found = client_files.FILES.get(request.match_info.get("name", ""))
        if found is None:
            return error(404, "not-found", f"GET {request.path}: the GUI server serves only "
                                           + " and ".join(f"/{client_files.PATH}{n}" for n in client_files.FILES))
        if self.client_dir is None:
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
        if not self._client_warned:
            self._client_warned = True
            log.warning("%s", message)

    async def get_static(self, request: web.Request) -> web.StreamResponse:
        not_found = web.Response(status=404, text="not found\n", content_type="text/plain")
        if request.headers.get("Service-Worker") == "script":
            return web.Response(status=404, text="the GUI server does not serve service workers\n",
                                content_type="text/plain")
        path, media, status = self.static.lookup(request.path, page_load(request))
        if status != 200:
            return not_found
        if path is None:
            return web.Response(text=NOT_BUILT, content_type="text/html", charset="utf-8")
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
