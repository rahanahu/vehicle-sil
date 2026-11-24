# SPDX-License-Identifier: MIT
"""Tests of .github/workflows/ci.yml: the job `all checks` (the one required check of the branch protection) must
wait for every other job (except `linux-tsan`, which does not hold a pull request and fails the run on main), must
run even when one of them failed or was cancelled, and the job `changes` must select the expected jobs for each
kind of change without leaving `linux` with an unguarded empty matrix.

    python tests/scripts/test_ci_workflow.py      (exit code 0 when every test passes; needs PyYAML)
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
CI_TEXT = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
RELEASE_TEXT = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
WORKFLOW = yaml.safe_load(CI_TEXT)
RELEASE_WORKFLOW = yaml.safe_load(RELEASE_TEXT)
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
        rjobs = RELEASE_WORKFLOW["jobs"]
        for builder in ("build-linux", "build-windows"):
            self.assertIn(builder, rjobs)
            self.assertNotIn("needs", rjobs[builder])
        self.assertEqual(sorted(rjobs["package-linux"]["needs"]), ["build-linux", "web"])
        self.assertEqual(sorted(rjobs["package-windows"]["needs"]), ["build-windows", "web"])
        self.assertEqual(sorted(rjobs["draft"]["needs"]), ["package-linux", "package-windows", "web"])


class WorkflowCachesAndSanitizers(unittest.TestCase):
    """Checks cache keys, apt-grpc warming, ccache content-hash gating, node_modules cache, and sanitizer flags."""

    def test_no_broken_env_image_version_in_workflows(self):
        for name, text, wf in (
            ("ci.yml", CI_TEXT, WORKFLOW),
            ("release.yml", RELEASE_TEXT, RELEASE_WORKFLOW),
        ):
            self.assertNotIn("env.ImageVersion", text, f"{name} must read ImageVersion via a runner-img step output")
            for job_name, job in wf["jobs"].items():
                steps = job.get("steps", [])
                seen_runner_img = False
                for step in steps:
                    if step.get("id") == "runner-img":
                        seen_runner_img = True
                    key = str((step.get("with") or {}).get("key", ""))
                    if "steps.runner-img.outputs.version" in key:
                        self.assertTrue(
                            seen_runner_img,
                            f"{name}:{job_name} references steps.runner-img.outputs.version before defining runner-img",
                        )

    def test_apt_grpc_warms_on_main_without_installing_and_installs_on_prs(self):
        steps = JOBS["linux"]["steps"]
        restore = next(s for s in steps if s.get("id") == "apt-grpc")
        self.assertTrue(str(restore.get("uses", "")).startswith("actions/cache/restore@"))
        self.assertIn("github.event_name == 'pull_request'", restore["if"])
        self.assertIn("github.ref == 'refs/heads/main'", restore["if"])
        self.assertEqual(restore["with"]["key"], "apt-grpc-ubuntu2404-${{ steps.runner-img.outputs.version }}")
        self.assertEqual(restore["with"]["lookup-only"], "${{ github.event_name != 'pull_request' }}")

        warm = next(s for s in steps if s.get("name") == "Warm gRPC package cache (main)")
        self.assertIn("github.event_name != 'pull_request'", warm["if"])
        self.assertIn("github.ref == 'refs/heads/main'", warm["if"])
        self.assertIn("steps.apt-grpc.outputs.cache-hit != 'true'", warm["if"])
        self.assertIn("--download-only", warm["run"])

        pr_install = next(s for s in steps if s.get("name") == "gRPC of the distribution (pull requests)")
        self.assertIn("github.event_name == 'pull_request'", pr_install["if"])
        self.assertIn("dpkg -i --no-triggers", pr_install["run"])
        self.assertIn("ldconfig", pr_install["run"])
        self.assertIn("dpkg -s libgrpc++-dev", pr_install["run"])

        saves = [
            s for s in steps
            if str(s.get("uses", "")).startswith("actions/cache/save@")
            and (s.get("with") or {}).get("path") == "~/.cache/apt-debs/grpc"
        ]
        self.assertEqual(len(saves), 1)
        self.assertIn("steps.apt-grpc.outputs.cache-hit != 'true'", saves[0]["if"])
        self.assertEqual(saves[0]["with"]["key"], restore["with"]["key"])

    def test_ccache_uses_content_hash_and_skips_save_on_exact_hit(self):
        self.assertEqual(JOBS["changes"]["outputs"]["cpp_hash"], "${{ steps.cpp-hash.outputs.hash }}")
        expected_jobs = {
            "linux": (
                "ccache-${{ matrix.preset }}-",
                "ccache-${{ matrix.preset }}-${{ steps.runner-img.outputs.version }}-${{ needs.changes.outputs.cpp_hash }}",
            ),
            "linux-tsan": (
                "ccache-linux-clang-tsan-",
                "ccache-linux-clang-tsan-${{ steps.runner-img.outputs.version }}-${{ needs.changes.outputs.cpp_hash }}",
            ),
            "linux-oldest-cmake": (
                "ccache-linux-oldest-cmake-",
                "ccache-linux-oldest-cmake-${{ steps.runner-img.outputs.version }}-${{ needs.changes.outputs.cpp_hash }}",
            ),
            "windows": (
                "ccache-windows-mingw-release-",
                "ccache-windows-mingw-release-${{ steps.runner-img.outputs.version }}-"
                "${{ steps.msys2-key.outputs.hash }}-${{ needs.changes.outputs.cpp_hash }}",
            ),
        }
        for job_name, (prefix, expected_key) in expected_jobs.items():
            steps = JOBS[job_name]["steps"]
            restore = next(s for s in steps if s.get("id") == "ccache-restore")
            self.assertTrue(str(restore.get("uses", "")).startswith("actions/cache/restore@"), job_name)
            self.assertEqual(restore["with"]["key"], expected_key, job_name)
            self.assertEqual(restore["with"]["restore-keys"], prefix, job_name)

            saves = [
                s for s in steps
                if str(s.get("uses", "")).startswith("actions/cache/save@")
                and (s.get("with") or {}).get("path") == ".ccache"
            ]
            self.assertEqual(len(saves), 1, job_name)
            save_if = str(saves[0]["if"])
            self.assertIn("github.ref == 'refs/heads/main'", save_if, job_name)
            self.assertIn("steps.ccache-restore.outputs.cache-hit != 'true'", save_if, job_name)
            self.assertEqual(saves[0]["with"]["key"], expected_key, job_name)

    @unittest.skipIf(
        BASH is None or any(shutil.which(cmd) is None for cmd in ("git", "sha256sum", "tee")),
        "bash, git, sha256sum and tee are needed to run the cpp-hash step",
    )
    def test_cpp_hash_paths_exist_and_produce_hex_digest(self):
        step = next(s for s in JOBS["changes"]["steps"] if s.get("id") == "cpp-hash")
        paths = re.findall(r"HEAD:([^\s\\)]+)", step["run"])
        self.assertGreaterEqual(len(paths), 13)
        for required in (".github/workflows/ci.yml", "CMakeLists.txt", "CMakePresets.json", ".gitmodules", "libs", "tests"):
            self.assertIn(required, paths)
        for rel in paths:
            self.assertTrue((ROOT / rel).exists(), f"cpp-hash path missing on HEAD: {rel}")

        wrapper = 'export PATH="/usr/bin:/bin:$PATH"\nset -euo pipefail\n' + step["run"]
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "github_output.txt"
            env = {**os.environ, "GITHUB_OUTPUT": out.as_posix()}
            proc = subprocess.run(
                [BASH, "-c", wrapper],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(proc.returncode, 0, f"stdout: {proc.stdout}\nstderr: {proc.stderr}")
            lines = [line.strip() for line in out.read_text(encoding="utf-8").splitlines() if "=" in line]
        outputs = dict(line.split("=", 1) for line in lines)
        self.assertRegex(outputs["hash"], r"^[0-9a-f]{16}$")

    def test_fast_offline_dpkg_install_retains_verification_and_fallback(self):
        checked = [
            (JOBS["linux"], "Install Ninja, ccache, ethtool and libpcap", "dpkg -s ninja-build"),
            (JOBS["linux-tsan"], "Install Ninja, ccache, ethtool and libpcap", "dpkg -s ninja-build"),
            (JOBS["linux-oldest-cmake"], "Install Ninja, ccache, ethtool and libpcap", "dpkg -s ninja-build"),
            (JOBS["gui"], "Install the browser of the end-to-end tests", "playwright install-deps --dry-run"),
            (RELEASE_WORKFLOW["jobs"]["web"], "Install the browser of the unit tests of the GUI parts", "playwright install-deps --dry-run"),
            (RELEASE_WORKFLOW["jobs"]["build-linux"], "Install GCC 12, Ninja and ccache", "dpkg -s g++-12"),
            (RELEASE_WORKFLOW["jobs"]["package-linux"], "Install GCC 12, Ninja and ccache", "dpkg -s g++-12"),
        ]
        for job, step_name, verify_marker in checked:
            step = next(s for s in job["steps"] if s.get("name") == step_name)
            script = step["run"]
            self.assertIn("NEEDRESTART_SUSPEND=1", script, step_name)
            self.assertIn("DEBIAN_FRONTEND=noninteractive", script, step_name)
            self.assertIn("dpkg -i --no-triggers", script, step_name)
            self.assertIn("ldconfig", script, step_name)
            self.assertIn(verify_marker, script, step_name)
            if "playwright" in verify_marker:
                self.assertIn("fc-cache", script, step_name)
                self.assertIn("npx playwright install-deps chromium", script, step_name)
            else:
                self.assertIn("apt-get install -f", script, step_name)

    def test_web_node_modules_cached_consistently_in_ci_and_release(self):
        expected_key = (
            "node-modules-${{ runner.os }}-${{ runner.arch }}-"
            "${{ steps.setup-node.outputs.node-version }}-"
            "${{ hashFiles('gui/web/package-lock.json') }}"
        )
        for name, job in (("ci:gui", JOBS["gui"]), ("release:web", RELEASE_WORKFLOW["jobs"]["web"])):
            steps = job["steps"]
            setup_node = next(s for s in steps if str(s.get("uses", "")).startswith("actions/setup-node@"))
            self.assertEqual(setup_node.get("id"), "setup-node", name)
            self.assertNotIn("cache", setup_node.get("with") or {}, name)

            nm_cache = next(s for s in steps if s.get("id") == "web-node-modules")
            self.assertEqual(nm_cache["with"]["path"], "gui/web/node_modules", name)
            self.assertEqual(nm_cache["with"]["key"], expected_key, name)

            install = next(s for s in steps if s.get("name") == "Install the packages")
            self.assertEqual(install["if"], "steps.web-node-modules.outputs.cache-hit != 'true'", name)

    def test_ci_sanitizers_use_o1_g1_and_tsan_warns_on_pr_failure(self):
        linux_run = next(s for s in JOBS["linux"]["steps"] if s.get("name") == "Configure, build and test")["run"]
        self.assertIn("linux-clang-asan)", linux_run)
        self.assertIn('-DCMAKE_C_FLAGS_DEBUG=-O1 -g1', linux_run)
        self.assertIn('-DCMAKE_CXX_FLAGS_DEBUG=-O1 -g1', linux_run)

        tsan_steps = JOBS["linux-tsan"]["steps"]
        tsan_run = next(s for s in tsan_steps if s.get("name") == "Configure, build and test")["run"]
        self.assertIn('-DCMAKE_C_FLAGS_DEBUG=-O1 -g1', tsan_run)
        self.assertIn('-DCMAKE_CXX_FLAGS_DEBUG=-O1 -g1', tsan_run)

        warn = next(s for s in tsan_steps if s.get("name") == "Warn on PR TSan failure")
        self.assertEqual(warn["if"], "failure() && github.event_name == 'pull_request'")
        self.assertIn("::warning::linux-clang-tsan failed", warn["run"])
        self.assertIn("GITHUB_STEP_SUMMARY", warn["run"])

    def test_python_docs_only_fast_path_and_ctest_filter_wiring(self):
        self.assertEqual(JOBS["changes"]["outputs"]["ctest_filter"], "${{ steps.look.outputs.ctest_filter }}")

        python_job = JOBS["python"]
        needs = python_job.get("needs")
        needs = [needs] if isinstance(needs, str) else list(needs or [])
        self.assertIn("changes", needs)
        py_step = next(s for s in python_job["steps"] if s.get("name") == "Python checks")
        self.assertEqual(
            py_step["env"]["DOCS_ONLY"],
            "${{ github.event_name == 'pull_request' && needs.changes.outputs.docs_only == 'true' }}",
        )
        self.assertIn(".python-version", py_step["run"])
        self.assertIn("--skip-lowest --only schema,template", py_step["run"])

        for job_name in ("linux", "windows"):
            step = next(s for s in JOBS[job_name]["steps"] if s.get("name") == "Configure, build and test")
            self.assertEqual(step["env"]["CTEST_FILTER"], "${{ needs.changes.outputs.ctest_filter }}", job_name)
            self.assertIn('--tests "$CTEST_FILTER"', step["run"], job_name)

        recordings_step = next(
            s for s in JOBS["linux"]["steps"] if s.get("name") == "The GUI's recordings against this launcher"
        )
        self.assertIn("needs.changes.outputs.ctest_filter == ''", recordings_step["if"])

    def test_non_gui_scripts_allowlist_has_no_gui_script_imports(self):
        look = next(s for s in JOBS["changes"]["steps"] if s.get("id") == "look")["run"]
        m = re.search(r"non_gui_scripts='([^']+)'", look)
        self.assertIsNotNone(m)
        pattern = re.compile(m.group(1))
        gui_scripts = {
            "busy_machine.py",
            "job_summary.py",
            "run-gui.py",
            "verify_common.py",
            "verify-gui.py",
            "verify-web.py",
            "web_i18n.py",
        }
        non_gui_modules: set[str] = set()
        for path in sorted((ROOT / "scripts").glob("*.py")):
            rel = path.relative_to(ROOT).as_posix()
            if pattern.search(rel):
                self.assertNotIn(path.name, gui_scripts, f"{rel} is shared with GUI jobs and must not be in non_gui_scripts")
                non_gui_modules.add(path.stem.replace("-", "_"))
            else:
                self.assertIn(path.name, gui_scripts, f"classify new script {rel} in ci.yml non_gui_scripts or gui_scripts")

        for name in sorted(gui_scripts):
            text = (ROOT / "scripts" / name).read_text(encoding="utf-8")
            for mod in sorted(non_gui_modules):
                self.assertIsNone(
                    re.search(rf"^\s*(?:import|from)\s+{re.escape(mod)}\b", text, re.MULTILINE),
                    f"GUI/shared script {name} imports {mod}; remove it from non_gui_scripts",
                )


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
        control = '["linux-gcc-control"]'
        std = '["linux-gcc-debug", "linux-gcc-control"]'
        full_pr = '["linux-gcc-debug", "linux-gcc-release", "linux-gcc-static", "linux-gcc-control"]'
        full_main = '["linux-gcc-debug", "linux-gcc-release", "linux-gcc-static", "linux-gcc-control", "linux-clang-asan"]'
        gui_server_filter = r"^(launch\.shared_values|schema\.)"
        scripts_filter = r"^(scripts|template)\."
        trace_scripts_filter = r"^(scripts|template)\.|^launch_run\.trace_files$"
        cases = [
            # (description, event, files, expected outputs — ctest_filter defaults to "")
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
                "GUI server fast path runs scoped linux-gcc-control alongside gui and gui-server",
                "pull_request",
                ["gui/server/src/vehicle_sil_gui/server.py"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false",
                 "presets": control, "ctest_filter": gui_server_filter},
            ),
            (
                "dashboard.schema.json fast path runs scoped linux-gcc-control alongside gui and gui-server",
                "pull_request",
                ["gui/protocol/dashboard.schema.json"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false",
                 "presets": control, "ctest_filter": gui_server_filter},
            ),
            (
                "GUI server + shared_values frontend file stays on scoped linux-gcc-control (runs launch.shared_values)",
                "pull_request",
                ["gui/server/src/vehicle_sil_gui/launcher.py", "gui/client/src/client.ts", "docs/gui.md"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false",
                 "presets": control, "ctest_filter": gui_server_filter},
            ),
            (
                "GUI server + C++-tested reference doc falls back to full debug and control",
                "pull_request",
                ["gui/server/src/vehicle_sil_gui/server.py", "docs/simulation-config.md"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": std},
            ),
            (
                "GUI server + control-server-tested guide falls back to full debug and control",
                "pull_request",
                ["gui/server/src/vehicle_sil_gui/server.py", "docs/howto/first-ecu.md"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "false", "presets": std},
            ),
            (
                "pure non-GUI scripts and script tests run python plus scoped debug and Windows",
                "pull_request",
                ["scripts/package.py", "scripts/hotspots.py", "scripts/verify.py", "tests/scripts/test_package.py"],
                {"docs_only": "false", "full": "false", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "true",
                 "presets": debug, "ctest_filter": scripts_filter},
            ),
            (
                "pure scripts/trace-report.py also runs launch_run.trace_files",
                "pull_request",
                ["scripts/trace-report.py", "tests/scripts/test_trace_report.py"],
                {"docs_only": "false", "full": "false", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "false", "windows": "true",
                 "presets": debug, "ctest_filter": trace_scripts_filter},
            ),
            (
                "GUI-related and shared scripts still run gui, gui-server, debug, control, tsan and Windows",
                "pull_request",
                ["scripts/verify-gui.py", "scripts/verify-web.py", "scripts/web_i18n.py",
                 "scripts/busy_machine.py", "scripts/run-gui.py", "scripts/verify_common.py", "scripts/job_summary.py"],
                {"docs_only": "false", "full": "false", "gui": "true", "gui_server": "true",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": std},
            ),
            (
                "non-GUI script mixed with C++ skips gui/gui-server and runs full C++",
                "pull_request",
                ["scripts/package.py", "libs/launch/src/run.cpp"],
                {"docs_only": "false", "full": "false", "gui": "false", "gui_server": "false",
                 "gui_docs": "false", "gui_pack_docs": "false", "tsan": "true", "windows": "true", "presets": std},
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
                "tools/knowledge/ templates (.md) are not docs_only so full knowledge Python tests still run",
                "pull_request",
                ["tools/knowledge/templates/STATE.md"],
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
                self.assertEqual(self.evaluate(event, files), {"ctest_filter": "", **expected}, description)


if __name__ == "__main__":
    unittest.main()
