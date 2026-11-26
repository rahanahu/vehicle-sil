# SPDX-License-Identifier: MIT
"""A client of the vehicle-sil Control API v1 (gRPC)."""
from __future__ import annotations

import dataclasses
import math
import sys
import threading
from time import monotonic as _monotonic
import warnings
from typing import Dict, Iterator, List, Mapping, Optional, Sequence, Union

import grpc

from . import __version__
from ._stubs import pb, rpc
from .grpc_helpers import (
    REASON_KEY,
    RUN_KEY,
    enum_name as _enum_name,
    from_proto_value as _value,
    proto_target_fields,
    to_proto_value,
    trailing_reason,
)
from .protocol import REASON_RUN_REQUIRED

Number = Union[bool, int, float]

NS_PER_MS = 1_000_000


def ms(value: float) -> int:
    """Milliseconds of simulation time, in nanoseconds."""
    return int(round(value * NS_PER_MS))


def s(value: float) -> int:
    """Seconds of simulation time, in nanoseconds."""
    return int(round(value * 1_000_000_000))


class ControlWarning(UserWarning):
    """The server answered, but could not tell everything (the message says what is missing)."""


def _warn_every_time(message: str) -> None:
    """Warns (ControlWarning) at the caller of the public method that calls this, on every call.
    warnings.warn() under the default filter shows a warning once per call site, and a client
    that asks again and again would miss the later ones. The user's filters still decide
    (ignore, error, once); only the per-site registry is not used. The filters are not changed."""
    frame = sys._getframe(2)
    warnings.warn_explicit(message, ControlWarning, frame.f_code.co_filename, frame.f_lineno,
                           module=frame.f_globals.get("__name__"), registry=None, module_globals=frame.f_globals)


class ControlError(Exception):
    """A call failed; `code` is the gRPC status code. `reason` is the server's word for why it
    refused the call, for a program that must tell the refusals apart; "" if it gave none. The
    launcher gives one with FAILED_PRECONDITION: "run-changed" (the Control is bound to another run
    than the server's: an earlier run's client at the same address), "run-required" (the server
    takes calls that name its run only: bind the Control to a run), "scenario-running" (the run
    plays a scenario, and a client cannot write meanwhile; it can move the time),
    "outside-real-time" (the run is connected outside the simulation and keeps to real time: no
    pause, step, run_until or speed other than 1; start it with --time-policy free to move its
    time), and the other reasons of a refused command ("not-running", "not-paused", "not-set-up":
    docs/simulation-events.md lists them)."""

    def __init__(self, code: grpc.StatusCode, message: str, reason: str = ""):
        super().__init__(f"{code.name}: {message}")
        self.code = code
        self.reason = reason


def _error(e: grpc.RpcError) -> ControlError:
    reason = trailing_reason(e) or ""
    details = e.details() or ""
    if reason == REASON_RUN_REQUIRED:
        hint = "hint: bind Control to a run with Control.for_current_run(address) or Control(address, run=<run_id>)"
        details = f"{details}\n{hint}" if details else hint
    return ControlError(e.code(), details, reason)


@dataclasses.dataclass(frozen=True)
class Variable:
    name: str
    type: str  # "bool", "integer", "real", "enum"
    causality: str  # "input", "output", "parameter"
    unit: str = ""
    min: Optional[float] = None
    max: Optional[float] = None
    step: Optional[float] = None
    description: str = ""
    items: Mapping[str, int] = dataclasses.field(default_factory=dict)
    # vehicle-sil gives it to the ECU ("ecu.", "peer.", "j1939."), the application did not declare it
    builtin: bool = False
    # the name the executable declared it under, if the simulation's configuration maps it (the
    # participant's "variables"); "" if it is not mapped. Everything else is as mapped.
    source: str = ""

    @property
    def writable(self) -> bool:
        return self.causality != "output"


@dataclasses.dataclass(frozen=True)
class Peer:
    participant: str
    incarnation: str
    connected: bool
    schema: List[Variable]
    has_agent: bool = False  # an agent has connected for it (else it is known from SIL Kit only)
    simulation_state: str = ""  # the SIL Kit lifecycle state the server sees ("" if unknown)
    # its agent runs on the simulation's time (false: in real time, or no agent); Snapshot.consistent
    # looks only at these peers
    virtual_time: bool = False


@dataclasses.dataclass(frozen=True)
class Target:
    """One peer's result of a write. Its writes are those of the request for this peer, in the
    order given; positions count from 0 (see TargetResult in control.proto).

    `status` is one of the open states ("sent", "queued", "disconnected") or final states
    ("applied", "late", "rejected", "cancelled", "invalid", "busy", "unknown"). `late` is True
    when `at` had already passed before the peer applied (with `late="apply_next"`) or refused
    (with `late="reject"`) the write."""
    peer: str
    status: str
    applied_at: Optional[int] = None  # ns
    late: bool = False
    reason: str = ""
    # how many writes took effect, from the first (None: not known, e.g. still queued)
    applied_writes: Optional[int] = None
    # the position of the write `reason` is about (None: none, or not known)
    failed_write: Optional[int] = None


@dataclasses.dataclass(frozen=True)
class Operation:
    seq: int
    targets: List[Target]
    done: bool

    def target(self, peer: str) -> Target:
        return next(t for t in self.targets if t.peer == peer)


@dataclasses.dataclass(frozen=True)
class TimeState:
    available: bool
    now: int  # ns
    paused: bool
    pause_target: Optional[int]
    speed: Optional[float]
    running: bool  # started and not ended
    ended: bool = False  # ran and is over (running is False before the start too)


@dataclasses.dataclass(frozen=True)
class SimulationState:
    """The coordinated simulation as the server sees it (watch(simulation=True))."""
    available: bool  # False: the server does not coordinate a simulation
    participants: Dict[str, str]  # required participant -> SIL Kit lifecycle state ("" if not seen yet)
    error: str = ""  # not empty: the server could not read the state (why); the rest is the last known
    failed_participant: str = ""  # a participant that failed the simulation ("" if none)
    failed_state: str = ""
    failure_reason: str = ""
    ended: bool = False


@dataclasses.dataclass(frozen=True)
class Values:
    peer: str
    incarnation: str
    time: int  # ns: the start of the ECU step at whose end the values were read
    sequence: int
    values: Dict[str, Number]
    _enums: Mapping[str, Mapping[int, str]] = dataclasses.field(default_factory=dict, repr=False, compare=False)

    def text(self, name: str) -> Optional[str]:
        """The enum item name for `name`'s current value, or None if `name` is absent, not an enum,
        or its integer value has no matching item."""
        if name not in self.values:
            return None
        mapping = self._enums.get(name)
        if not mapping:
            return None
        raw = self.values[name]
        if isinstance(raw, bool) or not isinstance(raw, int):
            return None
        return mapping.get(raw)


@dataclasses.dataclass(frozen=True)
class Snapshot:
    """The time state and the latest values of several peers, read by the server in one go
    (Control.snapshot()).

    For the peers that run on the simulation's time, when `time.available`: the values are never
    older than the time (every step that started before `time.now` is in them; they may be newer).
    With `consistent`, the time stood still at `time.now` and no value of such a peer is of a step
    after it: their values are those of that moment. A step that starts exactly at `time.now` may or may not be in
    them yet (see `Values.time`).

    This holds as long as every ECU publishes its samples from within its step (an ECU of ecu_main
    does; otherwise the launcher warns with control-link-late-values). "Every step" means the latest
    sample taken before that time: an agent samples every sample period. Nothing is promised for a
    peer that does not run on the simulation's time (`Peer.virtual_time` false in `peers()`), and
    `consistent` does not look at its values.
    The values of a peer that is no longer connected stay in the snapshot (see `peers()`)."""
    time: TimeState
    values: Dict[str, Values]  # peer -> its latest sample; a peer without a sample (or an agent) is not in it
    consistent: bool


@dataclasses.dataclass(frozen=True)
class EventInfo:
    peer: str
    time: int
    level: str
    text: str


def _enum_lookup(schema: Sequence[Variable]) -> Dict[str, Dict[int, str]]:
    lookup: Dict[str, Dict[int, str]] = {}
    for v in schema:
        if v.items:
            by_value: Dict[int, str] = {}
            for item_name, item_value in v.items.items():
                by_value.setdefault(item_value, item_name)
            lookup[v.name] = by_value
    return lookup


def _build_values(
    peer: str,
    incarnation: str,
    time_ns: int,
    sequence: int,
    schema: Sequence[Variable],
    raw_values: Sequence,
    *,
    builtin: bool = True,
    enums: Optional[Mapping[str, Mapping[int, str]]] = None,
) -> Values:
    named: Dict[str, Number] = {}
    for v, x in zip(schema, raw_values):
        if builtin or not v.builtin:
            named[v.name] = _value(x)
    enum_map = _enum_lookup(schema) if enums is None else enums
    return Values(peer, incarnation, time_ns, sequence, named, _enums=enum_map)


def _variable(v) -> Variable:
    return Variable(
        name=v.name,
        type=_enum_name(pb.ValueType, v.type, "VALUE_TYPE_"),
        causality=_enum_name(pb.Causality, v.causality, "CAUSALITY_"),
        unit=v.unit,
        min=v.min if v.HasField("min") else None,
        max=v.max if v.HasField("max") else None,
        step=v.step if v.HasField("step") else None,
        description=v.description,
        items={i.name: i.value for i in v.items},
        builtin=v.builtin,
        source=v.source,
    )


def _peer(p) -> Peer:
    return Peer(p.participant, p.incarnation, p.connected, [_variable(v) for v in p.schema], p.has_agent,
                p.simulation_state, p.virtual_time)


def _to_value(x: Number):
    return to_proto_value(x, pb)


def _operation(o) -> Operation:
    targets = [Target(*proto_target_fields(t, pb)) for t in o.targets]
    return Operation(o.seq, targets, o.done)


def _time(t) -> TimeState:
    return TimeState(t.available, t.now_ns, t.paused, t.pause_target_ns if t.HasField("pause_target_ns") else None,
                     t.speed if t.HasField("speed") else None, t.running, t.ended)


def _simulation(m) -> SimulationState:
    return SimulationState(m.available, {p.participant: p.state for p in m.participants}, m.error,
                           m.failed_participant, m.failed_state, m.failure_reason, m.ended)


class _MetadataInterceptor(grpc.UnaryUnaryClientInterceptor, grpc.UnaryStreamClientInterceptor,
                           grpc.StreamUnaryClientInterceptor, grpc.StreamStreamClientInterceptor):
    """Adds the same metadata to every call ("authorization: Bearer <token>", "vehicle-sil-run: <ID>")."""

    def __init__(self, metadata: Sequence[tuple]):
        self._metadata = list(metadata)

    def _details(self, details):
        metadata = list(details.metadata or []) + self._metadata
        return details._replace(metadata=metadata) if hasattr(details, "_replace") else _Details(details, metadata)

    def intercept_unary_unary(self, continuation, details, request):
        return continuation(self._details(details), request)

    def intercept_unary_stream(self, continuation, details, request):
        return continuation(self._details(details), request)

    def intercept_stream_unary(self, continuation, details, request_iterator):
        return continuation(self._details(details), request_iterator)

    def intercept_stream_stream(self, continuation, details, request_iterator):
        return continuation(self._details(details), request_iterator)


class _Details(grpc.ClientCallDetails):
    def __init__(self, base, metadata):
        self.method = base.method
        self.timeout = base.timeout
        self.metadata = metadata
        self.credentials = base.credentials
        self.wait_for_ready = getattr(base, "wait_for_ready", None)
        self.compression = getattr(base, "compression", None)


class _Simulation:
    """Time control (only when the server coordinates the simulation)."""

    def __init__(self, control: "Control"):
        self._c = control

    def time(self) -> TimeState:
        return _time(self._c._call(self._c._stub.GetTime, pb.GetTimeRequest()))

    def pause(self, at: Optional[int] = None) -> TimeState:
        """Pause immediately (`at=None`) or schedule a pause at `at` ns (replaces any earlier
        `pause_target`; `at` is rounded up to whole milliseconds and then up to the controller's
        time step). Does not resume from a normal post-start pause (only `--start-paused` before
        step 0 advances when `at > 0`); use `run_until(at)` to advance to `at` and pause."""
        request = pb.PauseRequest() if at is None else pb.PauseRequest(at_ns=at)
        return _time(self._c._call(self._c._stub.Pause, request))

    def resume(self) -> TimeState:
        """Resume the simulation and clear any planned `pause_target` (last call wins)."""
        return _time(self._c._call(self._c._stub.Resume, pb.ResumeRequest()))

    def run_until(self, at: int) -> TimeState:
        """Advance to `at` ns (resuming if paused before `at`) and pause there. Returns once
        accepted (call `wait_paused()` to wait until the simulation stands still); `at` is rounded
        up to whole milliseconds and then up to the controller's time step."""
        return _time(self._c._call(self._c._stub.RunUntil, pb.RunUntilRequest(at_ns=at)))

    def step(self, duration: int) -> TimeState:
        """Advance by `duration` ns from the current paused time and pause again (valid only while
        paused; raises ControlError FAILED_PRECONDITION otherwise). Returns once accepted (call
        `wait_paused()` to wait until paused); the stop time is rounded up to whole milliseconds
        and then up to the controller's time step."""
        return _time(self._c._call(self._c._stub.Step, pb.StepRequest(duration_ns=duration)))

    def set_speed(self, speed: Optional[float]) -> TimeState:
        """Set simulation speed (simulation seconds per wall-clock second; `None` for max speed)."""
        request = pb.SetSpeedRequest() if speed is None else pb.SetSpeedRequest(speed=speed)
        return _time(self._c._call(self._c._stub.SetSpeed, request))

    def wait_paused(self, timeout: float = 30.0) -> TimeState:
        """Wait up to `timeout` wall-clock seconds until the simulation pauses, returning its `TimeState`."""
        return _time(self._c._call(self._c._stub.WaitPaused, pb.WaitPausedRequest(timeout_ms=int(timeout * 1000)),
                                   timeout=timeout + 5))

    def stop(self, reason: str = "") -> TimeState:
        """Stop the simulation (`reason` is recorded in the audit log if enabled)."""
        return _time(self._c._call(self._c._stub.Stop, pb.StopRequest(reason=reason)))


class Control:
    """A connection to a control server.

        with Control("127.0.0.1:8510") as c:
            c.sim.pause(); c.sim.wait_paused()
            seq = c.write({"Body": {"cabin.parking_brake": True}}, at=c.sim.time().now + ms(10))
            c.wait(seq, until="queued")
            c.sim.step(ms(100)); c.sim.wait_paused()

    Making one does not connect. The first call (or watch) checks that the server is of this
    client's vehicle-sil release (__version__) and refuses another with ControlError.

    A Control bound to a run (`run`, or `Control.for_current_run()`) acts on that run only: once
    the server at the address belongs to another run (a launcher with `serve` runs one after
    another), every call but `ping()` and `current_run()` is refused with ControlError, `reason`
    "run-changed". gRPC connects again by itself to whatever server listens at the address, so
    without a run a Control may reach a later run there (a server that requires the run refuses
    it with "run-required").
    """

    def __init__(self, address: str, timeout: float = 10.0, token: Optional[str] = None,
                 root_certificates: Optional[bytes] = None, tls: bool = False, run: Optional[str] = None):
        """`token`: a bearer token of the server's credentials. `root_certificates`: PEM bytes of
        the roots the server's certificate must chain to (set: TLS). `tls` without
        root_certificates: TLS with the system's root certificates. `run`: the ID of the run the
        calls are for (sent as the metadata "vehicle-sil-run"; the launcher's events and
        `current_run()` tell it). It is no credential: the token decides what a client may do."""
        if root_certificates is not None or tls:
            self._channel = grpc.secure_channel(address, grpc.ssl_channel_credentials(root_certificates))
        else:
            self._channel = grpc.insecure_channel(address)
        if run is not None and not run:
            raise ValueError("run is the ID of a run (None: calls name no run), not an empty string")
        self.run = run  # the run the calls are for (None: they name none)
        metadata = ([("authorization", f"Bearer {token}")] if token else []) + ([(RUN_KEY, run)] if run else [])
        if metadata:
            self._channel = grpc.intercept_channel(self._channel, _MetadataInterceptor(metadata))
        self._stub = rpc.ControlServiceStub(self._channel)
        self._timeout = timeout
        self._schemas: Dict[tuple, List[Variable]] = {}  # (peer, incarnation) -> schema, for snapshot()
        self._enum_maps: Dict[tuple, Dict[str, Dict[int, str]]] = {}  # (peer, incarnation) -> {var: {int: name}}
        self._checked = False  # the server's release was compared with this client's (_check_release)
        self._check_lock = threading.Lock()
        self.sim = _Simulation(self)

    @classmethod
    def for_current_run(cls, address: str, **options) -> "Control":
        """A Control bound to the run that the server at `address` belongs to now (one Ping), so
        that its calls never reach a later run at the same address. `options` are those of
        Control() but `run`. ControlError FAILED_PRECONDITION if the server belongs to no run.

            with Control.for_current_run("127.0.0.1:8510") as c:
                c.sim.stop()   # this run, or ControlError "run-changed" once another has begun"""
        if "run" in options:
            raise TypeError("for_current_run() finds the run itself: give the ID to Control(run=...) instead")
        with cls(address, **options) as probe:
            run = probe.current_run()
        if not run:
            raise ControlError(grpc.StatusCode.FAILED_PRECONDITION,
                               f"the control server at {address} belongs to no run, so a client cannot be bound to one")
        return cls(address, run=run, **options)

    def __enter__(self) -> "Control":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        self._channel.close()

    def _check_release(self, timeout: Optional[float] = None) -> None:
        """Before the first call: the server must be of this client's vehicle-sil release (one Ping, once
        per Control; a Ping that fails is tried again at the next call). Another release:
        ControlError FAILED_PRECONDITION."""
        if self._checked:
            return
        if timeout is None:
            lock_deadline: Optional[float] = None
            self._check_lock.acquire()
        else:
            if math.isnan(timeout):
                raise ValueError("timeout must not be NaN")
            if timeout <= 0:
                raise ControlError(grpc.StatusCode.DEADLINE_EXCEEDED, "release check timed out")
            lock_deadline = _monotonic() + timeout
            if not self._check_lock.acquire(timeout=min(timeout, threading.TIMEOUT_MAX)):
                raise ControlError(grpc.StatusCode.DEADLINE_EXCEEDED, "release check timed out")
        try:
            if self._checked:
                return
            if lock_deadline is None:
                ping_timeout = self._timeout
            else:
                ping_timeout = lock_deadline - _monotonic()
                if ping_timeout <= 0:
                    raise ControlError(grpc.StatusCode.DEADLINE_EXCEEDED, "release check timed out")
            try:
                reply = self._stub.Ping(pb.PingRequest(), timeout=ping_timeout)
            except grpc.RpcError as e:
                raise _error(e) from None
            if reply.vehicle_sil != __version__:
                raise ControlError(grpc.StatusCode.FAILED_PRECONDITION,
                                   f"the control server is vehicle-sil {reply.vehicle_sil}; this vehicle_sil_control is "
                                   f"{__version__}: use the vehicle_sil_control of the same release as the launcher")
            self._checked = True
        finally:
            self._check_lock.release()

    def _call(self, method, request, timeout: Optional[float] = None):
        self._check_release()
        try:
            return method(request, timeout=timeout or self._timeout)
        except grpc.RpcError as e:
            raise _error(e) from None

    def ping(self, text: str = "") -> str:
        return self._call(self._stub.Ping, pb.PingRequest(text=text)).server_version

    def current_run(self) -> str:
        """The ID of the run the server belongs to now (PingReply.run; "" if none). Not refused for
        a Control bound to another run: compare it with `run` to see whether this Control still
        reaches its run."""
        return self._call(self._stub.Ping, pb.PingRequest()).run

    def peers(self) -> List[Peer]:
        """The peers. If the server could not read the simulation's participants, it warns
        (ControlWarning) and the list has their last known states."""
        reply = self._call(self._stub.ListPeers, pb.ListPeersRequest())
        if reply.simulation_error:
            _warn_every_time(f"ListPeers: {reply.simulation_error}")
        return [_peer(p) for p in reply.peers]

    def peer(self, name: str) -> Peer:
        for p in self.peers():
            if p.participant == name:
                return p
        raise ControlError(grpc.StatusCode.NOT_FOUND, f"no peer {name}")

    def values(self, peer: str, builtin: bool = True) -> Optional[Values]:
        """The peer's latest sample, by variable name (None if there is none yet).
        With `builtin=False`, returns only application-declared variables (`not v.builtin`)."""
        for _ in range(3):
            info = self.peer(peer)
            reply = self._call(self._stub.GetValues, pb.GetValuesRequest(peer=peer))
            if not reply.has_values:
                return None
            if reply.incarnation == info.incarnation:
                break  # the schema fits the values
        else:
            raise ControlError(grpc.StatusCode.UNAVAILABLE, f"{peer} keeps restarting")
        key = (peer, reply.incarnation)
        enums = self._enum_maps.get(key)
        if enums is None:
            enums = _enum_lookup(info.schema)
            self._enum_maps[key] = enums
        return _build_values(peer, reply.incarnation, reply.snapshot.time_ns, reply.snapshot.sequence,
                             info.schema, reply.snapshot.values, builtin=builtin, enums=enums)

    def snapshot(self, peers: Sequence[str] = (), builtin: bool = True) -> Snapshot:
        """The time state and the latest values of `peers` (empty: every peer with an agent), read
        by the server in one go: see Snapshot for what holds between the time and the values.
        After `sim.wait_paused()`, `consistent` says that the values are those of the time the
        simulation stands still at. With `builtin=False`, each peer's `Values` includes only
        application-declared variables (`not v.builtin`)."""
        if isinstance(peers, (str, bytes)):
            raise TypeError("peers is a sequence of names, not one name: pass [name]")
        reply = self._call(self._stub.GetSnapshot, pb.GetSnapshotRequest(peers=list(peers)))
        sampled = [v for v in reply.values if v.HasField("snapshot")]
        if any((v.peer, v.incarnation) not in self._schemas for v in sampled):
            # The names of the values: a schema belongs to an incarnation and does not change.
            for p in self._call(self._stub.ListPeers, pb.ListPeersRequest()).peers:
                if p.has_agent:
                    schema = [_variable(v) for v in p.schema]
                    key = (p.participant, p.incarnation)
                    self._schemas[key] = schema
                    self._enum_maps[key] = _enum_lookup(schema)
        values: Dict[str, Values] = {}
        for v in sampled:
            key = (v.peer, v.incarnation)
            schema = self._schemas.get(key)
            if schema is None:
                raise ControlError(grpc.StatusCode.UNAVAILABLE, f"{v.peer} restarted during the call: ask again")
            enums = self._enum_maps.get(key)
            if enums is None:
                enums = _enum_lookup(schema)
                self._enum_maps[key] = enums
            values[v.peer] = _build_values(v.peer, v.incarnation, v.snapshot.time_ns, v.snapshot.sequence,
                                           schema, v.snapshot.values, builtin=builtin, enums=enums)
        return Snapshot(_time(reply.time), values, reply.consistent)

    def write(self, writes: Mapping[str, Mapping[str, Number]], at: Optional[int] = None, late: str = "reject") -> int:
        """Writes {peer: {variable: value}}; returns the operation's sequence number.

        `at` is a simulation time in ns (None: the next step; after a pause set `at > sim.time().now`
        except in the `--start-paused` pre-start hold). `late` decides what happens if `at` has
        already passed when a peer receives the write: `"reject"` (default) leaves the variable
        unchanged with `Target.status == "late"`, while `"apply_next"` applies it in the next step
        with `Target.status == "applied"` and `Target.late == True`."""
        request = pb.WriteRequest(late=pb.LATE_POLICY_APPLY_NEXT if late == "apply_next" else pb.LATE_POLICY_REJECT)
        if at is not None:
            request.at_ns = at
        for peer, variables in writes.items():
            for name, value in variables.items():
                request.writes.add(peer=peer, name=name, value=_to_value(value))
        return self._call(self._stub.Write, request).seq

    def operation(self, seq: int) -> Operation:
        return _operation(self._call(self._stub.GetOperation, pb.GetOperationRequest(seq=seq)))

    def forget_peer(self, peer: str) -> None:
        """Removes a disconnected peer from the server (its open operations become "unknown")."""
        self._call(self._stub.ForgetPeer, pb.ForgetPeerRequest(peer=peer))

    def wait(self, seq: int, until: str = "done", timeout: float = 30.0) -> Operation:
        """Waits up to `timeout` wall-clock seconds for operation `seq`.

        `until="done"` (default) waits until every target reaches a final `Target.status`
        ("applied", "late", "rejected", "cancelled", "invalid", "busy", "unknown");
        `until="queued"` waits until no target is still "sent"."""
        request = pb.WaitOperationRequest(seq=seq, timeout_ms=int(timeout * 1000),
                                          until=pb.WAIT_FOR_QUEUED if until == "queued" else pb.WAIT_FOR_DONE)
        return _operation(self._call(self._stub.WaitOperation, request, timeout=timeout + 5))

    def watch(self, peers: Sequence[str] = (), values: bool = True, events: bool = True, operations: bool = True,
              capacity: int = 0, time: bool = False, simulation: bool = False,
              timeout: Optional[float] = None) -> Iterator[object]:
        """Yields Peer, ("removed", name), Values, EventInfo, Operation, TimeState (with time=True),
        SimulationState (with simulation=True: participants' states, a failure, the end) and
        ("initial_done",) once the current state has been sent, until the caller stops iterating,
        the server ends the stream, or `timeout` wall-clock seconds pass (raising ControlError with
        code `DEADLINE_EXCEEDED`; `None` waits indefinitely). When `timeout` is given, it bounds both
        the initial release check (if not yet performed) and the stream."""
        if timeout is None or (math.isinf(timeout) and timeout > 0):
            self._check_release()
            watch_timeout: Optional[float] = None
        else:
            if math.isnan(timeout):
                raise ValueError("timeout must not be NaN")
            if timeout <= 0:
                raise ControlError(grpc.StatusCode.DEADLINE_EXCEEDED, "watch timeout expired")
            deadline = _monotonic() + timeout
            ping_timeout = timeout if self._timeout is None else min(self._timeout, timeout)
            self._check_release(timeout=ping_timeout)
            watch_timeout = deadline - _monotonic()
            if watch_timeout <= 0:
                raise ControlError(grpc.StatusCode.DEADLINE_EXCEEDED, "watch timeout expired")
        schemas: Dict[tuple, List[Variable]] = {}
        enum_maps: Dict[tuple, Dict[str, Dict[int, str]]] = {}
        request = pb.WatchRequest(peers=list(peers), values=values, events=events, operations=operations,
                                  capacity=capacity, time=time, simulation=simulation)
        stream = self._stub.Watch(request, timeout=watch_timeout)
        try:
            for u in stream:
                kind = u.WhichOneof("kind")
                if kind == "peer":
                    peer = _peer(u.peer)
                    key = (peer.participant, peer.incarnation)
                    schemas[key] = peer.schema
                    enum_maps[key] = _enum_lookup(peer.schema)
                    yield peer
                elif kind == "removed_peer":
                    yield ("removed", u.removed_peer)
                elif kind == "values":
                    key = (u.values.peer, u.values.incarnation)
                    schema = schemas.get(key, [])
                    enums = enum_maps.get(key, {})
                    yield _build_values(u.values.peer, u.values.incarnation, u.values.snapshot.time_ns,
                                        u.values.snapshot.sequence, schema, u.values.snapshot.values, enums=enums)
                elif kind == "event":
                    e = u.event.event
                    yield EventInfo(u.event.peer, e.time_ns, _enum_name(pb.EventLevel, e.level, "EVENT_LEVEL_"),
                                    e.text)
                elif kind == "operation":
                    yield _operation(u.operation)
                elif kind == "time":
                    yield _time(u.time)
                elif kind == "simulation":
                    yield _simulation(u.simulation)
                elif kind == "initial_done":
                    yield ("initial_done",)
        except grpc.RpcError as e:
            if e.code() != grpc.StatusCode.CANCELLED:
                raise _error(e) from None
        finally:
            stream.cancel()
