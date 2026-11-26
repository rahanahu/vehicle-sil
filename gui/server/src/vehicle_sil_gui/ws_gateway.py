"""WebSocket gateway: connection pools, auth/eviction, replay snapshots, watch scheduling, and Hub ownership.

Owns:
- `Hub` instance (broadcast/per-client queues)
- `_sockets`, `_unauthenticated`, `_authenticated`, `_evicting`
- `_watching`, `_ticker`, `_tick_now`
- Clean awaited shutdown of `_ticker`, `_evicting`, and open WebSocket connections

Does not import `ConfigService`, `ServeRuntime`, or `server`.
"""

from __future__ import annotations

import asyncio
import hmac
import json
import logging
import time
from dataclasses import dataclass, field

from aiohttp import WSCloseCode, WSMsgType, web

from . import __version__, process
from .contracts import PROTOCOL, error_payload
from .hub import Client, Hub, Outgoing, encode
from .run_manager import SOCKETS_CLOSE_S, RunManager
from .variables import Limits as VariableLimits, Rejected, Watch, check_watch

MAX_WS_MESSAGE = 256 * 1024  # a watch that names many variables
MAX_SOCKETS = 32  # authenticated WebSocket connections
MAX_UNAUTHENTICATED = 16  # connections waiting for their auth; when full, the oldest is closed for a new one
AUTH_TIMEOUT_S = 5
WS_CLOSE_TIMEOUT_S = 2  # waiting for the screen's answer to a close

log = logging.getLogger("vehicle_sil_gui")


def same_secret(given: str, token: str) -> bool:
    """Constant time. surrogatepass: whatever the client sent (a header that is not UTF-8, a lone surrogate in
    JSON) is compared, never an error."""
    return hmac.compare_digest(given.encode("utf-8", "surrogatepass"), token.encode("utf-8", "surrogatepass"))


def count_or_none(value: object) -> int | None:
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else None


@dataclass(eq=False)
class Connection:
    """An authenticated screen: its queue and its watches (by the id the screen gave them)."""
    client: Client
    watches: dict[int, Watch] = field(default_factory=dict)


class WsGateway:
    """Owns the WebSocket upgrade lifecycle, Hub fan-out, replay snapshots, and variable watch scheduler."""

    def __init__(
        self,
        runs: RunManager,
        *,
        auth: bool = True,
        token: str = "",
        variable_limits: VariableLimits | None = None,
    ):
        self._runs = runs
        self._auth = auth
        self._token = token
        self._variable_limits = variable_limits or VariableLimits()
        self.hub = Hub(self.snapshot, next_stream_seq=runs.next_stream_seq)
        self._sockets: dict[web.WebSocketResponse, web.BaseRequest] = {}
        self._unauthenticated: dict[web.WebSocketResponse, None] = {}  # oldest first
        self._authenticated: set[web.WebSocketResponse] = set()
        self._evicting: set[asyncio.Task] = set()
        self._watching: dict[Watch, Client] = {}  # every watch of every connection, with its connection's queue
        self._ticker: asyncio.Task | None = None
        self._tick_now = asyncio.Event()

    def wake_watches(self) -> None:
        """Marks every active watch due immediately (called when variable state or schema changes)."""
        if self._watching:
            now = time.monotonic()
            for watch in self._watching:
                watch.due = now
            self._tick_now.set()

    def snapshot(self, run_id: str | None, after_stream_seq: int | None) -> list[dict]:
        s = self._runs.session
        sseq = self._runs.stream_seq
        hello = {
            "type": "hello", "vehicle_sil_gui": PROTOCOL, "version": __version__,
            "instance": self._runs.instance, "stream_seq": sseq,
            "session": self._runs.session_summary(s),
            "allowed": self._runs.allowed(s), "replay": None,
            "runtime": self._runs.runtime.status(),
        }
        if s is None:
            return [hello, {"type": "synced", "stream_seq": sseq, "run_id": None, "last_seq": None}]
        start = after_stream_seq if (run_id == s.id and after_stream_seq is not None) else None
        hello["replay"] = {"after_stream_seq": start}
        dropped, events = s.replay(start)
        return [hello, *([dropped] if dropped else []), *events,
                {"type": "synced", "stream_seq": sseq, "run_id": s.id, "last_seq": s.last_seq}]

    async def handle_ws(self, request: web.Request) -> web.StreamResponse:
        if self._runs.closing:
            return web.json_response(error_payload("shutting-down", "the GUI server is stopping"), status=503)
        if len(self._authenticated) >= MAX_SOCKETS:
            return web.json_response(
                error_payload("too-many-connections", "too many screens are connected: close some tabs"),
                status=503,
            )
        if self._auth and len(self._unauthenticated) >= MAX_UNAUTHENTICATED:
            # The oldest connection that has not sent its auth makes room: a process without the token that
            # opens connections cannot keep the screen out, only make it try again.
            oldest = next(iter(self._unauthenticated))
            del self._unauthenticated[oldest]
            task = asyncio.create_task(self._make_room(oldest))
            self._evicting.add(task)
            task.add_done_callback(self._evicting.discard)
        ws = web.WebSocketResponse(max_msg_size=MAX_WS_MESSAGE, heartbeat=30, timeout=WS_CLOSE_TIMEOUT_S)
        self._sockets[ws] = request  # counted from here, before the handshake's first await
        if self._auth:
            self._unauthenticated[ws] = None
        else:
            self._authenticated.add(ws)
        try:
            await ws.prepare(request)
            await self._talk(ws)
        finally:
            self._sockets.pop(ws, None)
            self._unauthenticated.pop(ws, None)
            self._authenticated.discard(ws)
        return ws

    async def shutdown(self) -> None:
        """Closes Hub queues, cancels and awaits background watch/eviction tasks, and closes all WS sockets."""
        self.hub.close_all()
        bg_tasks: list[asyncio.Task] = []
        if self._ticker is not None:
            ticker, self._ticker = self._ticker, None
            ticker.cancel()
            bg_tasks.append(ticker)
        for task in list(self._evicting):
            task.cancel()
            bg_tasks.append(task)
        if bg_tasks:
            results = await asyncio.gather(*bg_tasks, return_exceptions=True)
            for res in results:
                if isinstance(res, asyncio.CancelledError):
                    continue
                if isinstance(res, Exception):
                    log.exception("background server task failed during shutdown", exc_info=res)
                elif isinstance(res, BaseException):
                    raise res
        await self._close_sockets()

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

    @staticmethod
    async def _make_room(ws: web.WebSocketResponse) -> None:
        try:
            await ws.close(code=WSCloseCode.TRY_AGAIN_LATER, message=b"too-many-unauthenticated")
        except Exception:  # not prepared yet: _talk closes it when it is
            pass

    async def _authenticate(self, ws: web.WebSocketResponse) -> tuple[bool, str | None, int | None]:
        """Performs the initial auth frame exchange; returns (ok, replay_run_id, replay_after_stream_seq)."""
        if ws not in self._unauthenticated:  # it made room for a newer one during its handshake
            await ws.close(code=WSCloseCode.TRY_AGAIN_LATER, message=b"too-many-unauthenticated")
            return False, None, None
        try:
            async with asyncio.timeout(AUTH_TIMEOUT_S):
                msg = await ws.receive()
        except TimeoutError:
            await ws.close(code=4401, message=b"auth-timeout")
            return False, None, None
        if self._runs.closing or ws.closed or ws not in self._unauthenticated or \
                msg.type in (WSMsgType.CLOSE, WSMsgType.CLOSING, WSMsgType.CLOSED, WSMsgType.ERROR):
            return False, None, None
        auth = None
        if msg.type == WSMsgType.TEXT:
            try:
                auth = json.loads(msg.data)
            except (ValueError, RecursionError):
                pass
        if not (isinstance(auth, dict) and auth.get("type") == "auth" and isinstance(auth.get("token"), str)
                and same_secret(auth["token"], self._token)):
            await ws.send_json({"type": "error", "run_id": None, "code": "unauthorized",
                                "message": "the first message must be {\"type\":\"auth\",\"token\":...} with the "
                                           "token of the URL the server printed", "detail": None})
            await ws.close(code=4401, message=b"unauthorized")
            return False, None, None
        del self._unauthenticated[ws]
        if len(self._authenticated) >= MAX_SOCKETS:  # others authenticated since this one's handshake
            await ws.close(code=WSCloseCode.TRY_AGAIN_LATER, message=b"too-many-connections")
            return False, None, None
        self._authenticated.add(ws)
        req_run_id = auth.get("run_id") if isinstance(auth.get("run_id"), str) and auth.get("run_id") else None
        req_after_seq = count_or_none(auth.get("after_stream_seq"))
        if "instance" in auth and auth.get("instance") is not None and auth.get("instance") != self._runs.instance:
            req_run_id = None
            req_after_seq = None
        return True, req_run_id, req_after_seq

    async def _talk(self, ws: web.WebSocketResponse) -> None:
        req_run_id: str | None = None
        req_after_seq: int | None = None
        if self._auth:
            ok, req_run_id, req_after_seq = await self._authenticate(ws)
            if not ok:
                return
        elif self._runs.closing or ws.closed:
            return
        client = self.hub.client(ws.send_str)
        connection = Connection(client)
        try:
            await process.race_first(
                self.hub.serve(client, req_run_id, req_after_seq),
                self._listen(ws, connection),
                propagate=False,
            )
        finally:
            for watch in connection.watches.values():
                self._unwatch(watch)
            connection.watches.clear()
        await ws.close(code=WSCloseCode.GOING_AWAY if self._runs.closing else WSCloseCode.OK)

    async def _listen(self, ws: web.WebSocketResponse, connection: Connection) -> None:
        """Reads what the screen sends after auth (watch, unwatch), until it closes."""
        async for msg in ws:
            if msg.type in (WSMsgType.ERROR, WSMsgType.CLOSE):
                return
            if msg.type != WSMsgType.TEXT:
                continue
            try:
                self._from_screen(connection, msg.data)
            except Rejected as e:
                connection.client.put(Outgoing({"type": "error", "run_id": None, "code": e.code,
                                                "message": str(e), "detail": e.detail}))

    def _from_screen(self, connection: Connection, text: str) -> None:
        try:
            message = json.loads(text)
        except (ValueError, RecursionError):
            message = None
        if not isinstance(message, dict) or not isinstance(message.get("type"), str):
            raise Rejected(400, "invalid-message", 'a message must be a JSON object with "type" (watch, unwatch): '
                                                   "it is ignored")
        limits = self._variable_limits
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
            if not self._runs.closing and (self._ticker is None or self._ticker.done()):
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
        """The `values` message of a watch, made when its connection sends it."""
        run = self._runs.current
        if run is None or not run.variables.has_values:
            return None
        if client.variables_of != run.session.id:
            watch.due = time.monotonic()  # again, now behind the run's `variables` in the queue
            self._tick_now.set()
            return None
        return encode(run.variables.values_message(watch))

    async def _tick(self) -> None:
        """Refreshes due watched peers from the active run's control server and schedules changed watch frames."""
        read_again = 0.0
        while self._watching and not self._runs.closing:
            self._tick_now.clear()
            now = time.monotonic()
            try:
                due = [(watch, client) for watch, client in self._watching.items() if watch.due <= now]
                for watch, _ in due:
                    watch.due = max(watch.due + watch.period_s, now + watch.period_s / 2)
                run = self._runs.current
                if due and run is not None and run.variables.state == "ready" and now >= read_again:
                    names = {name for watch, _ in due for name in run.variables.peer_names(watch)}
                    if names:
                        await run.variables.refresh(names)  # ends within the read's timeout; never raises
                        read_again = time.monotonic() + (time.monotonic() - now)
                run = self._runs.current
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
