"""A stand-in for the control server of vehicle_sil_launch (gRPC, docs/control-api.md), for the GUI server's tests.

The fake launcher starts it when it is run with --control (fake_launcher.py), and tests start it by themselves.
It serves the part of ControlService that the GUI server uses (Ping, ListPeers, GetValues, GetTime, Write,
GetOperation, Watch), with the contract of the real one where the GUI server relies on it:

- Watch sends the peers, their last values and the time, then initial_done, then the changes; of values and of
  the time only the latest (a slow reader gets fewer, never older ones after newer ones);
- while the time stands still at P, the values of every step that began before P are there, and the step that
  begins at P has run too (step() is called before set_time(); the real launcher's ECUs have run it or are
  running it);
- a write is queued at once and applied in the first step that begins at or after its time (the next step
  without one). A write whose time is not after the last step that ran is late (libs/control: "a step at or
  after its time ran before the command arrived"): at the next step it gets the result late and is not made,
  or, when asked, is made in that step with late true. So standing still at P, a write for P is late, as with the real one;
- a parameter is refused once a step has run; an operation's number grows by one from 1;
- a call needs the role its credential has: reading the role viewer, writing operator (as the real server).

Each peer has the three variables of fake_launcher.variables(): <name>.demand (a real input, 0 to 100),
<name>.speed (an integer output: ten times the demand, plus the step's time in ms) and <name>.mode (an enum
parameter). Needs grpcio and the stubs of the Control API: those in VEHICLE_SIL_CONTROL_STUBS (conftest.py makes
them), else it makes them itself with grpcio-tools.
"""

from __future__ import annotations

import os
import sys
import threading
from concurrent import futures

NS_PER_MS = 1_000_000


def load():
    import grpc

    stubs = os.environ.get("VEHICLE_SIL_CONTROL_STUBS") or _make_stubs()
    if stubs not in sys.path:
        sys.path.insert(0, stubs)
    from vehicle_sil.control.v1 import control_pb2 as pb
    from vehicle_sil.control.v1 import control_pb2_grpc as rpc
    return grpc, pb, rpc


def _make_stubs() -> str:
    """The stubs, made now from the repository's proto file (needs grpcio-tools) into a directory that is removed
    at exit: for a fake launcher that is started without VEHICLE_SIL_CONTROL_STUBS (the e2e of gui/web)."""
    import atexit
    import shutil
    import tempfile
    from pathlib import Path

    from grpc_tools import protoc

    proto = Path(__file__).resolve().parents[3] / "proto"
    out = tempfile.mkdtemp(prefix="fake-control-stubs-")
    atexit.register(shutil.rmtree, out, ignore_errors=True)
    code = protoc.main(["protoc", f"-I{proto}", f"--python_out={out}", f"--grpc_python_out={out}",
                        str(proto / "vehicle_sil" / "control" / "v1" / "control.proto")])
    if code != 0:
        raise RuntimeError(f"protoc failed ({code})")
    return out


class FakeControl:
    def __init__(self, peers: dict[str, list[dict]], token: str | None = None, role: str = "operator"):
        """peers: the variables of each (the dicts of a check's `variables`). token: every call must carry it;
        role: what the token may do (viewer: read; operator, admin: write too)."""
        self.grpc, self.pb, self.rpc = load()
        self.token = token
        self.role = role
        self.last_step_ms: int | None = None  # the time the last step that ran began at
        self.requests: list = []  # the WatchRequests taken
        self.changed = threading.Condition()
        self.version = 0
        self.peers = peers
        self.values = {name: [self._initial(v) for v in variables] for name, variables in peers.items()}
        self.samples: dict[str, tuple[int, int, list, int]] = {}  # peer -> (time_ns, sequence, values, version)
        self.operations: dict[int, dict] = {}
        self.queued: list[dict] = []
        self.time = {"now_ns": 0, "paused": False, "running": False, "ended": False, "version": 0}
        self.steps = 0
        self.seq = 0
        self.stopped = False
        self.calls: list[str] = []
        self.server = None
        self.address = ""

    @staticmethod
    def _initial(variable: dict):
        return {"bool": False, "integer": 0, "real": 0.0, "enum": 0}[variable["type"]]

    # ------------------------------------------------------------ the launcher's side

    def start(self, address: str = "127.0.0.1:0") -> str:
        grpc, rpc = self.grpc, self.rpc
        outer = self

        class Service(rpc.ControlServiceServicer):
            def Ping(self, request, context):
                outer._enter("Ping", context)
                return outer.pb.PingReply(text=request.text, server_version="fake")

            def ListPeers(self, request, context):
                outer._enter("ListPeers", context)
                return outer.pb.ListPeersReply(peers=[outer._peer(name) for name in outer.peers])

            def GetValues(self, request, context):
                outer._enter("GetValues", context)
                with outer.changed:
                    sample = outer.samples.get(request.peer)
                    if request.peer not in outer.peers:
                        context.abort(grpc.StatusCode.NOT_FOUND, f"no peer {request.peer}")
                    if sample is None:
                        return outer.pb.GetValuesReply(has_values=False)
                    return outer.pb.GetValuesReply(has_values=True, incarnation="1",
                                                   snapshot=outer._snapshot(request.peer, sample))

            def GetTime(self, request, context):
                outer._enter("GetTime", context)
                with outer.changed:
                    return outer._time()

            def Write(self, request, context):
                outer._enter("Write", context, writes=True)
                return outer.pb.WriteReply(seq=outer._write(request, context))

            def GetOperation(self, request, context):
                outer._enter("GetOperation", context)
                with outer.changed:
                    op = outer.operations.get(request.seq)
                    if op is None:
                        context.abort(grpc.StatusCode.NOT_FOUND, f"no operation {request.seq}")
                    return outer._operation(op)

            def Watch(self, request, context):
                outer._enter("Watch", context)
                yield from outer._watch(request, context)

        self.server = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
        rpc.add_ControlServiceServicer_to_server(Service(), self.server)
        port = self.server.add_insecure_port(address)
        self.server.start()
        self.address = f"127.0.0.1:{port}"
        return self.address

    def step(self, start_ms: int) -> None:
        """A step that began at start_ms has run: the writes due are applied, and every peer is read."""
        with self.changed:
            self.steps += 1
            self.version += 1
            self.last_step_ms = start_ms
            due = [q for q in self.queued if q["at_ms"] is None or q["at_ms"] <= start_ms]
            self.queued = [q for q in self.queued if q not in due]
            for q in sorted(due, key=lambda q: q["seq"]):
                op = self.operations[q["seq"]]
                late = q["late"]
                for target in op["targets"]:
                    if target["status"] != "QUEUED":
                        continue
                    if late and not q["apply_late"]:
                        target.update(status="LATE", late=True, reason="its time had passed")
                        continue
                    for name, value in q["writes"][target["peer"]]:
                        index = [v["name"] for v in self.peers[target["peer"]]].index(name)
                        self.values[target["peer"]][index] = value
                    target.update(status="APPLIED", applied_at_ns=start_ms * NS_PER_MS, late=late)
                op["done"] = True
                op["version"] = self.version
            for name, variables in self.peers.items():
                values = self.values[name]
                for i, v in enumerate(variables):
                    if v["name"].endswith(".speed"):
                        demand = next(values[j] for j, w in enumerate(variables) if w["name"].endswith(".demand"))
                        values[i] = int(demand * 10) + start_ms
                self.samples[name] = (start_ms * NS_PER_MS, self.steps, list(values), self.version)
            self.changed.notify_all()

    def set_time(self, sim_ms: int, paused: bool, running: bool = True, ended: bool = False) -> None:
        with self.changed:
            new = {"now_ns": sim_ms * NS_PER_MS, "paused": paused, "running": running, "ended": ended}
            if all(self.time[k] == v for k, v in new.items()):
                return
            self.version += 1
            self.time = {**new, "version": self.version}
            if ended:  # what was not applied by now is cancelled
                for q in self.queued:
                    op = self.operations[q["seq"]]
                    for target in op["targets"]:
                        if target["status"] == "QUEUED":
                            target.update(status="CANCELLED", reason="the simulation ended")
                    op["done"], op["version"] = True, self.version
                self.queued = []
            self.changed.notify_all()

    def stop(self) -> None:
        with self.changed:
            self.stopped = True
            self.changed.notify_all()
        if self.server is not None:
            self.server.stop(0.5).wait(2)

    # ------------------------------------------------------------ the calls

    def _enter(self, name: str, context, writes: bool = False) -> None:
        grpc = self.grpc
        self.calls.append(name)
        if self.token is not None:
            given = dict(context.invocation_metadata()).get("authorization", "")
            if given != f"Bearer {self.token}":
                context.abort(grpc.StatusCode.UNAUTHENTICATED, "missing or wrong token")
            if writes and self.role not in ("operator", "admin"):
                context.abort(grpc.StatusCode.PERMISSION_DENIED, f"{name} needs the role operator")

    def _peer(self, name: str):
        pb = self.pb
        types = {"bool": pb.VALUE_TYPE_BOOL, "integer": pb.VALUE_TYPE_INTEGER, "real": pb.VALUE_TYPE_REAL,
                 "enum": pb.VALUE_TYPE_ENUM}
        roles = {"input": pb.CAUSALITY_INPUT, "output": pb.CAUSALITY_OUTPUT, "parameter": pb.CAUSALITY_PARAMETER}
        schema = []
        for v in self.peers[name]:
            info = pb.VariableInfo(name=v["name"], type=types[v["type"]], causality=roles[v["causality"]],
                                   unit=v["unit"], description=v["description"],
                                   items=[pb.EnumItem(name=i["name"], value=i["value"]) for i in v["items"]])
            for key in ("min", "max", "step"):
                if v[key] is not None:
                    setattr(info, key, v[key])
            schema.append(info)
        return pb.PeerInfo(participant=name, incarnation="1", connected=not self.time["ended"], schema=schema,
                           has_agent=True, simulation_state="Running")

    def _snapshot(self, name: str, sample: tuple):
        pb = self.pb
        time_ns, sequence, values, _ = sample
        out = []
        for v, x in zip(self.peers[name], values):
            out.append(pb.Value(b=x) if v["type"] == "bool" else pb.Value(d=x) if v["type"] == "real"
                       else pb.Value(i=x))
        return pb.Snapshot(time_ns=time_ns, sequence=sequence, values=out)

    def _time(self):
        t = self.time
        return self.pb.TimeState(available=True, now_ns=t["now_ns"], paused=t["paused"], running=t["running"],
                                 ended=t["ended"])

    def _operation(self, op: dict):
        pb = self.pb
        targets = []
        for t in op["targets"]:
            target = pb.TargetResult(peer=t["peer"], status=getattr(pb, "TARGET_STATUS_" + t["status"]),
                                     late=t.get("late", False), reason=t.get("reason", ""))
            if t.get("applied_at_ns") is not None:
                target.applied_at_ns = t["applied_at_ns"]
            targets.append(target)
        return pb.Operation(seq=op["seq"], targets=targets, done=op["done"])

    def _write(self, request, context) -> int:
        grpc = self.grpc
        if not request.writes:
            context.abort(grpc.StatusCode.INVALID_ARGUMENT, "a write needs at least one value")
        with self.changed:
            self.seq += 1
            self.version += 1
            by_peer: dict[str, list] = {}
            for w in request.writes:
                by_peer.setdefault(w.peer, []).append((w.name, getattr(w.value, w.value.WhichOneof("kind"))))
            targets, queued = [], False
            for peer, writes in by_peer.items():
                variables = {v["name"]: v for v in self.peers.get(peer, [])}
                problem = None
                if peer not in self.peers:
                    problem = f"no peer {peer}"
                elif self.time["ended"]:
                    problem = "the simulation has ended"
                for name, _ in writes:
                    v = variables.get(name)
                    if problem is None and v is None:
                        problem = f"no variable {name}"
                    elif problem is None and v["causality"] == "output":
                        problem = f"{name} is an output"
                    elif problem is None and v["causality"] == "parameter" and self.steps > 0:
                        problem = f"{name} is a parameter: it can be written before the first step only"
                if problem is not None:
                    targets.append({"peer": peer, "status": "INVALID", "reason": problem})
                else:
                    targets.append({"peer": peer, "status": "QUEUED"})
                    queued = True
            at_ms = request.at_ns // NS_PER_MS if request.HasField("at_ns") else None
            # Late: a step at or after its time ran before the write came. The ECU finds that out when it takes
            # the write, at its next step: until then the write is queued, as any other.
            late = at_ms is not None and self.last_step_ms is not None and self.last_step_ms >= at_ms
            op = {"seq": self.seq, "targets": targets, "done": not queued, "version": self.version}
            self.operations[self.seq] = op
            if queued:
                self.queued.append({"seq": self.seq, "writes": by_peer, "at_ms": None if late else at_ms,
                                    "late": late, "apply_late": request.late == self.pb.LATE_POLICY_APPLY_NEXT})
            self.changed.notify_all()
            return self.seq

    def _watch(self, request, context):
        pb = self.pb
        self.requests.append(request)
        with self.changed:
            seen = self.version
            first = [pb.Update(peer=self._peer(name)) for name in self.peers]
            if request.values:
                first += [pb.Update(values=pb.PeerValues(peer=name, incarnation="1",
                                                         snapshot=self._snapshot(name, s)))
                          for name, s in self.samples.items()]
            if request.time:
                first.append(pb.Update(time=self._time()))
            first.append(pb.Update(initial_done=True))
        yield from first
        while context.is_active():
            with self.changed:
                if self.stopped:
                    return
                if self.version == seen:
                    self.changed.wait(0.1)
                updates = []
                for op in self.operations.values():
                    if request.operations and op["version"] > seen:
                        updates.append(pb.Update(operation=self._operation(op)))
                for name, s in self.samples.items():
                    if request.values and s[3] > seen:
                        updates.append(pb.Update(values=pb.PeerValues(peer=name, incarnation="1",
                                                                      snapshot=self._snapshot(name, s))))
                if request.time and self.time["version"] > seen:
                    updates.append(pb.Update(time=self._time()))
                seen = self.version
            yield from updates
