"""The ECUs' variables of one run, as the screens see them: the schema, the last values, the writes and their results.

The values come from a source (source.Source: the launcher's control server over gRPC, or a stand-in of the
tests), which calls the on_* methods here on the event loop. Nothing here does I/O of its own, and nothing here
grows without a limit:

- the schema is kept up to Limits.peers and Limits.variables (what is left out is counted in the summary);
- of the values only the last sample of each peer is kept (a history is the screen's business), and they are
  read from the source only for the peers that a screen watches, as often as it asks (refresh);
- of the operations (the writes and their results) the last Limits.operations are kept; every change of one is
  sent to the screens, kept or not.

A `values` message is made when it is sent (values_message), from the state at that moment, in one step of the
event loop: its `paused`, `sim_ms` and values belong together. `paused` is true only after the time and the values
were read again from the source while it stood still at one time (see _settle): then every value of a step that
began before `sim_ms` is in the message. A sample of a later step ends that at once.
"""

from __future__ import annotations

import asyncio
import logging
import math
from collections.abc import Callable
from dataclasses import dataclass

from .source import LATE_POLICIES, OperationInfo, PeerInfo, Sample, Source, SourceError, TimeInfo

OFF, CONNECTING, READY, FAILED, ENDED = "off", "connecting", "ready", "failed", "ended"
NS_PER_MS = 1_000_000
INT64_MIN, INT64_MAX = -2 ** 63, 2 ** 63 - 1
EXACT = 2 ** 53  # whole numbers that a JSON number carries exactly
AT_MS_MAX = 10 ** 12  # the latest time a write may name (as a step command's milliseconds)
TYPES = ("bool", "integer", "real", "enum")
ROLES = ("input", "output", "parameter")
FINAL = frozenset({"applied", "late", "rejected", "cancelled", "invalid", "busy", "unknown"})
WRITE_KEYS = ("writes", "at_ms", "late")
SETTLE_RETRY_S = 0.2  # between tries to read a standstill, after the first SETTLE_TRIES in a row
COMMAND_SETTLE_S = 0.3  # after a time command was passed on, before the standstill is read again by itself
READS_AT_ONCE = 32  # calls to the source for values that wait for their answer at one time
FORGOTTEN_OPEN = 100_000  # numbers kept of open operations that were forgotten (they get their end with the run)
STANDING_READ_S = 0.25  # while it stands still with every value read, the watched peers are read this often
SETTLE_TRIES = 3

log = logging.getLogger("vehicle_sil_gui")


@dataclass(frozen=True)
class Limits:
    peers: int = 256
    variables: int = 10_000  # of all peers together; the others are counted in variables_omitted
    enum_items: int = 256  # of one variable; a variable with more is left out
    name_chars: int = 256  # a peer's or a variable's name; one with a longer name is left out
    text_chars: int = 256  # a unit, a description, an enum item's name: cut
    reason_chars: int = 500  # a reason from the source or from an ECU: cut
    operations: int = 256  # kept for the server's own use (results that were dropped, the end of the run)
    operations_in_summary: int = 32  # the last ones, for a screen opened later
    watches: int = 16  # of one connection
    watch_variables: int = 2000  # named in one watch ("all" is not counted)
    period_ms_min: int = 20
    period_ms_max: int = 60_000
    writes_per_request: int = 256
    writes_waiting: int = 32  # api/write requests waiting for the one that is being sent
    write_timeout_s: float = 5.0  # for the source to take a write (and for a request to wait for its turn)
    read_timeout_s: float = 2.0  # one read of the time, of a peer's values or of an operation


class Rejected(Exception):
    """A request that the variables refuse: an HTTP status, a code of the protocol, a sentence."""

    def __init__(self, status: int, code: str, message: str, detail: object = None):
        super().__init__(message)
        self.status, self.code, self.detail = status, code, detail


def cut(text: object, limit: int) -> str:
    text = text if isinstance(text, str) else ""
    return text if len(text) <= limit else text[:limit] + "..."


class _Peer:
    __slots__ = ("name", "incarnation", "connected", "variables", "index", "types", "slots", "sample", "decoded",
                 "version")

    def __init__(self, name: str):
        self.name = name
        self.incarnation = ""
        self.connected = False
        self.variables: list[dict] = []  # as the summary shows them
        self.index: dict[str, dict] = {}  # by name
        self.types: list[str] = []  # of every variable the source has for it, in the source's order
        self.slots: list[dict | None] = []  # the summary's variable of each of those (None: left out)
        self.sample: Sample | None = None
        self.decoded: dict[str, object] | None = None  # the sample's values by name (made when first asked for)
        self.version = 0  # counts the samples taken

    def values(self) -> dict[str, object]:
        if self.decoded is None:
            raw = self.sample.decode(self.types) if self.sample is not None else None
            self.decoded = {} if raw is None else {v["name"]: x for v, x in zip(self.slots, raw) if v is not None}
        return self.decoded


class Watch:
    """One watch of one connection: the variables it names, how often, and what it was last sent. Its `values`
    message is made when the connection's queue comes to it (hub.Client.put_deferred), so a screen that reads
    slowly gets the latest values each time, and nothing waits for it in between."""

    size = 0  # in a queue it holds no text

    def __init__(self, id: int, wanted: list[tuple[str, str]] | None, period_s: float, now: float,
                 render: Callable[["Watch"], str | None]):
        self.id = id
        self.wanted = wanted  # None: all
        self.period_s = period_s
        self.due = now
        self.sent: object = None  # the stamp of what was sent last
        self.plan: tuple[object, list[tuple[_Peer, list[str] | None]]] | None = None
        self.cancelled = False
        self._render = render

    def render(self) -> str | None:
        return None if self.cancelled else self._render(self)


def check_watch(message: dict, limits: Limits) -> tuple[int, list[tuple[str, str]] | None, float]:
    """A `watch` message of a screen: (id, the variables or None for all, the period in seconds). Rejected
    (invalid-watch) with what to send instead."""
    def bad(what: str) -> Rejected:
        return Rejected(400, "invalid-watch", what + ': send {"type":"watch","id":1,"variables":"all","period_ms":100}',
                        {"id": id if _is_count(id) else None})

    id = message.get("id")
    if not _is_count(id):
        raise bad("a watch needs id, a whole number from 0 to 2^53 that names it on this connection")
    period = message.get("period_ms", limits.period_ms_min)
    if isinstance(period, bool) or not isinstance(period, int) or \
            not limits.period_ms_min <= period <= limits.period_ms_max:
        raise bad(f"period_ms must be a whole number of milliseconds from {limits.period_ms_min} to "
                  f"{limits.period_ms_max}")
    variables = message.get("variables")
    if variables == "all":
        return id, None, period / 1000
    if not isinstance(variables, list) or not all(
            isinstance(v, dict) and isinstance(v.get("peer"), str) and isinstance(v.get("name"), str)
            for v in variables):
        raise bad('variables must be "all" or a list of {"peer": <name>, "name": <name>}')
    if len(variables) > limits.watch_variables:
        raise bad(f"a watch names at most {limits.watch_variables} variables (this one {len(variables)}): watch "
                  '"all", or use more watches')
    wanted = list(dict.fromkeys((v["peer"], v["name"]) for v in variables))
    return id, wanted, period / 1000


def _is_count(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= EXACT


class Variables:
    def __init__(self, session_id: int, emit: Callable[[list[dict]], object], *, limits: Limits | None = None,
                 ending: Callable[[], bool] = lambda: False, secret: str | None = None):
        """emit(messages) sends messages to every screen. ending() says whether the run is ending (then a lost
        connection is the end, not a failure). secret: never shown in a reason (the source's token)."""
        self.session_id = session_id
        self.limits = limits or Limits()
        self._emit = emit
        self._ending = ending
        self._secret = secret
        self.state = OFF
        self.reason: str | None = None
        self.source: Source | None = None
        self.peers: dict[str, _Peer] = {}
        self.variables_omitted = 0
        self.peers_omitted = 0
        self._seen: dict[str, int] = {}  # how many variables the source has for each peer it named
        self.operations: dict[int, dict] = {}  # by seq, in the order they were first seen
        self.time: TimeInfo | None = None
        self.settled_ns: int | None = None  # standing still at this time, with every earlier step's values read
        self.schema_version = 0
        self.state_version = 0  # counts the changes of the time, of `paused` and of the state
        self._time_changes = 0  # counts the times the source told by itself
        self.was_ready = False
        self._schema_timer: asyncio.TimerHandle | None = None
        self._settling: asyncio.Task | None = None
        self._reconciling: asyncio.Task | None = None
        self._last_read: asyncio.Task | None = None
        self._lost: str | None = None  # why the connection was last lost
        self._since_lost: set[str] | None = None  # the peers told since the connection came back (None: not lost)
        self._forgotten_open: set[int] = set()  # open operations that _keep_operation had no room for
        self._reading = asyncio.Semaphore(READS_AT_ONCE)
        self._refreshed_at = 0.0
        self._hold_until = 0.0  # after a command was passed on: no reading of the standstill before this time
        self._writing = asyncio.Lock()
        self._writes_waiting = 0
        self._closed = False

    # ------------------------------------------------------------ the state

    def set_off(self, reason: str) -> None:
        """Before the run is told to the screens: it has no variables (why)."""
        self._set_state(OFF, reason, tell=False)

    def expect(self) -> None:
        """Before the run is told to the screens: the launcher was started with a control server, whose address
        comes as an event."""
        self._set_state(CONNECTING, None, tell=False)

    def connect(self, source: Source) -> None:
        self.source = source
        source.start(self)

    def fail(self, reason: str) -> None:
        self._set_state(FAILED, reason)

    def _set_state(self, state: str, reason: str | None, tell: bool = True) -> None:
        reason = self._clean(reason) if reason is not None else None
        if (state, reason) == (self.state, self.reason):
            return
        self.state, self.reason = state, reason
        self.was_ready = self.was_ready or state == READY
        self.state_version += 1
        if tell:
            self._tell_schema()

    def _clean(self, text: str) -> str:
        if self._secret:
            text = text.replace(self._secret, "<token>")
        return cut(text, self.limits.reason_chars)

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def has_values(self) -> bool:
        return self.state in (READY, ENDED)

    def summary(self) -> dict:
        kept = list(self.operations.values())[-self.limits.operations_in_summary:]
        return {"state": self.state, "reason": self.reason,
                "peers": [{"name": p.name, "connected": p.connected, "variables": p.variables}
                          for p in self.peers.values()],
                "peers_omitted": self.peers_omitted, "variables_omitted": self.variables_omitted,
                "operations": kept}

    def message(self) -> dict:
        return {"type": "variables", "session": self.session_id, "variables": self.summary()}

    def _tell_schema(self, soon: bool = False) -> None:
        """Sends the summary to the screens: now, or (soon) once for the changes of a short time (peers come one
        by one, and the summary of many variables is large)."""
        if self._schema_timer is not None:
            if soon:
                return
            self._schema_timer.cancel()
            self._schema_timer = None
        if soon and not self._closed:
            self._schema_timer = asyncio.get_running_loop().call_later(0.05, self._tell_schema)
            return
        self._emit([self.message()])

    # ------------------------------------------------------------ from the source (on the event loop)

    def on_peers(self, peers: list[PeerInfo], removed: list[str]) -> None:
        for name in removed:
            self.peers.pop(name, None)
            if self._since_lost is not None:
                self._since_lost.discard(name)
        for info in peers:
            if not isinstance(info.name, str) or not 0 < len(info.name) <= self.limits.name_chars:
                continue
            peer = self.peers.get(info.name)
            if peer is None:
                if len(self.peers) >= self.limits.peers:
                    continue
                peer = self.peers[info.name] = _Peer(info.name)
            if peer.incarnation != info.incarnation:  # another process under the name: its values are not these
                peer.sample, peer.decoded = None, None
                peer.version += 1
            peer.incarnation, peer.connected = info.incarnation, info.connected
            self._take_schema(peer, info.variables)
            if self._since_lost is not None:
                self._since_lost.add(info.name)
        self._count_omitted(peers, removed)
        self.schema_version += 1
        if self.state in (READY, ENDED):
            self._tell_schema(soon=True)

    def _count_omitted(self, peers: list[PeerInfo], removed: list[str]) -> None:
        for name in removed:
            self._seen.pop(name, None)
        for info in peers:
            self._seen[info.name] = len(info.variables)
        self.peers_omitted = sum(1 for name in self._seen if name not in self.peers)
        self.variables_omitted = sum(n for name, n in self._seen.items() if name not in self.peers) + \
            sum(self._seen.get(p.name, 0) - len(p.variables) for p in self.peers.values())

    def _take_schema(self, peer: _Peer, variables: list[dict]) -> None:
        room = self.limits.variables - sum(len(p.variables) for p in self.peers.values() if p is not peer)
        before = (peer.types, [v["name"] if v is not None else None for v in peer.slots])
        peer.variables, peer.index, peer.types, peer.slots = [], {}, [], []
        peer.decoded = None
        for v in variables:
            entry = self._variable(v) if len(peer.variables) < room else None
            if entry is not None and entry["name"] in peer.index:
                entry = None
            peer.types.append(v.get("type") if v.get("type") in TYPES else "")
            peer.slots.append(entry)
            if entry is not None:
                peer.variables.append(entry)
                peer.index[entry["name"]] = entry
        if peer.sample is not None and before != (peer.types, [v["name"] if v is not None else None
                                                               for v in peer.slots]):
            # Other variables under the same incarnation: the sample was read with the old ones.
            peer.sample = None
            peer.version += 1

    def _variable(self, v: dict) -> dict | None:
        name, type_, role, items = v.get("name"), v.get("type"), v.get("role"), v.get("enum") or []
        if not isinstance(name, str) or not 0 < len(name) <= self.limits.name_chars or type_ not in TYPES or \
                len(items) > self.limits.enum_items:
            return None
        n = self.limits.text_chars
        return {"name": name, "type": type_, "role": role if role in ROLES else "output",
                "writable": role in ("input", "parameter"), "unit": cut(v.get("unit"), n),
                "min": _finite_or_none(v.get("min")), "max": _finite_or_none(v.get("max")),
                "step": _finite_or_none(v.get("step")), "description": cut(v.get("description"), n),
                "enum": [{"name": cut(i[0], n), "value": i[1]} for i in items] if type_ == "enum" else None}

    def on_samples(self, samples: dict[str, Sample]) -> None:
        for name, sample in samples.items():
            self._take_sample(name, sample)

    def _take_sample(self, name: str, sample: Sample, settling: bool = False) -> bool:
        peer = self.peers.get(name)
        if peer is None or sample.incarnation != peer.incarnation or sample.count != len(peer.types):
            return False  # of a peer or a schema not known (yet): the next one fits
        if peer.sample is not None and sample.sequence <= peer.sample.sequence:
            return False
        peer.sample, peer.decoded = sample, None
        peer.version += 1
        if self.settled_ns is not None and sample.time_ns > self.settled_ns:
            # A step after the time it stood still at: it runs again, whatever the time state last said.
            self.settled_ns = None
            self.state_version += 1
            if not settling:
                self._kick_settle()
        return True

    # ---- reading from the source. Everything that asks the source for values or for the time goes through the
    # next two methods: a source that can give several peers' values and the time of one moment in one call
    # replaces them (then _read_standing needs no second look at the time).

    async def _read_values(self, source: Source, names: list[str]) -> dict[str, Sample | None]:
        """The last sample of each peer named, as the source has them now (None: none yet; a peer whose read
        failed is left out). At most READS_AT_ONCE calls wait at one time."""
        async def one(name: str) -> tuple[str, object]:
            async with self._reading:
                try:
                    return name, await source.read_values(name, self.limits.read_timeout_s)
                except SourceError as e:
                    return name, e
        return {name: sample for name, sample in await asyncio.gather(*(one(n) for n in names))
                if not isinstance(sample, SourceError)}

    async def _read_standing(self, source: Source) -> tuple[TimeInfo, dict[str, Sample | None] | None]:
        """The time, and, if the source stands still, the values of every connected peer that belong to that
        standstill: (time, samples), or (time, None) when it does not stand still, or moved or could not be read
        completely while the values were read (the time is then the later one). SourceError when the time cannot
        be read. The source's promise makes the samples complete: standing still at P at both ends, no step ran in
        between, and what the source has then holds every step that began before P."""
        timeout = self.limits.read_timeout_s
        before = await source.read_time(timeout)
        if not before.paused:
            return before, None
        names = [p.name for p in self.peers.values() if p.connected]
        samples = await self._read_values(source, names)
        after = await source.read_time(timeout)
        if after != before or len(samples) != len(names):
            return after, None
        return before, samples

    async def refresh(self, names: set[str] | None = None) -> None:
        """Reads the last sample of the peers named (None: all) from the source, all at once. A read that
        fails leaves the peer's sample as it is. While it stands still with every value read, at most every
        STANDING_READ_S (only the step it stands at can still come)."""
        source = self.source
        if self._closed or self.state != READY or source is None:
            return
        now = asyncio.get_running_loop().time()
        if self.settled_ns is not None and now - self._refreshed_at < STANDING_READ_S:
            return
        self._refreshed_at = now
        peers = [p.name for p in self.peers.values() if p.connected and (names is None or p.name in names)]
        samples = await self._read_values(source, peers)
        if self._closed or self.source is not source:
            return
        for name, sample in samples.items():
            if sample is not None:
                self._take_sample(name, sample)

    def read_last(self) -> None:
        """The simulation has ended, and the source may close soon: reads every peer once more, so that the
        values kept after the run are its last ones (if the source is still there)."""
        if self._last_read is None and not self._closed and self.state == READY:
            self._refreshed_at = 0.0
            self._last_read = asyncio.create_task(self.refresh())

    def peer_names(self, watch: Watch) -> list[str]:
        return [peer.name for peer, _ in self._plan(watch)]

    def on_time(self, time: TimeInfo) -> None:
        if time == self.time:
            # Told again what is known. If it stands still and that is not confirmed yet, it is tried again.
            self._kick_settle()
            return
        self.time = time
        self._hold_until = 0.0  # news of the time: what a command did is known
        if time.ended:
            self.read_last()
        self._time_changes += 1
        self.state_version += 1
        if not time.paused or time.now_ns != self.settled_ns:
            self.settled_ns = None
        self._kick_settle()

    def unsettle(self) -> None:
        """A command that may end the standstill is on its way (this server passed it on): `paused` is not said
        until it is confirmed again. Without this, the values of the time it stood at would be called paused for
        the moment between the command and the news of the time. The standstill is not read again at once (the
        launcher may not have taken the command yet, and would still stand): the news of the time does it, or,
        if none comes (the command was refused), a read COMMAND_SETTLE_S later."""
        if self.settled_ns is not None:
            self.settled_ns = None
            self.state_version += 1
        loop = asyncio.get_running_loop()
        self._hold_until = loop.time() + COMMAND_SETTLE_S
        loop.call_later(COMMAND_SETTLE_S, self._kick_settle)

    def _kick_settle(self) -> None:
        if self._closed or self.source is None or self.state != READY or self.time is None or \
                not self.time.paused or self.settled_ns is not None:
            return
        if self._hold_until and asyncio.get_running_loop().time() < self._hold_until:
            return
        if self._settling is None or self._settling.done():
            self._settling = asyncio.create_task(self._settle())

    async def _settle(self) -> None:
        """As long as the source stands still and that is not confirmed: reads the standstill (_read_standing)
        and, when the values read belong to it, says `paused` for that time. The values that come by themselves
        cannot tell: the news of standing still may overtake the last of them. It does not give up while the
        time last known stands still: after SETTLE_TRIES tries in a row it tries every SETTLE_RETRY_S."""
        tries = 0
        while not self._closed and self.state == READY and self.time is not None and self.time.paused \
                and self.settled_ns is None:
            source, changes = self.source, self._time_changes
            try:
                time, samples = await self._read_standing(source)
            except SourceError:
                await asyncio.sleep(SETTLE_RETRY_S)  # slow, or lost (then on_lost has changed the state)
                continue
            except asyncio.CancelledError:
                raise
            except Exception:  # a source that breaks its word (SourceError only): not the end of trying
                log.exception("reading the variables while the simulation stands still")
                await asyncio.sleep(SETTLE_RETRY_S)
                continue
            if self._closed or self.state != READY or self.source is not source:
                return
            if self._time_changes != changes and self.time != time:
                continue  # the source told of another time while it was read: what was read may be older
            for name, sample in (samples or {}).items():
                if sample is not None:
                    self._take_sample(name, sample, settling=True)
            newer = any(p.sample is not None and p.sample.time_ns > time.now_ns for p in self.peers.values())
            if time != self.time:
                self.time = time
                self.state_version += 1
            if not time.paused:
                return
            if samples is not None and not newer:
                self.settled_ns = time.now_ns
                self.state_version += 1
                return
            tries += 1
            await asyncio.sleep(0 if tries < SETTLE_TRIES else SETTLE_RETRY_S)

    def on_operations(self, operations: list[OperationInfo]) -> None:
        messages = []
        for op in operations:
            entry = self._operation(op)
            if self.operations.get(op.seq) == entry:
                continue
            self._keep_operation(entry)
            messages.append({"type": "operation", "session": self.session_id, **entry})
        if messages:
            self._emit(messages)

    def _operation(self, op: OperationInfo) -> dict:
        peers = {}
        for t in op.targets:
            peers[t.peer] = {"state": t.status,
                             "applied_at_ms": None if t.applied_at_ns is None else t.applied_at_ns // NS_PER_MS,
                             "late": t.late, "reason": self._clean(t.reason) if t.reason else None}
        states = [p["state"] for p in peers.values()]
        if not op.done:
            state = "sent" if "sent" in states or not states else "queued"
        else:  # every peer the same: that; else mixed (the peers say which)
            state = "unknown" if not states else states[0] if len(set(states)) == 1 else "mixed"
        return {"seq": op.seq, "state": state, "done": op.done, "peers": peers}

    def _keep_operation(self, entry: dict) -> None:
        self.operations[entry["seq"]] = entry
        if entry["done"]:
            self._forgotten_open.discard(entry["seq"])
        if len(self.operations) > self.limits.operations:
            done = next((seq for seq, e in self.operations.items() if e["done"]), None)
            if done is None:  # only open ones: the oldest is forgotten, but for its number (it gets its end)
                done = next(iter(self.operations))
                if len(self._forgotten_open) < FORGOTTEN_OPEN:
                    self._forgotten_open.add(done)
            del self.operations[done]

    def on_synced(self) -> None:
        """The source has sent what there is now: the peers, their values, the time."""
        if self._closed:
            return
        again = self._since_lost is not None
        if again:  # connected again: a peer that was not told this time is gone
            gone = [name for name in self.peers if name not in self._since_lost]
            self._since_lost = None
            if gone:
                self.on_peers([], gone)
        self._set_state(READY, None)
        self._kick_settle()
        if again and any(not e["done"] for e in self.operations.values()):
            self._start_reconcile(peers=False)  # what became of the operations while it was not connected

    def on_dropped(self) -> None:
        """The source could not hold every change for this server: the peers and the operations still open are
        read again."""
        self._start_reconcile(peers=True)

    def _start_reconcile(self, peers: bool) -> None:
        if self._closed or (self._reconciling is not None and not self._reconciling.done()):
            return
        self._reconciling = asyncio.create_task(self._reconcile(peers))

    async def _reconcile(self, peers: bool) -> None:
        source, timeout = self.source, self.limits.read_timeout_s
        try:
            if peers and source is not None:
                known = await source.read_peers(timeout)
                if self._closed or self.source is not source or self.state != READY:
                    return
                names = {p.name for p in known}
                self.on_peers(known, [name for name in self.peers if name not in names])
            for seq in [seq for seq, e in self.operations.items() if not e["done"]]:
                if self._closed or self.source is not source or source is None:
                    return
                op = await source.read_operation(seq, timeout)
                if op is not None:
                    self.on_operations([op])
        except SourceError:
            return

    def on_lost(self, reason: str, fatal: bool) -> None:
        """The connection to the source ended. The source connects again by itself unless fatal."""
        if self._closed:
            return
        self.settled_ns = None
        self.state_version += 1
        self._since_lost = set()
        for peer in self.peers.values():
            peer.connected = False
        if (self.time is not None and self.time.ended) or self._ending():
            self._set_state(ENDED if self.was_ready else OFF,
                            None if self.was_ready else "the run ended before the variables were read")
        elif fatal:
            self._set_state(FAILED, reason)
        else:
            self._lost = self._clean(reason)
            self._set_state(CONNECTING, reason + ": connecting again")

    # ------------------------------------------------------------ the end of the run

    async def close(self) -> None:
        """The run is over: the source is closed, the operations still open get their end, the last state is
        told. The last values stay (a watch still gets them once)."""
        if self._closed:
            return
        self._closed = True
        for task in (self._settling, self._reconciling, self._last_read):
            if task is not None:
                task.cancel()
        if self._schema_timer is not None:
            self._schema_timer.cancel()
            self._schema_timer = None
        if self.source is not None:
            try:
                await self.source.close()
            except Exception:
                log.exception("closing the source of the variables")
        open_ones = [e for e in self.operations.values() if not e["done"]]
        for entry in open_ones:
            peers = {name: p if p["state"] in FINAL else
                     {**p, "state": "unknown", "reason": "the run ended before the result came"}
                     for name, p in entry["peers"].items()}
            states = {p["state"] for p in peers.values()}
            closed = {**entry, "done": True, "peers": peers,
                      "state": states.pop() if len(states) == 1 else "mixed" if states else "unknown"}
            self.operations[entry["seq"]] = closed
            self._emit([{"type": "operation", "session": self.session_id, **closed}])
        for seq in sorted(self._forgotten_open):  # open ones that were forgotten: their end, without the peers
            self._emit([{"type": "operation", "session": self.session_id, "seq": seq, "state": "unknown",
                         "done": True, "peers": {}}])
        self._forgotten_open.clear()
        self.settled_ns = None
        self.state_version += 1
        if self.state == OFF:
            return
        before = self.summary()
        for peer in self.peers.values():
            peer.connected = False
        if self.was_ready:
            lost = self.state not in (READY, ENDED)
            self._set_state(ENDED, "the connection to the run's control server was lost before the run ended: the "
                                   "values may be older than its end" if lost else None, tell=False)
        elif self.state == FAILED:
            pass  # it stays what went wrong
        elif self.source is None:
            self._set_state(OFF, "the run ended before its control server started", tell=False)
        else:
            self._set_state(FAILED, (self._lost + " until the run ended") if self._lost else
                            "the run ended before the variables were read", tell=False)
        if open_ones or self.summary() != before:
            self._emit([self.message()])

    # ------------------------------------------------------------ values for a watch

    def stamp(self, watch: Watch) -> object:
        """Changes when a `values` message for the watch would differ from the last one."""
        plan = self._plan(watch)
        return (self.session_id, self.state_version, sum(peer.version for peer, _ in plan))

    def _plan(self, watch: Watch) -> list[tuple[_Peer, list[str] | None]]:
        key = (self.session_id, self.schema_version)
        if watch.plan is None or watch.plan[0] != key:
            if watch.wanted is None:
                plan = [(p, None) for p in self.peers.values()]
            else:
                by_peer: dict[str, list[str]] = {}
                for peer, name in watch.wanted:
                    if peer in self.peers and name in self.peers[peer].index:
                        by_peer.setdefault(peer, []).append(name)
                plan = [(self.peers[peer], names) for peer, names in by_peer.items()]
            watch.plan = (key, plan)
        return watch.plan[1]

    def values_message(self, watch: Watch) -> dict:
        """The watch's variables as they are now, with the time they belong to. No await: one moment."""
        peers = {}
        for peer, names in self._plan(watch):
            if peer.sample is None:
                continue
            values = peer.values()
            peers[peer.name] = {"sample_ms": peer.sample.time_ns // NS_PER_MS,
                                "values": values if names is None else {n: values[n] for n in names if n in values}}
        paused = self.settled_ns is not None and self.state == READY
        now_ns = self.settled_ns if paused else self.time.now_ns if self.time is not None else None
        watch.sent = self.stamp(watch)
        return {"type": "values", "id": watch.id, "session": self.session_id,
                "sim_ms": None if now_ns is None else now_ns // NS_PER_MS, "paused": paused, "peers": peers}

    # ------------------------------------------------------------ writes

    def check_write(self, body: dict) -> tuple[list[tuple[str, str, object]], int | None, str]:
        """The body of POST api/write as what the source is given: (the writes, at in ns or None, late).
        Rejected with what is wrong and what to send."""
        extra = sorted(k for k in body if k not in WRITE_KEYS)
        if extra:
            raise Rejected(400, "invalid-field", f"a write takes no {', '.join(map(repr, extra[:5]))}: send "
                                                 '{"writes": {"<peer>": {"<variable>": <value>}}}')
        writes = body.get("writes")
        if not isinstance(writes, dict) or not writes or not all(isinstance(v, dict) and v for v in writes.values()):
            raise Rejected(400, "invalid-field", 'writes must be {"<peer>": {"<variable>": <value>}} with at least '
                                                 "one variable for each peer named")
        count = sum(len(v) for v in writes.values())
        if count > self.limits.writes_per_request:
            raise Rejected(400, "invalid-field", f"one request writes at most {self.limits.writes_per_request} "
                                                 f"variables (this one {count}): send more requests")
        at_ms = body.get("at_ms")
        if at_ms is not None and (isinstance(at_ms, bool) or not isinstance(at_ms, int) or not 0 <= at_ms <= AT_MS_MAX):
            raise Rejected(400, "invalid-field", f"at_ms must be a whole number of milliseconds of virtual time "
                                                 f"from 0 to {AT_MS_MAX}, or left out (the next step)")
        late = body.get("late", "reject")
        if late not in LATE_POLICIES:
            raise Rejected(400, "invalid-field", 'late must be "reject" (a write that comes after its time is not '
                                                 'made) or "apply" (it is made at the next step)')
        if self.state != READY or self.source is None:
            why = {OFF: "this run has no variables", CONNECTING: "the variables are not read yet: wait for the "
                   "state ready", ENDED: "the run has ended"}.get(self.state, "the variables cannot be reached")
            raise Rejected(409, "variables-not-ready", f"{why}{': ' + self.reason if self.reason else ''}")
        out = []
        for peer_name, variables in writes.items():
            peer = self.peers.get(peer_name)
            if peer is None:
                raise Rejected(400, "unknown-variable",
                               f"no peer named {cut(peer_name, 80)!r} has variables in this run: the peers are "
                               f"{', '.join(list(self.peers)[:20]) or 'none'}", {"peer": cut(peer_name, 256)})
            for name, value in variables.items():
                entry = peer.index.get(name)
                detail = {"peer": peer_name, "name": cut(name, 256)}
                if entry is None:
                    raise Rejected(400, "unknown-variable", f"{peer_name} has no variable {cut(name, 80)!r}: the "
                                                            "summary's variables.peers lists them", detail)
                if not entry["writable"]:
                    raise Rejected(400, "not-writable", f"{peer_name}'s {name} is an output: only inputs and "
                                                        "parameters can be written", detail)
                out.append((peer_name, name, _check_value(peer_name, entry, value, detail)))
        return out, None if at_ms is None else at_ms * NS_PER_MS, late

    async def write(self, writes: list[tuple[str, str, object]], at_ns: int | None, late: str) -> int:
        """Gives the source the write; the operation's number. One at a time, in the order asked: the source
        gets them in the order the requests came."""
        if self._writes_waiting >= self.limits.writes_waiting:
            raise Rejected(503, "write-busy", f"{self.limits.writes_waiting} writes are already waiting for the "
                                              "control server: wait for their answers, then write again")
        timeout = self.limits.write_timeout_s
        self._writes_waiting += 1
        try:
            try:
                async with asyncio.timeout(timeout):
                    await self._writing.acquire()
            except TimeoutError:
                raise Rejected(503, "write-busy", f"an earlier write has waited for the control server for more "
                                                  f"than {timeout:g} s: this one was not sent") from None
        finally:
            self._writes_waiting -= 1
        try:
            source = self.source
            if self._closed or self.state != READY or source is None:
                raise Rejected(409, "variables-not-ready", "the variables cannot be written any more (the run has "
                                                           "ended, or the connection to its control server is lost)")
            try:
                seq = await source.write(writes, at_ns, late, timeout)
            except SourceError as e:
                if e.kind == "timeout":
                    raise Rejected(504, "write-timeout",
                                   f"the control server did not answer the write in {timeout:g} s: it may or may "
                                   "not have been taken (it is not sent again)") from None
                if e.kind == "refused":
                    raise Rejected(400, "write-refused", f"the control server refused the write: {self._clean(str(e))}") \
                        from None
                raise Rejected(502, "write-failed", f"the write did not reach the control server: "
                                                    f"{self._clean(str(e))}") from None
        finally:
            self._writing.release()
        if seq not in self.operations and not self._closed:
            entry = {"seq": seq, "state": "sent", "done": False,
                     "peers": {peer: {"state": "sent", "applied_at_ms": None, "late": False, "reason": None}
                               for peer in dict.fromkeys(w[0] for w in writes)}}
            self._keep_operation(entry)
            self._emit([{"type": "operation", "session": self.session_id, **entry}])
        return seq


def _finite_or_none(value: object) -> float | int | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        return None
    return value


def _check_value(peer: str, entry: dict, value: object, detail: dict) -> bool | int | float:
    """A value of the request as the variable's type takes it. Compared before any conversion: a whole number of
    400 digits is out of range, not an overflow."""
    name, type_ = entry["name"], entry["type"]

    def bad(expected: str) -> Rejected:
        shown = cut(repr(value), 60)
        return Rejected(400, "invalid-field", f"{peer}'s {name} takes {expected}, not {shown}", detail)

    if type_ == "bool":
        if not isinstance(value, bool):
            raise bad("true or false")
        return value
    if type_ == "enum":
        items = entry["enum"] or []
        listed = ", ".join(f"{i['name']} ({i['value']})" for i in items[:20])
        if isinstance(value, str):
            for item in items:
                if item["name"] == value:
                    return item["value"]
            raise bad(f"one of {listed}")
        if isinstance(value, bool) or not isinstance(value, int) or value not in [i["value"] for i in items]:
            raise bad(f"one of {listed} (the name or the number)")
        return value
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise bad("a number")
    if isinstance(value, float) and not math.isfinite(value):
        raise bad("a finite number")
    low, high = entry["min"], entry["max"]
    if type_ == "integer":
        if isinstance(value, float):
            if value != int(value) or abs(value) > EXACT:
                raise bad("a whole number")
            value = int(value)
        if not INT64_MIN <= value <= INT64_MAX:
            raise bad("a whole number of 64 bits")
    elif isinstance(value, int) and abs(value) > 2 ** 1023:
        raise bad("a number a double can hold")
    if (low is not None and value < low) or (high is not None and value > high):
        raise bad(f"a number from {'-inf' if low is None else format(low, 'g')} to "
                  f"{'inf' if high is None else format(high, 'g')}")
    return value if type_ == "integer" else float(value)
