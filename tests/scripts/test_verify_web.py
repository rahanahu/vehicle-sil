# SPDX-License-Identifier: MIT
"""Tests of scripts/verify-web.py --summarize-junit on synthetic JUnit XML files: passed/failed/skipped counts,
per-file cumulative duration ordering, top-N slowest test ordering, --json output schema, missing/malformed XML
exit code 2, and distinct non-colliding JUnit paths for web-e2e, real, and workbench runs.

    python tests/scripts/test_verify_web.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import job_summary  # noqa: E402

VERIFY_WEB = ROOT / "scripts" / "verify-web.py"
VERIFY_GUI = ROOT / "scripts" / "verify-gui.py"

spec = importlib.util.spec_from_file_location("verify_web", VERIFY_WEB)
verify_web_mod = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = verify_web_mod
spec.loader.exec_module(verify_web_mod)

spec_gui = importlib.util.spec_from_file_location("verify_gui", VERIFY_GUI)
verify_gui_mod = importlib.util.module_from_spec(spec_gui)
sys.modules[spec_gui.name] = verify_gui_mod
spec_gui.loader.exec_module(verify_gui_mod)


SAMPLE_PASSING_XML = """<?xml version="1.0" encoding="UTF-8"?>
<testsuites id="" name="" tests="5" failures="0" skipped="1" errors="0" time="10.5">
  <testsuite name="fast.spec.ts" timestamp="2026-11-23T00:00:00.000Z" hostname="static" tests="2" failures="0" skipped="1" time="1.25" errors="0">
    <testcase name="fast › quick check" classname="fast.spec.ts" time="1.25">
    </testcase>
    <testcase name="fast › skipped workbench check" classname="fast.spec.ts">
      <skipped />
    </testcase>
  </testsuite>
  <testsuite name="run.spec.ts" timestamp="2026-11-23T00:00:00.000Z" hostname="static" tests="3" failures="0" skipped="0" time="8.75" errors="0">
    <testcase name="run › medium run" classname="run.spec.ts" time="2.50">
    </testcase>
    <testcase name="run › every width" classname="run.spec.ts" time="5.50">
    </testcase>
    <testcase name="run › short run" classname="run.spec.ts" time="0.75">
    </testcase>
  </testsuite>
</testsuites>
"""

SAMPLE_FAILING_XML = """<?xml version="1.0" encoding="UTF-8"?>
<testsuites id="" name="" tests="3" failures="1" skipped="0" errors="0" time="4.2">
  <testsuite name="design.spec.ts" tests="2" failures="1" skipped="0" time="3.2" errors="0">
    <testcase name="design › passes" classname="design.spec.ts" time="1.1">
    </testcase>
    <testcase name="design › fails assertion" classname="design.spec.ts" time="2.1">
      <failure message="Expected 1 to be 2" type="FAILURE">Error: Expected 1 to be 2</failure>
    </testcase>
  </testsuite>
  <testsuite name="check.spec.ts" tests="1" failures="0" skipped="0" time="1.0" errors="0">
    <testcase name="check › passes" classname="check.spec.ts" time="1.0">
    </testcase>
  </testsuite>
</testsuites>
"""


def run_verify_web(*args: str) -> subprocess.CompletedProcess[str]:
    env = {
        k: v
        for k, v in os.environ.items()
        if k
        not in (
            job_summary.SUMMARY_ENV,
            job_summary.NESTED_ENV,
            job_summary.ARTIFACT_ENV,
            "PYTHONUTF8",
            "PYTHONIOENCODING",
        )
    }
    return subprocess.run(
        [sys.executable, str(VERIFY_WEB), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdin=subprocess.DEVNULL,
        env=env,
    )


class VerifyWebJUnitTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_summarize_passing_xml_counts_per_file_and_top_ordering(self) -> None:
        xml_path = self.dir / "gui-web-e2e.junit.xml"
        xml_path.write_text(SAMPLE_PASSING_XML, encoding="utf-8")

        proc = run_verify_web("--summarize-junit", str(xml_path), "--top", "2")
        self.assertEqual(proc.returncode, 0, f"stderr: {proc.stderr}\nstdout: {proc.stdout}")
        out = proc.stdout
        self.assertIn("4 passed, 0 failed, 1 skipped, 5 total (10.0 s)", out)
        # Per-file table sorted by cumulative duration descending (run.spec.ts 8.75s before fast.spec.ts 1.25s)
        run_idx = out.find("run.spec.ts")
        fast_idx = out.find("fast.spec.ts")
        self.assertGreater(run_idx, 0, out)
        self.assertGreater(fast_idx, run_idx, f"run.spec.ts should appear before fast.spec.ts:\n{out}")
        self.assertIn("slowest tests (top 2 of 5):", out)
        self.assertIn("5.50 s  [passed]  run.spec.ts :: run › every width", out)
        self.assertIn("2.50 s  [passed]  run.spec.ts :: run › medium run", out)
        self.assertNotIn("run › short run", out)

    def test_summarize_json_schema_and_values(self) -> None:
        xml_path = self.dir / "gui-web-e2e.junit.xml"
        xml_path.write_text(SAMPLE_PASSING_XML, encoding="utf-8")

        proc = run_verify_web("--summarize-junit", str(xml_path), "--top", "3", "--json")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        data = json.loads(proc.stdout)
        self.assertEqual(data["vehicle_sil_verify_web_junit"], 1)
        self.assertTrue(data["ok"])
        self.assertEqual((data["total"], data["passed"], data["failed"], data["skipped"]), (5, 4, 0, 1))
        self.assertAlmostEqual(data["seconds"], 10.0)
        self.assertEqual([f["file"] for f in data["files"]], ["run.spec.ts", "fast.spec.ts"])
        self.assertEqual(
            data["files"][0],
            {
                "file": "run.spec.ts",
                "tests": 3,
                "passed": 3,
                "failed": 0,
                "skipped": 0,
                "seconds": 8.75,
                "max_seconds": 5.5,
            },
        )
        self.assertEqual(
            data["files"][1],
            {
                "file": "fast.spec.ts",
                "tests": 2,
                "passed": 1,
                "failed": 0,
                "skipped": 1,
                "seconds": 1.25,
                "max_seconds": 1.25,
            },
        )
        self.assertEqual(
            [t["name"] for t in data["slowest"]],
            ["run › every width", "run › medium run", "fast › quick check"],
        )

    def test_summarize_failing_xml_exits_one(self) -> None:
        xml_path = self.dir / "failing.junit.xml"
        xml_path.write_text(SAMPLE_FAILING_XML, encoding="utf-8")

        proc = run_verify_web("--summarize-junit", str(xml_path))
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("FAILED - 2 passed, 1 failed, 0 skipped, 3 total", proc.stdout)
        self.assertIn("[failed]  design.spec.ts :: design › fails assertion", proc.stdout)

        proc_json = run_verify_web("--summarize-junit", str(xml_path), "--json")
        self.assertEqual(proc_json.returncode, 1)
        data = json.loads(proc_json.stdout)
        self.assertFalse(data["ok"])
        self.assertEqual(data["failed"], 1)

    def test_missing_or_malformed_xml_exits_two(self) -> None:
        for blank in ("", "   "):
            proc_blank = run_verify_web("--summarize-junit", blank)
            self.assertEqual(proc_blank.returncode, 2)
            self.assertIn("--summarize-junit requires a file path", proc_blank.stderr)

        missing = self.dir / "does-not-exist.junit.xml"
        proc_missing = run_verify_web("--summarize-junit", str(missing))
        self.assertEqual(proc_missing.returncode, 2)
        self.assertIn("verify-web: error:", proc_missing.stderr)

        malformed = self.dir / "malformed.junit.xml"
        malformed.write_text("<testsuites><testsuite>", encoding="utf-8")
        proc_malformed = run_verify_web("--summarize-junit", str(malformed))
        self.assertEqual(proc_malformed.returncode, 2)
        self.assertIn("verify-web: error:", proc_malformed.stderr)

        wrong_root = self.dir / "wrong-root.xml"
        wrong_root.write_text("<project></project>", encoding="utf-8")
        proc_root = run_verify_web("--summarize-junit", str(wrong_root))
        self.assertEqual(proc_root.returncode, 2)
        self.assertIn("expected <testsuites> or <testsuite>", proc_root.stderr)

        for raw_time in ("not-a-float", "NaN", "inf", "-inf", "-2.5"):
            bad_time = self.dir / f"bad-time-{raw_time}.xml"
            bad_time.write_text(
                f'<testsuite name="a.spec.ts"><testcase name="t" time="{raw_time}"/></testsuite>',
                encoding="utf-8",
            )
            proc_time = run_verify_web("--summarize-junit", str(bad_time))
            self.assertEqual(proc_time.returncode, 2, raw_time)
            self.assertIn("invalid testcase time", proc_time.stderr)

        for bad_top in ("0", "-1"):
            proc_top = run_verify_web("--summarize-junit", str(wrong_root), "--top", bad_top)
            self.assertEqual(proc_top.returncode, 2, bad_top)
            self.assertIn("--top must be 1 or more", proc_top.stderr)

    def test_nested_testsuite_error_tag_file_precedence_and_empty_suite(self) -> None:
        nested_xml = self.dir / "nested.junit.xml"
        nested_xml.write_text(
            '<testsuite name="root">'
            '<testsuite name="suite-a.spec.ts">'
            '<testcase name="a › pass" file="override-a.spec.ts" classname="suite-a.spec.ts" time="1.5"/>'
            '<testcase name="a › err" classname="suite-a.spec.ts" time="0.5">'
            '<error message="boom"/>'
            "</testcase>"
            "</testsuite>"
            "</testsuite>",
            encoding="utf-8",
        )
        proc = run_verify_web("--summarize-junit", str(nested_xml), "--json")
        self.assertEqual(proc.returncode, 1, proc.stderr)
        data = json.loads(proc.stdout)
        self.assertFalse(data["ok"])
        self.assertEqual((data["total"], data["passed"], data["failed"], data["skipped"]), (2, 1, 1, 0))
        self.assertEqual([f["file"] for f in data["files"]], ["override-a.spec.ts", "suite-a.spec.ts"])
        self.assertEqual(data["slowest"][1]["status"], "failed")

        empty_xml = self.dir / "empty.junit.xml"
        empty_xml.write_text("<testsuites/>", encoding="utf-8")
        proc_empty = run_verify_web("--summarize-junit", str(empty_xml), "--json")
        self.assertEqual(proc_empty.returncode, 0, proc_empty.stderr)
        empty_data = json.loads(proc_empty.stdout)
        self.assertTrue(empty_data["ok"])
        self.assertEqual((empty_data["total"], empty_data["files"], empty_data["slowest"]), (0, [], []))

    def test_multi_file_non_overwrite_and_step_json_contract(self) -> None:
        # Three distinct runs (gui-web-e2e, gui-real, gui-workbench-e2e) produce distinct XML files in the same directory
        web_xml = self.dir / "gui-web-e2e.junit.xml"
        real_xml = self.dir / "gui-real.junit.xml"
        wb_xml = self.dir / "gui-workbench-e2e.junit.xml"

        web_xml.write_text(SAMPLE_PASSING_XML, encoding="utf-8")
        real_xml.write_text(
            '<testsuites><testsuite name="run-real.spec.ts">'
            '<testcase name="real › pass" classname="run-real.spec.ts" time="3.2"/>'
            "</testsuite></testsuites>",
            encoding="utf-8",
        )
        wb_xml.write_text(
            '<testsuites><testsuite name="workbench.spec.ts">'
            '<testcase name="workbench › run" classname="workbench.spec.ts" time="1.8"/>'
            "</testsuite></testsuites>",
            encoding="utf-8",
        )

        # Summarizing each file preserves all three files and reports their independent counts
        for path, expected_total, expected_file in (
            (web_xml, 5, "run.spec.ts"),
            (real_xml, 1, "run-real.spec.ts"),
            (wb_xml, 1, "workbench.spec.ts"),
        ):
            proc = run_verify_web("--summarize-junit", str(path), "--json")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            payload = json.loads(proc.stdout)
            self.assertEqual(payload["total"], expected_total)
            self.assertEqual(payload["files"][0]["file"], expected_file)
            self.assertTrue(path.is_file())

        # Step.to_json() in verify-web.py and verify-gui.py includes "junit" only when non-empty
        web_step = verify_web_mod.Step("e2e", ["npm", "run", "e2e"], log="build/logs/gui-web-e2e.log",
                                       junit="build/logs/gui-web-e2e.junit.xml")
        self.assertEqual(web_step.to_json()["junit"], "build/logs/gui-web-e2e.junit.xml")
        lint_step = verify_web_mod.Step("lint", ["npm", "run", "lint"], log="build/logs/gui-web-lint.log")
        self.assertNotIn("junit", lint_step.to_json())

        real_step = verify_gui_mod.Step("real", ["npm", "--prefix", "gui/web", "run", "e2e:real"],
                                        log="build/logs/gui-real.log", junit="build/logs/gui-real.junit.xml")
        self.assertEqual(real_step.to_json()["junit"], "build/logs/gui-real.junit.xml")
        sync_step = verify_gui_mod.Step("sync", ["uv", "sync", "--locked"], log="build/logs/gui-sync.log")
        self.assertNotIn("junit", sync_step.to_json())

        # Playwright config files define distinct fallback paths for static, dev, workbench, and real
        pw_cfg = (ROOT / "gui" / "web" / "playwright.config.ts").read_text(encoding="utf-8")
        pw_real_cfg = (ROOT / "gui" / "web" / "playwright.real.config.ts").read_text(encoding="utf-8")
        self.assertIn("VEHICLE_SIL_PLAYWRIGHT_JUNIT", pw_cfg)
        self.assertIn("VEHICLE_SIL_VERIFY_WORKBENCH", pw_cfg)
        self.assertIn("../../build/logs/web-e2e-${kind}.junit.xml", pw_cfg)
        self.assertIn("VEHICLE_SIL_PLAYWRIGHT_JUNIT", pw_real_cfg)
        self.assertIn("../../build/logs/web-e2e-real.junit.xml", pw_real_cfg)

    def _make_fake_uv(self, bin_dir: Path, args_log: Path | None = None) -> None:
        bin_dir.mkdir(exist_ok=True)
        fake_uv = bin_dir / ("uv.bat" if os.name == "nt" else "uv")
        if os.name == "nt":
            log_line = f'echo %* >> "{args_log}"\r\n' if args_log is not None else ""
            fake_uv.write_text(f"@echo off\r\n{log_line}exit /b 0\r\n", encoding="utf-8")
        else:
            log_line = f"printf '%s\\n' \"$*\" >> {args_log!s}\n" if args_log is not None else ""
            fake_uv.write_text(f"#!/bin/sh\n{log_line}exit 0\n", encoding="utf-8")
            fake_uv.chmod(0o755)

    def test_verify_gui_no_sync_missing_venv_exits_two(self) -> None:
        missing_venv = self.dir / "missing-venv"
        bin_dir = self.dir / "fake-bin"
        self._make_fake_uv(bin_dir)
        env = {
            k: v
            for k, v in os.environ.items()
            if k not in (job_summary.SUMMARY_ENV, job_summary.NESTED_ENV, job_summary.ARTIFACT_ENV)
        }
        env["UV_PROJECT_ENVIRONMENT"] = str(missing_venv)
        env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
        proc = subprocess.run(
            [sys.executable, str(VERIFY_GUI), "--skip-web", "--no-wait", "--no-sync"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdin=subprocess.DEVNULL,
            env=env,
        )
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn("--no-sync", proc.stderr)
        self.assertIn(str(missing_venv), proc.stderr)

    def test_verify_gui_no_sync_omits_sync_and_passes_no_sync_to_uv_run(self) -> None:
        venv = self.dir / "existing-venv"
        venv.mkdir()
        bin_dir = self.dir / "fake-bin"
        args_log = self.dir / "uv-calls.txt"
        self._make_fake_uv(bin_dir, args_log)
        env = {
            k: v
            for k, v in os.environ.items()
            if k not in (job_summary.SUMMARY_ENV, job_summary.NESTED_ENV, job_summary.ARTIFACT_ENV)
        }
        env["UV_PROJECT_ENVIRONMENT"] = str(venv)
        env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
        proc = subprocess.run(
            [sys.executable, str(VERIFY_GUI), "--skip-web", "--no-wait", "--no-sync", "--json", "--tag", "test-nosync"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdin=subprocess.DEVNULL,
            env=env,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
        payload = json.loads(proc.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual([s["step"] for s in payload["steps"]], ["server"])
        calls = args_log.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(calls), 1)
        self.assertIn("run --no-sync pytest", calls[0])
        self.assertNotIn("sync --locked", calls[0])


class VerifyWebFilteringTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.web_dir = self.dir / "web"
        self.web_dir.mkdir()
        (self.web_dir / "package.json").write_text(
            json.dumps({"engines": {"node": "^24.15.0 || ^26.0.0", "npm": ">=11.12.1"}}),
            encoding="utf-8",
        )
        self.log_dir = self.dir / "logs"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _run_main(
        self,
        argv: list[str],
        *,
        has_node_modules: bool = True,
        has_dist: bool = True,
        has_uv: bool = True,
    ) -> tuple[int, str, str, list[tuple[str, list[str]]], list[str]]:
        nm = self.web_dir / "node_modules"
        dist_browser = self.web_dir / "dist" / "browser"
        if has_node_modules:
            nm.mkdir(exist_ok=True)
        elif nm.exists():
            shutil.rmtree(nm)
        if has_dist:
            dist_browser.mkdir(parents=True, exist_ok=True)
        elif dist_browser.exists():
            shutil.rmtree(dist_browser)

        ran: list[tuple[str, list[str]]] = []
        waits: list[str] = []

        def fake_which(cmd: str) -> str | None:
            if cmd == "uv" and not has_uv:
                return None
            return f"/fake/{cmd}"

        def fake_run_step(step, _env, _log, _junit=None) -> None:
            ran.append((step.name, list(step.command)))
            step.ok = True
            step.code = 0

        def fake_wait(prog: str, _limit_s: float = 0) -> None:
            waits.append(prog)

        out, err = io.StringIO(), io.StringIO()
        code = 0
        with (
            mock.patch.object(verify_web_mod, "WEB", self.web_dir),
            mock.patch.object(verify_web_mod, "LOG_DIR", self.log_dir),
            mock.patch.object(verify_web_mod.shutil, "which", side_effect=fake_which),
            mock.patch.object(verify_web_mod, "check_engine"),
            mock.patch.object(verify_web_mod, "run_step", side_effect=fake_run_step),
            mock.patch.object(verify_web_mod, "check_translations"),
            mock.patch.object(verify_web_mod, "wait_until_quiet", side_effect=fake_wait),
            mock.patch.object(verify_web_mod.verify_common, "install_sigterm_handler"),
            mock.patch.object(sys, "argv", ["scripts/verify-web.py", *argv]),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            try:
                code = verify_web_mod.main()
            except SystemExit as exc:
                code = exc.code if isinstance(exc.code, int) else 1
        return code, out.getvalue(), err.getvalue(), ran, waits

    def test_heavy_steps_exclude_i18n_and_build(self) -> None:
        self.assertEqual(verify_web_mod.HEAVY, {"test", "client", "consumers", "e2e", "e2e-dev"})
        self.assertNotIn("i18n", verify_web_mod.HEAVY)
        self.assertNotIn("build", verify_web_mod.HEAVY)

    def test_only_canonical_order_and_repeated_flags(self) -> None:
        code, _out, err, ran, waits = self._run_main(
            ["--only", "e2e,build", "--only", "lint"],
            has_dist=False,  # build is included, so missing dist/browser is allowed
        )
        self.assertEqual(code, 0, err)
        self.assertEqual([name for name, _ in ran], ["lint", "build", "e2e"])
        # Only e2e is HEAVY; lint and build do not call wait_until_quiet
        self.assertEqual(waits, ["verify-web"])

    def test_only_format_lint_skips_uv_and_busy_wait(self) -> None:
        code, _out, err, ran, waits = self._run_main(["--only", "format,lint"], has_uv=False, has_dist=False)
        self.assertEqual(code, 0, err)
        self.assertEqual([name for name, _ in ran], ["format", "lint"])
        self.assertEqual(waits, [])

    def test_only_e2e_dev_does_not_require_dist_browser(self) -> None:
        code, _out, err, ran, _waits = self._run_main(["--only", "e2e-dev", "--no-wait"], has_dist=False)
        self.assertEqual(code, 0, err)
        self.assertEqual([name for name, _ in ran], ["e2e-dev"])

    def test_only_unknown_empty_and_conflicts_exit_two(self) -> None:
        for bad in ("bogus", "", "lint,,format"):
            code, _out, err, ran, _ = self._run_main(["--only", bad])
            self.assertEqual(code, 2, (bad, err))
            self.assertEqual(ran, [])
            self.assertIn("verify-web: error:", err)

        for conflict in (
            "--skip-install",
            "--skip-checks",
            "--skip-e2e",
            "--skip-build",
            "--e2e-dev",
            "--i18n-only",
            "--i18n-update",
            "--workbench",
        ):
            code, _out, err, ran, _ = self._run_main(["--only", "lint", conflict])
            self.assertEqual(code, 2, (conflict, err))
            self.assertEqual(ran, [])
            self.assertIn(conflict, err)

    def test_only_missing_prereqs_exit_two(self) -> None:
        code, _out, err, ran, _ = self._run_main(["--only", "lint"], has_node_modules=False)
        self.assertEqual(code, 2)
        self.assertIn("node_modules", err)
        self.assertEqual(ran, [])

        code, _out, err, ran, _ = self._run_main(["--only", "e2e"], has_dist=False)
        self.assertEqual(code, 2)
        self.assertIn("dist/browser", err)
        self.assertEqual(ran, [])

        code, _out, err, ran, _ = self._run_main(["--only", "e2e"], has_uv=False)
        self.assertEqual(code, 2)
        self.assertIn("uv", err)
        self.assertEqual(ran, [])

    def test_skip_checks_omits_package_while_default_keeps_package(self) -> None:
        # --skip-checks drops format..i18n AND package (keeps build, bundle, e2e)
        code, _out, err, ran, _ = self._run_main(["--skip-install", "--skip-checks", "--no-wait"])
        self.assertEqual(code, 0, err)
        self.assertEqual([name for name, _ in ran], ["build", "bundle", "e2e"])

        # Release / CI invocation (--skip-install --skip-e2e, without --skip-checks) still runs package and consumers
        code, _out, err, ran, _ = self._run_main(["--skip-install", "--skip-e2e", "--no-wait"])
        self.assertEqual(code, 0, err)
        self.assertEqual(
            [name for name, _ in ran],
            ["format", "lint", "test", "client", "timeline", "i18n", "build", "bundle", "package", "consumers"],
        )

        # --only package,consumers runs in canonical order and waits before heavy consumers step
        code, _out, err, ran, waits = self._run_main(["--only", "consumers,package"], has_dist=False)
        self.assertEqual(code, 0, err)
        self.assertEqual([name for name, _ in ran], ["package", "consumers"])
        self.assertEqual(waits, ["verify-web"])

    def test_skip_build_and_all_skipped_and_conflicts(self) -> None:
        code, _out, err, ran, _ = self._run_main(["--skip-install", "--skip-checks", "--skip-build", "--no-wait"])
        self.assertEqual(code, 0, err)
        self.assertEqual([name for name, _ in ran], ["e2e"])

        # Missing dist/browser with --skip-build -> exit 2
        code, _out, err, ran, _ = self._run_main(
            ["--skip-install", "--skip-checks", "--skip-build", "--no-wait"], has_dist=False
        )
        self.assertEqual(code, 2)
        self.assertIn("dist/browser", err)
        self.assertEqual(ran, [])

        # All skipped -> exit 2
        code, _out, err, ran, _ = self._run_main(
            ["--skip-install", "--skip-checks", "--skip-build", "--skip-e2e"]
        )
        self.assertEqual(code, 2)
        self.assertIn("nothing to run", err)

        # Conflict with --workbench / --i18n-only -> exit 2
        for flag in ("--workbench", "--i18n-only"):
            code, _out, err, _ran, _ = self._run_main(["--skip-build", flag])
            self.assertEqual(code, 2, (flag, err))

    def test_e2e_spec_forwarding_and_validation(self) -> None:
        code, _out, err, ran, _ = self._run_main([
            "--only", "e2e", "--no-wait",
            "--e2e-spec", "e2e/app.spec.ts",
            "--e2e-spec", "e2e/run.spec.ts",
            "--e2e-grep", "double click",
            "--e2e-repeat", "2",
        ])
        self.assertEqual(code, 0, err)
        self.assertEqual(len(ran), 1)
        self.assertEqual(
            ran[0][1],
            ["/fake/npm", "run", "e2e", "--", "e2e/app.spec.ts", "e2e/run.spec.ts",
             "--grep", "double click", "--repeat-each", "2"],
        )

        for bad in ("", "   ", "--grep"):
            code, _out, err, ran, _ = self._run_main(["--only", "e2e", "--e2e-spec", bad])
            self.assertEqual(code, 2, (bad, err))
            self.assertIn("--e2e-spec", err)
            self.assertEqual(ran, [])


class VerifyGuiFilteringTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.log_dir = self.dir / "logs"
        self.venv = self.dir / "venv"
        self.venv.mkdir()
        self.launcher = self.dir / "vehicle_sil_launch"
        self.launcher.write_text("#!/bin/sh\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _run_main(
        self,
        argv: list[str],
        *,
        has_node: bool = True,
        env_extra: dict[str, str] | None = None,
    ) -> tuple[int, str, str, list[tuple[str, list[str]]], list[str]]:
        ran: list[tuple[str, list[str]]] = []
        waited_steps: list[str] = []

        def fake_which(cmd: str) -> str | None:
            if cmd == "node" and not has_node:
                return None
            return f"/fake/{cmd}"

        def fake_run_step(step, step_env, _log, _junit=None) -> None:
            self.last_env = dict(step_env)
            ran.append((step.name, list(step.command)))
            step.ok = True
            step.code = 0

        def fake_wait(_prog: str, _limit_s: float = 0) -> None:
            waited_steps.append(f"before:{len(ran)}")

        env = {k: v for k, v in os.environ.items() if k != "VEHICLE_SIL_GUI_LAUNCHER"}
        env["UV_PROJECT_ENVIRONMENT"] = str(self.venv)
        if env_extra:
            env.update(env_extra)

        out, err = io.StringIO(), io.StringIO()
        code = 0
        with (
            mock.patch.object(verify_gui_mod, "LOG_DIR", self.log_dir),
            mock.patch.object(verify_gui_mod.shutil, "which", side_effect=fake_which),
            mock.patch.object(verify_gui_mod, "run_step", side_effect=fake_run_step),
            mock.patch.object(verify_gui_mod, "wait_until_quiet", side_effect=fake_wait),
            mock.patch.object(verify_gui_mod.verify_common, "install_sigterm_handler"),
            mock.patch.dict(os.environ, env, clear=True),
            mock.patch.object(sys, "argv", ["scripts/verify-gui.py", *argv]),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            try:
                code = verify_gui_mod.main()
            except SystemExit as exc:
                code = exc.code if isinstance(exc.code, int) else 1
        return code, out.getvalue(), err.getvalue(), ran, waited_steps

    def test_k_skips_web_example_and_real_without_skip_web(self) -> None:
        code, out, err, ran, _ = self._run_main(
            ["--no-sync", "--no-wait", "-k", "security", "--json"], has_node=False
        )
        self.assertEqual(code, 0, err + out)
        self.assertEqual([name for name, _ in ran], ["server"])
        cmd = ran[0][1]
        self.assertIn("gui/server/tests", cmd)
        self.assertIn("-k", cmd)
        self.assertNotIn("-n", cmd)
        payload = json.loads(out)
        skipped_names = {entry["step"] for entry in payload.get("skipped", [])}
        self.assertEqual(skipped_names, {"web", "example", "real"})

    def test_workers_option_overrides_default_xdist_behavior(self) -> None:
        # Explicit --workers 4 enables -n 4 even with -k (used by CI for test_real_).
        code, out, err, ran, _ = self._run_main(
            ["--no-sync", "--no-wait", "-k", "test_real_", "--workers", "4"], has_node=False
        )
        self.assertEqual(code, 0, err + out)
        self.assertEqual([name for name, _ in ran], ["server"])
        cmd = ran[0][1]
        self.assertIn("-k", cmd)
        idx = cmd.index("-n")
        self.assertEqual(cmd[idx + 1], "4")

        # --workers 1 disables -n even on a full server run.
        code, out, err, ran, _ = self._run_main(
            ["--skip-web", "--no-sync", "--no-wait", "--workers", "1"], has_node=False
        )
        self.assertEqual(code, 0, err + out)
        self.assertEqual([name for name, _ in ran], ["server"])
        self.assertNotIn("-n", ran[0][1])

        # Invalid --workers -> exit 2.
        for bad in ("0", "-2"):
            code, _out, err, ran, _ = self._run_main(["--skip-web", "--no-sync", "--workers", bad], has_node=False)
            self.assertEqual(code, 2, (bad, err))
            self.assertIn("--workers", err)
            self.assertEqual(ran, [])

    def test_positional_targets_implies_server_only_without_n_or_ignore(self) -> None:
        target = "gui/server/tests/test_availability.py"
        code, out, err, ran, _ = self._run_main(["--no-sync", "--no-wait", target], has_node=False)
        self.assertEqual(code, 0, err + out)
        self.assertEqual([name for name, _ in ran], ["server"])
        cmd = ran[0][1]
        self.assertIn(target, cmd)
        self.assertNotIn("gui/server/tests", cmd)
        self.assertNotIn("-n", cmd)
        self.assertFalse(any(a.startswith("--ignore=") for a in cmd), cmd)

    def test_skip_server_validation_and_real_only_mode(self) -> None:
        # --skip-server with positional target or -k -> exit 2
        for argv in (["--skip-server", "gui/server/tests/test_availability.py"], ["--skip-server", "-k", "foo"]):
            code, _out, err, ran, _ = self._run_main(argv)
            self.assertEqual(code, 2, (argv, err))
            self.assertEqual(ran, [])

        # --skip-server --skip-web without launcher -> exit 2
        code, _out, err, ran, _ = self._run_main(["--skip-server", "--skip-web"])
        self.assertEqual(code, 2, err)
        self.assertEqual(ran, [])

        # --skip-server --skip-web --skip-e2e with launcher -> exit 2
        code, _out, err, ran, _ = self._run_main(
            ["--skip-server", "--skip-web", "--skip-e2e", "--launcher", str(self.launcher)]
        )
        self.assertEqual(code, 2, err)
        self.assertEqual(ran, [])

        # --skip-server --skip-web --no-sync --launcher <path> runs real only with --real-spec & --real-grep
        code, out, err, ran, _ = self._run_main([
            "--skip-server", "--skip-web", "--no-sync", "--no-wait",
            "--launcher", str(self.launcher),
            "--real-spec", "e2e/real/scenario.spec.ts",
            "--real-grep", "stops cleanly",
        ])
        self.assertEqual(code, 0, err + out)
        self.assertEqual([name for name, _ in ran], ["real"])
        self.assertEqual(
            ran[0][1],
            ["/fake/npm", "--prefix", "gui/web", "run", "e2e:real", "--",
             "e2e/real/scenario.spec.ts", "--grep", "stops cleanly"],
        )

        # Invalid --real-spec -> exit 2
        for bad in ("", "--grep"):
            code, _out, err, ran, _ = self._run_main([
                "--skip-server", "--skip-web", "--no-sync", "--launcher", str(self.launcher), "--real-spec", bad,
            ])
            self.assertEqual(code, 2, (bad, err))
            self.assertIn("--real-spec", err)

    def test_wait_until_quiet_skipped_for_sync_and_web(self) -> None:
        code, out, err, ran, waited = self._run_main(["--launcher", str(self.launcher)])
        self.assertEqual(code, 0, err + out)
        self.assertEqual([name for name, _ in ran], ["sync", "server", "web", "example", "real"])
        # Waits before server (index 1), example (index 3), real (index 4); NOT before sync (0) or web (2)
        self.assertEqual(waited, ["before:1", "before:3", "before:4"])
        self.assertEqual(self.last_env.get("VEHICLE_SIL_GUI_UV_NO_SYNC"), "1")

    def test_stale_launcher_prints_non_fatal_warning(self) -> None:
        fake_root = self.dir / "repo"
        src_file = fake_root / "libs" / "launch" / "src" / "run.cpp"
        src_file.parent.mkdir(parents=True)
        src_file.write_text("// edited\n", encoding="utf-8")
        # Make launcher strictly older than src_file
        os.utime(self.launcher, (1_700_000_000, 1_700_000_000))
        os.utime(src_file, (1_700_000_100, 1_700_000_100))

        self.assertEqual(verify_gui_mod.stale_launcher_source(self.launcher, fake_root), src_file)

        with mock.patch.object(verify_gui_mod, "ROOT", fake_root):
            code, out, err, ran, _ = self._run_main(
                ["--skip-server", "--skip-web", "--no-sync", "--no-wait", "--launcher", str(self.launcher)]
            )
        self.assertEqual(code, 0, err + out)
        self.assertEqual([name for name, _ in ran], ["real"])
        self.assertIn("warning: launcher", err)
        self.assertIn("libs/launch/src/run.cpp", err)

        # Fresh launcher (newer than source) produces no stale warning
        os.utime(self.launcher, (1_700_000_200, 1_700_000_200))
        self.assertIsNone(verify_gui_mod.stale_launcher_source(self.launcher, fake_root))
        with mock.patch.object(verify_gui_mod, "ROOT", fake_root):
            code2, out2, err2, _ran2, _ = self._run_main(
                ["--skip-server", "--skip-web", "--no-sync", "--no-wait", "--launcher", str(self.launcher)]
            )
        self.assertEqual(code2, 0, err2 + out2)
        self.assertNotIn("warning: launcher", err2)


if __name__ == "__main__":
    unittest.main()
