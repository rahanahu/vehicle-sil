# SPDX-License-Identifier: MIT
"""Unit tests for scripts/verify_common.py and the verify*.py scripts that use it.

    python tests/scripts/test_verify_common.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import job_summary  # noqa: E402
import verify_common  # noqa: E402


def _load_script(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


class VerifyCommonTest(unittest.TestCase):
    def test_root_and_log_dir(self) -> None:
        self.assertEqual(verify_common.ROOT, ROOT)
        self.assertEqual(verify_common.LOG_DIR, ROOT / "build" / "logs")

    def test_cli_help_exits_zero(self) -> None:
        env = {k: v for k, v in os.environ.items()
               if k not in (job_summary.SUMMARY_ENV, job_summary.NESTED_ENV, job_summary.ARTIFACT_ENV)}
        for args in (["--help"], []):
            proc = subprocess.run(
                [sys.executable, str(ROOT / "scripts" / "verify_common.py"), *args],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                stdin=subprocess.DEVNULL, env=env,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            if args == ["--help"]:
                self.assertIn("Shared helpers for scripts/verify*.py", proc.stdout)

    def test_reconfigure_stdio(self) -> None:
        class FakeStream:
            def __init__(self) -> None:
                self.calls: list[dict[str, str]] = []

            def reconfigure(self, **kwargs: str) -> None:
                self.calls.append(kwargs)

        out, err = FakeStream(), FakeStream()
        with mock.patch.object(sys, "stdout", out), mock.patch.object(sys, "stderr", err):
            verify_common.reconfigure_stdio()
            self.assertEqual(out.calls, [{"errors": "backslashreplace"}])
            self.assertEqual(err.calls, [])

            verify_common.reconfigure_stdio(encoding="utf-8", stderr=True)
            self.assertEqual(out.calls[-1], {"errors": "backslashreplace", "encoding": "utf-8"})
            self.assertEqual(err.calls, [{"errors": "backslashreplace", "encoding": "utf-8"}])

        plain = io.StringIO()
        with mock.patch.object(sys, "stdout", plain), mock.patch.object(sys, "stderr", plain):
            verify_common.reconfigure_stdio(encoding="utf-8", stderr=True)

    def test_die_usage(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as cm:
            verify_common.die_usage("verify-web", "bad flag")
        self.assertEqual(cm.exception.code, 2)
        self.assertEqual(stderr.getvalue(), "verify-web: error: bad flag\n")

    def test_rel(self) -> None:
        custom_root = Path("/tmp/custom-root") if os.name != "nt" else Path("C:/custom-root")
        self.assertEqual(verify_common.rel(custom_root / "build" / "logs" / "a.log", custom_root), "build/logs/a.log")
        outside = Path("/var/tmp/other.log") if os.name != "nt" else Path("D:/other.log")
        self.assertEqual(verify_common.rel(outside, custom_root), str(outside))

    def test_validate_tag(self) -> None:
        self.assertEqual(verify_common.validate_tag("verify", "verify"), "verify")
        self.assertEqual(verify_common.validate_tag("verify", "ci-1.0_test"), "ci-1.0_test")
        for bad in ("", "a/b", "a b", "a:b"):
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as cm:
                verify_common.validate_tag("verify-gui", bad)
            self.assertEqual(cm.exception.code, 2)
            self.assertIn("verify-gui: error: --tag", stderr.getvalue())

    def test_clean_lines(self) -> None:
        raw = "\x1b[31merror line\x1b[0m   \n\x1b[?25h" + ("x" * 350) + "\n"
        cleaned = verify_common.clean_lines(raw)
        self.assertEqual(cleaned[0], "error line")
        self.assertEqual(len(cleaned[1]), verify_common.LINE_CHARS)
        self.assertEqual(verify_common.clean_lines("abcdef", limit=3), ["abc"])

    def test_process_group_kwargs(self) -> None:
        with mock.patch.object(verify_common.os, "name", "nt"), \
             mock.patch.object(verify_common.subprocess, "CREATE_NEW_PROCESS_GROUP", 512, create=True):
            self.assertEqual(verify_common.process_group_kwargs(), {"creationflags": 512})
        with mock.patch.object(verify_common.os, "name", "posix"):
            self.assertEqual(verify_common.process_group_kwargs(), {"start_new_session": True})

    def test_stop_group_windows(self) -> None:
        proc = mock.Mock(pid=4321)
        with mock.patch.object(verify_common.os, "name", "nt"), \
             mock.patch.object(verify_common.subprocess, "run") as run_mock:
            verify_common.stop_group(proc)
        run_mock.assert_called_once_with(["taskkill", "/T", "/F", "/PID", "4321"], capture_output=True)

    def test_stop_group_posix(self) -> None:
        proc = mock.Mock(pid=9999)
        kill_calls: list[tuple[int, int]] = []
        expected_sigkill = getattr(signal, "SIGKILL", 9)

        def killpg_immediate(pgid: int, sig: int) -> None:
            kill_calls.append((pgid, sig))

        with mock.patch.object(verify_common.os, "name", "posix"), \
             mock.patch.object(verify_common.os, "killpg", side_effect=killpg_immediate, create=True), \
             mock.patch.object(verify_common.signal, "SIGKILL", expected_sigkill, create=True):
            # (a) Immediate exit after SIGINT
            with mock.patch.object(verify_common, "_group_alive", side_effect=[True, False, False]), \
                 mock.patch.object(verify_common.time, "monotonic", side_effect=[0.0, 0.1]), \
                 mock.patch.object(verify_common.time, "sleep"):
                verify_common.stop_group(proc)
            self.assertEqual(kill_calls, [(9999, signal.SIGINT)])

            # (b) Already dead on probe
            kill_calls.clear()
            with mock.patch.object(verify_common, "_group_alive", return_value=False):
                verify_common.stop_group(proc)
            self.assertEqual(kill_calls, [])

            # (c) Timeout escalation SIGINT -> SIGTERM -> SIGKILL
            kill_calls.clear()
            with mock.patch.object(verify_common, "_group_alive", return_value=True), \
                 mock.patch.object(verify_common.time, "monotonic", side_effect=[0.0, 21.0, 0.0, 6.0, 0.0, 2.0]), \
                 mock.patch.object(verify_common.time, "sleep"):
                verify_common.stop_group(proc)
            self.assertEqual(kill_calls, [(9999, signal.SIGINT), (9999, signal.SIGTERM), (9999, expected_sigkill)])

            # (e) Reap after SIGKILL inside the 1s reap window
            kill_calls.clear()
            sigkill_reaped = False

            def poll_after_sigkill():
                nonlocal sigkill_reaped
                if (9999, expected_sigkill) in kill_calls:
                    sigkill_reaped = True
                return -expected_sigkill if sigkill_reaped else None

            def alive_until_sigkill_reap(_pgid: int) -> bool:
                return not sigkill_reaped

            proc_kill = mock.Mock(pid=9999)
            proc_kill.poll.side_effect = poll_after_sigkill
            with mock.patch.object(verify_common, "_group_alive", side_effect=alive_until_sigkill_reap), \
                 mock.patch.object(verify_common.time, "monotonic", side_effect=[0.0, 21.0, 0.0, 6.0, 0.0, 0.1]), \
                 mock.patch.object(verify_common.time, "sleep"):
                verify_common.stop_group(proc_kill)
            self.assertEqual(kill_calls, [(9999, signal.SIGINT), (9999, signal.SIGTERM), (9999, expected_sigkill)])
            self.assertTrue(sigkill_reaped)

            # (d) Leader is reaped by proc.poll() inside the wait loop so _group_alive turns False without escalation
            kill_calls.clear()
            reaped = False

            def poll_reaps():
                nonlocal reaped
                if kill_calls:
                    reaped = True
                return -signal.SIGINT if reaped else None

            def alive_after_reap(_pgid: int) -> bool:
                return not reaped

            proc_reap = mock.Mock(pid=9999)
            proc_reap.poll.side_effect = poll_reaps
            with mock.patch.object(verify_common, "_group_alive", side_effect=alive_after_reap), \
                 mock.patch.object(verify_common.time, "monotonic", side_effect=[0.0, 0.1]), \
                 mock.patch.object(verify_common.time, "sleep"):
                verify_common.stop_group(proc_reap)
            self.assertEqual(kill_calls, [(9999, signal.SIGINT)])
            self.assertGreaterEqual(proc_reap.poll.call_count, 2)

    @unittest.skipIf(os.name == "nt", "POSIX process groups")
    def test_stop_group_posix_real_subprocess_no_zombie_stall(self) -> None:
        # Case 1: Single process exiting on SIGINT (must reap leader zombie without stalling 20s+5s)
        script1 = (
            "import signal, sys, time\n"
            "signal.signal(signal.SIGINT, signal.SIG_DFL)\n"
            "print('ready', flush=True)\n"
            "time.sleep(30)\n"
        )
        proc = subprocess.Popen(
            [sys.executable, "-c", script1],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
            text=True,
            **verify_common.process_group_kwargs(),
        )
        try:
            self.assertEqual(proc.stdout.readline().strip(), "ready")
            started = time.monotonic()
            verify_common.stop_group(proc)
            elapsed = time.monotonic() - started
            self.assertLess(elapsed, 2.0)
            self.assertEqual(proc.returncode, -signal.SIGINT)
            self.assertFalse(verify_common._group_alive(proc.pid))
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            if proc.stdout is not None:
                proc.stdout.close()

        # Case 2: Leader exits immediately on SIGINT while child ignores SIGINT briefly, then exits
        with tempfile.TemporaryDirectory() as d:
            ready_file = Path(d) / "child.ready"
            pid_file = Path(d) / "child.pid"
            script2 = (
                "import pathlib, signal, subprocess, sys, time\n"
                "child_code = (\n"
                "    'import os, pathlib, signal, sys, time\\n'\n"
                "    'signal.signal(signal.SIGINT, signal.SIG_IGN)\\n'\n"
                "    'pathlib.Path(sys.argv[1]).write_text(str(os.getpid()))\\n'\n"
                "    'pathlib.Path(sys.argv[2]).write_text(\"ready\")\\n'\n"
                "    'time.sleep(0.4)\\n'\n"
                ")\n"
                "subprocess.Popen([sys.executable, '-c', child_code, sys.argv[1], sys.argv[2]])\n"
                "signal.signal(signal.SIGINT, signal.SIG_DFL)\n"
                "while not pathlib.Path(sys.argv[2]).exists():\n"
                "    time.sleep(0.01)\n"
                "print('ready', flush=True)\n"
                "time.sleep(30)\n"
            )
            proc2 = subprocess.Popen(
                [sys.executable, "-c", script2, str(pid_file), str(ready_file)],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                stdin=subprocess.DEVNULL,
                text=True,
                **verify_common.process_group_kwargs(),
            )
            try:
                self.assertEqual(proc2.stdout.readline().strip(), "ready")
                child_pid = int(pid_file.read_text().strip())
                started = time.monotonic()
                verify_common.stop_group(proc2)
                elapsed = time.monotonic() - started
                self.assertLess(elapsed, 2.0)
                self.assertEqual(proc2.returncode, -signal.SIGINT)
                self.assertFalse(verify_common._group_alive(proc2.pid))
                with self.assertRaises(ProcessLookupError):
                    os.kill(child_pid, 0)
            finally:
                if proc2.poll() is None:
                    proc2.kill()
                    proc2.wait()
                if proc2.stdout is not None:
                    proc2.stdout.close()

    def test_install_sigterm_handler(self) -> None:
        with mock.patch.object(verify_common.signal, "signal") as signal_mock:
            verify_common.install_sigterm_handler()
        signal_mock.assert_called_once()
        sig, handler = signal_mock.call_args.args
        self.assertEqual(sig, signal.SIGTERM)
        with self.assertRaises(KeyboardInterrupt):
            handler(signal.SIGTERM, None)

    def test_verify_scripts_import_and_help(self) -> None:
        env = {k: v for k, v in os.environ.items()
               if k not in (job_summary.SUMMARY_ENV, job_summary.NESTED_ENV, job_summary.ARTIFACT_ENV)}
        for script in ("verify.py", "verify-web.py", "verify-gui.py", "verify-python.py"):
            proc = subprocess.run(
                [sys.executable, str(ROOT / "scripts" / script), "--help"],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                stdin=subprocess.DEVNULL, env=env,
            )
            self.assertEqual(proc.returncode, 0, f"{script} --help failed: {proc.stderr}")

        for name, filename in (("verify_web_common_test", "verify-web.py"),
                               ("verify_gui_common_test", "verify-gui.py")):
            mod = _load_script(name, filename)
            for attr in ("LOG_DIR", "ROOT", "Step", "die_usage", "rel", "clean_lines", "stop_group"):
                self.assertTrue(hasattr(mod, attr), f"{filename} missing {attr}")


if __name__ == "__main__":
    unittest.main()
