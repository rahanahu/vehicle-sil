# SPDX-License-Identifier: MIT
"""vehicle_sil_launch run --control: the control server of the launcher, with the two ECUs of
examples/ecu_main as processes, driven from Python.

- The hold of --start-paused: a write before the first step takes effect at 0; the commands of
  standard input and of the control server go the same way and are events in the order they took
  effect (`source`); the refusals.
- The end: calls that wait (Watch, WaitOperation, WaitPaused) are still waiting while the run lasts,
  end with the run, and the run does not wait for them: at its end, on --timeout-ms, when a
  participant fails (also in the hold); a stop from standard input in the hold.
- TLS, credentials and roles, the audit log, no agents over gRPC; the usage errors (an empty
  credentials file too), a port in use; version --json.
(The limits of the start around the hold, --timeout-ms and Ctrl+C in the hold, the commands before
the window and a Stop after the end are tested in C++: launch_control.*, launch.*.)

Environment: VEHICLE_SIL_LAUNCH_EXE, VEHICLE_SIL_EXAMPLE_DIR (examples/ecu_main in the build tree),
VEHICLE_SIL_TEST_DATA_DIR (the TLS files), VEHICLE_SIL_LAUNCH_SCRATCH (a directory to write into),
PYTHONPATH with the repository's python/ directory, VEHICLE_SIL_CONTROL_STUBS.
"""
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import threading
import time
import unittest

import grpc

from vehicle_sil_control import Control, ControlError, Launch, LaunchError, TimeState, control_address, ms

EXE = ".exe" if os.name == "nt" else ""
TOKEN = "0123456789abcdef-test-token"
LAUNCH = EXAMPLE = TESTDATA = SCRATCH = None


def setUpModule():
    # Read here, not on import (scripts/verify-python.py imports the module without them).
    global LAUNCH, EXAMPLE, TESTDATA, SCRATCH
    LAUNCH = os.environ["VEHICLE_SIL_LAUNCH_EXE"]
    EXAMPLE = pathlib.Path(os.environ["VEHICLE_SIL_EXAMPLE_DIR"])
    TESTDATA = pathlib.Path(os.environ["VEHICLE_SIL_TEST_DATA_DIR"])
    SCRATCH = pathlib.Path(os.environ["VEHICLE_SIL_LAUNCH_SCRATCH"])


def config(name: str, stop_at_ms: int) -> pathlib.Path:
    """The simulation of examples/ecu_main with another stop_at_ms, in the scratch directory."""
    SCRATCH.mkdir(parents=True, exist_ok=True)
    engine = (EXAMPLE / "engine" / f"vehicle_sil_engine_ecu{EXE}").as_posix()
    dashboard = (EXAMPLE / "dashboard" / f"vehicle_sil_dashboard_ecu{EXE}").as_posix()
    path = SCRATCH / f"{name}.yaml"
    path.write_text(f"vehicle_sil_simulation: 1\nstop_at_ms: {stop_at_ms}\nnetworks:\n  - name: J1939_CAN\n    kind: can\n"
                    f"participants:\n  - name: Engine\n    command: ['{engine}']\n    step_ms: 10\n"
                    f"  - name: Dashboard\n    command: ['{dashboard}']\n    step_ms: 5\n")
    return path


def commands_of(launch: Launch):
    return [(e["command"], e["source"], e["accepted"], e.get("ms"), e.get("at_ms"))
            for e in launch.events if e["event"] == "command"]


def findings_of(result):
    return [(f["severity"], f["code"], f["participant"]) for f in result["findings"]]


def wait_until(test, what, condition, timeout=60.0):
    deadline = time.monotonic() + timeout
    while not condition():
        test.assertLess(time.monotonic(), deadline, f"not within {timeout} s: {what}")
        time.sleep(0.02)


class Calls:
    """Calls that wait, on threads of their own: how each ended, and whether it still waits."""

    def __init__(self):
        self.outcome = {}
        self._threads = {}

    def run(self, name, call):
        def body():
            try:
                self.outcome[name] = call()
            except ControlError as e:
                self.outcome[name] = e

        thread = threading.Thread(target=body, daemon=True)
        thread.start()
        self._threads[name] = thread

    def waiting(self):
        """The names of the calls that have not returned."""
        return {name for name, thread in self._threads.items() if thread.is_alive()}

    def join(self, timeout):
        deadline = time.monotonic() + timeout
        for thread in self._threads.values():
            thread.join(max(0.0, deadline - time.monotonic()))
        return not self.waiting()


def watch_to_the_end(c: Control):
    """Reads a watch until the stream ends: (whether "initial_done" came, the time states that say
    the simulation has ended)."""
    initial = False
    ended = []
    for u in c.watch(time=True, simulation=True):
        if u == ("initial_done",):
            initial = True
        elif isinstance(u, TimeState) and u.ended:
            ended.append(u)
    return initial, ended


class LauncherTest(unittest.TestCase):
    def launch(self, name, *, stop_at_ms=1000, start_paused=False, arguments=(), address="0"):
        return Launch(LAUNCH, config(name, stop_at_ms), start_paused=start_paused, arguments=arguments, address=address)

    def test_a_write_in_the_hold_and_the_commands_of_both_sources(self):
        with self.launch("hold", start_paused=True) as launch:
            with Control(launch.address) as c:
                # Before the first step: it stands still at 0 and has not started.
                self.assertEqual(c.sim.wait_paused(30).now, 0)
                t = c.sim.time()
                self.assertEqual((t.now, t.paused, t.running, t.ended), (0, True, False, False))
                self.assertTrue(c.peer("Engine").connected)
                seq = c.write({"Engine": {"engine.throttle": 50.0}})
                self.assertEqual(c.wait(seq, until="queued").target("Engine").status, "queued")
                # A step from standard input ends the hold.
                launch.send({"command": "step", "ms": 100})
                self.assertEqual(c.sim.wait_paused().now, ms(100))
                self.assertTrue(c.sim.time().running)
                done = c.wait(seq)
                self.assertEqual((done.target("Engine").status, done.target("Engine").applied_at), ("applied", 0))
                # The control server's: rounded up to the controller's steps (10 ms).
                c.sim.step(ms(45))
                self.assertEqual(c.sim.wait_paused().now, ms(150))
                c.sim.run_until(ms(201))
                self.assertEqual(c.sim.wait_paused().now, ms(210))
                # Standing at 210 ms: the sample of the last step before it is there (the engine
                # samples every 10 ms; the one of its step at 210 may come while it stands).
                self.assertIn(c.values("Engine").time, (ms(200), ms(210)))
                c.sim.resume()
                with self.assertRaises(ControlError) as refused:
                    c.sim.step(ms(10))
                self.assertEqual(refused.exception.code, grpc.StatusCode.FAILED_PRECONDITION)
                self.assertIn("step works while the simulation is paused", str(refused.exception))
                with self.assertRaises(ControlError) as refused:
                    c.sim.set_speed(0.0)
                self.assertEqual(refused.exception.code, grpc.StatusCode.INVALID_ARGUMENT)
                c.sim.pause()
                c.sim.wait_paused()
                c.sim.stop("the test is done")
            result = launch.wait()
        self.assertEqual(commands_of(launch), [
            ("step", "stdin", True, 100, None),
            ("step", "control_server", True, 50, None),
            ("run_until", "control_server", True, None, 210),
            ("resume", "control_server", True, None, None),
            ("step", "control_server", False, None, None),
            ("speed", "control_server", False, None, None),
            ("pause", "control_server", True, None, None),
            ("stop", "control_server", True, None, None),
        ])
        self.assertEqual((result["result"], result["interrupted_by"]), ("interrupted", "control server stop"))
        self.assertEqual(launch.returncode, 130)
        names = [e["event"] for e in launch.events]
        self.assertLess(names.index("control_server"), names.index("registry"))
        server = next(e for e in launch.events if e["event"] == "control_server")
        self.assertEqual((server["tls"], server["credentials"]), (False, False))
        self.assertTrue(server["address"].startswith("127.0.0.1:") and not server["address"].endswith(":0"))
        lines = [e["line"] for e in launch.events if e["event"] == "log" and e["source"] == "vehicle_sil_launch"]
        self.assertIn(f"control server at {server['address']}", lines)
        self.assertEqual(control_address(json.dumps(e) for e in launch.events), server["address"])

    def start_waiting_calls(self, c: Control, launch: Launch, calls: Calls, with_paused: bool = True):
        """In the hold: a watch, a write for a time that never comes and its WaitOperation; once the
        simulation runs (with_paused), a WaitPaused. They must all still wait."""
        seq = c.write({"Engine": {"engine.throttle": 10.0}}, at=ms(100_000))
        self.assertEqual(c.wait(seq, until="queued").target("Engine").status, "queued")
        calls.run("watch", lambda: watch_to_the_end(c))
        calls.run("operation", lambda: c.wait(seq, timeout=300))
        c.sim.resume()
        running = launch.wait_for_event("time", paused=False)
        if with_paused:
            calls.run("paused", lambda: c.sim.wait_paused(timeout=300))
        # The simulation has gone on for a moment (a time event after the first one).
        launch.wait_for_event("time", after=running)
        self.assertEqual(calls.waiting(), {"watch", "operation"} | ({"paused"} if with_paused else set()))

    def check_ended_calls(self, calls: Calls):
        self.assertTrue(calls.join(30), f"calls still wait: {calls.waiting()}")
        initial, _ = calls.outcome["watch"]
        self.assertTrue(initial, "the watch ended without its initial state")
        operation = calls.outcome["operation"]
        self.assertFalse(isinstance(operation, ControlError), operation)
        self.assertTrue(operation.done and operation.target("Engine").status in ("cancelled", "unknown"), operation)
        if "paused" in calls.outcome:
            paused = calls.outcome["paused"]
            self.assertIsInstance(paused, ControlError)
            self.assertIn(paused.code, (grpc.StatusCode.FAILED_PRECONDITION, grpc.StatusCode.CANCELLED))

    def test_calls_that_wait_end_with_the_run(self):
        with self.launch("end", stop_at_ms=1500, start_paused=True, arguments=["--speed", "1"]) as launch:
            with Control(launch.address) as c:
                c.sim.wait_paused(60)
                calls = Calls()
                self.start_waiting_calls(c, launch, calls)
                result = launch.wait(120)
                self.check_ended_calls(calls)
        self.assertEqual((result["result"], result["simulation"]), ("passed", "reached_stop_at"))
        # What the server says once the run is over is where it ended (when the watch saw it).
        _, ended = calls.outcome["watch"]
        for state in ended:
            self.assertEqual(state.now, ms(result["timing"]["sim_ms"]), state)

    def test_calls_that_wait_end_on_a_timeout(self):
        with self.launch("timeout", stop_at_ms=600000, start_paused=True,
                         arguments=["--speed", "1", "--timeout-ms", "4000"]) as launch:
            with Control(launch.address) as c:
                c.sim.wait_paused(60)
                calls = Calls()
                self.start_waiting_calls(c, launch, calls)
                result = launch.wait(120)
                self.check_ended_calls(calls)
        self.assertIn(("error", "timeout", None), findings_of(result))

    def test_calls_that_wait_end_when_a_participant_fails(self):
        with self.launch("fails", stop_at_ms=600000, start_paused=True, arguments=["--speed", "1"]) as launch:
            with Control(launch.address) as c:
                c.sim.wait_paused(60)
                calls = Calls()
                self.start_waiting_calls(c, launch, calls)
                engine = launch.wait_for_event("participant", name="Engine", state="started")
                os.kill(engine["pid"], signal.SIGTERM if os.name == "nt" else signal.SIGKILL)
                result = launch.wait(120)
                self.check_ended_calls(calls)
        self.assertIn(("error", "participant-failed", "Engine"), findings_of(result))

    def test_a_participant_that_fails_in_the_hold_ends_the_start_at_once(self):
        # At once: the start's limit is start_timeout_ms (30 s in this file) after the hold; the
        # launcher's own wait below is shorter only for a launcher that does not give up.
        with self.launch("hold_fails", start_paused=True) as launch:
            with Control(launch.address) as c:
                self.assertEqual(c.sim.wait_paused(60).now, 0)
                calls = Calls()
                calls.run("watch", lambda: watch_to_the_end(c))
                wait_until(self, "the watch has begun", lambda: "watch" in calls.waiting())
                engine = launch.wait_for_event("participant", name="Engine", state="started")
                os.kill(engine["pid"], signal.SIGTERM if os.name == "nt" else signal.SIGKILL)
                result = launch.wait(120)
                self.assertTrue(calls.join(30))
        self.assertEqual(result["simulation"], "not_started")
        self.assertIn(("error", "ended-before-start", "Engine"), findings_of(result))
        failed = [e for e in launch.events if e["event"] == "participant" and e["state"] == "failed"]
        self.assertEqual([(e["name"], e["reason"]) for e in failed], [("Engine", "ended_before_start")])

    def test_a_stop_on_standard_input_in_the_hold(self):
        with self.launch("hold_stop", start_paused=True) as launch:
            with Control(launch.address) as c:
                self.assertEqual(c.sim.wait_paused(60).now, 0)
                launch.stop()
                result = launch.wait(120)
        self.assertEqual((result["result"], result["interrupted_by"]), ("interrupted", "stop command"))

    def credentials(self, text, name="credentials.txt"):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        path = SCRATCH / name
        path.write_text(text)
        return path

    def test_credentials_roles_tls_audit_and_no_agents(self):
        viewer, operator = "viewer-token-0123456789", "operator-token-0123456789"
        credentials = self.credentials(f"admin tester {TOKEN}\nviewer v {viewer}\noperator o {operator}\n")
        audit = SCRATCH / "audit.log"
        audit.unlink(missing_ok=True)
        arguments = ["--control-credentials", str(credentials), "--control-tls-cert", str(TESTDATA / "server.crt"),
                     "--control-tls-key", str(TESTDATA / "server.key"), "--control-audit-log", str(audit)]
        roots = (TESTDATA / "server.crt").read_bytes()
        with Launch(LAUNCH, config("tls", 1000), start_paused=True, arguments=arguments, address="localhost:0",
                    stderr=subprocess.PIPE) as launch:
            server = launch.wait_for_event("control_server")
            self.assertEqual((server["tls"], server["credentials"]), (True, True))
            self.assertTrue(server["address"].startswith("localhost:"))
            with Control(launch.address, root_certificates=roots) as anonymous:
                with self.assertRaises(ControlError) as refused:
                    anonymous.sim.time()
                self.assertEqual(refused.exception.code, grpc.StatusCode.UNAUTHENTICATED)
            with Control(launch.address, root_certificates=roots, token=viewer) as v:
                self.assertEqual(v.sim.wait_paused(60).now, 0)
                with self.assertRaises(ControlError) as refused:
                    v.sim.pause()
                self.assertEqual(refused.exception.code, grpc.StatusCode.PERMISSION_DENIED)
            with Control(launch.address, root_certificates=roots, token=operator) as o:
                with self.assertRaises(ControlError) as refused:
                    o.sim.stop()
                self.assertEqual(refused.exception.code, grpc.StatusCode.PERMISSION_DENIED)
                o.sim.pause()
            # No agent connects over gRPC (the agents' service of earlier versions is not there).
            channel = grpc.secure_channel(launch.address, grpc.ssl_channel_credentials(roots))
            try:
                connect = channel.stream_stream("/vehicle_sil.control.v1.AgentService/Connect",
                                                request_serializer=lambda b: b, response_deserializer=lambda b: b)
                stream = connect(iter([b""]), timeout=30, metadata=[("authorization", f"Bearer {TOKEN}")])
                with self.assertRaises(grpc.RpcError) as refused:
                    next(stream)
                self.assertEqual(refused.exception.code(), grpc.StatusCode.UNIMPLEMENTED)
            finally:
                channel.close()
            with Control(launch.address, root_certificates=roots, token=TOKEN) as c:
                c.sim.stop("the test is done")
            result = launch.wait()
        self.assertEqual(result["interrupted_by"], "control server stop")
        lines = audit.read_text().splitlines()
        self.assertTrue(any(line.startswith("o@") and "pause" in line and line.endswith(": ok") for line in lines), lines)
        self.assertTrue(any(line.startswith("tester@") and "stop (the test is done)" in line for line in lines), lines)
        # The tokens are in no event, no line of standard error and no line of the audit.
        everything = json.dumps(launch.events) + launch.stderr_text + "\n".join(lines)
        for token in (TOKEN, viewer, operator):
            self.assertNotIn(token, everything)

    @unittest.skipIf(os.name == "nt", "a unix: socket")
    def test_a_unix_socket(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        path = SCRATCH / "control.sock"
        path.unlink(missing_ok=True)
        with Launch(LAUNCH, config("unix", 1000), start_paused=True, address=f"unix:{path}") as launch:
            self.assertEqual(launch.address, f"unix:{path}")
            with Control(launch.address) as c:
                self.assertEqual(c.sim.wait_paused(60).now, 0)
                c.sim.resume()
            self.assertEqual(launch.wait()["result"], "passed")

    def run_launcher(self, *arguments):
        return subprocess.run([LAUNCH, *arguments], capture_output=True, text=True, timeout=120)

    def test_usage(self):
        sim = str(config("usage", 1000))
        empty = self.credentials("# nothing here\n\n", "empty.txt")
        wrong_order = self.credentials(f"{TOKEN} tester admin\n", "wrong_order.txt")
        agent = self.credentials(f"admin tester {TOKEN}x\nagent ecu {TOKEN} Engine\n", "agent.txt")
        for arguments, message in [
            (["--control", "0.0.0.0:0"], "--control 0.0.0.0:0 is reachable from other computers: a control server there needs "
                                         "TLS and credentials (--control-tls-cert, --control-tls-key and "
                                         "--control-credentials)"),
            (["--control", "0", "--control-tls-cert", str(TESTDATA / "server.crt")],
             "--control-tls-cert and --control-tls-key go together"),
            (["--control", "localhost"], "--control must be <port> (on 127.0.0.1; 0: any free port), <host>:<port> (a port "
                                         "from 0 to 65535) or unix:<path>, not 'localhost'"),
            (["--control", "localhost:99999"], "--control must be <port>"),
            (["--control", "99999"], "--control 99999: a port is from 0 to 65535"),
            (["--control", "0", "--control-credentials", str(SCRATCH / "missing.txt")], "cannot read "),
            (["--control", "0", "--control-credentials", str(empty)], f"--control-credentials: {empty} has no credential"),
            (["--control", "0", "--control-credentials", str(wrong_order)], "the first word is no role"),
            (["--control", "0", "--control-credentials", str(agent)],
             f"--control-credentials: {agent}:2: the role agent is no longer used: the ECUs' control agents reach the "
             "server over SIL Kit, without a token; delete this line"),
        ]:
            out = self.run_launcher("run", *arguments, sim)
            self.assertEqual(out.returncode, 4, out.stderr)
            self.assertIn(message, out.stderr.splitlines()[0])
            self.assertTrue(out.stderr.startswith("error: "), out.stderr)
            self.assertNotIn(TOKEN, out.stderr)

    def test_a_port_in_use_is_a_setup_error(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as taken:
            if os.name == "nt":
                taken.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            taken.bind(("127.0.0.1", 0))
            taken.listen()
            port = taken.getsockname()[1]
            out = self.run_launcher("run", "--json", "--control", str(port), str(config("taken", 1000)))
        self.assertEqual(out.returncode, 3, out.stderr)
        result = json.loads(out.stdout)
        self.assertEqual([f["code"] for f in result["findings"]], ["setup"])
        self.assertIn("the control server cannot be started", result["findings"][0]["message"])

    def test_version_lists_the_control_server(self):
        out = self.run_launcher("version", "--json")
        self.assertEqual(out.returncode, 0)
        self.assertIn("control-server", json.loads(out.stdout)["features"])

    def test_control_address_says_why_there_is_none(self):
        with self.assertRaises(LaunchError):
            control_address(['{"event":"result","result":{"result":"failed","findings":[]}}'])

    def test_launch_skips_what_is_no_event(self):
        # A "launcher" that writes a line that is no JSON first: the line is kept aside, and the
        # events after it are read (Python runs the script "run" of the working directory).
        SCRATCH.mkdir(parents=True, exist_ok=True)
        (SCRATCH / "run").write_text(
            "print('no json')\n"
            "print('{\"event\":\"control_server\",\"seq\":0,\"address\":\"127.0.0.1:1\"}')\n"
            "print('{\"event\":\"result\",\"seq\":1,\"result\":{\"result\":\"passed\"}}')\n")
        with Launch(sys.executable, "x.yaml", cwd=str(SCRATCH)) as launch:
            self.assertEqual(launch.address, "127.0.0.1:1")
            self.assertEqual(launch.wait(60)["result"], "passed")
            self.assertEqual([line.strip() for line in launch.bad_lines], [b"no json"])

if __name__ == "__main__":
    unittest.main()
