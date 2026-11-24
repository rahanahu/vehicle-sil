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
import busy_machine  # noqa: E402
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

            # (f) Leader exits during process.wait(timeout=...) -> zero time.sleep calls
            kill_calls.clear()
            waited = False

            def poll_until_wait():
                return -signal.SIGINT if waited else None

            def wait_exits(timeout=None):
                nonlocal waited
                self.assertEqual(timeout, 20)
                waited = True
                return -signal.SIGINT

            proc_wait = mock.Mock(pid=9999)
            proc_wait.poll.side_effect = poll_until_wait
            proc_wait.wait.side_effect = wait_exits
            with mock.patch.object(verify_common, "_group_alive", side_effect=lambda _pgid: not waited), \
                 mock.patch.object(verify_common.time, "sleep") as sleep_mock:
                verify_common.stop_group(proc_wait)
            self.assertEqual(kill_calls, [(9999, signal.SIGINT)])
            proc_wait.wait.assert_called_once_with(timeout=20)
            sleep_mock.assert_not_called()

    def test_parse_steps(self) -> None:
        valid = ("a", "b", "c", "d")
        self.assertEqual(verify_common.parse_steps("prog", ["c,a", "b,a"], valid), ["a", "b", "c"])
        for bad in ([""], ["a,,b"], ["z"]):
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as cm:
                verify_common.parse_steps("prog", bad, valid)
            self.assertEqual(cm.exception.code, 2, bad)
            self.assertIn("prog: error:", stderr.getvalue())

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


class BusyMachineTest(unittest.TestCase):
    def test_linux_matches_ninja_build_and_formats_details(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            proc_root = Path(d)
            for pid, comm in (
                ("101", "ninja-build"),
                ("102", "ctest"),
                ("103", "qemu-system-x86"),
                ("104", "python3"),
                ("105", "ninja"),
            ):
                p = proc_root / pid
                p.mkdir()
                (p / "comm").write_text(comm + "\n", encoding="utf-8")
            (proc_root / "self").mkdir()
            (proc_root / "self" / "comm").write_text("python3\n", encoding="utf-8")

            cwds = {
                proc_root / "101" / "cwd": "/home/dev/wt-m5",
                proc_root / "102" / "cwd": "/home/dev/vehicle-sil",
                proc_root / "105" / "cwd": "/",
            }

            def fake_readlink(path: Path) -> str:
                if path in cwds:
                    return cwds[path]
                raise PermissionError("permission denied")

            with mock.patch.object(busy_machine.os, "readlink", side_effect=fake_readlink):
                names = busy_machine._names_linux(proc_root)
                self.assertIn("ninja-build", names)
                details = busy_machine._details_linux(proc_root)

            self.assertEqual(
                details,
                [
                    "ctest (pid 102, vehicle-sil)",
                    "ninja (pid 105, /)",
                    "ninja-build (pid 101, wt-m5)",
                    "qemu-system-x86 (pid 103)",
                ],
            )

    def test_windows_nt_path_matches_ninja_build_exe_and_degrades_details(self) -> None:
        csv_out = (
            '"System Idle Process","0","Services","0","8 K"\r\n'
            '"ninja-build.exe","4120","Console","1","12,000 K"\r\n'
            '"ctest.exe","4124","Console","1","10,000 K"\r\n'
            '"qemu-system-x86_64.exe","4200","Console","1","1,000,000 K"\r\n'
            '"python.exe","5000","Console","1","20,000 K"\r\n'
        )
        fake_res = mock.Mock(stdout=csv_out)
        with (
            mock.patch.object(busy_machine.os, "name", "nt"),
            mock.patch.object(busy_machine.subprocess, "run", return_value=fake_res) as run_mock,
        ):
            self.assertEqual(busy_machine.busy_processes(), ["ctest", "ninja-build", "qemu-system-x86_64"])
            self.assertEqual(busy_machine.busy_details(), ["ctest", "ninja-build", "qemu-system-x86_64"])
        run_mock.assert_called_with(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True, errors="replace")

    def test_ps_fallback_path(self) -> None:
        ps_out = "/usr/bin/ninja-build\nctest\n/bin/bash\n"
        fake_res = mock.Mock(stdout=ps_out)
        with (
            mock.patch.object(busy_machine.Path, "exists", return_value=False),
            mock.patch.object(busy_machine.os, "name", "posix"),
            mock.patch.object(busy_machine.subprocess, "run", return_value=fake_res),
        ):
            self.assertEqual(busy_machine.busy_processes(), ["ctest", "ninja-build"])
            self.assertEqual(busy_machine.busy_details(), ["ctest", "ninja-build"])

    def test_wait_until_quiet_formats_details_and_truncates(self) -> None:
        items = [f"ctest (pid {i}, wt-{i})" for i in range(6)]
        err = io.StringIO()
        with (
            mock.patch.object(busy_machine, "busy_details", side_effect=[items, []]),
            mock.patch.object(busy_machine.time, "monotonic", side_effect=[0.0, 1.0]),
            mock.patch.object(busy_machine.time, "sleep") as sleep_mock,
            contextlib.redirect_stderr(err),
        ):
            busy_machine.wait_until_quiet("verify-gui", limit_s=600, poll_s=1.0)
        sleep_mock.assert_called_once_with(1.0)
        msg = err.getvalue()
        self.assertIn("verify-gui: waiting for ctest (pid 0, wt-0)", msg)
        self.assertIn(", and 2 more to end", msg)
        self.assertNotIn("wt-4", msg)


class VerifyPythonFilteringTest(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = _load_script("verify_python_test", "verify-python.py")

    def _run_main(self, argv: list[str]) -> tuple[int, str, str, list[tuple[str, bool, list[str] | None]]]:
        calls: list[tuple[str, bool, list[str] | None]] = []

        def fake_check_version(_uv: str, version: str, _tag: str, _files: list[str],
                               lowest: bool = False, selected: list[str] | None = None):
            calls.append((version, lowest, selected))
            return True, "ok (sync 0 s)", [], None

        out, err = io.StringIO(), io.StringIO()
        code = 0
        with (
            mock.patch.object(self.mod, "pinned_version", return_value="3.14"),
            mock.patch.object(self.mod, "tracked_python_files", return_value=["scripts/verify.py"]),
            mock.patch.object(self.mod.shutil, "which", return_value="/fake/uv"),
            mock.patch.object(self.mod, "check_version", side_effect=fake_check_version),
            mock.patch.object(sys, "argv", ["scripts/verify-python.py", *argv]),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            try:
                code = self.mod.main()
            except SystemExit as exc:
                code = exc.code if isinstance(exc.code, int) else 1
        return code, out.getvalue(), err.getvalue(), calls

    def test_only_scripts_skips_lowest_automatically(self) -> None:
        code, out, err, calls = self._run_main(["--only", "scripts"])
        self.assertEqual(code, 0, err + out)
        self.assertEqual(calls, [("3.11", False, ["scripts"]), ("3.14", False, ["scripts"])])
        self.assertIn("python 3.11 lowest: skipped", out)
        self.assertIn("2 of 2 runs passed", out)

    def test_only_control_and_scripts_canonical_order_keeps_lowest(self) -> None:
        code, out, err, calls = self._run_main(["--only", "control,scripts"])
        self.assertEqual(code, 0, err + out)
        self.assertEqual(
            calls,
            [
                ("3.11", False, ["scripts", "control"]),
                ("3.14", False, ["scripts", "control"]),
                ("3.11", True, ["scripts", "control"]),
            ],
        )
        self.assertIn("3 of 3 runs passed", out)

    def test_only_unknown_or_empty_exits_two(self) -> None:
        for bad in ("bogus", "", "scripts,,control"):
            code, _out, err, calls = self._run_main(["--only", bad])
            self.assertEqual(code, 2, (bad, err))
            self.assertEqual(calls, [])
            self.assertIn("verify-python: error:", err)

    def test_check_version_filters_steps_for_normal_and_lowest(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tmp = Path(d)
            log_dir = tmp / "logs"
            log_dir.mkdir()
            env_dir = tmp / "envs"
            ran_cmds: list[list[str]] = []

            def fake_run(self_step, cmd: list[str], cwd: Path = ROOT) -> bool:
                ran_cmds.append(list(cmd))
                return True

            with (
                mock.patch.object(self.mod, "LOG_DIR", log_dir),
                mock.patch.object(self.mod, "ENV_DIR", env_dir),
                mock.patch.object(self.mod.Step, "run", fake_run),
                mock.patch.object(self.mod, "lower_bounds", return_value={}),
                mock.patch.object(self.mod, "installed_versions", return_value={}),
            ):
                ok, text, _lines, _log = self.mod.check_version(
                    "/fake/uv", "3.11", "t", ["scripts/verify.py"], lowest=False, selected=["codegen", "schema"]
                )
                self.assertTrue(ok, text)
                self.assertIn("sync", text)
                self.assertIn("codegen", text)
                self.assertIn("schema", text)
                for skipped in ("syntax", "knowledge", "template", "scripts", "control"):
                    self.assertNotIn(skipped, text)

                ok_low, text_low, _lines, _log = self.mod.check_version(
                    "/fake/uv", "3.11", "t", ["scripts/verify.py"], lowest=True, selected=["scripts", "schema"]
                )
                self.assertTrue(ok_low, text_low)
                self.assertIn("sync", text_low)
                self.assertIn("schema", text_low)
                self.assertNotIn("scripts", text_low)
                self.assertNotIn("codegen", text_low)
                self.assertNotIn("control", text_low)

    def test_env_python_windows_and_posix(self) -> None:
        env = Path("/tmp/fake-env")
        with mock.patch.object(self.mod.os, "name", "nt"):
            self.assertEqual(self.mod.env_python(env), env / "Scripts" / "python.exe")
        with mock.patch.object(self.mod.os, "name", "posix"):
            self.assertEqual(self.mod.env_python(env), env / "bin" / "python")


if __name__ == "__main__":
    unittest.main()
