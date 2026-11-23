# SPDX-License-Identifier: MIT
"""Tests of .github/workflows/ci.yml: the job `all checks` (the one required check of the branch protection) must
wait for every other job (except `linux-tsan`, which does not hold a pull request and fails the run on main), must
run even when one of them failed or was cancelled, and the job `changes` must select the expected jobs for each
kind of change without leaving `linux` with an unguarded empty matrix.

    python tests/scripts/test_ci_workflow.py      (exit code 0 when every test passes; needs PyYAML)
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = yaml.safe_load((ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8"))
ON = WORKFLOW.get("on") or WORKFLOW.get(True)
JOBS = WORKFLOW["jobs"]
GATE = "all-checks"
# Jobs that `all checks` does not wait for: the thread sanitizer, which does not hold a pull request (its result there
# is a success because of continue-on-error), and which fails the run on main (release.yml reads the run).
NOT_WAITED = {"linux-tsan"}


def find_bash() -> str | None:
    """Finds a POSIX bash (MSYS2 or Git for Windows on Windows, never the System32 WSL stub)."""
    exe = Path(sys.executable).resolve()
    if len(exe.parents) >= 3:
        msys_bash = exe.parents[2] / "usr" / "bin" / ("bash.exe" if os.name == "nt" else "bash")
        if msys_bash.is_file():
            return str(msys_bash)
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        if not directory or "system32" in directory.lower():
            continue
        candidate = shutil.which("bash", path=directory)
        if candidate:
            return candidate
    return None


BASH = find_bash()


class AllChecks(unittest.TestCase):
    def test_the_name_is_the_one_the_protection_uses(self):
        self.assertEqual(JOBS[GATE]["name"], "all checks")

    def test_needs_every_other_job(self):
        needs = JOBS[GATE]["needs"]
        needs = [needs] if isinstance(needs, str) else needs
        others = sorted(name for name in JOBS if name != GATE and name not in NOT_WAITED)
        self.assertEqual(sorted(needs), others,
                         f"add a new job to `needs` of {GATE} in .github/workflows/ci.yml (and to its RESULTS list)")

    def test_lists_every_result(self):
        results = JOBS[GATE]["steps"][0]["env"]["RESULTS"]
        for name in JOBS:
            if name != GATE and name not in NOT_WAITED:
                self.assertIn(f"needs.{name}.result", results, f"RESULTS of {GATE} does not list {name}")

    def test_a_job_not_waited_for_holds_only_main(self):
        # Not waited for, it must not fail a pull request's run silently, and must count on main.
        for name in NOT_WAITED:
            self.assertEqual(str(JOBS[name]["continue-on-error"]).strip(),
                             "${{ github.event_name == 'pull_request' }}", name)

    def test_runs_after_a_failure_and_a_cancel(self):
        # `!cancelled()` would leave the job skipped in a cancelled run, and a skipped required check passes.
        self.assertEqual(str(JOBS[GATE]["if"]).strip(), "always()")

    def test_no_workflow_level_paths_filter_blocks_the_required_check(self):
        # A workflow-level paths/paths-ignore filter on pull_request leaves `all checks` permanently Pending when a
        # change is filtered out, and on push to main leaves release.yml without a ci.yml run on the tagged commit.
        for trigger in ("push", "pull_request"):
            self.assertIn(trigger, ON)
            config = ON[trigger] or {}
            self.assertNotIn("paths", config, trigger)
            self.assertNotIn("paths-ignore", config, trigger)

    def test_linux_matrix_is_guarded_when_presets_is_empty(self):
        # GitHub Actions fails a workflow run if a matrix vector evaluates to `[]` unless the job's `if:` skips it.
        self.assertEqual(str(JOBS["linux"].get("if", "")).strip(), "needs.changes.outputs.presets != '[]'")

    def test_every_server_test_that_uses_the_real_launcher_matches_the_control_filter(self):
        # The leg linux-gcc-control runs verify-gui.py with `-k test_real_` (gui-server runs the stand-in tests).
        steps = JOBS["linux"]["steps"]
        step = next(s for s in steps if s.get("name") == "The GUI server against this launcher")
        self.assertIn("-k test_real_", step["run"])
        self.assertIn("--no-sync", step["run"])
        tests_dir = ROOT / "gui" / "server" / "tests"
        for path in sorted(tests_dir.rglob("test_*.py")):
            if path.relative_to(tests_dir).as_posix() == "test_example_client.py":
                continue
            text = path.read_text(encoding="utf-8")
            if any(token in text for token in ("VEHICLE_SIL_GUI_LAUNCHER", "VEHICLE_SIL_GUI_EXAMPLES", "real_configs")):
                self.assertTrue(
                    path.name.startswith("test_real_"),
                    f"{path.name} uses VEHICLE_SIL_GUI_LAUNCHER, VEHICLE_SIL_GUI_EXAMPLES or real_configs, so its "
                    "name must start with test_real_ to run in the linux-gcc-control step",
                )

    def test_release_workflow_decouples_cpp_builds_from_web(self):
        release = yaml.safe_load((ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8"))
        rjobs = release["jobs"]
        for builder in ("build-linux", "build-windows"):
            self.assertIn(builder, rjobs)
            self.assertNotIn("needs", rjobs[builder])
        self.assertEqual(sorted(rjobs["package-linux"]["needs"]), ["build-linux", "web"])
        self.assertEqual(sorted(rjobs["package-windows"]["needs"]), ["build-windows", "web"])
        self.assertEqual(sorted(rjobs["draft"]["needs"]), ["package-linux", "package-windows", "web"])


@unittest.skipIf(
    BASH is None or any(shutil.which(cmd) is None for cmd in ("grep", "tee")),
    "bash, grep and tee are needed to run the `changes` job script",
)
class ChangesTruthTable(unittest.TestCase):
    """Runs the bash script of the `changes` job against each change category and event."""

    @classmethod
    def setUpClass(cls):
        look = next(s for s in JOBS["changes"]["steps"] if s.get("id") == "look")
        cls.script = look["run"]

    def evaluate(self, event: str, files: list[str]) -> dict[str, str]:
        wrapper = (
            'export PATH="/usr/bin:/bin:$PATH"\n'
            "set -euo pipefail\n"
            'git() { printf "%s\\n" "$MOCK_FILES"; }\n'
            f"{self.script}\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "github_output.txt"
            env = {
                **os.environ,
                "EVENT": event,
                "MOCK_FILES": "\n".join(files),
                "GITHUB_OUTPUT": out.as_posix(),
            }
            proc = subprocess.run([BASH, "-c", wrapper], env=env, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, f"stdout: {proc.stdout}\nstderr: {proc.stderr}")
            lines = [line.strip() for line in out.read_text(encoding="utf-8").splitlines() if "=" in line]
        return dict(line.split("=", 1) for line in lines)

    def test_pull_request_and_push_truth_table(self):
        debug = '["linux-gcc-debug"]'
        std = '["linux-gcc-debug", "linux-gcc-control"]'
        full_pr = '["linux-gcc-debug", "linux-gcc-release", "linux-gcc-static", "linux-gcc-control"]'
        full_main = '["linux-gcc-debug", "linux-gcc-release", "linux-gcc-static", "linux-gcc-control", "linux-clang-asan"]'
        cases = [
            # (description, event, files, expected outputs)
            (
                "pure docs / CHANGELOG",
                "pull_request",
                ["CHANGELOG.md", "README.md", "docs/architecture.md", "docs/ja/architecture.md", "template/README.md"],
                {"docs_only": "true", "full": "false", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": "[]"},
            ),
            (
                "C++-tested reference docs",
                "pull_request",
                ["docs/simulation-config.md", "docs/scenario.md", "docs/ecu-executable.md", "docs/j1939-guide.md"],
                {"docs_only": "true", "full": "false", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": debug},
            ),
            (
                "control-server-tested guides",
                "pull_request",
                ["docs/howto/first-ecu.md", "docs/howto/operate-signals.md"],
                {"docs_only": "true", "full": "false", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": std},
            ),
            (
                "GUI manual (docs/gui.md) runs gui-server and timeline test",
                "pull_request",
                ["docs/gui.md", "docs/ja/gui.md"],
                {"docs_only": "true", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "true", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": "[]"},
            ),
            (
                "GUI protocol README runs gui-server and format check",
                "pull_request",
                ["gui/protocol/README.md", "gui/protocol/README.ja.md"],
                {"docs_only": "true", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "true", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": "[]"},
            ),
            (
                "GUI web README runs format check only",
                "pull_request",
                ["gui/web/README.md"],
                {"docs_only": "true", "full": "false", "gui": "true", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": "[]"},
            ),
            (
                "GUI starter and client READMEs run pack:client",
                "pull_request",
                ["examples/gui_starter/README.md", "gui/client/README.md"],
                {"docs_only": "true", "full": "false", "gui": "true", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "true", "tsan": "false", "windows": "false", "presets": "[]"},
            ),
            (
                "pure frontend (gui/web, gui/client, gui/timeline, examples/gui_*) skips C++ and Windows",
                "pull_request",
                ["gui/web/src/app/design/designer.ts", "gui/timeline/viewer.js", "examples/gui_client/run.mjs"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": "[]"},
            ),
            (
                "frontend file read by test_shared_values.cpp runs linux-gcc-debug",
                "pull_request",
                ["gui/client/src/client.ts"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": debug},
            ),
            (
                "GUI protocol examples skip C++ and Windows",
                "pull_request",
                ["gui/protocol/examples/app.json"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": "[]"},
            ),
            (
                "empty pull request diff falls back to full matrix",
                "pull_request",
                [],
                {"docs_only": "false", "full": "true", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": full_pr},
            ),
            (
                "mixed docs_only: GUI manual + control-server-tested guide",
                "pull_request",
                ["docs/gui.md", "docs/howto/first-ecu.md"],
                {"docs_only": "true", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "true", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": std},
            ),
            (
                "mixed docs_only: GUI manual + GUI starter README",
                "pull_request",
                ["docs/gui.md", "examples/gui_starter/README.md"],
                {"docs_only": "true", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "true", "gui_pack_docs": "true", "tsan": "false", "windows": "false", "presets": "[]"},
            ),
            (
                "GUI server runs debug and control alongside gui and gui-server",
                "pull_request",
                ["gui/server/src/vehicle_sil_gui/server.py"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": std},
            ),
            (
                "dashboard.schema.json runs debug and control alongside gui and gui-server",
                "pull_request",
                ["gui/protocol/dashboard.schema.json"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": std},
            ),
            (
                "C++ libraries, bridges, tools and tests skip gui and gui-server",
                "pull_request",
                ["libs/launch/src/run.cpp", "tests/launch/test_run.cpp", "bridges/ethernet/main.cpp"],
                {"docs_only": "false", "full": "false", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": std},
            ),
            (
                "examples/ecu_main dashboard and panels run gui-server alongside C++",
                "pull_request",
                ["examples/ecu_main/simulation.dashboard.yaml", "examples/ecu_main/panels/rpm-bar.js"],
                {"docs_only": "false", "full": "false", "gui": "false", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": std},
            ),
            (
                "JSON Schemas in schema/ skip gui and gui-server",
                "pull_request",
                ["schema/simulation.schema.json"],
                {"docs_only": "false", "full": "false", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": std},
            ),
            (
                "proto, python and scripts run C++, Windows, gui and gui-server",
                "pull_request",
                ["proto/vehicle_sil/control/v1/control.proto", "python/vehicle_sil_control/client.py", "scripts/verify.py"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": std},
            ),
            (
                "tools/knowledge/ runs python only",
                "pull_request",
                ["tools/knowledge/shared_edit.py", "tools/knowledge/test_shared_edit.py"],
                {"docs_only": "false", "full": "false", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": "[]"},
            ),
            (
                "build files inside tools/knowledge/ or gui/ still force full C++ matrix (full=true override)",
                "pull_request",
                ["tools/knowledge/CMakeLists.txt"],
                {"docs_only": "false", "full": "true", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": full_pr},
            ),
            (
                "CMakeLists.txt, cmake/, CMakePresets.json, patches/, external/, .gitmodules run full C++ matrix",
                "pull_request",
                ["CMakeLists.txt", "cmake/VehicleSilHelpers.cmake", ".gitmodules"],
                {"docs_only": "false", "full": "true", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": full_pr},
            ),
            (
                ".github/workflows/ci.yml runs full C++ matrix, gui and gui-server",
                "pull_request",
                [".github/workflows/ci.yml"],
                {"docs_only": "false", "full": "true", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": full_pr},
            ),
            (
                "push to main always runs full matrix including linux-clang-asan",
                "push",
                ["CHANGELOG.md"],
                {"docs_only": "false", "full": "true", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": full_main},
            ),
            (
                "workflow_dispatch runs full matrix including linux-clang-asan",
                "workflow_dispatch",
                [],
                {"docs_only": "false", "full": "true", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": full_main},
            ),
        ]
        for description, event, files, expected in cases:
            with self.subTest(description=description, event=event):
                self.assertEqual(self.evaluate(event, files), expected, description)


if __name__ == "__main__":
    unittest.main()
