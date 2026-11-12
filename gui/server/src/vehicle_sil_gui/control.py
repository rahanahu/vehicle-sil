"""The launcher's control server (gRPC, docs/control-api.md) as the source of a run's variables.

gRPC is optional: the GUI server runs without it, and then only the variables are missing (load() says why).
The stubs of proto/vehicle_sil/control/v1/control.proto are taken from, in this order: the directory named by
VEHICLE_SIL_CONTROL_STUBS; made now with grpcio-tools from the repository's proto file (into a temporary
directory, removed at exit); python/vehicle_sil_control/_generated of the repository (what
`python -m vehicle_sil_control.generate` makes). So the variables work when the server runs from the repository
(an editable install, `uv sync`); installed elsewhere, the optional dependency `variables` (grpcio, protobuf) is
not enough by itself: the stubs must be given with VEHICLE_SIL_CONTROL_STUBS until they are shipped in the
package.

Threads: one reader thread per source reads the Watch stream (the peers, the operations, the time) and keeps
only the latest of what it reads (per peer, per operation) until the event loop takes it; it never waits for the
loop, and the loop never waits for it. Single calls (the time, a peer's values, a write) are gRPC futures whose
result is passed to the loop.

The values are not taken from the stream: they are read with single calls when a screen watches them
(variables.Variables.refresh), as often as the screens ask and no more. A stream of values comes as fast as the
simulation runs, and what waits in it (in the connection's buffers, which this side cannot bound) would be old
by the time it is read; a read gives the latest sample at that moment, whatever the simulation's speed.

The token of the run is in a file that only this user can read (credentials_file), never in an argument, an
event or a message: every text that comes from the control server is cleaned of it before it is shown.
"""

from __future__ import annotations

import asyncio
import atexit
import importlib
import logging
import math
import os
import re
import secrets
import shutil
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path

from .source import OperationInfo, PeerInfo, Sink, SourceError, TargetInfo, TimeInfo

FEATURE = "control-server"  # of vehicle_sil_launch version --json: run --control and the event below
EVENT = "control_server"  # {"address", "tls", "credentials"}: where the run's control server listens
STUBS_VARIABLE = "VEHICLE_SIL_CONTROL_STUBS"
PROTO = Path("vehicle_sil") / "control" / "v1" / "control.proto"
ROLE_LINE = "operator vehicle-sil-gui {token}\n"  # reads, writes and time; not the stop (that goes by stdin)
ADDRESS = re.compile(r"127\.0\.0\.1:(\d{1,5})")  # what `--control 0` gives: nothing else is connected to
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


def repository() -> Path | None:
    """The repository this file is in, when it is run from one (an editable install)."""
    parents = Path(__file__).resolve().parents  # <root>/gui/server/src/vehicle_sil_gui/control.py
    root = parents[4] if len(parents) > 4 else None
    return root if root is not None and (root / "proto" / PROTO).is_file() else None


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
    stubs = _stubs_directory()
    if str(stubs) not in sys.path:
        sys.path.insert(0, str(stubs))
    try:
        pb = importlib.import_module("vehicle_sil.control.v1.control_pb2")
        rpc = importlib.import_module("vehicle_sil.control.v1.control_pb2_grpc")
    except Exception as e:  # stubs made for another grpcio or protobuf raise their own errors
        raise Unavailable(f"the Control API stubs in {stubs} cannot be loaded ({type(e).__name__}: {e}): make them "
                          "again with `python -m vehicle_sil_control.generate` (docs/control-api.md)") from None
    return Library(grpc, pb, rpc)


def _stubs_directory() -> Path:
    explicit = os.environ.get(STUBS_VARIABLE)
    if explicit:
        if not (Path(explicit) / PROTO.with_name("control_pb2.py")).is_file():
            raise Unavailable(f"{STUBS_VARIABLE}={explicit} has no {PROTO.with_name('control_pb2.py')}: make the "
                              "stubs with `python -m vehicle_sil_control.generate --out <directory>`")
        return Path(explicit)
    root = repository()
    generated = root / "python" / "vehicle_sil_control" / "_generated" if root is not None else None
    if root is not None:
        try:
            from grpc_tools import protoc
        except Exception:
            protoc = None
        if protoc is not None:
            out = Path(tempfile.mkdtemp(prefix="vehicle-sil-gui-stubs-"))
            atexit.register(shutil.rmtree, out, ignore_errors=True)
            code = protoc.main(["protoc", f"-I{root / 'proto'}", f"--python_out={out}", f"--grpc_python_out={out}",
                                str(root / "proto" / PROTO)])
            if code == 0:
                return out
            log.warning("making the Control API stubs failed (protoc exit code %s)", code)
    if generated is not None and (generated / PROTO.with_name("control_pb2.py")).is_file():
        return generated
    where = "this GUI server does not run from the vehicle-sil repository, which has the proto file they are " \
            "made from" if root is None else "grpcio-tools is not installed (`uv sync` installs it)"
    raise Unavailable(f"the Control API stubs are missing ({where}): run the GUI server from the repository after "
                      "`uv sync`, or make the stubs there with `python -m vehicle_sil_control.generate --out "
                      f"<directory>` and set {STUBS_VARIABLE} to that directory")


def credentials_file() -> tuple[Path, str]:
    """A new file with one credential for this server, readable by this user only (POSIX: mode 0600, made so;
    Windows: the user's temporary directory, with the access rights of that directory), and its token. The
    caller removes it (remove) once the launcher has read it, and in any case when the run ends."""
    token = secrets.token_urlsafe(32)
    fd, name = tempfile.mkstemp(prefix="vehicle-sil-gui-control-", suffix=".txt")
    try:
        with os.fdopen(fd, "w", encoding="ascii", newline="\n") as f:
            f.write(ROLE_LINE.format(token=token))
    except BaseException:
        remove(Path(name))
        raise
    return Path(name), token


def remove(path: Path | None) -> bool:
    """Removes the file; whether it is gone (or there was none)."""
    if path is None:
        return True
    try:
        path.unlink()
    except FileNotFoundError:
        return True
    except OSError:
        return False  # held open by another program (Windows): the caller tries again later
    return True


def check_address(address: object) -> str:
    """The address of a control_server event, if it is where `--control 0` listens (127.0.0.1 and a port).
    ValueError otherwise: the token is sent nowhere else."""
    m = ADDRESS.fullmatch(address) if isinstance(address, str) else None
    if m is None or not 0 < int(m.group(1)) <= 65535:
        raise ValueError("the launcher's control_server event names an address that is not 127.0.0.1:<port>")
    return address


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
        out = []
        for value, type_ in zip(self._values, types):
            if type_ == "real":
                x = value.d
                out.append(x if math.isfinite(x) else None)
            elif type_ == "bool":
                out.append(value.b)
            elif type_:
                out.append(value.i)
            else:
                kind = value.WhichOneof("kind")
                x = getattr(value, kind) if kind else None
                out.append(None if isinstance(x, float) and not math.isfinite(x) else x)
        return out


class GrpcSource:
    def __init__(self, library: Library, address: str, token: str | None):
        self._grpc, self._pb = library.grpc, library.pb
        self._address = address
        self._token = token
        self._metadata = (("authorization", f"Bearer {token}"),) if token else None
        self._channel = self._grpc.insecure_channel(address, options=[
            ("grpc.enable_retries", 0),
            ("grpc.enable_http_proxy", 0),
            ("grpc.max_receive_message_length", MAX_MESSAGE),
        ])
        self._stub = library.rpc.ControlServiceStub(self._channel)
        self._loop: asyncio.AbstractEventLoop | None = None
        self._sink: Sink | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._taken = threading.Event()  # the loop has taken a lost connection: the reader may connect again
        self._lock = threading.Lock()
        self._call = None
        self._scheduled = False
        self._closed = False
        # read by the reader, taken by the loop (under _lock)
        self._peers: dict[str, object] = {}
        self._removed: set[str] = set()
        self._values: dict[str, object] = {}
        self._operations: dict[int, object] = {}
        self._time = None
        self._synced = False
        self._dropped = False
        self._lost: tuple[str, bool] | None = None

    # ------------------------------------------------------------ the event loop's side

    def start(self, sink: Sink) -> None:
        self._loop = asyncio.get_running_loop()
        self._sink = sink
        self._thread = threading.Thread(target=self._read, name="vehicle_sil_gui control", daemon=True)
        self._thread.start()

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._stop.set()
        self._taken.set()
        with self._lock:
            call = self._call
        if call is not None:
            call.cancel()
        self._channel.close()  # ends every call
        deadline = time.monotonic() + CLOSE_WAIT_S
        while self._thread is not None and self._thread.is_alive() and time.monotonic() < deadline:
            await asyncio.sleep(0.01)

    async def read_time(self, timeout_s: float) -> TimeInfo:
        return _time(await self._unary(self._stub.GetTime, self._pb.GetTimeRequest(), timeout_s))

    async def read_peers(self, timeout_s: float) -> list[PeerInfo]:
        reply = await self._unary(self._stub.ListPeers, self._pb.ListPeersRequest(), timeout_s)
        return [self._peer(p) for p in reply.peers]

    async def read_values(self, peer: str, timeout_s: float):
        reply = await self._unary(self._stub.GetValues, self._pb.GetValuesRequest(peer=peer), timeout_s,
                                  missing=None)
        if reply is None or not reply.has_values:
            return None
        return _Snapshot(reply.incarnation, reply.snapshot)

    async def read_operation(self, seq: int, timeout_s: float) -> OperationInfo | None:
        reply = await self._unary(self._stub.GetOperation, self._pb.GetOperationRequest(seq=seq), timeout_s,
                                  missing=None)
        return None if reply is None else self._operation(reply)

    async def write(self, writes: list[tuple[str, str, object]], at_ns: int | None, late: str,
                    timeout_s: float) -> int:
        pb = self._pb
        request = pb.WriteRequest(late=pb.LATE_POLICY_APPLY_NEXT if late == "apply" else pb.LATE_POLICY_REJECT)
        if at_ns is not None:
            request.at_ns = at_ns
        for peer, name, value in writes:
            if isinstance(value, bool):
                request.writes.add(peer=peer, name=name, value=pb.Value(b=value))
            elif isinstance(value, int):
                request.writes.add(peer=peer, name=name, value=pb.Value(i=value))
            else:
                request.writes.add(peer=peer, name=name, value=pb.Value(d=value))
        return (await self._unary(self._stub.Write, request, timeout_s)).seq

    _NO_DEFAULT = object()

    async def _unary(self, method, request, timeout_s: float, missing: object = _NO_DEFAULT):
        """One call, without blocking the loop. missing: what NOT_FOUND gives (else it is refused)."""
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
        return text.replace(self._token, "<token>") if self._token else text

    def _take(self) -> None:
        """On the loop: passes on what the reader has read since the last time."""
        with self._lock:
            self._scheduled = False
            peers, self._peers = self._peers, {}
            removed, self._removed = self._removed, set()
            values, self._values = self._values, {}
            operations, self._operations = self._operations, {}
            time_, self._time = self._time, None
            synced, self._synced = self._synced, False
            dropped, self._dropped = self._dropped, False
            lost, self._lost = self._lost, None
        sink = self._sink
        if self._closed or sink is None:
            self._taken.set()
            return
        try:
            if peers or removed:
                sink.on_peers([self._peer(p) for p in peers.values()], sorted(removed))
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
                self._taken.set()

    def _peer(self, p) -> PeerInfo:
        pb = self._pb
        types = {pb.VALUE_TYPE_BOOL: "bool", pb.VALUE_TYPE_INTEGER: "integer", pb.VALUE_TYPE_REAL: "real",
                 pb.VALUE_TYPE_ENUM: "enum"}
        roles = {pb.CAUSALITY_INPUT: "input", pb.CAUSALITY_OUTPUT: "output", pb.CAUSALITY_PARAMETER: "parameter"}
        variables = [{"name": v.name, "type": types.get(v.type, ""), "role": roles.get(v.causality, "output"),
                      "unit": v.unit, "min": v.min if v.HasField("min") else None,
                      "max": v.max if v.HasField("max") else None, "step": v.step if v.HasField("step") else None,
                      "description": v.description, "enum": [(i.name, i.value) for i in v.items]}
                     for v in p.schema]
        return PeerInfo(p.participant, p.incarnation, p.connected, variables)

    def _operation(self, o) -> OperationInfo:
        pb = self._pb
        targets = []
        for t in o.targets:
            try:
                status = pb.TargetStatus.Name(t.status)[len("TARGET_STATUS_"):].lower()
            except ValueError:
                status = "unknown"
            if status == "unspecified":
                status = "unknown"
            targets.append(TargetInfo(t.peer, status, t.applied_at_ns if t.HasField("applied_at_ns") else None,
                                      t.late, t.reason))
        return OperationInfo(o.seq, o.done, targets)

    # ------------------------------------------------------------ the reader thread

    def _wake(self) -> None:
        """Under _lock: asks the loop to take what was read, once however much is read before it does."""
        if self._scheduled:
            return
        self._scheduled = True
        try:
            self._loop.call_soon_threadsafe(self._take)
        except RuntimeError:  # the loop is closed
            self._stop.set()

    def _read(self) -> None:
        grpc, pb = self._grpc, self._pb
        delay = RECONNECT_MIN_S
        fatal_codes = (grpc.StatusCode.UNAUTHENTICATED, grpc.StatusCode.PERMISSION_DENIED,
                       grpc.StatusCode.UNIMPLEMENTED, grpc.StatusCode.INVALID_ARGUMENT)
        while not self._stop.is_set():
            reason, fatal, synced = "the control server ended the connection", False, False
            silent = threading.Event()
            try:
                call = self._stub.Watch(pb.WatchRequest(values=False, events=False, operations=True, time=True),
                                        metadata=self._metadata)
                with self._lock:
                    self._call = call
                if self._stop.is_set():
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
                fatal = code in fatal_codes
                reason = f"the control server at {self._address} "
                if silent.is_set():
                    reason += f"did not send its state in {SYNC_TIMEOUT_S:g} s"
                elif code in (grpc.StatusCode.UNAUTHENTICATED, grpc.StatusCode.PERMISSION_DENIED):
                    reason += f"refused this GUI server ({code.name}: {e.details()})"
                elif code == grpc.StatusCode.UNAVAILABLE:
                    reason += "cannot be reached"
                else:
                    reason += f"ended the connection ({code.name}: {e.details()})"
            except Exception as e:  # the channel was closed under the call, or a message this side cannot read
                reason = f"reading from the control server failed ({type(e).__name__}: {e})"
            if self._stop.is_set():
                return
            if synced:
                delay = RECONNECT_MIN_S
            self._taken.clear()
            with self._lock:
                self._call = None
                self._lost = (reason, fatal)
                self._wake()
            if fatal:
                return
            # Not before the loop has taken the end of this connection: what the next one sends comes after it.
            while not self._taken.wait(0.1):
                if self._stop.is_set():
                    return
            if self._stop.wait(delay):
                return
            delay = min(delay * 2, RECONNECT_MAX_S)

    def _follow(self, call, watchdog: threading.Timer) -> bool:
        """Reads the stream to its end; whether it got as far as the first full state."""
        synced = False
        dropped_seen = 0
        for update in call:
            kind = update.WhichOneof("kind")
            with self._lock:
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
                    self._removed.add(update.removed_peer)
                elif kind == "operation":
                    self._operations[update.operation.seq] = update.operation
                    if len(self._operations) > OPERATIONS_WAITING:  # the loop does not take them: read again later
                        del self._operations[next(iter(self._operations))]
                        self._dropped = True
                elif kind == "time":
                    self._time = update.time
                elif kind == "initial_done":
                    watchdog.cancel()
                    self._synced = synced = True
                self._wake()
        return synced


def _time(t) -> TimeInfo:
    return TimeInfo(t.now_ns, t.paused, t.running, t.ended)
