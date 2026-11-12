"""The screens connected over WebSocket, each with a bounded queue of messages to send.

Each message is turned into text once, however many screens get it. A screen that reads too slowly does not
make the server keep everything for it: when its queue is over its limit (messages or bytes), the log lines and
the events that are not in the summary are dropped from the queue and counted in one `dropped` message, and
`time` events are reduced to the last one. Only when that is not enough is the queue emptied, and the screen gets
`resync` followed by a fresh hello (what a reconnection gets).
"""

from __future__ import annotations

import asyncio
import json
import logging
from collections import deque
from collections.abc import Awaitable, Callable

QUEUE_LIMIT = 2000  # messages waiting for one screen
QUEUE_BYTES = 24 * 1024 * 1024  # and their size (more than the largest summary, so one summary never overflows)
SEND_TIMEOUT_S = 30  # one message that cannot be sent in this time closes the connection

# Events that the summary is made of: never dropped for a slow screen (the others can be, and are counted).
SUMMARY_EVENTS = frozenset({"hello", "phase", "check", "registry", "participant", "finding", "command", "result"})
DROPPED_SOURCES = 64  # sources named in one slow-client `dropped`; the others are counted under "*"

log = logging.getLogger("vehicle_sil_gui")


def encode(message: dict) -> str:
    text = json.dumps(message, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    try:
        text.encode("utf-8")
    except UnicodeEncodeError:  # a lone surrogate from the launcher's JSON: escape it, a text frame must be UTF-8
        text = json.dumps(message, ensure_ascii=True, separators=(",", ":"), allow_nan=False)
    return text


class Outgoing:
    """One message for the screens, as text, with what a slow screen's queue needs to know about it."""

    __slots__ = ("text", "size", "kind", "session", "source")

    def __init__(self, message: dict, log_key: Callable[[str], str] | None = None):
        """log_key: the name a log line's source has in the session's summary (where the `dropped` counts of
        a slow screen are put)."""
        self.text = encode(message)
        self.size = len(self.text)
        self.session = message.get("session") if isinstance(message.get("session"), int) else None
        self.source = None
        self.kind = "keep"
        if message.get("type") == "event":
            event = message["event"]["event"]
            if event == "log":
                source = message["event"].get("source")
                if isinstance(source, str) and log_key is not None:
                    source = log_key(source)
                self.kind, self.source = "log", source
            elif event == "time":
                self.kind = "time"
            elif event not in SUMMARY_EVENTS:
                self.kind = "other"


class _Dropped:
    """A `dropped` message waiting in a queue; more drops are added to it until it is sent."""

    __slots__ = ("session", "log", "events", "size")

    def __init__(self, session: int | None):
        self.session = session
        self.log: dict[str, int] = {}
        self.events = 0
        self.size = 200

    @property
    def text(self) -> str:
        return encode({"type": "dropped", "session": self.session, "reason": "slow-client", "log": self.log,
                       "at_least": [], "events": self.events})


class _Marker:
    pass


RESYNC = _Marker()
CLOSE = _Marker()


class Client:
    def __init__(self, send: Callable[[str], Awaitable[None]], limit: int = QUEUE_LIMIT, max_bytes: int = QUEUE_BYTES):
        self.send = send
        self.items: deque = deque()
        self.bytes = 0
        self.limit = limit
        self.max_bytes = max_bytes
        self.ready = asyncio.Event()
        self.resyncs = 0
        self.sheds = 0

    def put(self, item: Outgoing) -> None:
        if self.items and (self.items[0] is RESYNC or self.items[-1] is CLOSE):
            return  # a resync sends everything again from the state at that time; a closing queue takes nothing
        self.items.append(item)
        self.bytes += item.size
        if len(self.items) > self.limit or self.bytes > self.max_bytes:
            self._shed()
        self.ready.set()

    def _shed(self) -> None:
        """Drops what can be dropped; when that is not enough, everything (resync)."""
        self.sheds += 1
        kept: deque = deque()
        notices: dict[int | None, _Dropped] = {}
        last_time: dict[int | None, Outgoing] = {}
        for item in self.items:
            if isinstance(item, Outgoing) and item.kind == "time":
                last_time[item.session] = item
        for item in self.items:
            if isinstance(item, _Dropped) and item.session not in notices:
                notices[item.session] = item
                kept.append(item)
                continue
            if not isinstance(item, Outgoing) or item.kind == "keep" or \
                    (item.kind == "time" and last_time.get(item.session) is item):
                kept.append(item)
                continue
            if item.kind == "time":
                continue
            notice = notices.get(item.session)
            if notice is None:
                notice = notices[item.session] = _Dropped(item.session)
                kept.append(notice)  # where the first dropped message was
            if item.kind == "log":
                key = item.source if isinstance(item.source, str) else "*"
                if key not in notice.log and len(notice.log) >= DROPPED_SOURCES:
                    key = "*"
                notice.log[key] = notice.log.get(key, 0) + 1
            else:
                notice.events += 1
        self.items = kept
        self.bytes = sum(item.size for item in kept if not isinstance(item, _Marker))
        if len(kept) > 1 and (len(kept) > self.limit or self.bytes > self.max_bytes):
            self.items = deque([RESYNC])
            self.bytes = 0
            self.resyncs += 1

    def close(self) -> None:
        self.items = deque([CLOSE])
        self.bytes = 0
        self.ready.set()

    async def next(self) -> object:
        while not self.items:
            self.ready.clear()
            await self.ready.wait()
        item = self.items.popleft()
        if item is RESYNC:  # what is still queued is in the snapshot taken next
            self.items.clear()
            self.bytes = 0
        elif not isinstance(item, _Marker):
            self.bytes -= item.size
        return item


class Hub:
    def __init__(self, snapshot: Callable[[int | None, int | None], list[dict]], limit: int = QUEUE_LIMIT,
                 max_bytes: int = QUEUE_BYTES):
        """snapshot(session, from_seq) gives hello, what is replayed and synced, in order."""
        self.snapshot = snapshot
        self.limit = limit
        self.max_bytes = max_bytes
        self.clients: set[Client] = set()
        self.closed = False

    def client(self, send: Callable[[str], Awaitable[None]]) -> Client:
        return Client(send, self.limit, self.max_bytes)

    def broadcast(self, messages: list[dict], log_key: Callable[[str], str] | None = None) -> bool:
        """Queues the messages for every screen. Never raises: a message that cannot be turned into JSON text (a
        bug) is left out and logged, and the others are sent. Whether every message could be."""
        items, ok = [], True
        for m in messages:
            try:
                items.append(Outgoing(m, log_key))
            except (ValueError, TypeError, RecursionError, KeyError):
                log.exception("a message for the screens cannot be sent: %.200r", m)
                ok = False
        for client in list(self.clients):
            for item in items:
                client.put(item)
        return ok

    def close_all(self) -> None:
        self.closed = True
        for client in self.clients:
            client.close()

    async def serve(self, client: Client, session: int | None, from_seq: int | None) -> None:
        """Sends hello, the replay and then every broadcast message until the connection fails or close_all.
        The snapshot is taken in the same step as joining, so nothing is lost or sent twice between them."""
        if self.closed:
            return
        self.clients.add(client)
        try:
            pending = [encode(m) for m in self.snapshot(session, from_seq)]
            while True:
                for text in pending:
                    async with asyncio.timeout(SEND_TIMEOUT_S):
                        await client.send(text)
                item = await client.next()
                if item is CLOSE:
                    return
                if item is RESYNC:
                    pending = [encode({"type": "resync", "reason": "slow-client",
                                       "message": "this screen read too slowly and messages were dropped: the "
                                                  "state is sent again from the start"})]
                    pending += [encode(m) for m in self.snapshot(None, None)]
                else:
                    pending = [item.text]
        finally:
            self.clients.discard(client)
