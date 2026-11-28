# SPDX-License-Identifier: MIT
"""Unit tests for python/vehicle_sil_control/serve.py (standard library only, no C++ build)."""
from __future__ import annotations

import ast
import collections
import gc
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
import weakref
from unittest import mock

_REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
_PYTHON_DIR = _REPO_ROOT / "python"
if str(_PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(_PYTHON_DIR))

from vehicle_sil_control import Launch, LaunchError, RunHandle, Serve, ServeError, __version__, control_address  # noqa: E402
from vehicle_sil_control.launcher import _MAX_FINDING_DETAIL_CHARS, _split_once_arguments, _why  # noqa: E402
from vehicle_sil_control.serve import MAX_COMMAND_ID, MAX_COMMAND_LINE, _MAX_BAD_LINES, _MAX_STDERR_CHARS  # noqa: E402


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
            "import vehicle_sil_control as c, vehicle_sil_control.launcher, vehicle_sil_control.serve; "
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

        for mod_name in ("launcher.py", "serve.py"):
            tree = ast.parse((_PYTHON_DIR / "vehicle_sil_control" / mod_name).read_text(encoding="utf-8"))
            for node in tree.body:
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

    def test_removed_stream_history_api_and_run_handle_gc(self) -> None:
        with self.assertRaises(TypeError):
            Serve("/fake/launch", "sim.yaml", keep_log_skeleton=True)  # type: ignore[call-arg]
        with self.assertRaises(TypeError):
            Serve("/fake/launch", "sim.yaml", on_event="not-callable")  # type: ignore[arg-type]

        seq = [2]
        procs: list[_FakeProcess] = []

        def next_seq() -> int:
            val = seq[0]
            seq[0] += 1
            return val

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "run":
                run_id = f"0123456789abcdef-{cmd['id']}"
                proc.stdout.push_event({"event": "run_start", "seq": next_seq(), "t_ms": 2, "run": run_id, "id": cmd["id"]})
                proc.stdout.push_event({"event": "phase", "seq": next_seq(), "t_ms": 3, "run": run_id, "phase": "running"})
                proc.stdout.push_event({
                    "event": "result",
                    "seq": next_seq(),
                    "t_ms": 4,
                    "run": run_id,
                    "launcher_ends": False,
                    "status": {"launcher_state": "ready", "last_run_id": run_id},
                    "result": {"result": "passed", "payload": "x" * 4096},
                })

        def make_proc(cmd, **kw):
            p = _FakeProcess(cmd, handler)
            procs.append(p)
            return p

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=make_proc):
            with Serve("/fake/launch", "sim.yaml") as runner:
                for removed in ("events", "snapshot", "window", "wait_for", "wait_event", "ready"):
                    self.assertFalse(hasattr(runner, removed), f"Serve should not expose {removed}")

                refs: list[weakref.ref[RunHandle]] = []
                for _ in range(5):
                    run = runner.start()
                    refs.append(weakref.ref(run))
                    res = run.wait()
                    self.assertEqual(res["result"], "passed")
                    self.assertIsNone(runner._active_run)
                    del run
                    gc.collect()
                    self.assertIsNone(refs[-1]())

                # No further events arrive while Serve is idle; all completed handles stay collected.
                self.assertTrue(all(ref() is None for ref in refs))

        # EOF without a result event also releases _active_run so the handle can be GC'd.
        def crash_mid_run_handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "run":
                proc.stdout.push_event({"event": "run_start", "seq": 10, "t_ms": 2, "run": "0123456789abcdef-1", "id": cmd["id"]})
                proc.finish(1)

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=lambda cmd, **kw: _FakeProcess(cmd, crash_mid_run_handler)):
            with Serve("/fake/launch", "sim.yaml") as runner:
                run = runner.start()
                ref = weakref.ref(run)
                with self.assertRaises(ServeError):
                    run.wait()
                self.assertIsNone(runner._active_run)
                del run
                gc.collect()
                self.assertIsNone(ref())

    def test_rapid_run_start_result_and_immediate_eof_race_free(self) -> None:
        # Simulate once=True or ultra-fast run where run_start, result, and EOF arrive before start() waits.
        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "run":
                run_id = "0123456789abcdef-1"
                proc.stdout.push_event({"event": "run_start", "seq": 2, "t_ms": 2, "run": run_id, "id": cmd["id"]})
                proc.stdout.push_event({"event": "phase", "seq": 3, "t_ms": 3, "run": run_id, "phase": "running"})
                proc.stdout.push_event({
                    "event": "result",
                    "seq": 4,
                    "t_ms": 4,
                    "run": run_id,
                    "launcher_ends": True,
                    "status": {"launcher_state": "ended", "last_run_id": run_id},
                    "result": {"result": "passed"},
                })
                proc.finish(0)
                # Give the reader thread a chance to consume all events and EOF before start() enters wait().
                time.sleep(0.02)

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=lambda cmd, **kw: _FakeProcess(cmd, handler)):
            with Serve("/fake/launch", "sim.yaml", once=True) as runner:
                run = runner.start()
                ref = weakref.ref(run)
                self.assertEqual(run.id, "0123456789abcdef-1")
                self.assertEqual(run.wait()["result"], "passed")
                self.assertTrue(run.launcher_ends)
                self.assertEqual([e["event"] for e in run.events], ["run_start", "phase", "result"])
                self.assertIsNone(runner._active_run)
                self.assertEqual(runner._pending, {})
                del run
                gc.collect()
                self.assertIsNone(ref())

    def test_on_event_order_happens_before_logs_isolation_lockfree_and_exceptions(self) -> None:
        seq = [2]

        def next_seq() -> int:
            val = seq[0]
            seq[0] += 1
            return val

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "prepare":
                proc.stdout.push_event({
                    "event": "prepare",
                    "seq": next_seq(),
                    "t_ms": 2,
                    "id": cmd["id"],
                    "accepted": True,
                    "status": {"launcher_state": "ready"},
                    "findings": [],
                })
            elif cmd["command"] == "run":
                run_id = "0123456789abcdef-1"
                proc.stdout.push_event({"event": "run_start", "seq": next_seq(), "t_ms": 3, "run": run_id, "id": cmd["id"]})
                proc.stdout.push_event({"event": "log", "seq": next_seq(), "t_ms": 4, "run": run_id, "text": "hello log"})
                proc.stdout.push_event({"event": "heartbeat", "seq": next_seq(), "t_ms": 5})
                proc.stdout.push_event({
                    "event": "result",
                    "seq": next_seq(),
                    "t_ms": 6,
                    "run": run_id,
                    "launcher_ends": False,
                    "status": {"launcher_state": "ready", "last_run_id": run_id},
                    "result": {"result": "passed"},
                })

        seen: list[tuple[str, int]] = []
        hook_calls: list[tuple[type, BaseException]] = []
        in_callback = threading.Event()
        release_callback = threading.Event()

        def on_event(ev: dict) -> None:
            etype = ev.get("event")
            seen.append((etype, ev.get("seq")))
            # Mutate the delivered shallow copy aggressively; Serve and RunHandle must not be affected.
            ev["event"] = "corrupted"
            ev["run"] = "wrong-run"
            ev.clear()
            if etype == "log":
                in_callback.set()
                release_callback.wait(timeout=5.0)
                raise RuntimeError("callback failure on log")

        def fake_excepthook(exc_type, exc_val, exc_tb) -> None:
            hook_calls.append((exc_type, exc_val))

        with mock.patch.object(sys, "excepthook", side_effect=fake_excepthook), \
             mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=lambda cmd, **kw: _FakeProcess(cmd, handler)):
            with Serve("/fake/launch", "sim.yaml", keep_logs=False, on_event=on_event) as runner:
                # Happens-before: hello and initial status are already delivered before __init__ returns.
                self.assertEqual([e for e, _ in seen[:2]], ["hello", "status"])
                self.assertEqual(runner.instance, "0123456789abcdef")

                prep = runner.prepare()
                self.assertTrue(prep["accepted"])
                self.assertIn("prepare", [e for e, _ in seen])

                run = runner.start()
                # Verify on_event runs outside self._changed: while on_event is paused inside "log",
                # reading runner.latest_status / runner.bad_lines from the main thread must not deadlock.
                self.assertTrue(in_callback.wait(timeout=5.0))
                self.assertIsNotNone(runner.latest_status)
                self.assertEqual(runner.bad_lines, [])
                release_callback.set()

                res = run.wait()
                self.assertEqual(res["result"], "passed")
                # Happens-before: result was already delivered to on_event before run.wait() returned.
                self.assertEqual(
                    [e for e, _ in seen],
                    ["hello", "status", "prepare", "run_start", "log", "heartbeat", "result"],
                )
                self.assertEqual([s for _, s in seen], [0, 1, 2, 3, 4, 5, 6])
                # keep_logs=False suppresses "log" from RunHandle.events, not from on_event.
                self.assertEqual([e["event"] for e in run.events], ["run_start", "result"])
                self.assertEqual(run.start_event["event"], "run_start")

        self.assertEqual(len(hook_calls), 1)
        self.assertIs(hook_calls[0][0], RuntimeError)
        self.assertEqual(str(hook_calls[0][1]), "callback failure on log")

    def test_pending_non_leak_unmatched_replies_duplicate_id_and_wait_hello(self) -> None:
        procs: list[_FakeProcess] = []
        block_status = threading.Event()

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "status" and cmd["id"] == 500:
                return  # timeout; delayed reply pushed later
            if cmd["command"] == "status" and cmd["id"] == 77:
                return  # held open to test duplicate in-flight id

        def make_proc(cmd, **kw):
            p = _FakeProcess(cmd, handler)
            procs.append(p)
            return p

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=make_proc):
            with Serve("/fake/launch", "sim.yaml") as runner:
                # 1. Timeout cleans up _pending, and a late reply does not repopulate it.
                with self.assertRaises(ServeError):
                    runner.status(timeout=0.05, id=500)
                self.assertEqual(runner._pending, {})
                procs[0].stdout.push_event({"event": "status", "seq": 10, "t_ms": 2, "id": 500, "launcher_state": "idle"})

                # 2. Unmatched replies from raw stdin commands do not leak into _pending.
                procs[0].stdout.push_event({"event": "prepare", "seq": 11, "t_ms": 3, "id": 901, "accepted": True})
                procs[0].stdout.push_event({
                    "event": "command",
                    "seq": 12,
                    "t_ms": 4,
                    "command": "run",
                    "accepted": False,
                    "reason": "busy",
                    "id": 902,
                })
                procs[0].stdout.push_event({"event": "status", "seq": 13, "t_ms": 5, "id": 903, "launcher_state": "ready"})

                # 3. Duplicate in-flight explicit id raises without disturbing the original waiter.
                first_result: list[dict] = []

                def first_call() -> None:
                    block_status.set()
                    first_result.append(runner.status(timeout=5.0, id=77))

                t = threading.Thread(target=first_call)
                t.start()
                self.assertTrue(block_status.wait(timeout=2.0))
                deadline = time.monotonic() + 2.0
                while 77 not in runner._pending and time.monotonic() < deadline:
                    time.sleep(0.005)
                self.assertIn(77, runner._pending)
                with self.assertRaises(ServeError) as dup:
                    runner.status(timeout=1.0, id=77)
                self.assertEqual(dup.exception.reason, "invalid-command")
                procs[0].stdout.push_event({"event": "status", "seq": 14, "t_ms": 6, "id": 77, "launcher_state": "ready"})
                t.join(timeout=5.0)
                self.assertFalse(t.is_alive())
                self.assertEqual(first_result[0]["id"], 77)
                self.assertEqual(runner._pending, {})

        # 4. _wait_hello() works with wait_ready=False and fails cleanly on EOF without hello.
        def delayed_hello(cmd, **kw):
            p = _FakeProcess(cmd, auto_hello=False)
            def push() -> None:
                time.sleep(0.02)
                p.stdout.push_event({"event": "hello", "seq": 0, "t_ms": 0, "vehicle_sil_events": 4, "launcher": "fedcba9876543210"})
            threading.Thread(target=push, daemon=True).start()
            return p

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=delayed_hello):
            with Serve("/fake/launch", "sim.yaml", wait_ready=False) as runner:
                self.assertEqual(runner.instance, "fedcba9876543210")
                self.assertEqual(runner.hello["launcher"], "fedcba9876543210")

        def no_hello_eof(cmd, **kw):
            p = _FakeProcess(cmd, auto_hello=False)
            p.finish(0)
            return p

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=no_hello_eof):
            with Serve("/fake/launch", "sim.yaml", wait_ready=False) as runner:
                with self.assertRaises(ServeError):
                    _ = runner.hello

    def test_bad_lines_and_stderr_text_are_bounded(self) -> None:
        procs: list[_FakeProcess] = []

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "status":
                proc.stdout.push_event({"event": "status", "seq": 999, "t_ms": 10, "id": cmd["id"], "launcher_state": "ready"})

        def make_proc(cmd, **kw):
            p = _FakeProcess(cmd, handler, stderr_pipe=True)
            procs.append(p)
            return p

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=make_proc):
            with Serve("/fake/launch", "sim.yaml", stderr=subprocess.PIPE) as runner:
                assert procs[0].stderr is not None
                for i in range(_MAX_BAD_LINES + 20):
                    procs[0].stdout.push_raw(f"{{bad-{i}\n".encode("ascii"))
                procs[0].stdout.push_raw(b"123\n")
                procs[0].stdout.push_raw(b"[1, 2]\n")
                # Sync reader thread via status().
                runner.status()

                bad = runner.bad_lines
                self.assertEqual(len(bad), _MAX_BAD_LINES)
                self.assertEqual(bad[-2:], [b"123\n", b"[1, 2]\n"])
                # Mutating the returned copy must not affect internal storage.
                bad.clear()
                self.assertEqual(len(runner.bad_lines), _MAX_BAD_LINES)

                # Push > 64 KiB of stderr across many chunks (verifies multi-chunk tail trimming).
                multi_chunks: list[str] = []
                for i in range(100):
                    chunk = f"line-{i:04d}-" + ("a" * 1000) + "\n"
                    multi_chunks.append(chunk)
                    procs[0].stderr.push_raw(chunk.encode("ascii"))
                multi_full = "".join(multi_chunks)
                deadline = time.monotonic() + 2.0
                while not runner.stderr_text.endswith("line-0099-" + ("a" * 1000) + "\n") and time.monotonic() < deadline:
                    time.sleep(0.005)
                self.assertEqual(len(runner.stderr_text), _MAX_STDERR_CHARS)
                self.assertEqual(runner.stderr_text, multi_full[-_MAX_STDERR_CHARS:])

                # Then push a single line exceeding 64 KiB (verifies single-chunk cap).
                oversized = "Z" * (_MAX_STDERR_CHARS + 500) + "\n"
                procs[0].stderr.push_raw(oversized.encode("ascii"))

            self.assertEqual(len(runner.stderr_text), _MAX_STDERR_CHARS)
            self.assertEqual(runner.stderr_text, oversized[-_MAX_STDERR_CHARS:])

    def test_on_event_reentrancy_fails_fast_and_preserves_cleanup(self) -> None:
        procs: list[_FakeProcess] = []
        seq = [2]

        def next_seq() -> int:
            val = seq[0]
            seq[0] += 1
            return val

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "run":
                if "request" in cmd:
                    # Leave oversized start pending so it times out and leaves a temp file to clean on close().
                    return
                run_id = "0123456789abcdef-1"
                proc.stdout.push_event({"event": "run_start", "seq": next_seq(), "t_ms": 2, "run": run_id, "id": cmd["id"]})
            elif cmd["command"] == "stop":
                proc.stdout.push_event({
                    "event": "result",
                    "seq": next_seq(),
                    "t_ms": 9,
                    "run": cmd["run"],
                    "launcher_ends": False,
                    "status": {"launcher_state": "ready", "last_run_id": cmd["run"]},
                    "result": {"result": "stopped"},
                })

        def make_proc(cmd, **kw):
            p = _FakeProcess(cmd, handler)
            procs.append(p)
            return p

        results: dict[str, object] = {}
        active_run_holder: list[RunHandle] = []
        run_ready = threading.Event()
        checked_phase = threading.Event()
        checked_after_cs = threading.Event()
        checked_close_event = threading.Event()

        class _ReentryServe(Serve):
            def __init__(self) -> None:
                super().__init__("/fake/launch", "sim.yaml", on_event=self._cb)

            def _cb(self, ev: dict) -> None:
                etype = ev.get("event")
                if etype == "hello":
                    for name, fn in (("hello", lambda: self.hello), ("instance", lambda: self.instance)):
                        try:
                            fn()
                            results[f"on_hello_{name}"] = "unexpected-ok"
                        except ServeError as exc:
                            results[f"on_hello_{name}"] = str(exc)
                elif etype == "status":
                    # After "hello" has been processed, reading hello/instance inside on_event succeeds.
                    results["on_status_instance"] = self.instance
                    results["on_status_hello"] = self.hello["launcher"]
                    for name, fn in (
                        ("prepare", lambda: self.prepare(timeout=5.0)),
                        ("status", lambda: self.status(timeout=5.0)),
                        ("start", lambda: self.start(timeout=5.0)),
                        ("wait", lambda: self.wait(timeout=5.0)),
                    ):
                        try:
                            fn()
                            results[f"on_status_{name}"] = "unexpected-ok"
                        except ServeError as exc:
                            results[f"on_status_{name}"] = str(exc)
                elif etype == "phase":
                    run_ready.wait(timeout=5.0)
                    run = active_run_holder[0]
                    for name, fn in (
                        ("address", lambda: run.address),
                        ("wait", lambda: run.wait(timeout=5.0)),
                        ("wait_for_event", lambda: run.wait_for_event("control_server", timeout=5.0)),
                    ):
                        try:
                            fn()
                            results[f"on_phase_{name}"] = "unexpected-ok"
                        except ServeError as exc:
                            results[f"on_phase_{name}"] = str(exc)
                    checked_phase.set()
                elif etype == "log":
                    # control_server has already been processed; run.address must succeed without blocking.
                    results["on_log_address"] = active_run_holder[0].address
                    checked_after_cs.set()
                elif etype == "heartbeat":
                    try:
                        self.close(timeout=5.0)
                        results["on_heartbeat_close"] = "unexpected-ok"
                    except ServeError as exc:
                        results["on_heartbeat_close"] = str(exc)
                    checked_close_event.set()

        t0 = time.monotonic()
        temp_paths: list[str] = []
        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=make_proc):
            with _ReentryServe() as runner:
                # Create a timed-out oversized intent so a temp file is tracked in runner._temp_files.
                big = "x" * (MAX_COMMAND_LINE + 4096)
                with self.assertRaises(ServeError):
                    runner.start(params={"pad": big}, timeout=0.05)
                temp_paths = list(runner._temp_files)
                self.assertEqual(len(temp_paths), 1)
                self.assertTrue(pathlib.Path(temp_paths[0]).exists())

                # Trigger close() from inside on_event; must fail fast and NOT poison _closed.
                procs[0].stdout.push_event({"event": "heartbeat", "seq": next_seq(), "t_ms": 3})
                self.assertTrue(checked_close_event.wait(timeout=5.0))
                self.assertFalse(runner._closed)

                # Start a real run and verify RunHandle fail-fast before control_server/result,
                # and immediate success for run.address once control_server has been processed.
                run = runner.start()
                active_run_holder.append(run)
                run_ready.set()
                procs[0].stdout.push_event({"event": "phase", "seq": next_seq(), "t_ms": 4, "run": run.id, "phase": "running"})
                self.assertTrue(checked_phase.wait(timeout=5.0))
                procs[0].stdout.push_event({
                    "event": "control_server",
                    "seq": next_seq(),
                    "t_ms": 5,
                    "run": run.id,
                    "address": "127.0.0.1:41234",
                })
                procs[0].stdout.push_event({"event": "log", "seq": next_seq(), "t_ms": 6, "run": run.id, "text": "after cs"})
                self.assertTrue(checked_after_cs.wait(timeout=5.0))
                run.stop()
                self.assertEqual(run.wait()["result"], "stopped")

            # Outer close() must still run and delete the temp file.
            self.assertTrue(runner._closed)
            self.assertFalse(pathlib.Path(temp_paths[0]).exists())

        self.assertLess(time.monotonic() - t0, 2.0)
        for key in (
            "on_hello_hello",
            "on_hello_instance",
            "on_status_prepare",
            "on_status_status",
            "on_status_start",
            "on_status_wait",
            "on_phase_address",
            "on_phase_wait",
            "on_phase_wait_for_event",
            "on_heartbeat_close",
        ):
            self.assertIn("on_event", str(results.get(key)), f"{key}: {results.get(key)!r}")
        self.assertEqual(results["on_status_instance"], "0123456789abcdef")
        self.assertEqual(results["on_status_hello"], "0123456789abcdef")
        self.assertEqual(results["on_log_address"], "127.0.0.1:41234")

    def test_latest_status_schema_consistent_across_status_prepare_and_result(self) -> None:
        seq = [2]

        def next_seq() -> int:
            val = seq[0]
            seq[0] += 1
            return val

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "prepare":
                proc.stdout.push_event({
                    "event": "prepare",
                    "seq": next_seq(),
                    "t_ms": 2,
                    "id": cmd["id"],
                    "accepted": True,
                    "status": {"instance": "0123456789abcdef", "status_rev": 1, "launcher_state": "ready"},
                    "findings": [],
                })
            elif cmd["command"] == "status":
                proc.stdout.push_event({
                    "event": "status",
                    "seq": next_seq(),
                    "t_ms": 3,
                    "id": cmd["id"],
                    "instance": "0123456789abcdef",
                    "status_rev": 2,
                    "launcher_state": "ready",
                    "active_run_id": None,
                })
            elif cmd["command"] == "run":
                run_id = "0123456789abcdef-1"
                proc.stdout.push_event({"event": "run_start", "seq": next_seq(), "t_ms": 4, "run": run_id, "id": cmd["id"]})
                proc.stdout.push_event({
                    "event": "result",
                    "seq": next_seq(),
                    "t_ms": 5,
                    "run": run_id,
                    "launcher_ends": False,
                    "status": {
                        "instance": "0123456789abcdef",
                        "status_rev": 3,
                        "launcher_state": "ready",
                        "last_run_id": run_id,
                    },
                    "result": {"result": "passed"},
                })

        envelope_keys = {"event", "seq", "t_ms", "id"}
        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=lambda cmd, **kw: _FakeProcess(cmd, handler)):
            with Serve("/fake/launch", "sim.yaml") as runner:
                st_init = runner.latest_status
                self.assertIsNotNone(st_init)
                assert st_init is not None
                self.assertEqual(st_init["launcher_state"], "unprepared")
                self.assertEqual(st_init["status_rev"], 0)
                self.assertTrue(envelope_keys.isdisjoint(st_init.keys()))

                runner.prepare()
                st_prep = runner.latest_status
                assert st_prep is not None
                self.assertEqual(st_prep["launcher_state"], "ready")
                self.assertEqual(st_prep["status_rev"], 1)
                self.assertTrue(envelope_keys.isdisjoint(st_prep.keys()))

                ev_status = runner.status()
                self.assertEqual(ev_status["event"], "status")
                self.assertIn("seq", ev_status)
                st_query = runner.latest_status
                assert st_query is not None
                self.assertEqual(st_query["status_rev"], 2)
                self.assertTrue(envelope_keys.isdisjoint(st_query.keys()))

                run = runner.start()
                run.wait()
                st_done = runner.latest_status
                assert st_done is not None
                self.assertEqual(st_done["status_rev"], 3)
                self.assertEqual(st_done["last_run_id"], "0123456789abcdef-1")
                self.assertTrue(envelope_keys.isdisjoint(st_done.keys()))

    def test_why_includes_finding_detail_with_cap(self) -> None:
        short_detail = "the end of its standard error:\nmissing libfoo.so.1"
        ev = {
            "event": "result",
            "result": {
                "result": "failed",
                "findings": [],
                "check": {
                    "findings": [
                        {"severity": "warning", "message": "ignored warning", "detail": "ignored detail"},
                        {"severity": "error", "message": "describe failed", "detail": short_detail},
                        {"severity": "error", "message": "second error", "detail": None},
                        {"severity": "error", "message": "blank detail", "detail": "   \n  "},
                    ],
                },
            },
        }
        why = _why(ev)
        self.assertIn("describe failed", why)
        self.assertIn("missing libfoo.so.1", why)
        self.assertIn("second error", why)
        self.assertIn("blank detail", why)
        self.assertNotIn("ignored warning", why)
        self.assertNotIn("ignored detail", why)
        self.assertNotIn("second error (", why)
        self.assertNotIn("blank detail (", why)

        with self.assertRaises(LaunchError) as ctx:
            control_address([ev])
        self.assertIn("missing libfoo.so.1", str(ctx.exception))

        # Long detail keeps both leading context (head) and final error line (tail), bounded.
        head_marker = "the end of its standard error:\nTraceback (most recent call last):"
        tail_marker = "ModuleNotFoundError: No module named 'missing_dep'"
        long_detail = f"{head_marker}\n" + ("middle line\n" * 60) + tail_marker
        self.assertGreater(len(long_detail), _MAX_FINDING_DETAIL_CHARS)
        long_ev = {
            "event": "result",
            "result": {
                "result": "failed",
                "findings": [{"severity": "error", "message": "boom", "detail": long_detail}],
            },
        }
        long_why = _why(long_ev)
        self.assertIn(head_marker, long_why)
        self.assertIn(" ... ", long_why)
        self.assertIn(tail_marker, long_why)
        self.assertLess(len(long_why), _MAX_FINDING_DETAIL_CHARS + 64)

    def test_abnormal_exit_includes_command_returncode_and_stderr_tail(self) -> None:
        # 1. Serve startup crash includes command, exit code, and stderr tail.
        def crash_startup(cmd, **kw):
            proc = _FakeProcess(cmd, auto_hello=False, stderr_pipe=True)
            assert proc.stderr is not None
            proc.stderr.push_raw(b"startup boom on stderr\n")
            proc.finish(7)
            return proc

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=crash_startup):
            with self.assertRaises(ServeError) as ctx:
                Serve("/fake/launch", "sim.yaml", control=True, stderr=subprocess.PIPE, startup_timeout=5.0)
        msg = str(ctx.exception)
        self.assertIn("exit code 7", msg)
        self.assertIn("/fake/launch serve --control 0 -- sim.yaml", msg)
        self.assertIn("startup boom on stderr", msg)

        # 2. Serve mid-run crash includes command, exit code, and stderr tail in RunHandle.wait() and wait_for_event().
        def crash_mid_run(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "run":
                proc.stdout.push_event({"event": "run_start", "seq": 10, "t_ms": 2, "run": "0123456789abcdef-1", "id": cmd["id"]})
                assert proc.stderr is not None
                proc.stderr.push_raw(b"mid-run segfault detail\n")
                proc.finish(139)

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen",
                        side_effect=lambda cmd, **kw: _FakeProcess(cmd, crash_mid_run, stderr_pipe=True)):
            with Serve("/fake/launch", "sim.yaml", stderr=subprocess.PIPE) as runner:
                run = runner.start()
                with self.assertRaises(ServeError) as wait_ctx:
                    run.wait()
                self.assertIn("exit code 139", str(wait_ctx.exception))
                self.assertIn("/fake/launch serve -- sim.yaml", str(wait_ctx.exception))
                self.assertIn("mid-run segfault detail", str(wait_ctx.exception))
                with self.assertRaises(ServeError) as ev_ctx:
                    run.wait_for_event("control_server")
                self.assertIn("exit code 139", str(ev_ctx.exception))
                self.assertIn("mid-run segfault detail", str(ev_ctx.exception))

        # 3. Serve crashes while answering prepare().
        def crash_on_prepare(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "prepare":
                assert proc.stderr is not None
                proc.stderr.push_raw(b"prepare crashed\n")
                proc.finish(2)

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen",
                        side_effect=lambda cmd, **kw: _FakeProcess(cmd, crash_on_prepare, stderr_pipe=True)):
            with Serve("/fake/launch", "sim.yaml", stderr=subprocess.PIPE) as runner:
                with self.assertRaises(ServeError) as prep_ctx:
                    runner.prepare()
                self.assertIn("exit code 2", str(prep_ctx.exception))
                self.assertIn("/fake/launch serve -- sim.yaml", str(prep_ctx.exception))
                self.assertIn("prepare crashed", str(prep_ctx.exception))

        # 4. Launch crash without result includes command, exit code, and stderr tail.
        def crash_launch(cmd, **kw):
            proc = _FakeProcess(cmd, auto_hello=False, stderr_pipe=True)
            assert proc.stderr is not None
            proc.stderr.push_raw(b"launch fatal stderr\n")
            proc.finish(5)
            return proc

        with mock.patch("vehicle_sil_control.launcher.subprocess.Popen", side_effect=crash_launch):
            with Launch("/fake/launch", "sim.yaml", stderr=subprocess.PIPE) as launch:
                with self.assertRaises(LaunchError) as l_ctx:
                    launch.wait()
                self.assertIn("code 5", str(l_ctx.exception))
                self.assertIn("/fake/launch serve --once --control 0 -- sim.yaml", str(l_ctx.exception))
                self.assertIn("launch fatal stderr", str(l_ctx.exception))

    def test_idle_crash_between_commands_and_broken_pipe_include_exit_context(self) -> None:
        procs: list[_FakeProcess] = []

        def make_proc(cmd, **kw):
            p = _FakeProcess(cmd, stderr_pipe=True)
            procs.append(p)
            return p

        # 1. Idle crash between commands: _next_command_id() reports exit code, command, and stderr tail.
        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=make_proc):
            with Serve("/fake/launch", "sim.yaml", stderr=subprocess.PIPE) as runner:
                assert procs[0].stderr is not None
                procs[0].stderr.push_raw(b"idle crash fatal stderr\n")
                procs[0].finish(134)
                deadline = time.monotonic() + 2.0
                while not runner._ended and time.monotonic() < deadline:
                    time.sleep(0.005)
                self.assertTrue(runner._ended)
                for call in (runner.prepare, runner.status, runner.start):
                    with self.assertRaises(ServeError) as ctx:
                        call()
                    msg = str(ctx.exception)
                    self.assertIn("exit code 134", msg)
                    self.assertIn("/fake/launch serve -- sim.yaml", msg)
                    self.assertIn("idle crash fatal stderr", msg)

                # Explicit id= bypasses _next_command_id() and hits _write_line() BrokenPipeError.
                def broken_write(_data: bytes) -> int:
                    raise BrokenPipeError("broken pipe")

                procs[0].stdin.write = broken_write  # type: ignore[method-assign]
                for call in (
                    lambda: runner.prepare(id=41),
                    lambda: runner.status(id=42),
                    lambda: runner.start(id=43),
                ):
                    with self.assertRaises(ServeError) as w_ctx:
                        call()
                    msg = str(w_ctx.exception)
                    self.assertIn("exit code 134", msg)
                    self.assertIn("/fake/launch serve -- sim.yaml", msg)
                    self.assertIn("idle crash fatal stderr", msg)

        # 2. Normal close() does NOT attach abnormal-exit context on post-close calls.
        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", side_effect=lambda cmd, **kw: _FakeProcess(cmd, stderr_pipe=True)):
            runner = Serve("/fake/launch", "sim.yaml", stderr=subprocess.PIPE)
            runner.close()
            with self.assertRaises(ServeError) as closed_ctx:
                runner.status()
            self.assertEqual(str(closed_ctx.exception), "vehicle_sil_launch serve has ended")

    def test_launch_pid_and_participant_pids(self) -> None:
        # 1. Launch.pid and Launch.participant_pids.
        def launch_popen(cmd, **kw):
            proc = _FakeProcess(cmd, auto_hello=False)
            proc.pid = 7777
            proc.stdout.push_event({"event": "control_server", "seq": 1, "address": "127.0.0.1:5000"})
            proc.stdout.push_event({"event": "participant", "seq": 2, "name": "Engine", "state": "started", "pid": 8001})
            proc.stdout.push_event({"event": "participant", "seq": 3, "name": "Dashboard", "state": "started", "pid": 8002})
            proc.stdout.push_event({"event": "participant", "seq": 4, "name": "Engine", "state": "joined"})
            proc.stdout.push_event({"event": "participant", "seq": 5, "name": "Disabled", "state": "disabled"})
            proc.stdout.push_event({"event": "participant", "seq": 6, "name": "BadPid", "state": "started", "pid": True})
            proc.stdout.push_event({"event": "result", "seq": 7, "result": {"result": "passed"}})
            proc.finish(0)
            return proc

        with mock.patch("vehicle_sil_control.launcher.subprocess.Popen", side_effect=launch_popen):
            with Launch("/fake/launch", "sim.yaml") as launch:
                self.assertEqual(launch.pid, 7777)
                self.assertEqual(launch.wait()["result"], "passed")
                pids = launch.participant_pids
                self.assertEqual(pids, {"Engine": 8001, "Dashboard": 8002})
                # Mutating the returned copy must not affect subsequent calls.
                pids["Engine"] = 0
                self.assertEqual(launch.participant_pids, {"Engine": 8001, "Dashboard": 8002})

        # 2. RunHandle.participant_pids is isolated per run.
        run_num = [0]

        def serve_handler(proc: _FakeProcess, cmd: dict) -> None:
            if cmd["command"] == "run":
                run_num[0] += 1
                n = run_num[0]
                run_id = f"0123456789abcdef-{n}"
                proc.stdout.push_event({"event": "run_start", "seq": n * 10, "t_ms": 1, "run": run_id, "id": cmd["id"]})
                proc.stdout.push_event({
                    "event": "participant",
                    "seq": n * 10 + 1,
                    "t_ms": 2,
                    "run": run_id,
                    "name": "Engine",
                    "state": "started",
                    "pid": 9000 + n,
                })
                proc.stdout.push_event({
                    "event": "result",
                    "seq": n * 10 + 2,
                    "t_ms": 3,
                    "run": run_id,
                    "launcher_ends": False,
                    "status": {"launcher_state": "ready"},
                    "result": {"result": "passed"},
                })

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen",
                        side_effect=lambda cmd, **kw: _FakeProcess(cmd, serve_handler)):
            with Serve("/fake/launch", "sim.yaml") as runner:
                r1 = runner.start()
                r1.wait()
                self.assertEqual(r1.participant_pids, {"Engine": 9001})
                r2 = runner.start()
                r2.wait()
                self.assertEqual(r2.participant_pids, {"Engine": 9002})
                self.assertEqual(r1.participant_pids, {"Engine": 9001})

    def test_serve_start_case_params_and_after_scenario(self) -> None:
        sent: list[dict] = []

        def handler(proc: _FakeProcess, cmd: dict) -> None:
            sent.append(cmd)
            if cmd["command"] == "run":
                run_id = f"0123456789abcdef-{len(sent)}"
                proc.stdout.push_event({"event": "run_start", "seq": len(sent) * 10, "t_ms": 1, "run": run_id, "id": cmd["id"]})
                proc.stdout.push_event({
                    "event": "result",
                    "seq": len(sent) * 10 + 1,
                    "t_ms": 2,
                    "run": run_id,
                    "launcher_ends": False,
                    "status": {"launcher_state": "ready"},
                    "result": {"result": "passed"},
                })

        with mock.patch("vehicle_sil_control.serve.subprocess.Popen",
                        side_effect=lambda cmd, **kw: _FakeProcess(cmd, handler)):
            with Serve("/fake/launch", "sim.yaml") as runner:
                r = runner.start(
                    scenario="scenarios/cases.yaml",
                    case="-10C",
                    params={"lvl": 50, "tag": "0123", "flag": True, "gain": 1.5},
                    after_scenario="pause",
                )
                self.assertEqual(r.wait()["result"], "passed")
                self.assertEqual(
                    sent[0]["intent"],
                    {
                        "scenario": "scenarios/cases.yaml",
                        "case": "-10C",
                        "params": {"lvl": 50, "tag": "0123", "flag": True, "gain": 1.5},
                        "after_scenario": "pause",
                    },
                )

                with self.assertRaises(TypeError):
                    runner.start(scenario="s.yaml", case=123)  # type: ignore[arg-type]
                with self.assertRaises(TypeError):
                    runner.start(scenario="s.yaml", after_scenario=True)  # type: ignore[arg-type]
                with self.assertRaises(ServeError) as big_ctx:
                    runner.start(intent={"scenario": "s.yaml", "cli_params": {"pad": "x" * 70000}})
                self.assertEqual(big_ctx.exception.reason, "invalid-command")

    def test_launch_split_once_arguments_case_cli_params_and_kwargs(self) -> None:
        # --param forwards raw strings verbatim under cli_params without type guessing.
        host, cmd = _split_once_arguments(
            [
                "--scenario",
                "scn.yaml",
                "--case",
                "cold",
                "--after-scenario",
                "pause",
                "--param",
                "code=0123",
                "--param",
                "tag=50",
                "--param",
                "hex=0x10",
                "--param",
                "eq=a=b",
                "--log-dir",
                "logs",
            ],
            start_paused=False,
        )
        self.assertEqual(host, ["--log-dir", "logs"])
        self.assertEqual(
            cmd,
            {
                "command": "run",
                "intent": {
                    "scenario": "scn.yaml",
                    "case": "cold",
                    "after_scenario": "pause",
                    "cli_params": {"code": "0123", "tag": "50", "hex": "0x10", "eq": "a=b"},
                },
            },
        )

        # Typed params kwarg coexists with cli_params from --param alongside case and after_scenario kwargs.
        _, merged = _split_once_arguments(
            ["--scenario", "scn.yaml", "--param", "tag=50"],
            start_paused=False,
            case="kw_case",
            params={"tag": 10, "keep": True, "ratio": 0.25},
            after_scenario="pause",
        )
        self.assertEqual(
            merged["intent"],
            {
                "scenario": "scn.yaml",
                "case": "kw_case",
                "params": {"tag": 10, "keep": True, "ratio": 0.25},
                "after_scenario": "pause",
                "cli_params": {"tag": "50"},
            },
        )

        # Validation errors: malformed/duplicate --param, bad types, conflicting/duplicate case or
        # after_scenario, and combining --request with any inline run option.
        with self.assertRaises(ValueError):
            _split_once_arguments(["--param", "missing_equals"], start_paused=False)
        with self.assertRaises(ValueError) as dup_param:
            _split_once_arguments(["--param", "n_thr=11", "--param", "n_thr=22"], start_paused=False)
        self.assertIn("n_thr", str(dup_param.exception))
        with self.assertRaises(TypeError):
            _split_once_arguments([], start_paused=False, case=1)  # type: ignore[arg-type]
        with self.assertRaises(TypeError):
            _split_once_arguments([], start_paused=False, params=["a=1"])  # type: ignore[arg-type]
        with self.assertRaises(TypeError):
            _split_once_arguments([], start_paused=False, after_scenario=1)  # type: ignore[arg-type]
        with self.assertRaises(ValueError):
            _split_once_arguments(["--case", "cli_case"], start_paused=False, case="kw_case")
        with self.assertRaises(ValueError):
            _split_once_arguments(["--case", "c1", "--case", "c2"], start_paused=False)
        with self.assertRaises(ValueError):
            _split_once_arguments(["--after-scenario", "stop"], start_paused=False, after_scenario="pause")
        with self.assertRaises(ValueError):
            _split_once_arguments(
                ["--after-scenario", "pause", "--after-scenario", "stop"], start_paused=False
            )
        for bad_args, bad_kwargs in [
            (["--request", "bench.json"], {"case": "c1"}),
            (["--request", "bench.json"], {"params": {"k": 1}}),
            (["--request", "bench.json"], {"after_scenario": "pause"}),
            (["--request", "bench.json", "--case", "high"], {}),
            (["--request", "bench.json", "--param", "n_thr=77"], {}),
            (["--request", "bench.json", "--after-scenario", "pause"], {}),
            (["--request", "bench.json", "--variant", "v1"], {}),
            (["--request", "bench.json"], {"start_paused": True}),
        ]:
            with self.assertRaises(ValueError):
                _split_once_arguments(
                    bad_args,
                    start_paused=bool(bad_kwargs.pop("start_paused", False)),
                    **bad_kwargs,
                )
        # Host flags and --timeline still work alongside --request.
        req_host, req_cmd = _split_once_arguments(
            ["--request", "bench.json", "--timeline", "out.json", "--log-dir", "logs"],
            start_paused=False,
        )
        self.assertEqual(req_host, ["--log-dir", "logs"])
        self.assertEqual(req_cmd, {"command": "run", "request": "bench.json", "timeline": "out.json"})

        # Launch constructor forwards case, params, after_scenario, and --param cli_params to stdin.
        captured_procs: list[_FakeProcess] = []

        def launch_popen(argv, **kw):
            proc = _FakeProcess(argv, auto_hello=False)
            proc.stdout.push_event({"event": "result", "seq": 1, "result": {"result": "passed"}})
            proc.finish(0)
            captured_procs.append(proc)
            return proc

        with mock.patch("vehicle_sil_control.launcher.subprocess.Popen", side_effect=launch_popen):
            with Launch(
                "/fake/launch",
                "sim.yaml",
                case="nominal",
                params={"keep": 7},
                after_scenario="stop",
                arguments=["--scenario", "scn.yaml", "--param", "code=0123"],
            ) as launch:
                self.assertEqual(launch.wait()["result"], "passed")
        self.assertEqual(
            captured_procs[0].stdin.commands[0],
            {
                "command": "run",
                "intent": {
                    "case": "nominal",
                    "params": {"keep": 7},
                    "after_scenario": "stop",
                    "scenario": "scn.yaml",
                    "cli_params": {"code": "0123"},
                },
            },
        )

    def test_record_signal_arguments_and_validation(self) -> None:
        # _split_once_arguments merges record kwarg + repeatable --record preserving order and deduplicating.
        host, cmd = _split_once_arguments(
            ["--record", "Dashboard.speed", "--record", "Engine.mode", "--record", "Dashboard.speed"],
            start_paused=False,
            record=["Engine.mode", "Engine.throttle"],
        )
        self.assertEqual(host, [])
        self.assertEqual(
            cmd,
            {
                "command": "run",
                "intent": {"record": ["Engine.mode", "Engine.throttle", "Dashboard.speed"]},
            },
        )

        # Reject non-sequence string/bytes with "signal names" wording, empty strings, and --request conflicts.
        with self.assertRaises(TypeError) as str_err:
            _split_once_arguments([], start_paused=False, record="Engine.speed")  # type: ignore[arg-type]
        self.assertIn("signal names", str(str_err.exception))
        with self.assertRaises(ValueError):
            _split_once_arguments([], start_paused=False, record=[""])
        with self.assertRaises(ValueError):
            _split_once_arguments(["--record", ""], start_paused=False)
        with self.assertRaises(ValueError):
            _split_once_arguments(["--request", "bench.json"], start_paused=False, record=["Engine.speed"])
        with self.assertRaises(ValueError):
            _split_once_arguments(["--request", "bench.json", "--record", "Engine.speed"], start_paused=False)

        # Serve.start(record=...) deduplicates and validates with aligned "signal names" wording.
        def handler(proc: _FakeProcess, c: dict) -> None:
            if c.get("command") == "run":
                proc.stdout.push_event({"event": "run_start", "seq": 2, "t_ms": 2, "run": "r-1", "id": c["id"]})
                proc.stdout.push_event({
                    "event": "result",
                    "seq": 3,
                    "t_ms": 3,
                    "run": "r-1",
                    "launcher_ends": False,
                    "status": {"instance": "0123456789abcdef", "status_rev": 1, "launcher_state": "ready"},
                    "result": {"result": "passed", "ok": True},
                })

        fake_proc = _FakeProcess(["/fake/launch", "serve", "--", "sim.yaml"], handler=handler)
        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", return_value=fake_proc):
            with Serve("/fake/launch", "sim.yaml") as srv:
                with self.assertRaises(TypeError) as serve_str_err:
                    srv.start(record="Engine.speed")  # type: ignore[arg-type]
                self.assertIn("signal names", str(serve_str_err.exception))
                with self.assertRaises(ValueError):
                    srv.start(record=[""])
                run = srv.start(record=["Dashboard.speed", "Engine.mode", "Dashboard.speed"])
                self.assertEqual(run.wait()["result"], "passed")
        run_cmds = [c for c in fake_proc.stdin.commands if c.get("command") == "run"]
        self.assertEqual(run_cmds[0]["intent"], {"record": ["Dashboard.speed", "Engine.mode"]})

    def test_record_after_fail_ms_arguments_and_validation(self) -> None:
        # _split_once_arguments accepts record_after_fail_ms kwarg or --record-after-fail-ms CLI flag.
        host, cmd = _split_once_arguments(
            ["--scenario", "scn.yaml", "--record-after-fail-ms", "300"],
            start_paused=False,
        )
        self.assertEqual(host, [])
        self.assertEqual(
            cmd,
            {"command": "run", "intent": {"scenario": "scn.yaml", "record_after_fail_ms": 300}},
        )

        host2, cmd2 = _split_once_arguments(
            ["--scenario", "scn.yaml"],
            start_paused=False,
            record_after_fail_ms=600000,
        )
        self.assertEqual(host2, [])
        self.assertEqual(
            cmd2,
            {"command": "run", "intent": {"scenario": "scn.yaml", "record_after_fail_ms": 600000}},
        )

        # Duplicate specification is refused.
        with self.assertRaises(ValueError):
            _split_once_arguments(
                ["--record-after-fail-ms", "200"],
                start_paused=False,
                record_after_fail_ms=100,
            )
        with self.assertRaises(ValueError):
            _split_once_arguments(
                ["--record-after-fail-ms", "100", "--record-after-fail-ms", "200"],
                start_paused=False,
            )

        # Reject bool, non-int, 0, negative, and > 600000.
        for bad_type in (True, False, 100.0, "100"):
            with self.assertRaises(TypeError):
                _split_once_arguments([], start_paused=False, record_after_fail_ms=bad_type)  # type: ignore[arg-type]
        for bad_val in (0, -1, 600001):
            with self.assertRaises(ValueError):
                _split_once_arguments([], start_paused=False, record_after_fail_ms=bad_val)
            with self.assertRaises(ValueError):
                _split_once_arguments(["--record-after-fail-ms", str(bad_val)], start_paused=False)
        with self.assertRaises(ValueError):
            _split_once_arguments(["--record-after-fail-ms", "not-an-int"], start_paused=False)
        with self.assertRaises(ValueError):
            _split_once_arguments(["--request", "bench.json"], start_paused=False, record_after_fail_ms=300)

        # Serve.start(record_after_fail_ms=...) validates and forwards inline.
        def handler(proc: _FakeProcess, c: dict) -> None:
            if c.get("command") == "run":
                proc.stdout.push_event({"event": "run_start", "seq": 2, "t_ms": 2, "run": "r-1", "id": c["id"]})
                proc.stdout.push_event({
                    "event": "result",
                    "seq": 3,
                    "t_ms": 3,
                    "run": "r-1",
                    "launcher_ends": False,
                    "status": {"instance": "0123456789abcdef", "status_rev": 1, "launcher_state": "ready"},
                    "result": {"result": "passed", "ok": True},
                })

        fake_proc = _FakeProcess(["/fake/launch", "serve", "--", "sim.yaml"], handler=handler)
        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", return_value=fake_proc):
            with Serve("/fake/launch", "sim.yaml") as srv:
                for bad_type in (True, False, 50.5, "50"):
                    with self.assertRaises(TypeError):
                        srv.start(record_after_fail_ms=bad_type)  # type: ignore[arg-type]
                for bad_val in (0, -5, 600001):
                    with self.assertRaises(ValueError):
                        srv.start(record_after_fail_ms=bad_val)
                run = srv.start(scenario="scn.yaml", record_after_fail_ms=300)
                self.assertEqual(run.wait()["result"], "passed")
        run_cmds = [c for c in fake_proc.stdin.commands if c.get("command") == "run"]
        self.assertEqual(run_cmds[0]["intent"], {"scenario": "scn.yaml", "record_after_fail_ms": 300})

    def test_record_events_arguments_and_validation(self) -> None:
        # _split_once_arguments accepts record_events kwarg and --record-events CLI flag.
        host, cmd = _split_once_arguments(["--record-events"], start_paused=False)
        self.assertEqual(host, [])
        self.assertEqual(cmd, {"command": "run", "intent": {"record_events": True}})

        host2, cmd2 = _split_once_arguments([], start_paused=False, record_events=True)
        self.assertEqual(host2, [])
        self.assertEqual(cmd2, {"command": "run", "intent": {"record_events": True}})

        host3, cmd3 = _split_once_arguments([], start_paused=False, record_events=False)
        self.assertEqual(host3, [])
        self.assertEqual(cmd3, {"command": "run"})

        for bad_type in (1, 0, "true", None):
            with self.assertRaises(TypeError):
                _split_once_arguments([], start_paused=False, record_events=bad_type)  # type: ignore[arg-type]
        with self.assertRaises(ValueError):
            _split_once_arguments(["--request", "bench.json"], start_paused=False, record_events=True)
        with self.assertRaises(ValueError):
            _split_once_arguments(["--request", "bench.json", "--record-events"], start_paused=False)

        # Serve.start(record_events=...) validates bool and forwards inline.
        def handler(proc: _FakeProcess, c: dict) -> None:
            if c.get("command") == "run":
                proc.stdout.push_event({"event": "run_start", "seq": 2, "t_ms": 2, "run": "r-1", "id": c["id"]})
                proc.stdout.push_event({
                    "event": "result",
                    "seq": 3,
                    "t_ms": 3,
                    "run": "r-1",
                    "launcher_ends": False,
                    "status": {"instance": "0123456789abcdef", "status_rev": 1, "launcher_state": "ready"},
                    "result": {"result": "passed", "ok": True},
                })

        fake_proc = _FakeProcess(["/fake/launch", "serve", "--", "sim.yaml"], handler=handler)
        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", return_value=fake_proc):
            with Serve("/fake/launch", "sim.yaml") as srv:
                for bad_type in (1, 0, "true"):
                    with self.assertRaises(TypeError):
                        srv.start(record_events=bad_type)  # type: ignore[arg-type]
                run1 = srv.start(record_events=True)
                self.assertEqual(run1.wait()["result"], "passed")
                run2 = srv.start(record_events=False)
                self.assertEqual(run2.wait()["result"], "passed")
        run_cmds = [c for c in fake_proc.stdin.commands if c.get("command") == "run"]
        self.assertEqual(run_cmds[0]["intent"], {"record_events": True})
        self.assertEqual(run_cmds[1]["intent"], {"record_events": False})

    def test_record_can_arguments_and_validation(self) -> None:
        # _split_once_arguments accepts record_can kwarg and --record-can CLI flag.
        host, cmd = _split_once_arguments(["--record-can"], start_paused=False)
        self.assertEqual(host, [])
        self.assertEqual(cmd, {"command": "run", "intent": {"record_can": True}})

        host2, cmd2 = _split_once_arguments([], start_paused=False, record_can=True)
        self.assertEqual(host2, [])
        self.assertEqual(cmd2, {"command": "run", "intent": {"record_can": True}})

        host3, cmd3 = _split_once_arguments([], start_paused=False, record_can=False)
        self.assertEqual(host3, [])
        self.assertEqual(cmd3, {"command": "run"})

        for bad_type in (1, 0, "true", None):
            with self.assertRaises(TypeError):
                _split_once_arguments([], start_paused=False, record_can=bad_type)  # type: ignore[arg-type]
        with self.assertRaises(ValueError):
            _split_once_arguments(["--request", "bench.json"], start_paused=False, record_can=True)
        with self.assertRaises(ValueError):
            _split_once_arguments(["--request", "bench.json", "--record-can"], start_paused=False)

        # Serve.start(record_can=...) validates bool and forwards inline.
        def handler(proc: _FakeProcess, c: dict) -> None:
            if c.get("command") == "run":
                proc.stdout.push_event({"event": "run_start", "seq": 2, "t_ms": 2, "run": "r-1", "id": c["id"]})
                proc.stdout.push_event({
                    "event": "result",
                    "seq": 3,
                    "t_ms": 3,
                    "run": "r-1",
                    "launcher_ends": False,
                    "status": {"instance": "0123456789abcdef", "status_rev": 1, "launcher_state": "ready"},
                    "result": {"result": "passed", "ok": True},
                })

        fake_proc = _FakeProcess(["/fake/launch", "serve", "--", "sim.yaml"], handler=handler)
        with mock.patch("vehicle_sil_control.serve.subprocess.Popen", return_value=fake_proc):
            with Serve("/fake/launch", "sim.yaml") as srv:
                for bad_type in (1, 0, "true"):
                    with self.assertRaises(TypeError):
                        srv.start(record_can=bad_type)  # type: ignore[arg-type]
                run1 = srv.start(record_can=True)
                self.assertEqual(run1.wait()["result"], "passed")
                run2 = srv.start(record_can=False)
                self.assertEqual(run2.wait()["result"], "passed")
        run_cmds = [c for c in fake_proc.stdin.commands if c.get("command") == "run"]
        self.assertEqual(run_cmds[0]["intent"], {"record_can": True})
        self.assertEqual(run_cmds[1]["intent"], {"record_can": False})


if __name__ == "__main__":
    unittest.main()
