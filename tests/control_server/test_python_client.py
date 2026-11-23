# SPDX-License-Identifier: MIT
"""The Python client and the CLI.

- LaunchedTest: against vehicle_sil_launch run --control with the two ECUs of examples/ecu_main
  (started here).
- ConversionTest: what the client makes of variables and values, without a server.
- ReleaseTest: the release check of the first call, against a server here that answers Ping alone.
- RunBindingTest: a Control bound to a run (run=..., for_current_run()), against a server here that
  checks the run as the launcher's does.
(TLS, tokens and roles, the audit log and the CLI with them: test_launcher.py.)

Environment: VEHICLE_SIL_LAUNCH_EXE, VEHICLE_SIL_EXAMPLE_DIR (examples/ecu_main in the build tree),
VEHICLE_SIL_LAUNCH_SCRATCH (a directory to write into), PYTHONPATH with the repository's python/
directory, VEHICLE_SIL_CONTROL_STUBS (generated stubs).
"""
import os
import pathlib
import subprocess
import sys
import threading
import time
import unittest
import warnings

from vehicle_sil_control import (Control, ControlError, ControlWarning, EventInfo, Launch, Operation, Peer,
                                 SimulationState, Values, ms)

EXE = ".exe" if os.name == "nt" else ""


def first(test, updates, pred, timeout=20.0):
    """The first of `updates` for which `pred` holds; fails after `timeout` s instead of hanging
    (the caller closes the client, which ends the iteration left behind)."""
    found = []

    def run():
        for u in updates:
            if pred(u):
                found.append(u)
                return

    reader = threading.Thread(target=run, daemon=True)
    reader.start()
    reader.join(timeout)
    test.assertTrue(found, f"no matching update within {timeout} s")
    return found[0]


class LaunchedTest(unittest.TestCase):
    """The examples/ecu_main simulation (Engine: engine.throttle, engine.speed; Dashboard:
    dashboard.engine_speed, received on J1939) under the launcher, held before its first step."""

    @classmethod
    def setUpClass(cls):
        example = pathlib.Path(os.environ["VEHICLE_SIL_EXAMPLE_DIR"])
        scratch = pathlib.Path(os.environ["VEHICLE_SIL_LAUNCH_SCRATCH"])
        scratch.mkdir(parents=True, exist_ok=True)
        engine = (example / "engine" / f"vehicle_sil_engine_ecu{EXE}").as_posix()
        dashboard = (example / "dashboard" / f"vehicle_sil_dashboard_ecu{EXE}").as_posix()
        config = scratch / "python_client.yaml"
        config.write_text("vehicle_sil_simulation: 2\nnetworks:\n  - name: J1939_CAN\n    kind: can\n"
                          f"participants:\n  - name: Engine\n    command: ['{engine}']\n    step_ms: 10\n"
                          f"  - name: Dashboard\n    command: ['{dashboard}']\n    step_ms: 5\n")
        cls.launch = Launch(os.environ["VEHICLE_SIL_LAUNCH_EXE"], config, start_paused=True, arguments=["--stop-at-ms", "600000"])
        cls.addClassCleanup(cls.launch.close)
        cls.address = cls.launch.address
        cls.c = Control(cls.address)
        cls.addClassCleanup(cls.c.close)

    def cli(self, *args):
        out = subprocess.run([sys.executable, "-m", "vehicle_sil_control", "--server", self.address, *args],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stderr)
        return out.stdout

    def test_a_scenario(self):
        c = self.c
        # It stands still before its first step, with both agents connected.
        t = c.sim.wait_paused(60)
        self.assertTrue(t.available and t.paused and not t.running)
        self.assertEqual(t.now, 0)
        peers = {p.participant: p for p in c.peers()}
        self.assertEqual(set(peers), {"Engine", "Dashboard"})
        self.assertTrue(all(p.connected for p in peers.values()))
        throttle = next(v for v in peers["Engine"].schema if v.name == "engine.throttle")
        self.assertEqual((throttle.type, throttle.causality, throttle.unit, throttle.min, throttle.max),
                         ("real", "input", "%", 0.0, 100.0))
        shown = next(v for v in peers["Dashboard"].schema if v.name == "dashboard.engine_speed")
        self.assertEqual((shown.type, shown.causality, shown.unit), ("integer", "output", "rpm"))
        # An ECU of ecu_main runs on the simulation's time; vehicle-sil's variables are marked.
        self.assertTrue(peers["Engine"].virtual_time)
        builtin = {v.name: v.builtin for v in peers["Engine"].schema}
        self.assertEqual((builtin.get("ecu.connected"), builtin["engine.throttle"]), (True, False))

        # On to 10 ms: it runs.
        c.sim.run_until(ms(10))
        t = c.sim.wait_paused(20)
        self.assertEqual((t.now, t.running, t.ended), (ms(10), True, False))

        # ListPeers and Watch report the same peers alike (with the agent and the SIL Kit state).
        listed = {p.participant: p for p in c.peers()}
        watched = {}
        with Control(self.address) as w:
            def note(u):
                if isinstance(u, Peer):
                    watched[u.participant] = u
                return u == ("initial_done",)
            first(self, w.watch(values=False, events=False, operations=False), note)
        self.assertEqual(watched, listed)
        self.assertTrue(all(p.has_agent and p.simulation_state == "Running" for p in listed.values()))

        # A watcher in the background.
        seen = []
        stop = threading.Event()

        def watch():
            with Control(self.address) as w:
                for u in w.watch():
                    seen.append(u)
                    if stop.is_set():
                        break

        watcher = threading.Thread(target=watch, daemon=True)
        watcher.start()

        # Schedule a write at 100 ms, make sure it is queued, then run.
        seq = c.write({"Engine": {"engine.throttle": 80.0}}, at=ms(100))
        op = c.wait(seq, until="queued")
        self.assertEqual({t.status for t in op.targets}, {"queued"})
        self.assertEqual((op.target("Engine").applied_writes, op.target("Engine").failed_write), (None, None))
        c.sim.run_until(ms(600))
        self.assertEqual(c.sim.wait_paused(20).now, ms(600))
        op = c.wait(seq)
        self.assertTrue(op.done)
        self.assertEqual(op.target("Engine").applied_at, ms(100))
        self.assertEqual((op.target("Engine").applied_writes, op.target("Engine").failed_write), (1, None))

        # The second of two writes is refused (an output): none took effect, and the client hears which.
        refused = c.wait(c.write({"Engine": {"engine.throttle": 30.0, "engine.speed": 5}}))
        target = refused.target("Engine")
        self.assertEqual((target.status, target.applied_writes, target.failed_write), ("invalid", 0, 1))
        self.assertIn("engine.speed", target.reason)

        # The engine sped up (towards 2400 rpm) and the dashboard received it over J1939.
        deadline = time.time() + 10
        while time.time() < deadline:
            v = c.values("Dashboard")
            if v and v.time >= ms(590):
                break
            time.sleep(0.05)
        engine = c.values("Engine").values
        dashboard = c.values("Dashboard").values
        self.assertEqual(engine["engine.throttle"], 80.0)
        self.assertEqual(engine["engine.speed"], 2400)
        self.assertGreater(dashboard["dashboard.engine_speed"], 2000)
        self.assertEqual(engine["j1939.J1939_CAN.node0.address_state"], 3)  # claimed

        # The simulation's state: the required participants run, nobody failed, not over.
        with Control(self.address) as w:
            u = first(self, w.watch(values=False, events=False, operations=False, simulation=True),
                      lambda u: isinstance(u, SimulationState))
        self.assertTrue(u.available)
        self.assertEqual(u.participants, {"Engine": "Running", "Dashboard": "Running"})
        self.assertEqual((u.failed_participant, u.ended, u.error), ("", False, ""))

        # A refused call raises ControlError (an operation that an agent refuses is a result instead).
        with self.assertRaises(ControlError):
            c.write({})
        stop.set()
        watcher.join(10)
        self.assertTrue(any(isinstance(u, Peer) for u in seen))
        self.assertTrue(any(isinstance(u, Values) for u in seen))
        self.assertTrue(any(isinstance(u, Operation) and u.seq == seq and u.done for u in seen))
        self.assertTrue(any(isinstance(u, EventInfo) and "claimed" in u.text for u in seen))

    def test_b_cli(self):
        self.assertIn("Engine\tconnected", self.cli("peers"))
        self.assertIn("engine.throttle", self.cli("show", "Engine"))
        out = self.cli("set", "Engine", "engine.throttle=30", "--at-ms", "700", "--wait", "queued")
        self.assertIn("Engine: queued", out)
        out = self.cli("run-until", "800")
        self.assertIn("paused at 800 ms", out)
        self.assertIn("speed max", self.cli("time"))
        out = self.cli("watch", "--peer", "Dashboard", "--count", "1")
        self.assertIn("peer Dashboard connected", out)
        out = self.cli("set", "Engine", "engine.throttle=500")
        self.assertIn("Engine: invalid", out)
        self.assertIn("out of range", out)
        self.assertNotIn("writes took effect", out)  # one write: nothing to tell apart
        out = self.cli("set", "Engine", "engine.throttle=30", "engine.speed=5")
        self.assertIn("Engine: invalid", out)
        self.assertIn(", 0 of 2 writes took effect, refused: engine.speed", out)

    def test_d_the_run(self):
        # run (one run, the run is not required): the server reports its run; a Control bound to it
        # is served, one bound to another run is refused, one bound to none is served.
        import grpc
        run = self.c.current_run()
        self.assertTrue(run)
        with Control.for_current_run(self.address) as bound:
            self.assertEqual(bound.run, run)
            bound.sim.time()
        with Control(self.address, run=run + "-earlier") as other:
            with self.assertRaises(ControlError) as refused:
                other.sim.time()
            self.assertEqual((refused.exception.code, refused.exception.reason),
                             (grpc.StatusCode.FAILED_PRECONDITION, "run-changed"))
        self.c.sim.time()
        # The CLI: --run binds the command to a run.
        self.assertIn("paused at", self.cli("--run", run, "time"))
        out = subprocess.run([sys.executable, "-m", "vehicle_sil_control", "--server", self.address, "--run", run + "-earlier",
                              "time"], capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 1, out.stderr)
        self.assertIn(f"the call is for the run '{run}-earlier'", out.stderr)

    def test_c_a_warning_comes_on_every_call(self):
        from vehicle_sil_control._stubs import pb
        with Control(self.address) as c:
            c._call = lambda method, request, timeout=None: pb.ListPeersReply(simulation_error="the registry is gone")
            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter("default")
                filters = list(warnings.filters)
                for _ in range(3):
                    c.peers()
                self.assertEqual(warnings.filters, filters)  # the user's filters stay as they are
            self.assertEqual([str(w.message) for w in caught], ["ListPeers: the registry is gone"] * 3)
            self.assertTrue(all(w.category is ControlWarning and w.filename == __file__ for w in caught))
            with warnings.catch_warnings():
                warnings.simplefilter("error", ControlWarning)
                with self.assertRaises(ControlWarning):
                    c.peers()
            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter("ignore", ControlWarning)
                c.peers()
            self.assertEqual(caught, [])


class ConversionTest(unittest.TestCase):
    """What the client makes of the server's messages, without a server."""

    def test_variables_and_values(self):
        from vehicle_sil_control import client
        from vehicle_sil_control._stubs import pb
        lamp = client._variable(pb.VariableInfo(name="cabin.lamp", type=pb.VALUE_TYPE_ENUM, causality=pb.CAUSALITY_INPUT,
                                                items=[pb.EnumItem(name="off", value=0), pb.EnumItem(name="high", value=2)]))
        self.assertEqual((lamp.type, lamp.causality, lamp.items), ("enum", "input", {"off": 0, "high": 2}))
        self.assertTrue(lamp.writable)
        brake = client._variable(pb.VariableInfo(name="cabin.parking_brake", type=pb.VALUE_TYPE_BOOL,
                                                 causality=pb.CAUSALITY_OUTPUT))
        self.assertEqual((brake.type, brake.writable), ("bool", False))
        for value in (True, False, 7, 2.5):
            back = client._value(client._to_value(value))
            self.assertEqual((type(back), back), (type(value), value))

    def test_which_writes_took_effect(self):
        from vehicle_sil_control import client
        from vehicle_sil_control._stubs import pb
        op = client._operation(pb.Operation(seq=4, done=True, targets=[
            pb.TargetResult(peer="Engine", status=pb.TARGET_STATUS_REJECTED, reason="engine.fault: a setter threw: no",
                            applied_writes=1, failed_write=1),
            pb.TargetResult(peer="Body", status=pb.TARGET_STATUS_INVALID, applied_writes=0),
            pb.TargetResult(peer="Gate", status=pb.TARGET_STATUS_UNKNOWN)]))
        self.assertEqual([(t.status, t.applied_writes, t.failed_write) for t in op.targets],
                         [("rejected", 1, 1), ("invalid", 0, None), ("unknown", None, None)])


def ping_server(test, release):
    """A gRPC server on a free local port whose ControlService answers Ping alone (with `release` as
    the server's vehicle-sil release) and every other call with UNIMPLEMENTED. Returns (its address,
    the list of the Ping requests' texts)."""
    from concurrent import futures
    import grpc
    from vehicle_sil_control._stubs import pb, rpc
    pings = []

    class Service(rpc.ControlServiceServicer):
        def Ping(self, request, context):
            pings.append(request.text)
            return pb.PingReply(text=request.text, server_version="vehicle-sil control v1", vehicle_sil=release)

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=2))
    rpc.add_ControlServiceServicer_to_server(Service(), server)
    port = server.add_insecure_port("127.0.0.1:0")
    server.start()
    test.addCleanup(server.stop, None)
    return f"127.0.0.1:{port}", pings


class ReleaseTest(unittest.TestCase):
    """The client pings once before its first call and refuses a server of another release."""

    def test_the_same_release_is_checked_once(self):
        import vehicle_sil_control
        address, pings = ping_server(self, vehicle_sil_control.__version__)
        c = Control(address)
        self.addCleanup(c.close)
        self.assertEqual(pings, [])  # making a Control does not connect
        self.assertEqual(c.ping("one"), "vehicle-sil control v1")
        self.assertEqual(c.ping("two"), "vehicle-sil control v1")
        self.assertEqual(pings, ["", "one", "two"])  # the check, then the calls

    def refused(self, release):
        import grpc
        address, pings = ping_server(self, release)
        with Control(address) as c:
            for call in (c.peers, lambda: next(iter(c.watch()))):  # a call, and a watch
                with self.assertRaises(ControlError) as refused:
                    call()
                self.assertEqual(refused.exception.code, grpc.StatusCode.FAILED_PRECONDITION)
        self.assertEqual(pings, ["", ""])  # refused at the check, before the call; asked again next time
        return str(refused.exception)

    def test_another_release_is_refused(self):
        import vehicle_sil_control
        self.assertEqual(self.refused("0.0.1"),
                         f"FAILED_PRECONDITION: the control server is vehicle-sil 0.0.1; this vehicle_sil_control is "
                         f"{vehicle_sil_control.__version__}: use the vehicle_sil_control of the same release as the "
                         "launcher")

    def test_a_server_that_does_not_report_its_release_is_refused(self):
        import vehicle_sil_control
        self.assertEqual(self.refused(""),
                         "FAILED_PRECONDITION: the control server does not report its vehicle-sil release (a 0.1.0 "
                         "launcher, or one built before this check); this vehicle_sil_control is "
                         f"{vehicle_sil_control.__version__}: use the vehicle_sil_control that came with the launcher")

    def test_stubs_from_before_the_field_are_refused_without_a_call(self):
        import types
        from unittest import mock
        import grpc
        from vehicle_sil_control import client
        address, pings = ping_server(self, "")
        old = types.SimpleNamespace(DESCRIPTOR=types.SimpleNamespace(fields_by_name={}))  # a PingReply without the field
        with Control(address) as c, mock.patch.object(client.pb, "PingReply", old):
            with self.assertRaises(ControlError) as refused:
                c.peers()
        self.assertEqual(refused.exception.code, grpc.StatusCode.FAILED_PRECONDITION)
        self.assertIn("are older than this vehicle_sil_control (their PingReply has no vehicle_sil): make them again "
                      "with `python -m vehicle_sil_control.generate`", str(refused.exception))
        self.assertEqual(pings, [])


def run_server(test, run, required=False):
    """A gRPC server on a free local port whose ControlService answers Ping (with `run` as its run)
    and GetTime, and refuses a GetTime for another run (or without one, if `required`) as the
    launcher's server does. Returns (its address, the metadata of each call as a dict)."""
    from concurrent import futures
    import grpc
    import vehicle_sil_control
    from vehicle_sil_control._stubs import pb, rpc
    calls = []

    class Service(rpc.ControlServiceServicer):
        def Ping(self, request, context):
            calls.append(("Ping", dict(context.invocation_metadata())))
            return pb.PingReply(text=request.text, server_version="vehicle-sil control v1",
                                vehicle_sil=vehicle_sil_control.__version__, run=run)

        def GetTime(self, request, context):
            metadata = dict(context.invocation_metadata())
            calls.append(("GetTime", metadata))
            asked = metadata.get("vehicle-sil-run")
            why = "run-required" if asked is None and required else "run-changed" if asked not in (None, run) else ""
            if why:
                context.set_trailing_metadata((("vehicle-sil-reason", why),))
                context.abort(grpc.StatusCode.FAILED_PRECONDITION, f"cannot read the time: {why}")
            return pb.TimeState(available=True, paused=True)

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=2))
    rpc.add_ControlServiceServicer_to_server(Service(), server)
    port = server.add_insecure_port("127.0.0.1:0")
    server.start()
    test.addCleanup(server.stop, None)
    return f"127.0.0.1:{port}", calls


class RunBindingTest(unittest.TestCase):
    """The run a Control is bound to goes with every call; a refusal for the run has its reason."""

    def test_the_run_and_the_token_go_with_every_call(self):
        address, calls = run_server(self, "4f2a-1", required=True)
        with Control(address, token="t" * 16, run="4f2a-1") as c:
            self.assertEqual(c.run, "4f2a-1")
            self.assertTrue(c.sim.time().paused)
            self.assertEqual(c.current_run(), "4f2a-1")
        self.assertEqual([name for name, _ in calls], ["Ping", "GetTime", "Ping"])  # the release check first
        for _, metadata in calls:
            self.assertEqual(metadata.get("vehicle-sil-run"), "4f2a-1")
            self.assertEqual(metadata.get("authorization"), "Bearer " + "t" * 16)

    def test_without_a_run_no_run_is_sent(self):
        address, calls = run_server(self, "4f2a-1")
        with Control(address) as c:
            self.assertIsNone(c.run)
            self.assertTrue(c.sim.time().paused)  # served: the run is not required
        self.assertTrue(all("vehicle-sil-run" not in metadata for _, metadata in calls))

    def test_a_refusal_for_the_run_has_its_reason(self):
        import grpc
        address, _ = run_server(self, "4f2a-2", required=True)
        for run, reason in (("4f2a-1", "run-changed"), (None, "run-required")):
            with Control(address, run=run) as c:
                with self.assertRaises(ControlError) as refused:
                    c.sim.time()
                self.assertEqual(refused.exception.code, grpc.StatusCode.FAILED_PRECONDITION)
                self.assertEqual(refused.exception.reason, reason)
                self.assertEqual(c.current_run(), "4f2a-2")  # Ping is not refused: it tells the run now

    def test_for_current_run_binds_to_the_run_ping_reports(self):
        address, calls = run_server(self, "4f2a-3", required=True)
        with Control.for_current_run(address, timeout=5.0) as c:
            self.assertEqual(c.run, "4f2a-3")
            self.assertTrue(c.sim.time().paused)
        self.assertEqual(calls[-1], ("GetTime", calls[-1][1]))
        self.assertEqual(calls[-1][1].get("vehicle-sil-run"), "4f2a-3")
        with self.assertRaises(TypeError):
            Control.for_current_run(address, run="4f2a-3")

    def test_for_current_run_needs_a_run(self):
        import grpc
        address, _ = run_server(self, "")
        with self.assertRaises(ControlError) as refused:
            Control.for_current_run(address)
        self.assertEqual(refused.exception.code, grpc.StatusCode.FAILED_PRECONDITION)
        self.assertIn("belongs to no run", str(refused.exception))

    def test_an_empty_run_is_refused(self):
        with self.assertRaises(ValueError):
            Control("127.0.0.1:1", run="")


if __name__ == "__main__":
    unittest.main()
