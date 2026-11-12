"""The screens connected over WebSocket, each with a bounded queue of messages to send.

Each message is turned into text once, however many screens get it. A screen that reads too slowly does not
make the server keep everything for it: when its queue is over its limit (messages or bytes), the log lines and
the events that are not in the summary are dropped from the queue and counted in one `dropped` message (a
launcher's `log_dropped` adds its counts to the log lines of its sources), `time` events are reduced to the last
one, and heartbeats are dropped without a count (the server watches them; a screen does not need them). Only when that is not enough is the queue emptied, and the screen gets
`resync` followed by a fresh hello (what a reconnection gets).

A screen that asked for it in its auth (`session_without_check`) gets the `session` messages without the summary's
`check` (it has the check from hello and the `check` event; it does not change during a run and can be large).
That text is made once per message, when the message is queued (the summary shares lists with the session, which
change later), without encoding the check a second time.

The values of a watch are not queued as text: the watch itself waits in the queue, at most once (put_deferred),
and its message is made when the queue comes to it, from the values at that time. A slow screen so gets the
latest values each time it reads, costs nothing in between, and its queue does not grow with the values.
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

    __slots__ = ("text", "text_without_check", "size", "kind", "session", "source", "counts", "variables_of")

    def __init__(self, message: dict, log_key: Callable[[str], str] | None = None):
        """log_key: the name a log line's source has in the session's summary (where the `dropped` counts of
        a slow screen are put)."""
        self.text = encode(message)
        self.size = len(self.text)
        self.text_without_check = self.text  # for screens that asked for summaries without the check
        summary = message.get("session")
        if message.get("type") == "session" and isinstance(summary, dict) and "check" in summary:
            self.text_without_check = encode({**message, "session": {k: v for k, v in summary.items()
                                                                      if k != "check"}})
        self.session = message.get("session") if isinstance(message.get("session"), int) else None
        # The run whose variables this message tells (a `variables` message): after it, the values of that run
        # may be sent to the screen (Client.variables_of).
        self.variables_of = self.session if message.get("type") == "variables" else None
        self.source = None
        self.counts: dict[str, int] | None = None
        self.kind = "keep"
        if message.get("type") == "event":
            event = message["event"]["event"]
            if event == "log":
                source = message["event"].get("source")
                if isinstance(source, str) and log_key is not None:
                    source = log_key(source)
                self.kind, self.source = "log", source
            elif event == "log_dropped":  # the lines the launcher did not write, by source
                self.kind, self.counts = "log", {}
                for source, count in message["event"].get("sources", {}).items():
                    key = log_key(source) if log_key is not None else source
                    self.counts[key] = self.counts.get(key, 0) + count
            elif event == "time":
                self.kind = "time"
            elif event == "heartbeat":
                self.kind = "beat"
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
        self.without_check = False  # session messages without the summary's check (session_without_check)
        self.deferred: set = set()  # the items of put_deferred that wait in the queue
        # The run whose variables this screen was last sent (hello's summary, or a `variables` message): a `values`
        # message of another run would come before the screen knows the run it belongs to.
        self.variables_of: int | None = None

    def put(self, item: Outgoing) -> None:
        if self.items and (self.items[0] is RESYNC or self.items[-1] is CLOSE):
            return  # a resync sends everything again from the state at that time; a closing queue takes nothing
        self.items.append(item)
        self.bytes += item.size
        if len(self.items) > self.limit or self.bytes > self.max_bytes:
            self._shed()
        self.ready.set()

    def put_deferred(self, item) -> None:
        """Queues an item whose text is made when it is sent (item.render() -> str or None for nothing; item.size
        is 0). An item that already waits is not queued again: it will send what is latest then."""
        if item in self.deferred or (self.items and (self.items[0] is RESYNC or self.items[-1] is CLOSE)):
            return
        self.deferred.add(item)
        self.items.append(item)
        self.ready.set()

    def drop_deferred(self, item) -> None:
        """Takes an item of put_deferred out of the queue (a watch that has ended): it does not wait there for a
        screen that does not read."""
        if item in self.deferred:
            self.deferred.discard(item)
            try:
                self.items.remove(item)
            except ValueError:
                pass

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
            if item.kind in ("time", "beat"):
                continue
            notice = notices.get(item.session)
            if notice is None:
                notice = notices[item.session] = _Dropped(item.session)
                kept.append(notice)  # where the first dropped message was
            if item.kind == "log":
                for source, count in (item.counts if item.counts is not None else {item.source: 1}).items():
                    key = source if isinstance(source, str) else "*"
                    if key not in notice.log and len(notice.log) >= DROPPED_SOURCES:
                        key = "*"
                    notice.log[key] = notice.log.get(key, 0) + count
            else:
                notice.events += 1
        self.items = kept
        self.bytes = sum(item.size for item in kept if not isinstance(item, _Marker))
        waiting = len(kept) - len(self.deferred)  # the deferred ones are few, hold nothing, and are never dropped
        if waiting > 1 and (waiting > self.limit or self.bytes > self.max_bytes):
            self.items = deque([RESYNC])
            self.deferred.clear()
            self.bytes = 0
            self.resyncs += 1

    def close(self) -> None:
        self.deferred.clear()
        self.items = deque([CLOSE])
        self.bytes = 0
        self.ready.set()

    async def next(self) -> object:
        while not self.items:
            self.ready.clear()
            await self.ready.wait()
        item = self.items.popleft()
        self.deferred.discard(item)
        if item is RESYNC:  # what is still queued is in the snapshot taken next
            self.items.clear()
            self.deferred.clear()
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
        def told(messages: list[dict]) -> list[str]:
            for m in messages:
                if m.get("type") == "hello":
                    client.variables_of = m["session"]["id"] if isinstance(m.get("session"), dict) else None
            return [encode(m) for m in messages]

        try:
            pending = told(self.snapshot(session, from_seq))
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
                    pending += told(self.snapshot(None, None))
                elif isinstance(item, Outgoing):
                    if item.variables_of is not None:
                        client.variables_of = item.variables_of
                    pending = [item.text_without_check if client.without_check else item.text]
                elif isinstance(item, _Dropped):
                    pending = [item.text]
                else:  # deferred: made now
                    try:
                        text = item.render()
                    except Exception:
                        log.exception("a message for a screen could not be made")
                        text = None
                    pending = [] if text is None else [text]
        finally:
            self.clients.discard(client)
