"""The ECUs' variables of one run, as the screens see them: the schema, the last values, the writes and their results.

The values come from a source (source.Source: the launcher's control server over gRPC, or a stand-in of the
tests), which calls the on_* methods here on the event loop. Nothing here does I/O of its own, and nothing here
grows without a limit:

- the schema is kept up to Limits.peers and Limits.variables (what is left out is counted in the summary);
- of the values only the last sample of each peer is kept (a history is the screen's business), and they are
  read from the source only for the peers that a screen watches, as often as it asks (refresh);
- of the operations (the writes and their results) the last Limits.operations are kept; every change of one is
  sent to the screens, kept or not. The summary has the open ones that are kept and the last few, and says from
  which number on it has them all (operations_from_seq) and how many open ones are not kept
  (operations_open_omitted): a screen that connects again can tell a result it missed from one still to come.

The values and their time are read together (Source.read_snapshot: the control server's GetSnapshot), and only
such a read says of which moment values are. Each peer remembers the read its values are from (_Read). A `values`
message is made when it is sent (values_message), from the state at that moment, in one step of the event loop.
Its `paused` is true when every peer of the watch was last read standing still at one and the same time, and every
read since found it standing still at that time too: then the source's promise holds for each of them (every step
that began before `sim_ms` is in the values, none after it). Peers read by different calls are put together only
so: each call's promise is about its own peers and the time P it names, and what is true of each peer for the same
P is true of them together. Only a read makes `paused` true (there is no second look at the time and no trying
again); what ends it without a read is a command passed on, a lost connection, and news of the time that it has
gone past where it stood or has ended (the time does not go back, so that news is never too old to be true).
"""

from __future__ import annotations

import asyncio
import logging
import math
from collections.abc import Callable
from dataclasses import dataclass

from .source import LATE_POLICIES, OperationInfo, PeerInfo, Sample, SnapshotInfo, Source, SourceError, TimeInfo

OFF, CONNECTING, READY, FAILED, ENDED = "off", "connecting", "ready", "failed", "ended"
NS_PER_MS = 1_000_000
INT64_MIN, INT64_MAX = -2 ** 63, 2 ** 63 - 1
EXACT = 2 ** 53  # whole numbers that a JSON number carries exactly
AT_MS_MAX = 10 ** 12  # the latest time a write may name (as a step command's milliseconds)
TYPES = ("bool", "integer", "real", "enum")
ROLES = ("input", "output", "parameter")
FINAL = frozenset({"applied", "late", "rejected", "cancelled", "invalid", "busy", "unknown"})
WRITE_KEYS = ("session", "writes", "at_ms", "late")
FORGOTTEN_OPEN = 100_000  # numbers kept of open operations that were forgotten (they get their end with the run)
STANDING_READ_S = 0.25  # while it stands still with every value read, the watched peers are read this often
READ_FAILURES = 3  # reads in a row that gave nothing, before the summary says so (state ready, with a reason)

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
    operations_in_summary: int = 32  # the last ones, for a screen opened later (the open ones are all in it)
    watches: int = 16  # of one connection
    watch_variables: int = 2000  # named in one watch ("all" is not counted)
    period_ms_min: int = 20
    period_ms_max: int = 60_000
    writes_per_request: int = 256
    writes_waiting: int = 32  # api/write requests waiting for the one that is being sent
    write_timeout_s: float = 5.0  # for the source to take a write (and for a request to wait for its turn)
    read_timeout_s: float = 2.0  # one read of the time and the values, or of an operation


class Rejected(Exception):
    """A request that the variables refuse: an HTTP status, a code of the protocol, a sentence."""

    def __init__(self, status: int, code: str, message: str, detail: object = None):
        super().__init__(message)
        self.status, self.code, self.detail = status, code, detail


def cut(text: object, limit: int) -> str:
    text = text if isinstance(text, str) else ""
    return text if len(text) <= limit else text[:limit] + "..."


@dataclass(frozen=True)
class _Read:
    """A read of the source (Source.read_snapshot) as the peers read by it remember it."""
    now_ns: int  # the time it gave (the values read are not older)
    standing: bool  # it stood still at now_ns, and the values read are those of that moment
    epoch: int  # Variables._epoch then: `standing` counts as long as that has not changed
    at: float  # when it was taken (the event loop's clock)


class _Peer:
    __slots__ = ("name", "incarnation", "connected", "variables", "index", "types", "slots", "sample", "decoded",
                 "version", "read")

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
        self.read: _Read | None = None  # the read that `sample` (or its absence) is from (None: not from a read)

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
        self.time: TimeInfo | None = None  # as the source last told by itself (the values' time comes with a read)
        self.schema_version = 0
        self.state_version = 0  # counts the changes of the state
        self._epoch = 0  # grows when a standstill that was read may be over (see _Read.epoch)
        self._last: tuple[bool, int] | None = None  # the last read: (standing, the time)
        self._per_call: int | None = None  # at most this many peers in one call (None: any): more did not fit
        self._too_large: set[str] = set()  # peers whose values no answer can hold: not read (until they change)
        self._read_failures = 0  # reads in a row that gave nothing
        self.was_ready = False
        self._schema_timer: asyncio.TimerHandle | None = None
        self._reconciling: asyncio.Task | None = None
        self._last_read: asyncio.Task | None = None
        self._lost: str | None = None  # why the connection was last lost
        self._since_lost: set[str] | None = None  # the peers told since the connection came back (None: not lost)
        self._forgotten_open: set[int] = set()  # open operations that _keep_operation had no room for
        self._forgotten_max = -1  # the highest number of an operation that is not kept any more
        self._reading = asyncio.Lock()  # one read at a time: they are taken in the order the source answered
        self._news = False  # the time may have changed since the last read: the next one is not put off
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
        # the operations: every open one that is kept, and the last ones. Every operation from `from_seq` on is
        # in the list; one that is not in it and has a lower number is done, unless open ones had to be forgotten
        first_recent = len(self.operations) - self.limits.operations_in_summary
        listed, left_out = [], self._forgotten_max
        for at, entry in enumerate(self.operations.values()):
            if at >= first_recent or not entry["done"]:
                listed.append(entry)
            else:
                left_out = max(left_out, entry["seq"])
        return {"state": self.state, "reason": self.reason,
                "peers": [{"name": p.name, "connected": p.connected, "variables": p.variables}
                          for p in self.peers.values()],
                "peers_omitted": self.peers_omitted, "variables_omitted": self.variables_omitted,
                "operations": listed, "operations_from_seq": left_out + 1,
                "operations_open_omitted": len(self._forgotten_open)}

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
        self._per_call = None  # other peers or variables: what fits one answer is found again
        for name in removed:
            self.peers.pop(name, None)
            self._too_large.discard(name)
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
                peer.sample, peer.decoded, peer.read = None, None, None
                peer.version += 1
                self._too_large.discard(peer.name)
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
        if before != (peer.types, [v["name"] if v is not None else None for v in peer.slots]):
            self._too_large.discard(peer.name)
            if peer.sample is not None:  # other variables under the same incarnation: read with the old ones
                peer.sample, peer.read = None, None
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
        """Samples the source tells by itself (the control server is not asked to). They are the peers' latest
        values, but of no read: nothing is said about their moment until the peer is read."""
        for name, sample in samples.items():
            peer = self._take_sample(name, sample)
            if peer is not None:
                peer.read = None

    def _take_sample(self, name: str, sample: Sample) -> _Peer | None:
        """The peer whose latest sample this now is (None: not taken)."""
        peer = self.peers.get(name)
        if peer is None or sample.incarnation != peer.incarnation or sample.count != len(peer.types):
            return None  # of a peer or a schema not known (yet): the next one fits
        if peer.sample is not None and sample.sequence <= peer.sample.sequence:
            return None
        peer.sample, peer.decoded = sample, None
        peer.version += 1
        return peer

    # ---- reading from the source. Everything that asks the source for values or for their time goes through
    # refresh(), one read at a time.

    async def refresh(self, names: set[str] | None = None) -> None:
        """Reads the time and the last sample of the peers named (None: all) from the source. A read that fails
        changes nothing (READ_FAILURES in a row: the summary says why the values are old). While they stand still
        with their values read, at most every STANDING_READ_S (only the step it stands at can still come), unless
        there is news of the time or a command was passed on."""
        source = self.source
        if self._closed or self.state != READY or source is None:
            return
        peers = [p for p in self.peers.values() if (names is None or p.name in names) and p.name not in self._too_large]
        recent = asyncio.get_running_loop().time() - STANDING_READ_S
        if not peers or (not self._news and all(self._stands(p.read) and p.read.at > recent for p in peers)):
            return
        async with self._reading:
            if self._closed or self.source is not source or self.state != READY:
                return
            self._news = False
            epoch = self._epoch
            try:
                read = await self._read(source, [p.name for p in peers if p.name not in self._too_large])
            except Exception:  # a source that breaks its word (SourceError only): not the end of reading
                log.exception("reading the variables")
                return
            if self._closed or self.source is not source or self.state != READY:
                return
            time, samples, standing, error = read
            if time is not None:
                # A standstill that was called over while this was read (a command, news of the time) is not
                # called standing by an answer that may be from before that.
                self._take_read(time, samples, standing and epoch == self._epoch)
            self._count_read(time is not None, error)

    async def _read(self, source: Source, names: list[str]) \
            -> tuple[TimeInfo | None, dict[str, Sample | None], bool, SourceError | None]:
        """One read: the time, the samples of the peers that could be read, whether these are the values of the
        moment it stands still at, and an error if a call failed. The time is None when nothing could be read.
        One call, unless the source says that the values do not fit one answer: then the peers of that call are
        divided in two and asked for again (and the next reads begin with calls of that size). With several
        calls the time is the earliest (no value is older), and it stands still if every call says so for the
        same time (each call's promise is about its own peers; see the top of this file)."""
        timeout = self.limits.read_timeout_s
        size = self._per_call or len(names)
        pending = [names[i:i + size] for i in range(0, len(names), size)]
        good: list[SnapshotInfo] = []
        error: SourceError | None = None
        while pending:
            answers = await asyncio.gather(*(source.read_snapshot(group, timeout) for group in pending),
                                           return_exceptions=True)
            again: list[list[str]] = []
            for group, answer in zip(pending, answers):
                if isinstance(answer, SnapshotInfo):
                    good.append(answer)
                elif not isinstance(answer, SourceError):
                    raise answer
                elif answer.kind != "too-large":
                    error = answer
                elif len(group) > 1:
                    half = len(group) // 2
                    again += [group[:half], group[half:]]
                    if self._per_call is None:
                        log.warning("the values of the watched ECUs do not fit one answer of the control server: "
                                    "they are read with several calls")
                    self._per_call = min(self._per_call or half, half)
                else:
                    self._too_large.add(group[0])
                    log.warning("the values of %s do not fit one answer of the control server: they are not read",
                                group[0])
            pending = again
        if not good:
            return None, {}, False, error
        samples: dict[str, Sample | None] = {}
        for answer in good:
            samples.update(answer.samples)
        time = min((a.time for a in good), key=lambda time: time.now_ns)
        return time, samples, all(a.consistent and a.time.now_ns == time.now_ns for a in good), error

    def _count_read(self, ok: bool, error: SourceError | None) -> None:
        """The values that stop coming do so with a reason: after READ_FAILURES reads in a row that gave nothing,
        the summary's reason says why (the state stays ready: the rest works), until a read gives values again."""
        if ok:
            self._read_failures = 0
            if self.reason is not None:
                self._set_state(READY, None)
        elif error is not None:
            self._read_failures += 1
            if self._read_failures == READ_FAILURES:
                self._set_state(READY, f"the values cannot be read from the control server ({error.kind}: {error}): "
                                       "those shown are the last that were read; it is tried again")

    def _take_read(self, time: TimeInfo, samples: dict[str, Sample | None], standing: bool) -> None:
        if (standing, time.now_ns) != self._last:
            self._last = (standing, time.now_ns)
            self._epoch += 1  # it moved, or stands elsewhere: what was read standing before is not "now" any more
        read = _Read(time.now_ns, standing, self._epoch, asyncio.get_running_loop().time())
        for name, sample in samples.items():
            peer = self.peers.get(name)
            if peer is None:
                continue
            if sample is not None:
                self._take_sample(name, sample)
            # The read speaks for the peer only if what is kept is what was read (not when the sample was not
            # taken: another incarnation or schema than the one known here).
            mine = peer.sample
            same = mine is None if sample is None else mine is not None and \
                (mine.incarnation, mine.sequence) == (sample.incarnation, sample.sequence)
            peer.read = read if same else None

    def _stands(self, read: _Read | None) -> bool:
        return read is not None and read.standing and read.epoch == self._epoch

    def read_last(self) -> None:
        """The simulation has ended, and the source may close soon: reads every peer once more, so that the
        values kept after the run are its last ones (if the source is still there)."""
        if self._last_read is None and not self._closed and self.state == READY:
            self._news = True
            self._last_read = asyncio.create_task(self.refresh())

    def peer_names(self, watch: Watch) -> list[str]:
        return [peer.name for peer, _ in self._plan(watch)]

    def on_time(self, time: TimeInfo) -> None:
        """News of the time. What a `values` message says comes from a read; the news makes the next one be read
        without waiting (and is the time shown while there is no read). News that it has gone past the time it
        was last read standing at, or has ended, ends `paused` whether or not the next read works: the time does
        not go back, so such news is not older than that read. (News of that very time, not paused, may be older,
        and says nothing.)"""
        if time == self.time:
            return
        self.time = time
        self._news = True
        if self._last is not None and self._last[0] and (time.ended or time.now_ns > self._last[1]):
            self._last = None
            self._epoch += 1
        if time.ended:
            self.read_last()

    def unsettle(self) -> None:
        """A command that may end the standstill is on its way (this server passed it on): `paused` is not said
        until the peers are read again, which the next watch that is due does without waiting. (If the launcher
        has not taken the command by then, the read finds it standing where it stood, which is true.)"""
        self._epoch += 1
        self._news = True

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
            self._forgotten_max = max(self._forgotten_max, done)
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
        self._epoch += 1
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
        for task in (self._reconciling, self._last_read):
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
        self._epoch += 1
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
        return (self.session_id, self.state_version, self._moment(plan), sum(peer.version for peer, _ in plan))

    def _moment(self, plan: list[tuple[_Peer, list[str] | None]]) -> tuple[bool, int | None]:
        """(`paused`, `sim_ms`) of a message with these peers: see the top of this file."""
        reads = [peer.read for peer, _ in plan]
        times = {read.now_ns for read in reads if read is not None}
        paused = self.state == READY and len(times) == 1 and all(self._stands(read) for read in reads)
        # Not paused: the time of the (earliest) read, which the values are not older than; without a read, what
        # the source last told.
        now_ns = min(times) if times else self.time.now_ns if self.time is not None else None
        return paused, None if now_ns is None else now_ns // NS_PER_MS

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
        paused, sim_ms = self._moment(self._plan(watch))
        watch.sent = self.stamp(watch)
        return {"type": "values", "id": watch.id, "session": self.session_id, "sim_ms": sim_ms, "paused": paused,
                "peers": peers}

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
        if seq not in self.operations:
            # taken while the run ended: close() has given the open ones their end already, so this one gets it here
            # (a screen that waits for the result of its write is told that none will come)
            state, reason = ("unknown", "the run ended before the result came") if self._closed else ("sent", None)
            entry = {"seq": seq, "state": state, "done": self._closed,
                     "peers": {peer: {"state": state, "applied_at_ms": None, "late": False, "reason": reason}
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
