"""The HTTP and WebSocket server: the API, the checks of every request, the launcher as a child process."""

from __future__ import annotations

import asyncio
import hmac
import itertools
import json
import secrets
from dataclasses import dataclass, field
from pathlib import Path

from aiohttp import WSMsgType, web

from . import __version__, process
from .hub import Client, Hub
from .launcher import (SPEED_MAX, SPEED_MIN, InvalidRequest, Launcher, check_command, check_disable, check_speed,
                       run_command)
from .paths import ConfigRoot, PathRejected
from .session import Limits, Session
from .static import NOT_BUILT, StaticFiles

PROTOCOL = 1  # vehicle_sil_gui: the version of the shapes in gui/protocol
MAX_BODY = 64 * 1024
MAX_WS_MESSAGE = 16 * 1024
AUTH_TIMEOUT_S = 10
CHECK_TIMEOUT_S = 120
MAX_LINE = 16 * 1024 * 1024  # one line of the launcher's events (a check with many variables is long)
MAX_STDERR_LINES = 50  # launcher stderr lines reported as problems in one session
ESCALATE_WAIT_S = 5  # after asking the group to stop, before killing it

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


def error(status: int, code: str, message: str, detail: object = None) -> web.Response:
    return web.json_response({"error": {"code": code, "message": message, "detail": detail}}, status=status)


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
    limits: Limits = field(default_factory=Limits)


class GuiServer:
    def __init__(self, options: Options):
        self.o = options
        self.root = ConfigRoot(options.root)
        self.launcher = options.launcher
        self.instance = secrets.token_hex(8)
        self.static = StaticFiles(options.web_dir)
        self.session: Session | None = None
        self._ids = itertools.count(1)
        self._proc: asyncio.subprocess.Process | None = None
        self._watch: asyncio.Task | None = None
        self._check_lock = asyncio.Lock()
        self._names: dict[str, tuple[tuple, list[str]]] = {}
        self._hurry = asyncio.Event()
        self._closing = False
        self.hub = Hub(self.snapshot)
        port = options.port
        self.hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}
        self.origins = {f"http://127.0.0.1:{port}", f"http://localhost:{port}"}
        if options.dev_origin:
            self.origins.add(options.dev_origin)
            self.hosts.add(options.dev_origin.split("://", 1)[1])

    # ------------------------------------------------------------ the application

    def app(self) -> web.Application:
        app = web.Application(client_max_size=MAX_BODY, middlewares=[self._headers, self._guard])
        app.router.add_get("/api/app", self.get_app)
        app.router.add_get("/api/configs", self.get_configs)
        app.router.add_post("/api/check", self.post_check)
        app.router.add_post("/api/run", self.post_run)
        app.router.add_post("/api/stop", self.post_stop)
        app.router.add_get("/api/session", self.get_session)
        app.router.add_get("/ws", self.websocket)
        app.router.add_route("*", "/api/{rest:.*}", self.api_not_found)
        app.router.add_get("/{path:.*}", self.get_static)
        return app

    @web.middleware
    async def _headers(self, request: web.Request, handler) -> web.StreamResponse:
        try:
            response = await handler(request)
        except web.HTTPException as e:  # the router's 404 and 405
            response = error(e.status, "not-found" if e.status == 404 else "method-not-allowed",
                             f"{request.method} {request.path}: {e.reason}")
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
            if request.method == "POST" and request.content_type != "application/json":
                return error(415, "unsupported-media-type", "send the body as JSON (Content-Type: application/json)")
        return await handler(request)

    def _token_ok(self, value: str) -> bool:
        scheme, _, given = value.partition(" ")
        return scheme.lower() == "bearer" and hmac.compare_digest(given.strip().encode(), self.o.token.encode())

    async def _body(self, request: web.Request) -> dict:
        try:
            body = await request.json()
        except (ValueError, UnicodeDecodeError):
            raise InvalidRequest("invalid-json", "the body is not JSON") from None
        if not isinstance(body, dict):
            raise InvalidRequest("invalid-json", "the body must be a JSON object")
        return body

    # ------------------------------------------------------------ API

    async def get_app(self, request: web.Request) -> web.Response:
        await self.launcher.probe()
        return web.json_response({
            "vehicle_sil_gui": PROTOCOL, "version": __version__, "instance": self.instance,
            "root": str(self.root.root), "initial_file": self.o.initial_file,
            "launcher": self.launcher.info(),
            "limits": {"log_lines_per_source": self.o.limits.log_lines_per_source,
                       "check_timeout_s": self.o.check_timeout_s, "speed_min": SPEED_MIN, "speed_max": SPEED_MAX},
        })

    async def get_configs(self, request: web.Request) -> web.Response:
        files, truncated = await asyncio.to_thread(self.root.list)
        return web.json_response({"root": str(self.root.root), "files": files, "truncated": truncated})

    async def get_session(self, request: web.Request) -> web.Response:
        return web.json_response({"session": self.session.summary() if self.session else None})

    async def api_not_found(self, request: web.Request) -> web.Response:
        return error(404, "not-found", f"{request.method} {request.path}: no such API")

    async def post_check(self, request: web.Request) -> web.Response:
        try:
            body = await self._body(request)
            config = self.root.resolve(body.get("file"))
            await self._usable()
            if self._check_lock.locked():
                return error(409, "check-busy", "another check is running: wait for its answer, then check again")
            wanted = body.get("disable")
            known = await self.participant_names(config.relative) if wanted else []
            disable = check_disable(wanted, known)
            code, result = await self._check(config.relative, disable)
        except PathRejected as e:
            return error(404 if e.code == "file-not-found" else 400, e.code, str(e))
        except InvalidRequest as e:
            return error(400 if e.code != "launcher-unusable" else 503, e.code, str(e))
        except LauncherFailed as e:
            return error(e.status, e.code, str(e), e.detail)
        return web.json_response({"file": config.relative, "disable": disable, "exit_code": code, "check": result})

    async def post_run(self, request: web.Request) -> web.Response:
        try:
            body = await self._body(request)
            if self.session is not None and self.session.state == "running":
                return error(409, "session-running", "a simulation is running: stop it (POST api/stop) and wait "
                                                     "for it to end before starting another")
            config = self.root.resolve(body.get("file"))
            await self._usable()
            speed = check_speed(body.get("speed"))
            start_paused = body.get("start_paused", False)
            if not isinstance(start_paused, bool):
                raise InvalidRequest("invalid-field", "start_paused must be true or false")
            for wanted, feature in ((speed is not None, "speed"), (start_paused, "start-paused")):
                if wanted and feature not in self.launcher.features:
                    raise InvalidRequest("unsupported", f"this vehicle_sil_launch has no '{feature}': leave "
                                                        f"{'speed null' if feature == 'speed' else 'start_paused false'}"
                                                        ", or build a newer launcher")
            known = await self.participant_names(config.relative)
            disable = check_disable(body.get("disable"), known)
            if self.session is not None and self.session.state == "running":  # started while the check ran
                return error(409, "session-running", "a simulation is running: stop it and wait for it to end")
            argv = run_command(self.launcher, config.relative, disable, speed, start_paused)
            try:
                proc = await process.start(argv, cwd=self.root.root)
            except OSError as e:
                raise LauncherFailed(502, "launcher-start", f"cannot start {self.launcher.path}: {e}") from None
        except PathRejected as e:
            return error(404 if e.code == "file-not-found" else 400, e.code, str(e))
        except InvalidRequest as e:
            return error(400 if e.code != "launcher-unusable" else 503, e.code, str(e))
        except LauncherFailed as e:
            return error(e.status, e.code, str(e), e.detail)
        session = Session(next(self._ids), config.relative, disable, speed, start_paused, limits=self.o.limits)
        session.launcher_pid = proc.pid
        self.session, self._proc = session, proc
        self.hub.broadcast([session.message()])
        self._watch = asyncio.create_task(self._follow(session, proc))
        return web.json_response({"session": session.summary()})

    async def post_stop(self, request: web.Request) -> web.Response:
        session, proc = self.session, self._proc
        if session is None or session.state != "running" or proc is None or proc.stdin is None:
            return error(409, "not-running", "no simulation is running")
        try:
            proc.stdin.write(b'{"command":"stop"}\n')
            await proc.stdin.drain()
        except (BrokenPipeError, ConnectionResetError, RuntimeError):
            return error(409, "not-running", "the launcher is already ending (its input is closed)")
        self.hub.broadcast(session.stop_requested())
        return web.json_response({"session": session.summary()})

    async def _usable(self) -> None:
        await self.launcher.probe()
        if not self.launcher.usable:
            raise InvalidRequest("launcher-unusable",
                                 "; ".join(p["message"] for p in self.launcher.problems) or "the launcher is not usable")

    async def participant_names(self, relative: str) -> list[str]:
        """The participant names of the file, from the last check of it, or from a new check (no --disable)
        when the file has changed since."""
        stamp = self._stamp(relative)
        cached = self._names.get(relative)
        if cached and cached[0] == stamp:
            return cached[1]
        await self._check(relative, [])
        return self._names.get(relative, ((), []))[1]

    def _stamp(self, relative: str) -> tuple:
        st = self.root.resolve(relative).absolute.stat()
        return (st.st_mtime_ns, st.st_size)

    async def _check(self, relative: str, disable: list[str]) -> tuple[int, dict]:
        async with self._check_lock:
            stamp = self._stamp(relative)
            try:
                code, out, err = await process.run_to_end(check_command(self.launcher, relative, disable),
                                                          cwd=self.root.root, timeout_s=self.o.check_timeout_s)
            except TimeoutError:
                raise LauncherFailed(504, "check-timeout", f"the check did not end in {self.o.check_timeout_s:g} s "
                                                           "(it was stopped): an ECU's --describe may hang") from None
            except OSError as e:
                raise LauncherFailed(502, "launcher-start", f"cannot start {self.launcher.path}: {e}") from None
            try:
                result = json.loads(out)
            except ValueError:
                result = None
            if not isinstance(result, dict) or not isinstance(result.get("participants"), list):
                tail = err.decode("utf-8", "replace").strip().splitlines()[-10:]
                raise LauncherFailed(502, "launcher-output", f"check --json exited with code {code} and did not "
                                                             "write the check object", tail)
            names = [p["name"] for p in result["participants"] if isinstance(p, dict) and isinstance(p.get("name"), str)]
            if names or result.get("result") not in ("usage",):
                self._names[relative] = (stamp, names)
            return code, result

    # ------------------------------------------------------------ the launcher's run

    async def _follow(self, session: Session, proc: asyncio.subprocess.Process) -> None:
        async def stdout() -> None:
            async for line in process.lines(proc.stdout, MAX_LINE):
                if line is None:
                    self.hub.broadcast(session.problem("line-too-long", f"the launcher wrote a line longer than "
                                                       f"{MAX_LINE} bytes: it is skipped", None))
                elif line.strip():
                    self.hub.broadcast(session.feed_line(line))

        async def stderr() -> None:
            count = 0
            async for line in process.lines(proc.stderr, 64 * 1024):
                count += 1
                if line and count <= MAX_STDERR_LINES:
                    self.hub.broadcast(session.problem("launcher-stderr", f"the launcher wrote: {line}", None))

        try:
            await asyncio.gather(stdout(), stderr())
        finally:
            code = await proc.wait()
            process.kill_tree(proc)  # what the launcher left in its group (POSIX)
            self.hub.broadcast(session.ended(code))

    async def shutdown(self) -> None:
        """Stops the run: end of input (the launcher stops in order), then the group is asked to stop, then
        killed. A second call (a second Ctrl+C) skips the waiting."""
        if self._closing:
            self._hurry.set()
            return
        self._closing = True
        proc, session = self._proc, self.session
        if proc is not None and proc.returncode is None and session is not None:
            if proc.stdin is not None:
                proc.stdin.close()
            if not await self._wait(proc, self.o.stop_timeout_s):
                self.hub.broadcast(session.problem("stop-escalated", "the launcher did not stop in "
                                                   f"{self.o.stop_timeout_s:g} s after its input closed: it was "
                                                   "asked to stop (SIGTERM / Ctrl+Break)", None))
                process.ask_to_stop(proc)
                if not await self._wait(proc, ESCALATE_WAIT_S):
                    process.kill_tree(proc)
        if self._watch is not None:
            try:
                async with asyncio.timeout(ESCALATE_WAIT_S):
                    await self._watch
            except TimeoutError:
                pass
        self.hub.close_all()

    async def _wait(self, proc: asyncio.subprocess.Process, seconds: float) -> bool:
        hurry = asyncio.create_task(self._hurry.wait())
        done = asyncio.create_task(proc.wait())
        await asyncio.wait({hurry, done}, timeout=seconds, return_when=asyncio.FIRST_COMPLETED)
        hurry.cancel()
        if not done.done():
            done.cancel()
            return False
        return True

    # ------------------------------------------------------------ WebSocket

    def snapshot(self, session_id: int | None, from_seq: int | None) -> list[dict]:
        s = self.session
        hello = {"type": "hello", "vehicle_sil_gui": PROTOCOL, "version": __version__, "instance": self.instance,
                 "session": s.summary() if s else None, "replay": None}
        if s is None:
            return [hello, {"type": "synced", "session": None, "last_seq": None}]
        meta, events = s.replay(from_seq if session_id == s.id else None)
        hello["replay"] = meta
        return [hello, *events, {"type": "synced", "session": s.id, "last_seq": s.last_seq}]

    async def websocket(self, request: web.Request) -> web.StreamResponse:
        if self.hub.full():
            return error(503, "too-many-connections", "too many screens are connected: close some tabs")
        ws = web.WebSocketResponse(max_msg_size=MAX_WS_MESSAGE, heartbeat=30)
        await ws.prepare(request)
        try:
            async with asyncio.timeout(AUTH_TIMEOUT_S):
                msg = await ws.receive()
        except TimeoutError:
            await ws.close(code=4401, message=b"no auth message")
            return ws
        auth = None
        if msg.type == WSMsgType.TEXT:
            try:
                auth = json.loads(msg.data)
            except ValueError:
                pass
        if not (isinstance(auth, dict) and auth.get("type") == "auth" and isinstance(auth.get("token"), str)
                and hmac.compare_digest(auth["token"].encode(), self.o.token.encode())):
            await ws.send_json({"type": "error", "session": None, "code": "unauthorized",
                                "message": "the first message must be {\"type\":\"auth\",\"token\":...} with the "
                                           "token of the URL the server printed", "detail": None})
            await ws.close(code=4401, message=b"unauthorized")
            return ws
        session_id = auth.get("session") if isinstance(auth.get("session"), int) else None
        from_seq = auth.get("from_seq") if isinstance(auth.get("from_seq"), int) else None

        async def send(message: dict) -> None:
            await ws.send_str(json.dumps(message, ensure_ascii=False, separators=(",", ":")))

        client = Client(send, self.hub.limit)
        sender = asyncio.create_task(self.hub.serve(client, session_id, from_seq))
        reader = asyncio.create_task(self._drain(ws))
        await asyncio.wait({sender, reader}, return_when=asyncio.FIRST_COMPLETED)
        for task in (sender, reader):
            task.cancel()
        await asyncio.gather(sender, reader, return_exceptions=True)
        await ws.close()
        return ws

    @staticmethod
    async def _drain(ws: web.WebSocketResponse) -> None:
        """Reads (and ignores) what the screen sends after auth, until it closes."""
        async for msg in ws:
            if msg.type in (WSMsgType.ERROR, WSMsgType.CLOSE):
                return

    # ------------------------------------------------------------ the screen

    async def get_static(self, request: web.Request) -> web.StreamResponse:
        path, media, status = self.static.lookup(request.path)
        if status != 200:
            return web.Response(status=404, text="not found\n", content_type="text/plain")
        if path is None:
            return web.Response(text=NOT_BUILT, content_type="text/html", charset="utf-8")
        return web.FileResponse(path, headers={"Content-Type": media})


class LauncherFailed(Exception):
    def __init__(self, status: int, code: str, message: str, detail: object = None):
        super().__init__(message)
        self.status, self.code, self.detail = status, code, detail
