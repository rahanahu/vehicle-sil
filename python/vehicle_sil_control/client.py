# SPDX-License-Identifier: MIT
"""A client of the vehicle-sil Control API v1 (gRPC)."""
from __future__ import annotations

import dataclasses
import warnings
from typing import Dict, Iterator, List, Mapping, Optional, Sequence, Union

import grpc

from ._stubs import pb, rpc

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


class ControlError(Exception):
    """A call failed; `code` is the gRPC status code."""

    def __init__(self, code: grpc.StatusCode, message: str):
        super().__init__(f"{code.name}: {message}")
        self.code = code


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

    @property
    def writable(self) -> bool:
        return self.causality != "output"


@dataclasses.dataclass(frozen=True)
class Peer:
    participant: str
    incarnation: str
    connected: bool
    schema: List[Variable]
    failed: bool = False
    error: str = ""
    has_agent: bool = False  # an agent has connected for it (else it is known from SIL Kit only)
    simulation_state: str = ""  # the SIL Kit lifecycle state the server sees ("" if unknown)


@dataclasses.dataclass(frozen=True)
class Target:
    peer: str
    status: str  # "sent", "queued", "disconnected", "applied", "late", ...
    applied_at: Optional[int] = None  # ns
    late: bool = False
    reason: str = ""


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
    time: int  # ns
    sequence: int
    values: Dict[str, Number]


@dataclasses.dataclass(frozen=True)
class EventInfo:
    peer: str
    time: int
    level: str
    text: str


def _enum_name(enum, value: int, prefix: str) -> str:
    try:
        return enum.Name(value)[len(prefix):].lower()
    except ValueError:
        return "unknown"  # a value from a newer server


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
    )


def _peer(p) -> Peer:
    return Peer(p.participant, p.incarnation, p.connected, [_variable(v) for v in p.schema], p.status.failed,
                p.status.error, p.has_agent, p.simulation_state)


def _value(v) -> Number:
    kind = v.WhichOneof("kind")
    return {"b": v.b, "i": v.i, "d": v.d}[kind] if kind else None


def _to_value(x: Number):
    if isinstance(x, bool):
        return pb.Value(b=x)
    if isinstance(x, int):
        return pb.Value(i=x)
    return pb.Value(d=float(x))


def _operation(o) -> Operation:
    targets = [
        Target(t.peer, _enum_name(pb.TargetStatus, t.status, "TARGET_STATUS_"),
               t.applied_at_ns if t.HasField("applied_at_ns") else None, t.late, t.reason) for t in o.targets
    ]
    return Operation(o.seq, targets, o.done)


def _time(t) -> TimeState:
    return TimeState(t.available, t.now_ns, t.paused, t.pause_target_ns if t.HasField("pause_target_ns") else None,
                     t.speed if t.HasField("speed") else None, t.running, t.ended)


def _simulation(m) -> SimulationState:
    return SimulationState(m.available, {p.participant: p.state for p in m.participants}, m.error,
                           m.failed_participant, m.failed_state, m.failure_reason, m.ended)


class _TokenInterceptor(grpc.UnaryUnaryClientInterceptor, grpc.UnaryStreamClientInterceptor,
                        grpc.StreamUnaryClientInterceptor, grpc.StreamStreamClientInterceptor):
    """Adds "authorization: Bearer <token>" to every call."""

    def __init__(self, token: str):
        self._metadata = ("authorization", f"Bearer {token}")

    def _details(self, details):
        metadata = list(details.metadata or []) + [self._metadata]
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
        request = pb.PauseRequest() if at is None else pb.PauseRequest(at_ns=at)
        return _time(self._c._call(self._c._stub.Pause, request))

    def resume(self) -> TimeState:
        return _time(self._c._call(self._c._stub.Resume, pb.ResumeRequest()))

    def run_until(self, at: int) -> TimeState:
        return _time(self._c._call(self._c._stub.RunUntil, pb.RunUntilRequest(at_ns=at)))

    def step(self, duration: int) -> TimeState:
        return _time(self._c._call(self._c._stub.Step, pb.StepRequest(duration_ns=duration)))

    def set_speed(self, speed: Optional[float]) -> TimeState:
        request = pb.SetSpeedRequest() if speed is None else pb.SetSpeedRequest(speed=speed)
        return _time(self._c._call(self._c._stub.SetSpeed, request))

    def wait_paused(self, timeout: float = 30.0) -> TimeState:
        return _time(self._c._call(self._c._stub.WaitPaused, pb.WaitPausedRequest(timeout_ms=int(timeout * 1000)),
                                   timeout=timeout + 5))

    def stop(self, reason: str = "") -> TimeState:
        return _time(self._c._call(self._c._stub.Stop, pb.StopRequest(reason=reason)))


class Control:
    """A connection to a control server.

        with Control("127.0.0.1:8510") as c:
            c.sim.pause(); c.sim.wait_paused()
            seq = c.write({"Body": {"cabin.parking_brake": True}}, at=c.sim.time().now + ms(10))
            c.wait(seq, until="queued")
            c.sim.step(ms(100))
    """

    def __init__(self, address: str, timeout: float = 10.0, token: Optional[str] = None,
                 root_certificates: Optional[bytes] = None, tls: bool = False):
        """`token`: a bearer token of the server's credentials. `root_certificates`: PEM bytes of
        the roots the server's certificate must chain to (set: TLS). `tls` without
        root_certificates: TLS with the system's root certificates."""
        if root_certificates is not None or tls:
            self._channel = grpc.secure_channel(address, grpc.ssl_channel_credentials(root_certificates))
        else:
            self._channel = grpc.insecure_channel(address)
        if token:
            interceptor = _TokenInterceptor(token)
            self._channel = grpc.intercept_channel(self._channel, interceptor)
        self._stub = rpc.ControlServiceStub(self._channel)
        self._timeout = timeout
        self.sim = _Simulation(self)

    def __enter__(self) -> "Control":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        self._channel.close()

    def _call(self, method, request, timeout: Optional[float] = None):
        try:
            return method(request, timeout=timeout or self._timeout)
        except grpc.RpcError as e:
            raise ControlError(e.code(), e.details()) from None

    def ping(self, text: str = "") -> str:
        return self._call(self._stub.Ping, pb.PingRequest(text=text)).server_version

    def peers(self) -> List[Peer]:
        """The peers. If the server could not read the simulation's participants, it warns
        (ControlWarning) and the list has their last known states."""
        reply = self._call(self._stub.ListPeers, pb.ListPeersRequest())
        if reply.simulation_error:
            warnings.warn(f"ListPeers: {reply.simulation_error}", ControlWarning, stacklevel=2)
        return [_peer(p) for p in reply.peers]

    def peer(self, name: str) -> Peer:
        for p in self.peers():
            if p.participant == name:
                return p
        raise ControlError(grpc.StatusCode.NOT_FOUND, f"no peer {name}")

    def values(self, peer: str) -> Optional[Values]:
        """The peer's latest sample, by variable name (None if there is none yet)."""
        for _ in range(3):
            info = self.peer(peer)
            reply = self._call(self._stub.GetValues, pb.GetValuesRequest(peer=peer))
            if not reply.has_values:
                return None
            if reply.incarnation == info.incarnation:
                break  # the schema fits the values
        else:
            raise ControlError(grpc.StatusCode.UNAVAILABLE, f"{peer} keeps restarting")
        values = {v.name: _value(x) for v, x in zip(info.schema, reply.snapshot.values)}
        return Values(peer, reply.incarnation, reply.snapshot.time_ns, reply.snapshot.sequence, values)

    def write(self, writes: Mapping[str, Mapping[str, Number]], at: Optional[int] = None, late: str = "reject") -> int:
        """Writes {peer: {variable: value}}; returns the operation's sequence number. `at` is a
        simulation time in ns (None: the next step); late is "reject" or "apply_next"."""
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
        """Waits until every target is final ("done") or none is still being sent ("queued")."""
        request = pb.WaitOperationRequest(seq=seq, timeout_ms=int(timeout * 1000),
                                          until=pb.WAIT_FOR_QUEUED if until == "queued" else pb.WAIT_FOR_DONE)
        return _operation(self._call(self._stub.WaitOperation, request, timeout=timeout + 5))

    def watch(self, peers: Sequence[str] = (), values: bool = True, events: bool = True, operations: bool = True,
              capacity: int = 0, time: bool = False, simulation: bool = False) -> Iterator[object]:
        """Yields Peer, ("removed", name), Values, EventInfo, Operation, TimeState (with time=True),
        SimulationState (with simulation=True: participants' states, a failure, the end) and
        ("initial_done",) once the current state has been sent, until the caller stops iterating (or
        the server ends the stream)."""
        schemas: Dict[str, List[Variable]] = {}
        request = pb.WatchRequest(peers=list(peers), values=values, events=events, operations=operations,
                                  capacity=capacity, time=time, simulation=simulation)
        stream = self._stub.Watch(request)
        try:
            for u in stream:
                kind = u.WhichOneof("kind")
                if kind == "peer":
                    peer = _peer(u.peer)
                    schemas[(peer.participant, peer.incarnation)] = peer.schema
                    yield peer
                elif kind == "removed_peer":
                    yield ("removed", u.removed_peer)
                elif kind == "values":
                    schema = schemas.get((u.values.peer, u.values.incarnation), [])
                    names = [v.name for v in schema]
                    vals = {n: _value(x) for n, x in zip(names, u.values.snapshot.values)}
                    yield Values(u.values.peer, u.values.incarnation, u.values.snapshot.time_ns,
                                 u.values.snapshot.sequence, vals)
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
                raise ControlError(e.code(), e.details()) from None
        finally:
            stream.cancel()
