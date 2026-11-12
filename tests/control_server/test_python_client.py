# SPDX-License-Identifier: MIT
"""The Python client and the CLI.

- LaunchedTest: against vehicle_sil_launch run --control with the two ECUs of examples/ecu_main
  (started here).
- SecureServerTest: with a token and TLS against vehicle_sil_control_server without --required
  (started here, with a SIL Kit registry of its own).
- AbortedSimulationTest: that server ends when the simulation it joined is aborted.
- ConversionTest: what the client makes of variables and values, without a server.

Environment: VEHICLE_SIL_LAUNCH_EXE, VEHICLE_SIL_EXAMPLE_DIR (examples/ecu_main in the build tree),
VEHICLE_SIL_LAUNCH_SCRATCH (a directory to write into), VEHICLE_SIL_CONTROL_SERVER_EXE,
VEHICLE_SIL_REGISTRY_EXE (sil-kit-registry), VEHICLE_SIL_SIM_CONTROLLER_EXE, PYTHONPATH with the repository's python/ directory,
VEHICLE_SIL_CONTROL_STUBS (generated stubs).
"""
import os
import pathlib
import re
import socket
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


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def stop(proc):
    """Ends a child process (terminate, then kill)."""
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(30)
    if proc.stdout is not None:
        proc.stdout.close()


def start_registry(add_cleanup) -> str:
    """A SIL Kit registry on a free port, ended by the cleanup; returns its URI once it accepts
    connections (a participant that comes earlier fails: SIL Kit tries to connect once)."""
    port = free_port()
    proc = subprocess.Popen([os.environ["VEHICLE_SIL_REGISTRY_EXE"], "--listen-uri", f"silkit://127.0.0.1:{port}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    add_cleanup(stop, proc)
    deadline = time.monotonic() + 30
    while True:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return f"silkit://127.0.0.1:{port}"
        except OSError:
            if proc.poll() is not None:
                raise AssertionError(f"the registry ended (exit code {proc.returncode}) before it listened on {port}")
            if time.monotonic() > deadline:
                raise AssertionError(f"the registry did not listen on {port} within 30 s")
            time.sleep(0.05)


class Lines:
    """The standard output of a child process, read on a thread of its own."""

    def __init__(self, proc):
        self.lines = []
        self._changed = threading.Condition()
        threading.Thread(target=self._read, args=(proc,), daemon=True).start()

    def _read(self, proc):
        for line in proc.stdout:
            with self._changed:
                self.lines.append(line.rstrip("\n"))
                self._changed.notify_all()

    def wait_for(self, pattern, timeout=60.0):
        """The first match of `pattern` (a regular expression) in a line; AssertionError after `timeout` s."""
        deadline = time.monotonic() + timeout
        with self._changed:
            while True:
                for line in self.lines:
                    m = re.search(pattern, line)
                    if m:
                        return m
                left = deadline - time.monotonic()
                if left <= 0:
                    raise AssertionError(f"no line matching {pattern!r} within {timeout} s; the lines: {self.lines}")
                self._changed.wait(left)


def start_server(add_cleanup, *arguments):
    proc = subprocess.Popen([os.environ["VEHICLE_SIL_CONTROL_SERVER_EXE"], "--listen", "127.0.0.1:0", *arguments],
                            stdout=subprocess.PIPE, text=True)
    add_cleanup(stop, proc)
    return proc, Lines(proc)


def start_engine(add_cleanup, registry_uri):
    """The engine ECU of examples/ecu_main, as a participant of the simulation at `registry_uri`."""
    engine = pathlib.Path(os.environ["VEHICLE_SIL_EXAMPLE_DIR"]) / "engine" / f"vehicle_sil_engine_ecu{EXE}"
    proc = subprocess.Popen([str(engine), "--registry-uri", registry_uri, "--start-timeout-ms", "120000"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    add_cleanup(stop, proc)
    return proc


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
        config.write_text("vehicle_sil_simulation: 1\nstop_at_ms: 600000\nnetworks:\n  - name: J1939_CAN\n    kind: can\n"
                          f"participants:\n  - name: Engine\n    command: ['{engine}']\n    step_ms: 10\n"
                          f"  - name: Dashboard\n    command: ['{dashboard}']\n    step_ms: 5\n")
        cls.launch = Launch(os.environ["VEHICLE_SIL_LAUNCH_EXE"], config, start_paused=True)
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
        c.sim.run_until(ms(600))
        self.assertEqual(c.sim.wait_paused(20).now, ms(600))
        op = c.wait(seq)
        self.assertTrue(op.done)
        self.assertEqual(op.target("Engine").applied_at, ms(100))

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


class SecureServerTest(unittest.TestCase):
    """The client with a token and TLS against vehicle_sil_control_server (started here, without
    --required: it joins the registry as a participant of its own and controls no time), and an
    engine ECU that waits for a controller that never comes: its hello reaches the server all the
    same."""

    @classmethod
    def setUpClass(cls):
        import tempfile
        cls.data = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        creds = os.path.join(cls.tmp.name, "credentials")
        with open(creds, "w") as f:
            f.write("viewer alice viewer-token-0123456789\noperator bob operator-token-0123456789\n")
        registry_uri = start_registry(cls.addClassCleanup)
        cls.server, lines = start_server(
            cls.addClassCleanup, "--registry-uri", registry_uri, "--credentials", creds,
            "--tls-cert", os.path.join(cls.data, "server.crt"), "--tls-key", os.path.join(cls.data, "server.key"),
            "--audit-log", os.path.join(cls.tmp.name, "audit.log"))
        cls.address = "127.0.0.1:" + lines.wait_for(r"listening on port (\d+)").group(1)
        lines.wait_for("joined the simulation as ControlServer")
        with open(os.path.join(cls.data, "server.crt"), "rb") as f:
            cls.roots = f.read()
        start_engine(cls.addClassCleanup, registry_uri)

    def test_tokens_and_roles(self):
        import grpc
        with Control(self.address, token="viewer-token-0123456789", root_certificates=self.roots) as c:
            self.assertIn("control", c.ping())
            self.assertFalse(c.sim.time().available)  # without --required: the time is not the server's
            # The engine's hello came over SIL Kit, before any simulation started.
            deadline = time.time() + 60
            while time.time() < deadline and not c.peers():
                time.sleep(0.05)
            peers = c.peers()
            self.assertEqual([(p.participant, p.connected, p.has_agent) for p in peers], [("Engine", True, True)])
            self.assertIn("engine.throttle", [v.name for v in peers[0].schema])
            with self.assertRaises(ControlError) as e:
                c.write({"X": {"y": 1}})
            self.assertEqual(e.exception.code, grpc.StatusCode.PERMISSION_DENIED)
        with Control(self.address, token="operator-token-0123456789", root_certificates=self.roots) as c:
            seq = c.write({"X": {"y": 1}})
            self.assertEqual(c.operation(seq).target("X").status, "invalid")  # no such peer
        with Control(self.address, root_certificates=self.roots) as c:
            with self.assertRaises(ControlError) as e:
                c.peers()
            self.assertEqual(e.exception.code, grpc.StatusCode.UNAUTHENTICATED)
        out = subprocess.run([sys.executable, "-m", "vehicle_sil_control", "--server", self.address, "--token",
                              "viewer-token-0123456789", "--root-cert", os.path.join(self.data, "server.crt"), "peers"],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stderr)
        with open(os.path.join(self.tmp.name, "audit.log")) as f:
            audit = f.read()
        self.assertIn("alice@", audit)
        self.assertIn("bob@", audit)


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


class AbortedSimulationTest(unittest.TestCase):
    """vehicle_sil_control_server without --required in a simulation that vehicle_sil_sim_controller
    coordinates: when the controller aborts the simulation (a required ECU's process was killed),
    the server's own participant leaves too, and the server says so and ends with exit code 1."""

    def test_the_server_ends_with_the_simulation_it_joined(self):
        registry_uri = start_registry(self.addCleanup)
        server, lines = start_server(self.addCleanup, "--registry-uri", registry_uri)
        address = "127.0.0.1:" + lines.wait_for(r"listening on port (\d+)").group(1)
        lines.wait_for("joined the simulation as ControlServer")
        engine = start_engine(self.addCleanup, registry_uri)
        controller = subprocess.Popen([os.environ["VEHICLE_SIL_SIM_CONTROLLER_EXE"], "--registry-uri", registry_uri,
                                       "--required", "Engine"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(stop, controller)
        with Control(address) as c:
            def engine_time():
                try:
                    return (c.values("Engine") or _NoValues()).time
                except ControlError:  # no such peer yet
                    return -1
            deadline = time.time() + 60
            while time.time() < deadline and engine_time() <= 0:
                time.sleep(0.05)
            self.assertGreater(engine_time(), 0, "the engine's values did not come: the simulation did not run")
        engine.kill()
        self.assertEqual(controller.wait(60), 1)  # a required participant failed: it aborts
        self.assertEqual(server.wait(60), 1)
        lines.wait_for(r"FAIL: the server's participant 'ControlServer' left the simulation \(state ")


class _NoValues:
    time = -1


if __name__ == "__main__":
    unittest.main()
