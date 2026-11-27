"""The launcher's control server (gRPC, docs/control-api.md) as the source of a run's variables.

gRPC is optional: the GUI server runs without it, and then only the variables are missing (load() says why).
The stubs of proto/vehicle_sil/control/v1/control.proto are taken from, in this order: the directory named by
VEHICLE_SIL_CONTROL_STUBS; made now with grpcio-tools from the repository's proto file (into a temporary
directory, removed as soon as the stubs are imported: a directory that waits for the process to exit stays
behind when the process is killed); python/vehicle_sil_control/_generated of the repository (what
`python -m vehicle_sil_control.generate` makes). So the variables work when the server runs from the repository
(an editable install, `uv sync`); installed elsewhere, the optional dependency `variables` (grpcio, protobuf) is
not enough by itself: the stubs must be given with VEHICLE_SIL_CONTROL_STUBS until they are shipped in the
package.

Threads: one reader thread per source reads the Watch stream (the peers, the operations, the time, the availability
of the variables) and keeps
only the latest of what it reads (per peer, per operation) until the event loop takes it; it never waits for the
loop, and the loop never waits for it. Single calls (the time and the values, a write) are gRPC futures whose
result is passed to the loop.

The values are not taken from the stream: they are read, together with the time, with one GetSnapshot when a
screen watches them (variables.Variables.refresh), as often as the screens ask and no more. A stream of values
comes as fast as the simulation runs, and what waits in it (in the connection's buffers, which this side cannot
bound) would be old by the time it is read; a read gives the latest sample at that moment, whatever the
simulation's speed.

The token of the run is in a file that only this user can read (credentials_file), never in an argument, an
event or a message: every text that comes from the control server is cleaned of it before it is shown.
"""

from __future__ import annotations

import asyncio
import concurrent.futures
import contextlib
import logging
import math
import threading
import time
from dataclasses import dataclass
from pathlib import Path

from vehicle_sil_control.grpc_helpers import (
    REASON_METADATA,
    REASON_RUN_CHANGED,
    RUN_METADATA,
    check_loopback_address as check_address,
    credentials_file,
    decode_proto_snapshot_values,
    enum_name,
    import_stubs,
    proto_target_fields,
    redact_token,
    remove_file as remove,
    repository,
    resolve_stubs_directory,
    to_proto_value,
    trailing_reason,
)

from .source import AVAILABILITY, OperationInfo, PeerInfo, Sink, SnapshotInfo, SourceError, TargetInfo, TimeInfo

FEATURE = "control-server"  # of vehicle_sil_launch version --json: run --control and the event below
EVENT = "control_server"  # {"address", "tls", "credentials"}: where the run's control server listens
SCENARIO_CLIENT = "scenario"  # the client of a scenario's writes (fixed: docs/control-api.md)
RECONNECT_MIN_S, RECONNECT_MAX_S = 0.2, 2.0
MAX_MESSAGE = 64 * 1024 * 1024  # one peer with the most variables the launcher takes
OPERATIONS_WAITING = 4096  # operations read and not yet taken by the event loop
CLOSE_WAIT_S = 2.0
SYNC_TIMEOUT_S = 10.0  # for the control server to send what there is, once connected; then the connection is ended

log = logging.getLogger("vehicle_sil_gui")


class Unavailable(Exception):
    """The variables cannot be used by this GUI server (the message says why, and what to do)."""


@dataclass(frozen=True)
class Library:
    grpc: object
    pb: object
    rpc: object


_library: Library | Unavailable | None = None
_library_lock = threading.Lock()


def load() -> Library:
    """gRPC and the stubs of the Control API, loaded once (blocking: call it off the event loop). Unavailable
    with the reason when they cannot be."""
    global _library
    with _library_lock:
        if _library is None:
            try:
                _library = _load()
            except Unavailable as e:
                _library = e
        if isinstance(_library, Unavailable):
            raise _library
        return _library


def _load() -> Library:
    try:
        import grpc
    except Exception as e:  # not installed, or an installation that does not load
        raise Unavailable(f"this GUI server has no gRPC ({type(e).__name__}: {e}): run it from the vehicle-sil "
                          "repository after `uv sync` (which installs grpcio, protobuf and grpcio-tools)") from None
    stubs, made = _stubs_directory()
    pb, rpc = import_stubs(stubs, made, error_cls=Unavailable)
    return Library(grpc, pb, rpc)


def _stubs_directory() -> tuple[Path, bool]:
    """Where the stubs are, and whether this call made the directory (a temporary one, which the caller removes
    once the stubs are imported)."""
    return resolve_stubs_directory(root=repository(), error_cls=Unavailable, allow_package_generated=False)


class _Snapshot:
    """A sample as it came (a protobuf Snapshot), turned into values only when a screen needs them."""

    __slots__ = ("incarnation", "time_ns", "sequence", "count", "_values")

    def __init__(self, incarnation: str, snapshot):
        self.incarnation = incarnation
        self.time_ns = snapshot.time_ns
        self.sequence = snapshot.sequence
        self._values = snapshot.values
        self.count = len(self._values)

    def decode(self, types: list[str]) -> list:
        return decode_proto_snapshot_values(self._values, types)


class GrpcSource:
    def __init__(self, library: Library, address: str, token: str | None, *, run_id: str | None = None):
        self._grpc, self._pb = library.grpc, library.pb
        self._address = address
        self._token = token
        metadata: list[tuple[str, str]] = []
        if token:
            metadata.append(("authorization", f"Bearer {token}"))
        if run_id:
            metadata.append((RUN_METADATA, run_id))
        self._metadata = tuple(metadata) or None
        self._channel = self._grpc.insecure_channel(address, options=[
            ("grpc.enable_retries", 0),
            ("grpc.enable_http_proxy", 0),
            ("grpc.max_receive_message_length", MAX_MESSAGE),
        ])
        self._stub = library.rpc.ControlServiceStub(self._channel)
        self._loop: asyncio.AbstractEventLoop | None = None
        self._sink: Sink | None = None
        self._thread: threading.Thread | None = None
        self._thread_done: asyncio.Event | None = None
        self._done_future: concurrent.futures.Future[None] | None = None
        self._reader_exited = threading.Event()
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._stop_flag = False
        self._taken_flag = False  # the loop has taken a lost connection: the reader may connect again
        self._call = None
        self._scheduled = False
        self._closed = False
        # read by the reader, taken by the loop (under _lock)
        self._peers: dict[str, object] = {}
        self._removed: set[str] = set()
        self._values: dict[str, object] = {}
        self._operations: dict[int, object] = {}
        self._availability: dict[str, object] = {}  # the latest PeerAvailability of each peer
        self._time = None
        self._synced = False
        self._dropped = False
        self._lost: tuple[str, bool] | None = None

    # ------------------------------------------------------------ the event loop's side

    def start(self, sink: Sink) -> None:
        self._loop = asyncio.get_running_loop()
        self._sink = sink
        self._thread_done = asyncio.Event()
        self._done_future = concurrent.futures.Future()
        self._reader_exited.clear()
        with self._cond:
            self._stop_flag = False
            self._taken_flag = False
        self._thread = threading.Thread(target=self._read, name="vehicle_sil_gui control", daemon=True)
        self._thread.start()

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        with self._cond:
            self._stop_flag = True
            self._taken_flag = True
            self._cond.notify_all()
            call = self._call
        if call is not None:
            call.cancel()
        self._channel.close()  # ends every call
        thread = self._thread
        if thread is None or not thread.is_alive():
            return
        deadline = time.monotonic() + CLOSE_WAIT_S
        done = self._thread_done
        done_future = self._done_future
        same_loop = self._loop is asyncio.get_running_loop()
        if done is not None and same_loop and not self._reader_exited.is_set():
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(done.wait(), timeout=CLOSE_WAIT_S)
        elif done_future is not None and not self._reader_exited.is_set():
            # Cross-loop close(): wrap the thread-safe Future on the caller's loop (no sleep polling).
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(asyncio.wrap_future(done_future), timeout=CLOSE_WAIT_S)
        elif not self._reader_exited.is_set():
            await asyncio.to_thread(thread.join, CLOSE_WAIT_S)
            return
        if self._reader_exited.is_set() and thread.is_alive():
            # _read() has already reached its finally block; join the exiting OS thread off the event loop
            # so thread.is_alive() is reliably False when close() returns without blocking the loop.
            remaining = max(0.0, deadline - time.monotonic())
            if remaining > 0.0:
                await asyncio.to_thread(thread.join, remaining)

    async def read_peers(self, timeout_s: float) -> list[PeerInfo]:
        reply = await self._unary(self._stub.ListPeers, self._pb.ListPeersRequest(), timeout_s)
        return [self._peer(p) for p in reply.peers]

    async def read_snapshot(self, peers: list[str], timeout_s: float) -> SnapshotInfo:
        reply = await self._unary(self._stub.GetSnapshot, self._pb.GetSnapshotRequest(peers=peers), timeout_s,
                                  exhausted="too-large")
        samples = {v.peer: _Snapshot(v.incarnation, v.snapshot) if v.HasField("snapshot") else None
                   for v in reply.values}
        return SnapshotInfo(_time(reply.time), samples, reply.consistent)

    async def read_operation(self, seq: int, timeout_s: float) -> OperationInfo | None:
        reply = await self._unary(self._stub.GetOperation, self._pb.GetOperationRequest(seq=seq), timeout_s,
                                  missing=None)
        return None if reply is None else self._operation(reply)

    async def wait_queued(self, seq: int, timeout_s: float) -> OperationInfo | None:
        if timeout_s <= 0:
            raise SourceError("timeout", "DEADLINE_EXCEEDED: wait for queued timed out")
        pb = self._pb
        timeout_ms = max(1, min(2 ** 31 - 1, math.ceil(timeout_s * 1000)))
        request = pb.WaitOperationRequest(seq=seq, until=pb.WAIT_FOR_QUEUED, timeout_ms=timeout_ms)
        reply = await self._unary(self._stub.WaitOperation, request, timeout_s + 0.5, missing=None)
        return None if reply is None else self._operation(reply)

    async def write(self, writes: list[tuple[str, str, object]], at_ns: int | None, late: str,
                    timeout_s: float) -> int:
        pb = self._pb
        request = pb.WriteRequest(late=pb.LATE_POLICY_APPLY_NEXT if late == "apply" else pb.LATE_POLICY_REJECT)
        if at_ns is not None:
            request.at_ns = at_ns
        for peer, name, value in writes:
            request.writes.add(peer=peer, name=name, value=to_proto_value(value, pb))  # type: ignore[arg-type]
        return (await self._unary(self._stub.Write, request, timeout_s)).seq

    _NO_DEFAULT = object()

    async def _unary(self, method, request, timeout_s: float, missing: object = _NO_DEFAULT,
                     exhausted: str = "refused"):
        """One call, without blocking the loop. missing: what NOT_FOUND gives (else it is refused). exhausted: the
        kind of the SourceError for RESOURCE_EXHAUSTED (GetSnapshot: the answer would be too large)."""
        grpc = self._grpc
        if self._closed:
            raise SourceError("unavailable", "the connection to the control server is closed")
        loop = asyncio.get_running_loop()
        result: asyncio.Future = loop.create_future()
        try:
            call = method.future(request, timeout=timeout_s, metadata=self._metadata)
        except Exception as e:  # the channel was closed meanwhile
            raise SourceError("unavailable", f"the control server cannot be called ({type(e).__name__})") from None

        def settle() -> None:
            if result.done():
                return
            try:
                result.set_result(call.result())
            except grpc.RpcError as e:
                result.set_exception(e)
            except Exception as e:  # cancelled
                result.set_exception(SourceError("unavailable", f"the call was ended ({type(e).__name__})"))

        def done(_call) -> None:
            try:
                loop.call_soon_threadsafe(settle)
            except RuntimeError:  # the loop is closed
                pass

        call.add_done_callback(done)
        try:
            return await result
        except asyncio.CancelledError:
            call.cancel()
            raise
        except grpc.RpcError as e:
            code = e.code()
            if code == grpc.StatusCode.NOT_FOUND and missing is not self._NO_DEFAULT:
                return missing
            if code == grpc.StatusCode.RESOURCE_EXHAUSTED:
                raise SourceError(exhausted, self._clean(f"{code.name}: {e.details() or 'no details'}")) from None
            raise self._error(code, e.details()) from None

    def _error(self, code, details: str | None) -> SourceError:
        grpc = self._grpc
        text = self._clean(f"{code.name}: {details or 'no details'}")
        if code == grpc.StatusCode.DEADLINE_EXCEEDED:
            return SourceError("timeout", text)
        if code in (grpc.StatusCode.UNAVAILABLE, grpc.StatusCode.CANCELLED, grpc.StatusCode.UNKNOWN,
                    grpc.StatusCode.INTERNAL):
            return SourceError("unavailable", text)
        return SourceError("refused", text)

    def _clean(self, text: str) -> str:
        return redact_token(text, self._token)

    def _take(self) -> None:
        """On the loop: passes on what the reader has read since the last time."""
        with self._lock:
            self._scheduled = False
            peers, self._peers = self._peers, {}
            removed, self._removed = self._removed, set()
            values, self._values = self._values, {}
            operations, self._operations = self._operations, {}
            availability, self._availability = self._availability, {}
            time_, self._time = self._time, None
            synced, self._synced = self._synced, False
            dropped, self._dropped = self._dropped, False
            lost, self._lost = self._lost, None
        sink = self._sink
        if self._closed or sink is None:
            with self._cond:
                self._taken_flag = True
                self._cond.notify_all()
            return
        try:
            if peers or removed:
                sink.on_peers([self._peer(p) for p in peers.values()], sorted(removed))
            for a in availability.values():  # after the peers: the incarnation it is of may be one of them
                sink.on_availability(a.peer, a.incarnation, [self._state(s) for s in a.states])
            if operations:
                sink.on_operations([self._operation(o) for _, o in sorted(operations.items())])
            if values:
                sink.on_samples({name: _Snapshot(v.incarnation, v.snapshot) for name, v in values.items()})
            if time_ is not None:
                sink.on_time(_time(time_))
            if synced:
                sink.on_synced()
            if dropped:
                sink.on_dropped()
            if lost is not None:
                sink.on_lost(self._clean(lost[0]), lost[1])
        except Exception:
            log.exception("taking what the control server sent")
        finally:
            if lost is not None:
                with self._cond:
                    self._taken_flag = True
                    self._cond.notify_all()

    def _peer(self, p) -> PeerInfo:
        pb = self._pb
        types = {pb.VALUE_TYPE_BOOL: "bool", pb.VALUE_TYPE_INTEGER: "integer", pb.VALUE_TYPE_REAL: "real",
                 pb.VALUE_TYPE_ENUM: "enum"}
        roles = {pb.CAUSALITY_INPUT: "input", pb.CAUSALITY_OUTPUT: "output", pb.CAUSALITY_PARAMETER: "parameter"}
        variables = [{"name": v.name, "type": types.get(v.type, ""), "role": roles.get(v.causality, "output"),
                      "unit": v.unit, "min": v.min if v.HasField("min") else None,
                      "max": v.max if v.HasField("max") else None, "step": v.step if v.HasField("step") else None,
                      "description": v.description, "enum": [(i.name, i.value) for i in v.items],
                      "builtin": bool(v.builtin), "availability": self._declared(v)}
                     for v in p.schema]
        return PeerInfo(p.participant, p.incarnation, p.connected, variables)

    def _declared(self, v) -> str | None:
        """The state a VariableInfo starts with; None: it does not report (UNSPECIFIED)."""
        return self._state(v.availability) if v.availability else None

    def _state(self, value: int) -> str:
        """An Availability of the Control API as the screens name it. One this side does not know is shown as in
        use, as a variable that does not report."""
        name = enum_name(self._pb.Availability, value, "AVAILABILITY_", default="active")
        return name if name in AVAILABILITY else "active"

    def _operation(self, o) -> OperationInfo:
        pb = self._pb
        targets = [
            TargetInfo(*proto_target_fields(t, pb, unspecified_as_unknown=True))
            for t in o.targets
        ]
        return OperationInfo(o.seq, o.done, targets)

    # ------------------------------------------------------------ the reader thread

    def _wake(self) -> None:
        """Under _cond: asks the loop to take what was read, once however much is read before it does."""
        is_closed = getattr(self._loop, "is_closed", None)
        if callable(is_closed) and is_closed():
            self._stop_flag = True
            self._taken_flag = True
            self._cond.notify_all()
            return
        if self._scheduled:
            return
        self._scheduled = True
        try:
            self._loop.call_soon_threadsafe(self._take)
        except RuntimeError:  # the loop is closed
            self._stop_flag = True
            self._taken_flag = True
            self._cond.notify_all()

    def _notify_thread_done(self) -> None:
        self._reader_exited.set()
        done_future = self._done_future
        if done_future is not None:
            with contextlib.suppress(concurrent.futures.InvalidStateError):
                done_future.set_result(None)
        loop, done = self._loop, self._thread_done
        if loop is not None and done is not None and not loop.is_closed():
            try:
                loop.call_soon_threadsafe(done.set)
            except RuntimeError:  # the loop is closed
                pass

    def _read(self) -> None:
        try:
            self._read_loop()
        finally:
            self._notify_thread_done()

    def _read_loop(self) -> None:
        grpc, pb = self._grpc, self._pb
        delay = RECONNECT_MIN_S
        fatal_codes = (grpc.StatusCode.UNAUTHENTICATED, grpc.StatusCode.PERMISSION_DENIED,
                       grpc.StatusCode.UNIMPLEMENTED, grpc.StatusCode.INVALID_ARGUMENT)
        while True:
            with self._cond:
                if self._stop_flag:
                    return
            reason, fatal, synced = "the control server ended the connection", False, False
            silent = threading.Event()
            try:
                # Not the operations of the scenario's own writes: they can be one per step, and nothing here
                # uses them.
                request = pb.WatchRequest(
                    values=False,
                    events=False,
                    operations=True,
                    time=True,
                    availability=True,
                    operation_clients_excluded=[SCENARIO_CLIENT],
                )
                call = self._stub.Watch(request, metadata=self._metadata)
                with self._cond:
                    self._call = call
                    stopped = self._stop_flag
                if stopped:
                    call.cancel()
                # A server that takes the call and then says nothing (it hangs) is not waited for for ever.
                watchdog = threading.Timer(SYNC_TIMEOUT_S, lambda: (silent.set(), call.cancel()))
                watchdog.daemon = True
                watchdog.start()
                try:
                    synced = self._follow(call, watchdog)
                finally:
                    watchdog.cancel()
            except grpc.RpcError as e:
                code = e.code()
                reason_meta = trailing_reason(e)
                fatal = code in fatal_codes or (
                    code == grpc.StatusCode.FAILED_PRECONDITION and reason_meta == REASON_RUN_CHANGED
                )
                reason = f"the control server at {self._address} "
                if silent.is_set():
                    reason += f"did not send its state in {SYNC_TIMEOUT_S:g} s"
                elif code == grpc.StatusCode.FAILED_PRECONDITION and reason_meta == REASON_RUN_CHANGED:
                    reason += "switched away from this run (run-changed)"
                elif code in (grpc.StatusCode.UNAUTHENTICATED, grpc.StatusCode.PERMISSION_DENIED):
                    reason += f"refused this GUI server ({code.name}: {e.details()})"
                elif code == grpc.StatusCode.UNAVAILABLE:
                    reason += "cannot be reached"
                else:
                    reason += f"ended the connection ({code.name}: {e.details()})"
            except Exception as e:  # the channel was closed under the call, or a message this side cannot read
                reason = f"reading from the control server failed ({type(e).__name__}: {e})"
            if synced:
                delay = RECONNECT_MIN_S
            with self._cond:
                self._call = None
                if self._stop_flag:
                    return
                self._taken_flag = False
                self._lost = (reason, fatal)
                self._wake()
                if fatal:
                    return
                # Not before the loop has taken the end of this connection: what the next one sends comes after it.
                self._cond.wait_for(lambda: self._taken_flag or self._stop_flag)
                if self._stop_flag:
                    return
                if self._cond.wait_for(lambda: self._stop_flag, timeout=delay):
                    return
            delay = min(delay * 2, RECONNECT_MAX_S)

    def _follow(self, call, watchdog: threading.Timer) -> bool:
        """Reads the stream to its end; whether it got as far as the first full state."""
        synced = False
        dropped_seen = 0
        for update in call:
            kind = update.WhichOneof("kind")
            with self._cond:
                if update.dropped > dropped_seen:
                    dropped_seen = update.dropped
                    self._dropped = True
                if kind == "values":  # not asked for; a server that sends them anyway is taken at its word
                    self._values[update.values.peer] = update.values
                elif kind == "peer":
                    self._peers[update.peer.participant] = update.peer
                    self._removed.discard(update.peer.participant)
                elif kind == "removed_peer":
                    self._peers.pop(update.removed_peer, None)
                    self._values.pop(update.removed_peer, None)
                    self._availability.pop(update.removed_peer, None)
                    self._removed.add(update.removed_peer)
                elif kind == "operation":
                    self._operations[update.operation.seq] = update.operation
                    if len(self._operations) > OPERATIONS_WAITING:  # the loop does not take them: read again later
                        del self._operations[next(iter(self._operations))]
                        self._dropped = True
                elif kind == "availability":
                    self._availability[update.availability.peer] = update.availability
                elif kind == "time":
                    self._time = update.time
                elif kind == "initial_done":
                    watchdog.cancel()
                    self._synced = synced = True
                self._wake()
        return synced


def _time(t) -> TimeInfo:
    return TimeInfo(t.now_ns, t.paused, t.running, t.ended)
