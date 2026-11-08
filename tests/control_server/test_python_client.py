# SPDX-License-Identifier: MIT
"""The Python client and the CLI against vehicle_sil_control_demo (started here).

Environment: VEHICLE_SIL_CONTROL_DEMO (the demo executable), PYTHONPATH with the repository's
python/ directory, VEHICLE_SIL_CONTROL_STUBS (generated stubs).
"""
import os
import re
import subprocess
import sys
import threading
import unittest

from vehicle_sil_control import Control, ControlError, EventInfo, Operation, Peer, Values, ms


class DemoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.demo = subprocess.Popen([os.environ["VEHICLE_SIL_CONTROL_DEMO"], "--listen", "127.0.0.1:0"],
                                    stdout=subprocess.PIPE, text=True)
        port = None
        for line in cls.demo.stdout:
            m = re.match(r"control server on port (\d+)", line)
            if m:
                port = int(m.group(1))
                break
        assert port, "the demo did not report its port"
        cls.address = f"127.0.0.1:{port}"
        threading.Thread(target=lambda: [None for _ in cls.demo.stdout], daemon=True).start()
        cls.c = Control(cls.address)

    @classmethod
    def tearDownClass(cls):
        cls.c.close()
        cls.demo.terminate()
        cls.demo.wait(30)

    def cli(self, *args):
        out = subprocess.run([sys.executable, "-m", "vehicle_sil_control", "--server", self.address, *args],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stderr)
        return out.stdout

    def test_a_scenario(self):
        c = self.c
        # Both peers are there with their variables.
        import time
        deadline = time.time() + 20
        while time.time() < deadline and not (len(c.peers()) == 2 and all(p.connected for p in c.peers())):
            time.sleep(0.05)
        peers = {p.participant: p for p in c.peers()}
        self.assertEqual(set(peers), {"Engine", "Body"})
        throttle = next(v for v in peers["Engine"].schema if v.name == "engine.throttle")
        self.assertEqual((throttle.type, throttle.causality, throttle.unit, throttle.min, throttle.max),
                         ("real", "input", "%", 0.0, 100.0))
        lamp = next(v for v in peers["Body"].schema if v.name == "cabin.lamp")
        self.assertEqual(lamp.items, {"off": 0, "low": 1, "high": 2})

        # It starts paused at 0.
        t = c.sim.wait_paused(20)
        self.assertTrue(t.available and t.paused)
        self.assertEqual(t.now, 0)

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

        # Schedule writes on both peers at 100 ms, make sure they are queued, then run.
        seq = c.write({"Engine": {"engine.throttle": 50.0}, "Body": {"cabin.lamp": 2, "cabin.parking_brake": False}},
                      at=ms(100))
        op = c.wait(seq, until="queued")
        self.assertEqual({t.status for t in op.targets}, {"queued"})
        c.sim.run_until(ms(500))
        self.assertEqual(c.sim.wait_paused(20).now, ms(500))
        op = c.wait(seq)
        self.assertTrue(op.done)
        self.assertEqual(op.target("Engine").applied_at, ms(100))
        self.assertEqual(op.target("Body").applied_at, ms(100))

        # The engine sped up and the body received it over J1939.
        deadline = time.time() + 10
        while time.time() < deadline:
            v = c.values("Body")
            if v and v.time >= ms(490):
                break
            time.sleep(0.05)
        engine = c.values("Engine").values
        body = c.values("Body").values
        self.assertGreater(engine["engine.speed"], 2000.0)
        self.assertGreater(body["body.engine_speed"], 2000.0)
        self.assertEqual(body["cabin.lamp"], 2)
        self.assertFalse(body["cabin.parking_brake"])
        self.assertEqual(engine["j1939.J1939_CAN.node0.address_state"], 3)  # claimed

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
        out = self.cli("set", "Body", "cabin.lamp=1", "--at-ms", "700", "--wait", "queued")
        self.assertIn("Body: queued", out)
        out = self.cli("run-until", "800")
        self.assertIn("paused at 800 ms", out)
        self.assertIn("speed max", self.cli("time"))
        out = self.cli("watch", "--peer", "Body", "--count", "1")
        self.assertIn("peer Body connected", out)
        out = self.cli("set", "Engine", "engine.throttle=500")
        self.assertIn("Engine: invalid", out)
        self.assertIn("out of range", out)


if __name__ == "__main__":
    unittest.main()


class SecureServerTest(unittest.TestCase):
    """The client with a token and TLS against vehicle_sil_control_server (started here)."""

    @classmethod
    def setUpClass(cls):
        import tempfile
        cls.data = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata")
        cls.tmp = tempfile.TemporaryDirectory()
        creds = os.path.join(cls.tmp.name, "credentials")
        with open(creds, "w") as f:
            f.write("viewer alice viewer-token-0123456789\noperator bob operator-token-0123456789\n")
        cls.server = subprocess.Popen(
            [os.environ["VEHICLE_SIL_CONTROL_SERVER_EXE"], "--listen", "127.0.0.1:0", "--credentials", creds,
             "--tls-cert", os.path.join(cls.data, "server.crt"), "--tls-key", os.path.join(cls.data, "server.key"),
             "--audit-log", os.path.join(cls.tmp.name, "audit.log")],
            stdout=subprocess.PIPE, text=True)
        port = None
        for line in cls.server.stdout:
            m = re.search(r"listening on port (\d+)", line)
            if m:
                port = int(m.group(1))
                break
        assert port, "the server did not report its port"
        cls.address = f"127.0.0.1:{port}"
        threading.Thread(target=lambda: [None for _ in cls.server.stdout], daemon=True).start()
        with open(os.path.join(cls.data, "server.crt"), "rb") as f:
            cls.roots = f.read()

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        cls.server.wait(30)
        cls.tmp.cleanup()

    def test_tokens_and_roles(self):
        import grpc
        with Control(self.address, token="viewer-token-0123456789", root_certificates=self.roots) as c:
            self.assertIn("control", c.ping())
            self.assertEqual(c.peers(), [])
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
