# SPDX-License-Identifier: MIT
"""Tests of scripts/verify-web.py --summarize-junit on synthetic JUnit XML files: passed/failed/skipped counts,
per-file cumulative duration ordering, top-N slowest test ordering, --json output schema, missing/malformed XML
exit code 2, and distinct non-colliding JUnit paths for web-e2e, real, and workbench runs.

    python tests/scripts/test_verify_web.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

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

    def test_verify_gui_no_sync_missing_venv_exits_two(self) -> None:
        missing_venv = self.dir / "missing-venv"
        env = {
            k: v
            for k, v in os.environ.items()
            if k not in (job_summary.SUMMARY_ENV, job_summary.NESTED_ENV, job_summary.ARTIFACT_ENV)
        }
        env["UV_PROJECT_ENVIRONMENT"] = str(missing_venv)
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
        bin_dir.mkdir()
        args_log = self.dir / "uv-calls.txt"
        fake_uv = bin_dir / ("uv.bat" if os.name == "nt" else "uv")
        if os.name == "nt":
            fake_uv.write_text(f'@echo off\r\necho %*>> "{args_log}"\r\nexit /b 0\r\n', encoding="utf-8")
        else:
            fake_uv.write_text(
                f"#!/bin/sh\nprintf '%s\\n' \"$*\" >> {args_log!s}\nexit 0\n",
                encoding="utf-8",
            )
            fake_uv.chmod(0o755)
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


if __name__ == "__main__":
    unittest.main()
