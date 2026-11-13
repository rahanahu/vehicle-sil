"""Where the variables of a run come from, as the GUI server sees it: the types, and what a source must do.

The source of a real run is the launcher's control server (control.GrpcSource). The tests put a stand-in in its
place (server.Options.source). A source owns its threads and connections; everything it tells (the on_* methods
of its sink) it tells on the event loop that called start(), and its reads and writes are coroutines that never
block that loop and end within their timeout.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Protocol

LATE_POLICIES = ("reject", "apply")  # a write that comes after its time: not made, or made at the next step


class SourceError(Exception):
    """A read or a write did not work. kind: "timeout" (no answer in time: a write may have been taken),
    "refused" (the source said no: the message says why), "unavailable" (not connected, or closed), "too-large"
    (read_snapshot: the samples of the peers asked for do not fit one answer; fewer peers may)."""

    def __init__(self, kind: str, message: str):
        super().__init__(message)
        self.kind = kind


@dataclass(frozen=True)
class TimeInfo:
    now_ns: int  # the virtual time
    paused: bool  # standing still at now_ns: every step that began before it has run, none after it
    running: bool  # started and not ended
    ended: bool


@dataclass(frozen=True)
class PeerInfo:
    """A participant as the source knows it. variables: in the order of its samples, each {"name", "type" (bool,
    integer, real, enum), "role" (input, output, parameter), "unit", "min", "max", "step", "description",
    "enum": [(name, value), ...]}."""
    name: str
    incarnation: str  # changes when another process takes the name: a sample belongs to one incarnation
    connected: bool
    variables: list[dict] = field(default_factory=list)


class Sample(Protocol):
    """The values of every variable of a peer, read in one step."""
    incarnation: str
    time_ns: int  # the step's virtual time
    sequence: int  # grows with every sample of the incarnation
    count: int  # how many values

    def decode(self, types: list[str]) -> list:
        """The values in the schema's order: true/false, whole numbers, numbers; a number that is not finite is
        None. Called when a screen needs them, not for every sample."""


@dataclass(frozen=True)
class ListSample:
    """A sample whose values are already a list (the stand-ins of the tests)."""
    incarnation: str
    time_ns: int
    sequence: int
    values: list

    @property
    def count(self) -> int:
        return len(self.values)

    def decode(self, types: list[str]) -> list:
        return list(self.values)


@dataclass(frozen=True)
class SnapshotInfo:
    """The time and the last samples of several peers, read by the source in one go: the time first, then all the
    samples at one moment (GetSnapshot of docs/control-api.md, whose promise this carries). The samples are never
    older than the time. consistent: it stood still at time.now_ns and no sample is of a step after it, so the
    samples are those of that moment (every step that began before it, none after it; the step that begins
    exactly at it may or may not be in them yet)."""
    time: TimeInfo
    samples: dict[str, Sample | None]  # by peer, for every peer asked for (None: it has no sample yet, or is unknown)
    consistent: bool


@dataclass(frozen=True)
class TargetInfo:
    peer: str
    status: str  # sent, queued, disconnected (open); applied, late, rejected, cancelled, invalid, busy, unknown
    applied_at_ns: int | None = None
    late: bool = False
    reason: str = ""


@dataclass(frozen=True)
class OperationInfo:
    seq: int
    done: bool
    targets: list[TargetInfo]


class Sink(Protocol):
    """What a source tells, on the event loop (variables.Variables). After a connection is made: on_peers,
    on_samples and on_time for what there is, then on_synced; then every change. Of values and of the time only
    the latest need be told (the values need not be told at all: they are read with read_snapshot, and only a read
    says of which moment they are); of the operations every one, with its latest state. on_dropped when changes were
    lost on the way. on_lost when the connection ended (fatal: the source will not connect again); after it,
    nothing of that connection is told."""

    def on_peers(self, peers: list[PeerInfo], removed: list[str]) -> None: ...
    def on_samples(self, samples: dict[str, Sample]) -> None: ...
    def on_operations(self, operations: list[OperationInfo]) -> None: ...
    def on_time(self, time: TimeInfo) -> None: ...
    def on_synced(self) -> None: ...
    def on_dropped(self) -> None: ...
    def on_lost(self, reason: str, fatal: bool) -> None: ...


class Source(Protocol):
    def start(self, sink: Sink) -> None:
        """Begins (called once, on the event loop): connects, and connects again when the connection is lost."""

    async def read_peers(self, timeout_s: float) -> list[PeerInfo]:
        """Every peer the source knows now."""

    async def read_snapshot(self, peers: list[str], timeout_s: float) -> SnapshotInfo:
        """The time and the last sample of each peer named (at least one), as the source has them now. This is
        the only way the values and their time are asked for."""

    async def read_operation(self, seq: int, timeout_s: float) -> OperationInfo | None:
        """None: the source does not know it (any more)."""

    async def write(self, writes: list[tuple[str, str, object]], at_ns: int | None, late: str,
                    timeout_s: float) -> int:
        """Writes (peer, variable, value) together; the operation's number. Its result comes by on_operations."""

    async def close(self) -> None:
        """Ends everything of the source; nothing is told after it returns. Safe to call twice."""
