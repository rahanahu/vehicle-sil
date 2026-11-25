# SPDX-License-Identifier: MIT
"""Unit tests for python/vehicle_sil_control/serve.py (standard library only, no C++ build)."""
from __future__ import annotations

import ast
import collections
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest import mock

_REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
_PYTHON_DIR = _REPO_ROOT / "python"
if str(_PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(_PYTHON_DIR))

from vehicle_sil_control import Launch, LaunchError, RunHandle, Serve, ServeError, __version__, control_address  # noqa: E402
from vehicle_sil_control.serve import MAX_COMMAND_ID, MAX_COMMAND_LINE  # noqa: E402


class _FakeStdout:
    """Thread-safe line queue mocking subprocess stdout."""

    def __init__(self) -> None:
        self._cond = threading.Condition()
        self._lines: collections.deque[bytes] = collections.deque()
        self._closed = False

    def push_event(self, payload: dict) -> None:
        self.push_raw((json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8"))

    def push_raw(self, raw: bytes) -> None:
        with self._cond:
            if not self._closed:
                self._lines.append(raw)
                self._cond.notify_all()

    def close(self) -> None:
        with self._cond:
            self._closed = True
            self._cond.notify_all()

    def __iter__(self):
        return self

    def __next__(self) -> bytes:
        with self._cond:
            while not self._lines and not self._closed:
                self._cond.wait()
            if self._lines:
                return self._lines.popleft()
            raise StopIteration


class _FakeStdin:
    """Line-buffered stdin mock calling `on_line` on newline and `on_eof` on close."""

    def __init__(self, on_line, on_eof) -> None:
        self._on_line = on_line
        self._on_eof = on_eof
        self._buf = bytearray()
        self.closed = False
        self.raw_lines: list[bytes] = []
        self.commands: list[dict] = []

    def write(self, data: bytes) -> int:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        self._buf.extend(data)
        while b"\n" in self._buf:
            idx = self._buf.index(b"\n")
            line = bytes(self._buf[:idx])
            del self._buf[: idx + 1]
            self.raw_lines.append(line)
            cmd = json.loads(line.decode("ascii"))
            self.commands.append(cmd)
            self._on_line(cmd)
        return len(data)

    def flush(self) -> None:
        if self.closed:
            raise ValueError("I/O operation on closed file")

    def close(self) -> None:
        if not self.closed:
            self.closed = True
            self._on_eof()


class _FakeProcess:
    """Mock Popen process speaking Events v4 NDJSON."""

    def __init__(self, command: list[str], handler=None, *, auto_hello: bool = True,
                 stderr_pipe: bool = False, on_eof=None) -> None:
        self.command = list(command)
        self.pid = 42424
        self.returncode: int | None = None
        self._done = threading.Event()
        self.stdout = _FakeStdout()
        self.stderr = _FakeStdout() if stderr_pipe else None
        self._handler = handler
        self._on_eof = on_eof
        self.stdin = _FakeStdin(self._handle_line, self._handle_eof)
        if auto_hello:
            self.stdout.push_event({
                "event": "hello",
                "seq": 0,
                "t_ms": 0,
                "vehicle_sil_events": 4,
                "vehicle_sil": __version__,
                "pid": self.pid,
                "file": command[-1],
                "arguments": [],
                "launcher": "0123456789abcdef",
            })
            self.stdout.push_event({
                "event": "status",
                "seq": 1,
                "t_ms": 1,
                "id": None,
                "instance": "0123456789abcdef",
                "status_rev": 0,
                "launcher_state": "unprepared",
            })

    def _handle_line(self, cmd: dict) -> None:
        if self._handler is not None:
            self._handler(self, cmd)

    def _handle_eof(self) -> None:
        if self._on_eof is not None:
            self._on_eof(self)
        elif self.returncode is None:
            self.finish(0)

    def finish(self, code: int = 0) -> None:
        self.returncode = code
        self.stdout.close()
        if self.stderr is not None:
            self.stderr.close()
        self._done.set()

    def poll(self) -> int | None:
        return self.returncode

    def wait(self, timeout: float | None = None) -> int:
        if not self._done.wait(timeout):
            raise subprocess.TimeoutExpired(self.command, timeout or 0)
        assert self.returncode is not None
        return self.returncode

    def kill(self) -> None:
        self.finish(-9)


class ServeUnitTest(unittest.TestCase):
    def test_stdlib_only_import_without_site_packages(self) -> None:
        env = dict(os.environ)
        env["PYTHONPATH"] = str(_PYTHON_DIR)
        code = (
            "import vehicle_sil_control as c; "
            "from vehicle_sil_control import Serve, RunHandle, ServeError, Launch, LaunchError, control_address; "
            "assert {'Serve', 'RunHandle', 'ServeError', 'Launch', 'LaunchError', 'control_address'} <= set(c.__all__); "
            "assert issubclass(ServeError, LaunchError)"
        )
        proc = subprocess.run([sys.executable, "-S", "-c", code], env=env, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_ast_isolation_and_launch_compatibility(self) -> None:
        import vehicle_sil_control as pkg

        self.assertTrue({"Launch", "LaunchError", "control_address", "Serve", "RunHandle", "ServeError"} <= set(pkg.__all__))
        self.assertTrue(issubclass(ServeError, LaunchError))

        serve_tree = ast.parse((_PYTHON_DIR / "vehicle_sil_control" / "serve.py").read_text(encoding="utf-8"))
        for node in serve_tree.body:
            if isinstance(node, ast.Import):
                for alias in node.names:
                    self.assertNotIn("grpc", alias.name)
                    self.assertFalse(alias.name.startswith("tests"))
            elif isinstance(node, ast.ImportFrom):
                mod = node.module or ""
                self.assertNotIn("grpc", mod)
                self.assertNotEqual(mod, "client")
                self.assertNotEqual(mod, "_stubs")
                self.assertFalse(mod.startswith("tests"))

        support_path = _REPO_ROOT / "tests" / "serve" / "serve_support.py"
        support_tree = ast.parse(support_path.read_text(encoding="utf-8"))
        imports_control = False
        for node in ast.walk(support_tree):
            if isinstance(node, ast.Import):
                for alias in node.names:
                    self.assertNotIn("grpc", alias.name)
                    if alias.name.startswith("vehicle_sil_control"):
                        imports_control = True
            elif isinstance(node, ast.ImportFrom):
                mod = node.module or ""
                self.assertNotIn("grpc", mod)
                if mod.startswith("vehicle_sil_control"):
                    imports_control = True
        self.assertTrue(imports_control)

    def test_sys_modules_poisoned_lifecycle(self) -> None:
        seq = [2]

        def next_seq() -> int:
            val = seq[0]
            seq[0] += 1
            return val

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            name = cmd["command"]
            if name == "prepare":
                proc.stdout.push_event({
                    "event": "prepare",
                    "seq": next_seq(),
                    "t_ms": 2,
                    "id": cmd["id"],
                    "accepted": True,
                    "reason": None,
                    "message": None,
                    "status": {"launcher_state": "ready", "eligibility": {"eligible": True}},
                    "metadata": {"participants": ["Engine"], "networks": ["CAN1"], "variants": [], "file_sha256": "abc"},
                    "findings": [],
                })
            elif name == "status":
                proc.stdout.push_event({
                    "event": "status",
                    "seq": next_seq(),
                    "t_ms": 3,
                    "id": cmd["id"],
                    "launcher_state": "ready",
                })
            elif name == "run":
                run_id = "0123456789abcdef-1"
                proc.stdout.push_event({"event": "run_start", "seq": next_seq(), "t_ms": 4, "run": run_id, "id": cmd["id"]})
                proc.stdout.push_event({"event": "phase", "seq": next_seq(), "t_ms": 5, "run": run_id, "phase": "running"})
            elif name == "stop":
                run_id = cmd["run"]
                proc.stdout.push_event({
                    "event": "result",
                    "seq": next_seq(),
                    "t_ms": 6,
                    "run": run_id,
                    "launcher_ends": False,
                    "status": {"launcher_state": "ready", "active_run_id": None, "last_run_id": run_id},
                    "result": {"result": "stopped"},
                })

        poisoned = {
            "grpc": None,
            "vehicle_sil_control.client": None,
            "vehicle_sil_control._stubs": None,
        }
        with mock.patch.dict(sys.modules, poisoned), \
             mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=lambda cmd, **kw: _FakeProcess(cmd, handler)):
            with Serve("/fake/vehicle_sil_launch", "sim.yaml", control=True) as runner:
                self.assertEqual(runner.instance, "0123456789abcdef")
                prep = runner.prepare()
                self.assertTrue(prep["accepted"])
                st = runner.status()
                self.assertEqual(st["launcher_state"], "ready")
                run = runner.start(start_paused=True)
                self.assertEqual(run.id, "0123456789abcdef-1")
                run.stop(force=False)
                res = run.wait()
                self.assertEqual(res["result"], "stopped")
                self.assertFalse(run.launcher_ends)
            self.assertEqual(runner.returncode, 0)

    def test_constructor_flags_and_validation(self) -> None:
        with self.assertRaises(ValueError):
            Serve("/fake/launch", "sim.yaml", address="127.0.0.1:9000")
        with self.assertRaises(ValueError):
            Serve("/fake/launch", "sim.yaml", credentials="creds.txt")
        with self.assertRaises(TypeError):
            Serve("/fake/launch", "sim.yaml", arguments="--bad")
        with self.assertRaises(TypeError):
            Serve("/fake/launch", "sim.yaml", timeout_ms=True)

        seen_cmd: list[str] = []
        seen_cwd: list[str | None] = []

        def fake_popen(cmd, **kw):
            seen_cmd.extend(cmd)
            seen_cwd.append(kw.get("cwd"))
            return _FakeProcess(cmd)

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=fake_popen):
            with Serve(
                pathlib.Path("/fake/launch"),
                pathlib.Path("sim.yaml"),
                control=True,
                address="127.0.0.1:8510",
                credentials=pathlib.Path("c.txt"),
                tls_cert="cert.pem",
                tls_key="key.pem",
                audit_log="audit.log",
                log_dir="logs",
                timeout_ms=5000,
                describe_timeout_ms=1000,
                participant_start_timeout_ms=2000,
                participant_stop_timeout_ms=3000,
                events_stall_ms=4000,
                arguments=("--extra",),
                cwd=pathlib.Path("/work"),
            ) as runner:
                self.assertEqual(runner.pid, 42424)
                self.assertEqual(runner.hello["launcher"], "0123456789abcdef")

        self.assertEqual(seen_cwd, ["/work"])
        self.assertEqual(
            seen_cmd,
            [
                "/fake/launch",
                "serve",
                "--control",
                "127.0.0.1:8510",
                "--control-credentials",
                "c.txt",
                "--control-tls-cert",
                "cert.pem",
                "--control-tls-key",
                "key.pem",
                "--control-audit-log",
                "audit.log",
                "--log-dir",
                "logs",
                "--timeout-ms",
                "5000",
                "--describe-timeout-ms",
                "1000",
                "--participant-start-timeout-ms",
                "2000",
                "--participant-stop-timeout-ms",
                "3000",
                "--events-stall-ms",
                "4000",
                "--extra",
                "--",
                "sim.yaml",
            ],
        )

    def test_startup_fast_fail_trace_early_eof_and_timeout(self) -> None:
        def trace_popen(cmd, **kw):
            proc = _FakeProcess(cmd, auto_hello=False)
            proc.stdout.push_event({"event": "hello", "seq": 0, "t_ms": 0, "vehicle_sil_events": 4, "launcher": "0123456789abcdef"})
            proc.stdout.push_event({
                "event": "result",
                "seq": 1,
                "t_ms": 1,
                "launcher_ends": True,
                "result": {
                    "result": "usage",
                    "findings": [{"severity": "error", "code": "trace-enabled", "message": "serve does not run with VEHICLE_SIL_TRACE set"}],
                },
            })
            proc.finish(4)
            return proc

        t0 = time.monotonic()
        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=trace_popen):
            with self.assertRaises(ServeError) as ctx:
                Serve("/fake/launch", "sim.yaml", startup_timeout=30.0)
        self.assertLess(time.monotonic() - t0, 2.0)
        self.assertEqual(ctx.exception.reason, "trace-enabled")
        self.assertIn("VEHICLE_SIL_TRACE", str(ctx.exception))

        def crash_popen(cmd, **kw):
            proc = _FakeProcess(cmd, auto_hello=False, stderr_pipe=True)
            assert proc.stderr is not None
            proc.stderr.push_raw(b"fatal startup failure\n")
            proc.finish(3)
            return proc

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=crash_popen):
            with self.assertRaises(ServeError) as ctx2:
                Serve("/fake/launch", "sim.yaml", stderr=subprocess.PIPE, startup_timeout=30.0)
        self.assertIn("exit code 3", str(ctx2.exception))
        self.assertIn("fatal startup failure", str(ctx2.exception))

        def stall_popen(cmd, **kw):
            proc = _FakeProcess(cmd, auto_hello=False)
            proc.stdout.push_event({"event": "hello", "seq": 0, "t_ms": 0, "vehicle_sil_events": 3, "launcher": "0123456789abcdef"})
            return proc

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=stall_popen):
            with self.assertRaises(ServeError) as ctx3:
                Serve("/fake/launch", "sim.yaml", startup_timeout=0.05)
        self.assertIn("did not become ready", str(ctx3.exception))

    def test_prepare_and_status_replies_and_refusal(self) -> None:
        procs: list[_FakeProcess] = []

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "prepare":
                if cmd.get("variant") == "refuse":
                    proc.stdout.push_event({
                        "event": "command",
                        "seq": 10,
                        "t_ms": 2,
                        "command": "prepare",
                        "source": "stdin",
                        "accepted": False,
                        "reason": "invalid-argument",
                        "message": "bad prepare",
                        "id": cmd["id"],
                        "asked_run": None,
                    })
                else:
                    proc.stdout.push_event({
                        "event": "prepare",
                        "seq": 10,
                        "t_ms": 2,
                        "id": cmd["id"],
                        "accepted": cmd.get("variant") != "busy",
                        "reason": "busy" if cmd.get("variant") == "busy" else None,
                        "message": "busy" if cmd.get("variant") == "busy" else None,
                        "status": {"launcher_state": "ready"},
                        "metadata": None,
                        "findings": [],
                    })
            elif cmd["command"] == "status":
                proc.stdout.push_event({"event": "status", "seq": 11, "t_ms": 3, "id": cmd["id"], "launcher_state": "idle"})

        def make_proc(cmd, **kw):
            p = _FakeProcess(cmd, handler)
            procs.append(p)
            return p

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=make_proc):
            with Serve("/fake/launch", "sim.yaml") as runner:
                p1 = runner.prepare(variant="v1", disable=["D"], enable=["E"], manual=["M"])
                self.assertTrue(p1["accepted"])
                p2 = runner.prepare(variant="busy")
                self.assertFalse(p2["accepted"])
                self.assertEqual(p2["reason"], "busy")
                st = runner.status()
                self.assertEqual(st["launcher_state"], "idle")
                with self.assertRaises(ServeError) as ctx:
                    runner.prepare(variant="refuse")
                self.assertEqual(ctx.exception.reason, "invalid-argument")

                sent = [c for c in procs[0].stdin.commands if c["command"] == "prepare"]
                self.assertEqual(sent[0], {
                    "command": "prepare",
                    "id": 1,
                    "variant": "v1",
                    "disable": ["D"],
                    "enable": ["E"],
                    "manual": ["M"],
                })

    def test_start_inline_intent_and_oversized_fallback_cleanup(self) -> None:
        procs: list[_FakeProcess] = []
        captured_files: list[dict] = []
        seen_paths: list[str] = []

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "run":
                if "request" in cmd:
                    p = pathlib.Path(cmd["request"])
                    seen_paths.append(str(p))
                    captured_files.append(json.loads(p.read_text(encoding="utf-8")))
                if cmd.get("intent", {}).get("variant") == "refuse" or (
                    captured_files and captured_files[-1].get("variant") == "refuse"
                ):
                    proc.stdout.push_event({
                        "event": "command",
                        "seq": 20,
                        "t_ms": 2,
                        "command": "run",
                        "source": "stdin",
                        "accepted": False,
                        "reason": "busy",
                        "message": "a run is under way",
                        "id": cmd["id"],
                        "asked_run": None,
                    })
                    return
                if cmd.get("intent", {}).get("variant") == "timeout" or (
                    captured_files and captured_files[-1].get("variant") == "timeout"
                ):
                    return
                run_id = f"0123456789abcdef-{cmd['id']}"
                proc.stdout.push_event({"event": "run_start", "seq": 20, "t_ms": 2, "run": run_id, "id": cmd["id"]})
                proc.stdout.push_event({
                    "event": "result",
                    "seq": 21,
                    "t_ms": 3,
                    "run": run_id,
                    "launcher_ends": False,
                    "status": {"launcher_state": "ready"},
                    "result": {"result": "passed"},
                })

        def make_proc(cmd, **kw):
            p = _FakeProcess(cmd, handler)
            procs.append(p)
            return p

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=make_proc):
            with Serve("/fake/launch", "sim.yaml") as runner:
                with self.assertRaises(ValueError):
                    runner.start(request="req.json", start_paused=True)
                with self.assertRaises(ValueError):
                    runner.start(intent={"start_paused": True}, variant="v1")

                # Small inline kwargs -> sent inline.
                r1 = runner.start(scenario="s.yaml", start_paused=True, params={"k": 1}, stop_at_ms=200)
                self.assertEqual(r1.wait()["result"], "passed")
                self.assertIn("intent", procs[0].stdin.commands[0])
                self.assertEqual(procs[0].stdin.commands[0]["intent"]["scenario"], "s.yaml")

                # Oversized inline intent -> temp file "request" fallback, cleaned immediately after run_start.
                big = "x" * (MAX_COMMAND_LINE + 4096)
                r2 = runner.start(params={"pad": big}, stop_at_ms=200)
                self.assertEqual(r2.wait()["result"], "passed")
                self.assertIn("request", procs[0].stdin.commands[1])
                self.assertLessEqual(len(procs[0].stdin.raw_lines[1]), MAX_COMMAND_LINE)
                self.assertEqual(captured_files[0]["vehicle_sil_run_request"], 1)
                self.assertEqual(captured_files[0]["params"]["pad"], big)
                self.assertFalse(pathlib.Path(seen_paths[0]).exists())

                # Oversized intent refused -> temp file still cleaned immediately.
                with self.assertRaises(ServeError) as refused:
                    runner.start(intent={"variant": "refuse", "params": {"pad": big}})
                self.assertEqual(refused.exception.reason, "busy")
                self.assertFalse(pathlib.Path(seen_paths[1]).exists())

                # Oversized intent times out -> temp file kept until Serve.close().
                with self.assertRaises(ServeError):
                    runner.start(intent={"variant": "timeout", "params": {"pad": big}}, timeout=0.05)
                self.assertTrue(pathlib.Path(seen_paths[2]).exists())
            self.assertFalse(pathlib.Path(seen_paths[2]).exists())

    def test_stop_modes_early_exit_and_exception_propagation(self) -> None:
        procs: list[_FakeProcess] = []
        run_counter = [0]

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "run":
                run_counter[0] += 1
                run_id = f"0123456789abcdef-{run_counter[0]}"
                proc.stdout.push_event({"event": "run_start", "seq": 30, "t_ms": 2, "run": run_id, "id": cmd["id"]})
            elif cmd["command"] == "stop":
                run_id = cmd["run"]
                proc.stdout.push_event({
                    "event": "result",
                    "seq": 31,
                    "t_ms": 3,
                    "run": run_id,
                    "launcher_ends": False,
                    "status": {"launcher_state": "ready"},
                    "result": {"result": "stopped"},
                })

        def make_proc(cmd, **kw):
            p = _FakeProcess(cmd, handler)
            procs.append(p)
            return p

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=make_proc):
            with Serve("/fake/launch", "sim.yaml") as runner:
                r1 = runner.start()
                r1.stop()
                self.assertEqual(r1.wait()["result"], "stopped")
                r1.stop(force=True)  # no-op after result
                with self.assertRaises(TypeError):
                    r1.stop(force="yes")  # type: ignore[arg-type]

                with runner.start() as r2:
                    r2.stop(force=True)
                    r2.wait()

                # Early normal exit sends graceful stop(force=False) and keeps Serve alive.
                with runner.start() as r3:
                    pass
                self.assertEqual(r3.result, {"result": "stopped"})
                self.assertIsNone(runner.returncode)

                # Exception inside `with` still sends graceful stop and propagates original exception.
                with self.assertRaises(RuntimeError):
                    with runner.start() as r4:
                        raise RuntimeError("user body error")
                self.assertEqual(r4.result, {"result": "stopped"})

                stop_cmds = [c for c in procs[0].stdin.commands if c["command"] == "stop"]
                self.assertEqual(stop_cmds, [
                    {"command": "stop", "run": "0123456789abcdef-1"},
                    {"command": "stop", "run": "0123456789abcdef-2", "force": True},
                    {"command": "stop", "run": "0123456789abcdef-3", "force": False},
                    {"command": "stop", "run": "0123456789abcdef-4", "force": False},
                ])

    def test_run_control_lazy_import_and_run_id_binding(self) -> None:
        calls: list[tuple[tuple, dict]] = []

        class _FakeControl:
            def __init__(self, address: str, **kwargs) -> None:
                calls.append(((address,), kwargs))

        fake_client = types.ModuleType("vehicle_sil_control.client")
        fake_client.Control = _FakeControl  # type: ignore[attr-defined]

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "run":
                run_id = f"0123456789abcdef-{cmd['id']}"
                proc.stdout.push_event({"event": "run_start", "seq": 40, "t_ms": 2, "run": run_id, "id": cmd["id"]})
                if cmd["id"] == 1:
                    proc.stdout.push_event({
                        "event": "control_server",
                        "seq": 41,
                        "t_ms": 3,
                        "run": run_id,
                        "address": "127.0.0.1:45678",
                    })
                else:
                    proc.stdout.push_event({
                        "event": "result",
                        "seq": 42,
                        "t_ms": 4,
                        "run": run_id,
                        "launcher_ends": False,
                        "status": {"launcher_state": "ready"},
                        "result": {"result": "passed"},
                    })
            elif cmd["command"] == "stop":
                run_id = cmd["run"]
                proc.stdout.push_event({
                    "event": "result",
                    "seq": 44,
                    "t_ms": 6,
                    "run": run_id,
                    "launcher_ends": False,
                    "status": {"launcher_state": "ready"},
                    "result": {"result": "stopped"},
                })

        with mock.patch.dict(sys.modules, {"vehicle_sil_control.client": fake_client}), \
             mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=lambda cmd, **kw: _FakeProcess(cmd, handler)):
            with Serve("/fake/launch", "sim.yaml", control=True) as runner:
                with runner.start() as r1:
                    ctrl = r1.control(token="secret", timeout=5.0)
                    self.assertIsInstance(ctrl, _FakeControl)
                    self.assertEqual(calls, [(("127.0.0.1:45678",), {"run": r1.id, "token": "secret", "timeout": 5.0})])
                    with self.assertRaises(TypeError):
                        r1.control(run="other")

                # Run 2 has no control_server event; must not pick up Run 1's address.
                with runner.start() as r2:
                    r2.wait()
                    with self.assertRaises(ServeError):
                        _ = r2.address

    def test_command_id_rollover_send_and_bad_lines(self) -> None:
        procs: list[_FakeProcess] = []

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "status":
                proc.stdout.push_raw(b"{not-json\n")
                proc.stdout.push_event({"event": "status", "seq": 50, "t_ms": 1, "id": cmd["id"], "launcher_state": "idle"})
            elif cmd["command"] == "run":
                run_id = "0123456789abcdef-1"
                proc.stdout.push_event({"event": "run_start", "seq": 51, "t_ms": 2, "run": run_id, "id": cmd["id"]})
            elif cmd["command"] == "stop":
                proc.stdout.push_event({
                    "event": "result",
                    "seq": 52,
                    "t_ms": 3,
                    "run": cmd["run"],
                    "launcher_ends": False,
                    "status": {"launcher_state": "ready"},
                    "result": {"result": "stopped"},
                })

        def make_proc(cmd, **kw):
            p = _FakeProcess(cmd, handler)
            procs.append(p)
            return p

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=make_proc):
            with Serve("/fake/launch", "sim.yaml") as runner:
                runner._next_id = MAX_COMMAND_ID
                st1 = runner.status()
                st2 = runner.status()
                self.assertEqual(st1["id"], MAX_COMMAND_ID)
                self.assertEqual(st2["id"], 1)
                self.assertEqual(runner.bad_lines, [b"{not-json\n", b"{not-json\n"])

                with runner.start() as run:
                    run.send({"command": "pause"})
                    with self.assertRaises(ValueError):
                        run.send({"command": "pause", "run": "wrong"})
                    with self.assertRaises(ServeError) as too_big:
                        run.send({"command": "pause", "pad": "x" * MAX_COMMAND_LINE})
                    self.assertEqual(too_big.exception.reason, "invalid-command")
                self.assertEqual(procs[0].stdin.commands[3], {"command": "pause", "run": run.id})


if __name__ == "__main__":
    unittest.main()
