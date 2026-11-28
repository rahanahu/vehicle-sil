# SPDX-License-Identifier: MIT
"""Tests of scripts/package.py: MSYS2 license staging creates missing parent directories and
`tools --build-only` on Windows exports binaries, compiler metadata, and MSYS2/gRPC licenses.

    python tests/scripts/test_package.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "scripts" / "package.py"


def load_package_module():
    spec = importlib.util.spec_from_file_location("vehicle_sil_test_package", PACKAGE)
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(PACKAGE.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(PACKAGE.parent))
    return module


package_mod = load_package_module()


class PackageWindowsExportTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.log_dir = self.dir / "logs"
        self.addCleanup(self.tmp.cleanup)

    def _make_fake_run(self, *, license_files: dict[str, Path] | None = None, empty_licenses: bool = False):
        mingw_lib = self.dir / "mingw" / "lib"
        mingw_lib.mkdir(parents=True, exist_ok=True)
        license_files = license_files or {}

        def fake_run(step_self, command, cwd=None, env=None, check=True, capture=False, timeout=None):
            cmd = [str(c) for c in command]
            prog = Path(cmd[0]).name
            stdout = ""
            if prog == "gcc" and len(cmd) >= 2 and cmd[1].startswith("-print-file-name="):
                name = cmd[1].split("=", 1)[1]
                stdout = f"{mingw_lib / name}\n"
            elif prog == "cygpath" and len(cmd) >= 3 and cmd[1] == "-u":
                stdout = f"/ucrt64/lib/{Path(cmd[2]).name}\n"
            elif prog == "cygpath" and len(cmd) >= 3 and cmd[1] == "-w":
                posix = cmd[2]
                if posix not in license_files:
                    raise AssertionError(f"unexpected cygpath -w target: {posix}")
                stdout = f"{license_files[posix]}\n"
            elif prog == "pacman" and len(cmd) >= 3 and cmd[1] == "-Qqo":
                stdout = "mingw-w64-ucrt-x86_64-gcc-libs\n"
            elif prog == "pacman" and len(cmd) >= 3 and cmd[1] == "-Qlq":
                if empty_licenses:
                    stdout = "/ucrt64/lib/libstdc++.a\n"
                else:
                    stdout = "".join(f"{p}\n" for p in sorted(license_files))
            elif prog == "objdump" and len(cmd) >= 2 and cmd[1] == "-p":
                stdout = "        DLL Name: api-ms-win-crt-runtime-l1-1-0.dll\n"
            elif prog == "cmake":
                stdout = ""
            else:
                raise AssertionError(f"unexpected command in stubbed Step.run: {cmd}")
            return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr="")

        return fake_run

    def test_stage_msys2_licenses_creates_missing_parent_dir(self) -> None:
        into = self.dir / "bin-windows" / "licenses" / "msys2"
        self.assertFalse(into.parent.exists())

        src_license = self.dir / "msys2-share" / "RUNTIME.LIBRARY.EXCEPTION"
        src_license.parent.mkdir(parents=True)
        src_license.write_text("GCC Runtime Library Exception\n", encoding="utf-8")
        posix_license = "/ucrt64/share/licenses/gcc-libs/RUNTIME.LIBRARY.EXCEPTION"

        dll_path = self.dir / "mingw" / "bin" / "libstdc++-6.dll"
        dll_path.parent.mkdir(parents=True, exist_ok=True)
        dll_path.write_bytes(b"MZ")

        fake_run = self._make_fake_run(license_files={posix_license: src_license})
        with (
            mock.patch.object(package_mod, "LOG_DIR", self.log_dir),
            mock.patch.object(package_mod.Step, "run", fake_run),
        ):
            step = package_mod.Step("test", "msys2")
            try:
                package_mod.stage_msys2_licenses(step, into, [dll_path], allow_missing=False)
            finally:
                step.close()

        packages_txt = (into / "PACKAGES.txt").read_text(encoding="utf-8")
        self.assertIn("mingw-w64-ucrt-x86_64-gcc-libs:", packages_txt)
        self.assertIn("libstdc++-6.dll", packages_txt)
        self.assertIn("libstdc++.a (the C++ library, linked in)", packages_txt)
        self.assertIn("-> licenses/msys2/gcc-libs/", packages_txt)

        copied = into / "gcc-libs" / "RUNTIME.LIBRARY.EXCEPTION"
        self.assertTrue(copied.is_file())
        self.assertEqual(copied.read_text(encoding="utf-8"), "GCC Runtime Library Exception\n")

    def test_stage_msys2_licenses_missing_license_flag(self) -> None:
        fake_run = self._make_fake_run(empty_licenses=True)
        with (
            mock.patch.object(package_mod, "LOG_DIR", self.log_dir),
            mock.patch.object(package_mod.Step, "run", fake_run),
        ):
            into_fail = self.dir / "missing-fail" / "licenses" / "msys2"
            step_fail = package_mod.Step("test", "missing-fail")
            try:
                with self.assertRaises(package_mod.StepFailed) as ctx:
                    package_mod.stage_msys2_licenses(step_fail, into_fail, [], allow_missing=False)
            finally:
                step_fail.close()
            self.assertIn("mingw-w64-ucrt-x86_64-gcc-libs", str(ctx.exception))
            self.assertIn("--allow-missing-licenses", str(ctx.exception))

            into_ok = self.dir / "missing-ok" / "licenses" / "msys2"
            step_ok = package_mod.Step("test", "missing-ok")
            try:
                package_mod.stage_msys2_licenses(step_ok, into_ok, [], allow_missing=True)
            finally:
                step_ok.close()
            packages_txt = (into_ok / "PACKAGES.txt").read_text(encoding="utf-8")
            self.assertIn("NO LICENSE FILES", packages_txt)
            self.assertTrue(any("MISSING for" in note for note in step_ok.notes))

    def test_tools_build_only_windows_export_creates_licenses_and_binaries(self) -> None:
        build = self.dir / "build-win"
        (build / "CMakeFiles" / "3.30.0").mkdir(parents=True)
        (build / "CMakeFiles" / "3.30.0" / "CMakeCXXCompiler.cmake").write_text(
            'set(CMAKE_CXX_COMPILER_ID "GNU")\n',
            encoding="utf-8",
        )
        (build / "CMakeCache.txt").write_text("", encoding="utf-8")

        launcher = build / "tools" / "launch" / "vehicle_sil_launch.exe"
        launcher.parent.mkdir(parents=True)
        launcher.write_bytes(b"MZ-launcher")

        bridge = build / "bridges" / "ethernet" / "vehicle_sil_ethernet_bridge.exe"
        bridge.parent.mkdir(parents=True)
        bridge.write_bytes(b"MZ-bridge")

        grpc_src = build / "_deps" / "grpc-src"
        for rel in package_mod.GRPC_LICENSES:
            target = grpc_src / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"license text for {rel}\n", encoding="utf-8")

        mingw_bin = self.dir / "mingw" / "bin"
        mingw_bin.mkdir(parents=True)
        (mingw_bin / "gcc.exe").write_bytes(b"")

        src_license = self.dir / "msys2-share" / "COPYING.MinGW-w64-runtime.txt"
        src_license.parent.mkdir(parents=True)
        src_license.write_text("MinGW-w64 license\n", encoding="utf-8")
        posix_license = "/ucrt64/share/licenses/gcc-libs/COPYING.MinGW-w64-runtime.txt"

        build_only = self.dir / "bin-windows"
        fake_run = self._make_fake_run(license_files={posix_license: src_license})

        def fake_need(tool: str, why: str) -> str:
            return str(mingw_bin / f"{tool}.exe")

        args = argparse.Namespace(
            build_dir=str(build),
            no_build=False,
            build_only=str(build_only),
            prebuilt=None,
            web=str(self.dir / "unused-web"),
            client=str(self.dir / "unused-client.mjs"),
            client_package=None,
            out=str(self.dir / "dist"),
            jobs=1,
            cmake_arg=None,
            allow_missing_licenses=False,
            shared_runtime=False,
            tag="test-win-export",
        )

        stdout = io.StringIO()
        with (
            mock.patch.object(package_mod, "WINDOWS", True),
            mock.patch.object(package_mod, "PLATFORM", "windows-x86_64"),
            mock.patch.object(package_mod, "EXE", ".exe"),
            mock.patch.object(package_mod, "LAUNCHER", "vehicle_sil_launch.exe"),
            mock.patch.object(package_mod, "ETHERNET_BRIDGE", "vehicle_sil_ethernet_bridge.exe"),
            mock.patch.object(package_mod, "LOG_DIR", self.log_dir),
            mock.patch.object(package_mod, "need", fake_need),
            mock.patch.object(package_mod.Step, "run", fake_run),
            contextlib.redirect_stdout(stdout),
        ):
            rc = package_mod.tools_command(args)

        self.assertEqual(rc, 0, stdout.getvalue())
        self.assertTrue((build_only / "bin" / "vehicle_sil_launch.exe").is_file())
        self.assertTrue((build_only / "bin" / "vehicle_sil_ethernet_bridge.exe").is_file())
        self.assertEqual((build_only / "compiler.txt").read_text(encoding="ascii").strip(), "GNU")
        self.assertTrue((build_only / "licenses" / "msys2" / "PACKAGES.txt").is_file())
        self.assertEqual(
            (build_only / "licenses" / "msys2" / "gcc-libs" / "COPYING.MinGW-w64-runtime.txt").read_text(
                encoding="utf-8"
            ),
            "MinGW-w64 license\n",
        )
        for rel in package_mod.GRPC_LICENSES:
            self.assertTrue((build_only / "licenses" / "grpc" / rel).is_file(), f"missing grpc license {rel}")
        self.assertTrue((self.log_dir / "test-win-export-export.log").is_file())


class PackageVersionSyncTest(unittest.TestCase):
    VERSION_FILES = (
        "CMakeLists.txt",
        "CHANGELOG.md",
        "pyproject.toml",
        "gui/server/pyproject.toml",
        "python/pyproject.toml",
        "python/vehicle_sil_control/__init__.py",
        "gui/server/src/vehicle_sil_gui/__init__.py",
        "template/get_tools.py",
        "template/CMakeLists.txt",
        "examples/fetchcontent_consumer/CMakeLists.txt",
        "template/README.md",
        "template/README.ja.md",
    )

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def _copy_version_tree(self, *, with_uv_lock: bool = True) -> Path:
        tree = self.dir / "repo"
        names = [*self.VERSION_FILES]
        if with_uv_lock:
            names.append("uv.lock")
        for rel in names:
            dst = tree / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text((ROOT / rel).read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
        return tree

    def _set_cmake_version(self, tree: Path, base: str, prerelease: str = "") -> None:
        text = (
            f"cmake_minimum_required(VERSION 3.21)\n"
            f"project(\n  vehicle_sil\n  VERSION {base}\n  LANGUAGES C CXX)\n"
            f'set(VEHICLE_SIL_VERSION_PRERELEASE "{prerelease}")\n'
        )
        (tree / "CMakeLists.txt").write_text(text, encoding="utf-8", newline="\n")

    def test_pep440_version_conversions_and_rejection(self) -> None:
        self.assertEqual(package_mod.pep440_version("0.4.0"), "0.4.0")
        self.assertEqual(package_mod.pep440_version("0.4.0-rc.1"), "0.4.0rc1")
        self.assertEqual(package_mod.pep440_version("0.4.0-rc.10"), "0.4.0rc10")
        self.assertEqual(package_mod.pep440_version("1.2.3-beta.2"), "1.2.3b2")
        self.assertEqual(package_mod.pep440_version("1.2.3-alpha.0"), "1.2.3a0")
        with self.assertRaises(ValueError):
            package_mod.pep440_version("0.4.0-rc.01")

    def test_uv_lock_problems_checks_workspace_packages_without_uv(self) -> None:
        tree = self.dir / "lock-only"
        tree.mkdir()
        with mock.patch.object(package_mod, "ROOT", tree):
            self.assertEqual(package_mod.uv_lock_problems("0.4.0-rc.1"), [])
            (tree / "uv.lock").write_text(
                'version = 1\n\n[[package]]\nname = "vehicle-sil-dev"\nversion = "0.0.0"\n\n'
                '[[package]]\nname = "vehicle-sil-gui"\nversion = "0.4.0rc1"\n\n'
                '[[package]]\nname = "vehicle-sil-control"\nversion = "0.4.0rc1"\n',
                encoding="utf-8",
            )
            self.assertEqual(package_mod.uv_lock_problems("0.4.0-rc.1"), [])
            self.assertEqual(len(package_mod.uv_lock_problems("0.4.0")), 2)

            (tree / "uv.lock").write_text(
                'version = 1\n\n[[package]]\nname = "vehicle-sil-dev"\nversion = "0.0.0"\n\n'
                '[[package]]\nname = "vehicle-sil-control"\nversion = "0.4.0"\n',
                encoding="utf-8",
            )
            problems = package_mod.uv_lock_problems("0.4.0")
            self.assertEqual(len(problems), 1)
            self.assertIn("vehicle-sil-gui", problems[0])

    def test_sync_dry_run_does_not_modify_files_and_warns_on_changelog(self) -> None:
        tree = self._copy_version_tree(with_uv_lock=True)
        self._set_cmake_version(tree, "9.8.7", "rc.1")
        before = {rel: (tree / rel).read_text(encoding="utf-8") for rel in (*self.VERSION_FILES, "uv.lock")}

        out, err = io.StringIO(), io.StringIO()
        with (
            mock.patch.object(package_mod, "ROOT", tree),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            rc = package_mod.version_command(argparse.Namespace(sync=True, dry_run=True))

        self.assertEqual(rc, 0, err.getvalue())
        self.assertIn("9.8.7-rc.1", out.getvalue())
        self.assertNotIn("would update pyproject.toml", out.getvalue())
        self.assertIn("would update gui/server/pyproject.toml", out.getvalue())
        self.assertIn("would update python/pyproject.toml", out.getvalue())
        self.assertIn("would update examples/fetchcontent_consumer/CMakeLists.txt", out.getvalue())
        self.assertIn("would update template/README.ja.md", out.getvalue())
        self.assertIn("would update uv.lock (via uv lock --offline)", out.getvalue())
        self.assertIn("warning: CHANGELOG.md", err.getvalue())
        self.assertIn("not modified by --sync", err.getvalue())
        for rel, content in before.items():
            self.assertEqual((tree / rel).read_text(encoding="utf-8"), content, f"{rel} was modified during --dry-run")

    def test_sync_updates_derived_files_and_uv_lock_and_is_idempotent(self) -> None:
        tree = self._copy_version_tree(with_uv_lock=True)
        root_pyproject_before = (tree / "pyproject.toml").read_bytes()

        def fake_uv_run(cmd, cwd=None, capture_output=False, text=False):
            self.assertEqual(Path(cmd[0]).name, "uv")
            self.assertEqual(cmd[1:], ["lock", "--offline"])
            self.assertEqual(Path(cwd), tree)
            ver = package_mod.project_version()
            pep = package_mod.pep440_version(ver)
            lock_text = (
                f'version = 1\n\n[[package]]\nname = "vehicle-sil-dev"\nversion = "0.0.0"\n\n'
                f'[[package]]\nname = "vehicle-sil-gui"\nversion = "{pep}"\n\n'
                f'[[package]]\nname = "vehicle-sil-control"\nversion = "{pep}"\n'
            )
            (tree / "uv.lock").write_text(lock_text, encoding="utf-8", newline="\n")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        for base, pre, expected_ver in (("9.8.7", "rc.1", "9.8.7-rc.1"), ("9.8.7", "", "9.8.7")):
            changelog_before = (tree / "CHANGELOG.md").read_text(encoding="utf-8")
            self._set_cmake_version(tree, base, pre)
            out, err = io.StringIO(), io.StringIO()
            with (
                mock.patch.object(package_mod, "ROOT", tree),
                mock.patch.object(package_mod.shutil, "which", return_value="/usr/bin/uv"),
                mock.patch.object(package_mod.subprocess, "run", side_effect=fake_uv_run) as run_mock,
                contextlib.redirect_stdout(out),
                contextlib.redirect_stderr(err),
            ):
                rc = package_mod.version_command(argparse.Namespace(sync=True, dry_run=False))
                self.assertEqual(rc, 0, err.getvalue())
                self.assertEqual(run_mock.call_count, 1)
                self.assertEqual((tree / "CHANGELOG.md").read_text(encoding="utf-8"), changelog_before)
                self.assertEqual((tree / "pyproject.toml").read_bytes(), root_pyproject_before)
                self.assertIn("warning: CHANGELOG.md", err.getvalue())

                non_changelog = [p for p in package_mod.version_problems(expected_ver) if not p.startswith("CHANGELOG.md")]
                self.assertEqual(non_changelog, [])
                self.assertIn(
                    f'set(VEHICLE_SIL_GIT_TAG "v{expected_ver}"',
                    (tree / "examples" / "fetchcontent_consumer" / "CMakeLists.txt").read_text(encoding="utf-8"),
                )

                # Second run is a no-op and does not re-run uv lock.
                after_first = {rel: (tree / rel).read_bytes() for rel in (*self.VERSION_FILES, "uv.lock")}
                out2, err2 = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out2), contextlib.redirect_stderr(err2):
                    rc2 = package_mod.version_command(argparse.Namespace(sync=True, dry_run=False))
                self.assertEqual(rc2, 0, err2.getvalue())
                self.assertEqual(run_mock.call_count, 1)
                self.assertIn("already in sync", out2.getvalue())
                after_second = {rel: (tree / rel).read_bytes() for rel in (*self.VERSION_FILES, "uv.lock")}
                self.assertEqual(after_second, after_first)

            # Adding the heading makes read-only --check exit 0.
            (tree / "CHANGELOG.md").write_text(
                f"# Changelog\n\n## [{expected_ver}] - Unreleased\n\n- note\n",
                encoding="utf-8",
                newline="\n",
            )
            with mock.patch.object(package_mod, "ROOT", tree):
                out3, err3 = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out3), contextlib.redirect_stderr(err3):
                    rc3 = package_mod.version_command(argparse.Namespace(check=True))
                self.assertEqual(rc3, 0, err3.getvalue())
                self.assertEqual(out3.getvalue().strip(), expected_ver)

    def test_sync_missing_uv_fails_before_modifying_any_files_and_dry_run_still_succeeds(self) -> None:
        tree = self._copy_version_tree(with_uv_lock=True)
        self._set_cmake_version(tree, "9.8.7", "rc.1")
        tracked = (*self.VERSION_FILES, "uv.lock")
        before = {rel: (tree / rel).read_bytes() for rel in tracked}

        out, err = io.StringIO(), io.StringIO()
        with (
            mock.patch.object(package_mod, "ROOT", tree),
            mock.patch.object(package_mod.shutil, "which", return_value=None),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            dry_rc = package_mod.version_command(argparse.Namespace(sync=True, dry_run=True))
            self.assertEqual(dry_rc, 0, err.getvalue())
            self.assertIn("would update uv.lock", out.getvalue())
            for rel, content in before.items():
                self.assertEqual((tree / rel).read_bytes(), content, f"{rel} changed during --dry-run")

            with self.assertRaises(package_mod.Usage) as ctx:
                package_mod.version_command(argparse.Namespace(sync=True, dry_run=False))
            self.assertIn("uv not found", str(ctx.exception))

        for rel, content in before.items():
            self.assertEqual((tree / rel).read_bytes(), content, f"{rel} was modified when uv was missing")

    def test_sync_rolls_back_all_files_when_uv_lock_fails(self) -> None:
        tree = self._copy_version_tree(with_uv_lock=True)
        self._set_cmake_version(tree, "9.8.7", "rc.1")
        tracked = (*self.VERSION_FILES, "uv.lock")
        before = {rel: (tree / rel).read_bytes() for rel in tracked}

        def failing_uv(cmd, cwd=None, capture_output=False, text=False):
            # Simulate partial uv.lock mutation before failing.
            (Path(cwd) / "uv.lock").write_bytes(b"corrupted-partial-lock\n")
            return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="simulated offline failure")

        out, err = io.StringIO(), io.StringIO()
        with (
            mock.patch.object(package_mod, "ROOT", tree),
            mock.patch.object(package_mod.shutil, "which", return_value="/usr/bin/uv"),
            mock.patch.object(package_mod.subprocess, "run", side_effect=failing_uv),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            rc = package_mod.version_command(argparse.Namespace(sync=True, dry_run=False))

        self.assertEqual(rc, 1)
        self.assertIn("uv lock --offline failed: simulated offline failure", err.getvalue())
        for rel, content in before.items():
            self.assertEqual((tree / rel).read_bytes(), content, f"{rel} was not rolled back after uv failure")

    def test_sync_rolls_back_all_files_when_post_sync_check_fails(self) -> None:
        tree = self._copy_version_tree(with_uv_lock=True)
        self._set_cmake_version(tree, "9.8.7", "rc.1")
        tracked = (*self.VERSION_FILES, "uv.lock")
        before = {rel: (tree / rel).read_bytes() for rel in tracked}

        def noop_uv(cmd, cwd=None, capture_output=False, text=False):
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        out, err = io.StringIO(), io.StringIO()
        with (
            mock.patch.object(package_mod, "ROOT", tree),
            mock.patch.object(package_mod.shutil, "which", return_value="/usr/bin/uv"),
            mock.patch.object(package_mod.subprocess, "run", side_effect=noop_uv),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            rc = package_mod.version_command(argparse.Namespace(sync=True, dry_run=False))

        self.assertEqual(rc, 1)
        self.assertIn("uv.lock", err.getvalue())
        for rel, content in before.items():
            self.assertEqual((tree / rel).read_bytes(), content, f"{rel} was not rolled back on post-sync error")

    def test_sync_rollback_continues_when_one_restore_raises_oserror(self) -> None:
        tree = self._copy_version_tree(with_uv_lock=True)
        self._set_cmake_version(tree, "9.8.7", "rc.1")
        tracked = (*self.VERSION_FILES, "uv.lock")
        before = {rel: (tree / rel).read_bytes() for rel in tracked}
        fail_rel = "gui/server/pyproject.toml"
        fail_path = tree / fail_rel
        orig_write_bytes = Path.write_bytes

        def failing_uv(cmd, cwd=None, capture_output=False, text=False):
            (Path(cwd) / "uv.lock").write_bytes(b"corrupted-partial-lock\n")
            return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="simulated offline failure")

        def guarded_write_bytes(self_path: Path, data: bytes) -> int:
            if self_path == fail_path:
                raise OSError("simulated restore permission error")
            return orig_write_bytes(self_path, data)

        out, err = io.StringIO(), io.StringIO()
        with (
            mock.patch.object(package_mod, "ROOT", tree),
            mock.patch.object(package_mod.shutil, "which", return_value="/usr/bin/uv"),
            mock.patch.object(package_mod.subprocess, "run", side_effect=failing_uv),
            mock.patch.object(Path, "write_bytes", guarded_write_bytes),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            rc = package_mod.version_command(argparse.Namespace(sync=True, dry_run=False))

        self.assertEqual(rc, 1)
        stderr_text = err.getvalue()
        self.assertIn(f"warning: failed to restore {fail_path}: simulated restore permission error", stderr_text)
        self.assertIn("error: uv lock --offline failed: simulated offline failure", stderr_text)
        for rel, content in before.items():
            if rel == fail_rel:
                continue
            self.assertEqual((tree / rel).read_bytes(), content, f"{rel} was not restored after {fail_rel} failed")

    def test_version_flag_validation_and_cli_exit_code_2(self) -> None:
        with self.assertRaises(package_mod.Usage):
            package_mod.version_command(argparse.Namespace(dry_run=True))
        with self.assertRaises(package_mod.Usage):
            package_mod.version_command(argparse.Namespace(sync=True, expect="v1.0.0"))
        with self.assertRaises(package_mod.Usage):
            package_mod.version_command(argparse.Namespace(sync=True, check=True))

        for bad_args in (["--dry-run"], ["--sync", "--expect", "v1.0.0"], ["--sync", "--check"]):
            p = subprocess.run([sys.executable, str(PACKAGE), "version", *bad_args], capture_output=True, text=True)
            self.assertEqual(p.returncode, 2, (bad_args, p.stdout, p.stderr))
            self.assertIn("package: error:", p.stderr)

    def test_sync_with_real_uv_lock_rc_and_release_preserves_root_pyproject(self) -> None:
        import shutil
        import tomllib

        uv = shutil.which("uv")
        if uv is None:
            self.skipTest("uv is required for real uv lock --offline verification")

        tree = self._copy_version_tree(with_uv_lock=True)
        root_pyproject_before = (tree / "pyproject.toml").read_bytes()

        for base, pre, expected_ver, expected_pep in (
            ("0.7.0", "rc.1", "0.7.0-rc.1", "0.7.0rc1"),
            ("0.7.0", "", "0.7.0", "0.7.0"),
        ):
            self._set_cmake_version(tree, base, pre)
            out, err = io.StringIO(), io.StringIO()
            with (
                mock.patch.object(package_mod, "ROOT", tree),
                contextlib.redirect_stdout(out),
                contextlib.redirect_stderr(err),
            ):
                rc = package_mod.version_command(argparse.Namespace(sync=True, dry_run=False))
                self.assertEqual(rc, 0, err.getvalue())
                self.assertEqual((tree / "pyproject.toml").read_bytes(), root_pyproject_before)

                lock_data = tomllib.loads((tree / "uv.lock").read_text(encoding="utf-8"))
                pkgs = {p["name"]: p["version"] for p in lock_data["package"]}
                self.assertEqual(pkgs["vehicle-sil-dev"], "0.0.0")
                self.assertEqual(pkgs["vehicle-sil-gui"], expected_pep)
                self.assertEqual(pkgs["vehicle-sil-control"], expected_pep)

                check_lock = subprocess.run(
                    [uv, "lock", "--check", "--offline"], cwd=tree, capture_output=True, text=True
                )
                self.assertEqual(check_lock.returncode, 0, check_lock.stderr)

                # Re-running --sync is idempotent and leaves every file untouched.
                snapshot = {rel: (tree / rel).read_bytes() for rel in (*self.VERSION_FILES, "uv.lock")}
                out2, err2 = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out2), contextlib.redirect_stderr(err2):
                    rc2 = package_mod.version_command(argparse.Namespace(sync=True, dry_run=False))
                self.assertEqual(rc2, 0, err2.getvalue())
                self.assertIn("already in sync", out2.getvalue())
                self.assertEqual(
                    {rel: (tree / rel).read_bytes() for rel in (*self.VERSION_FILES, "uv.lock")},
                    snapshot,
                )

                # Release tag check passes with a dated CHANGELOG heading.
                (tree / "CHANGELOG.md").write_text(
                    f"# Changelog\n\n## [{expected_ver}] - 2026-11-29\n\n- change\n",
                    encoding="utf-8",
                    newline="\n",
                )
                out3, err3 = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out3), contextlib.redirect_stderr(err3):
                    rc3 = package_mod.version_command(argparse.Namespace(expect=f"v{expected_ver}"))
                self.assertEqual(rc3, 0, err3.getvalue())

                # Mutating root pyproject.toml does not fail version checks.
                (tree / "pyproject.toml").write_text(
                    root_pyproject_before.decode("utf-8").replace('version = "0.0.0"', 'version = "9.9.9"'),
                    encoding="utf-8",
                    newline="\n",
                )
                self.assertEqual(package_mod.version_problems(expected_ver, release=True), [])
                (tree / "pyproject.toml").write_bytes(root_pyproject_before)

                # Intentional mismatches fail and identify the mismatch.
                out4, err4 = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out4), contextlib.redirect_stderr(err4):
                    rc4 = package_mod.version_command(argparse.Namespace(expect="v9.9.9"))
                self.assertEqual(rc4, 1)
                self.assertIn("v9.9.9", err4.getvalue())

                saved_lock = (tree / "uv.lock").read_text(encoding="utf-8")
                import re

                drifted_lock, n_sub = re.subn(
                    r'(name = "vehicle-sil-gui"\s*\nversion = ")[^"]*(")',
                    r"\g<1>0.0.1\2",
                    saved_lock,
                    count=1,
                )
                self.assertEqual(n_sub, 1)
                (tree / "uv.lock").write_text(drifted_lock, encoding="utf-8", newline="\n")
                out5, err5 = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out5), contextlib.redirect_stderr(err5):
                    rc5 = package_mod.version_command(argparse.Namespace(expect=f"v{expected_ver}"))
                self.assertEqual(rc5, 1)
                self.assertIn("uv.lock", err5.getvalue())
                (tree / "uv.lock").write_text(saved_lock, encoding="utf-8", newline="\n")

    def test_uv_lock_problems_handles_invalid_utf8_and_non_list_package(self) -> None:
        tree = self.dir / "bad-lock"
        tree.mkdir()
        with mock.patch.object(package_mod, "ROOT", tree):
            (tree / "uv.lock").write_bytes(b"\xff\xfe\xfd")
            problems_utf8 = package_mod.uv_lock_problems("0.6.0")
            self.assertEqual(len(problems_utf8), 1)
            self.assertIn("uv.lock could not be parsed", problems_utf8[0])

            (tree / "uv.lock").write_text("package = 123\n", encoding="utf-8")
            problems_type = package_mod.uv_lock_problems("0.6.0")
            self.assertEqual(problems_type, ["uv.lock has non-list 'package' section"])

    def test_expect_empty_string_is_rejected(self) -> None:
        with self.assertRaises(package_mod.Usage):
            package_mod.version_command(argparse.Namespace(sync=True, expect=""))
        p = subprocess.run(
            [sys.executable, str(PACKAGE), "version", "--expect", ""],
            capture_output=True,
            text=True,
        )
        self.assertEqual(p.returncode, 1, (p.stdout, p.stderr))
        self.assertIn("the tag is ", p.stderr)

    def test_tools_archive_bundles_all_offline_gui_protocol_specs(self) -> None:
        protocol_dir = ROOT / "gui" / "protocol"
        on_disk = {"openapi.yaml", *(f"schemas/{p.name}" for p in sorted((protocol_dir / "schemas").glob("*.json")))}
        self.assertEqual(set(package_mod.PROTOCOL_SPECS), on_disk)
        for rel in package_mod.PROTOCOL_SPECS:
            self.assertTrue((protocol_dir / rel).is_file(), f"missing gui/protocol/{rel}")
        self.assertIn("| `protocol/` |", package_mod.README)
        source = PACKAGE.read_text(encoding="utf-8")
        self.assertIn('stage / "protocol" / rel', source)
        self.assertIn('f"protocol/{rel}" for rel in PROTOCOL_SPECS', source)


class ControlOnlyRequirementsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.log_dir = self.dir / "logs"
        self.addCleanup(self.tmp.cleanup)

    def _load_verify_python_module(self):
        script = ROOT / "scripts" / "verify-python.py"
        spec = importlib.util.spec_from_file_location("vehicle_sil_test_verify_python", script)
        module = importlib.util.module_from_spec(spec)
        sys.path.insert(0, str(script.parent))
        try:
            spec.loader.exec_module(module)
        finally:
            sys.path.remove(str(script.parent))
        return module

    def test_python_pyproject_keeps_zero_dependencies_and_declares_grpc_extra(self) -> None:
        import tomllib

        data = tomllib.loads((ROOT / "python" / "pyproject.toml").read_text(encoding="utf-8"))
        project = data["project"]
        self.assertEqual(project["dependencies"], [])
        grpc_extra = project["optional-dependencies"]["grpc"]
        self.assertTrue(any(dep.startswith("grpcio>=") for dep in grpc_extra), grpc_extra)
        self.assertTrue(any(dep.startswith("protobuf>=") for dep in grpc_extra), grpc_extra)

    def test_verify_python_lower_bounds_includes_python_optional_dependencies(self) -> None:
        vp = self._load_verify_python_module()
        bounds = vp.lower_bounds()
        self.assertIn("grpcio", bounds)
        self.assertIn("protobuf", bounds)

        # Conflicting lower bound in python/pyproject.toml is caught by lower_bounds().
        fake_root = self.dir / "repo"
        (fake_root / "python").mkdir(parents=True)
        (fake_root / "pyproject.toml").write_text(
            '[dependency-groups]\ncontrol = ["grpcio>=1.85.0", "protobuf>=7.35.1"]\n',
            encoding="utf-8",
        )
        (fake_root / "python" / "pyproject.toml").write_text(
            '[project]\nname = "vehicle-sil-control"\ndependencies = []\n'
            '[project.optional-dependencies]\ngrpc = ["grpcio>=1.84.0", "protobuf>=7.35.1"]\n',
            encoding="utf-8",
        )
        with mock.patch.object(vp, "ROOT", fake_root):
            with self.assertRaises(ValueError) as ctx:
                vp.lower_bounds()
        self.assertIn("conflicting lower bounds for grpcio", str(ctx.exception))
        self.assertIn("python/pyproject.toml", str(ctx.exception))

        # A package declared only in python/pyproject.toml optional-dependencies is extracted.
        (fake_root / "python" / "pyproject.toml").write_text(
            '[project]\nname = "vehicle-sil-control"\ndependencies = []\n'
            '[project.optional-dependencies]\ngrpc = ["grpcio>=1.85.0", "protobuf>=7.35.1", "extra-only>=2.0.0"]\n',
            encoding="utf-8",
        )
        with mock.patch.object(vp, "ROOT", fake_root):
            extracted = vp.lower_bounds()
        self.assertEqual(extracted["extra-only"], "2.0.0")

    def _run_prebuilt_tools(self, *, windows: bool) -> tuple[Path, list[list[str]]]:
        version = package_mod.project_version()
        suffix = "win" if windows else "linux"
        exe = ".exe" if windows else ""
        platform = "windows-x86_64" if windows else "linux-x86_64"
        build = self.dir / f"build-pkg-{suffix}"
        prebuilt = self.dir / f"prebuilt-{suffix}"
        (prebuilt / "bin").mkdir(parents=True)
        (prebuilt / "bin" / f"vehicle_sil_launch{exe}").write_bytes(b"MZ-launch" if windows else b"ELF-launch")
        (prebuilt / "bin" / f"vehicle_sil_ethernet_bridge{exe}").write_bytes(b"MZ-eth" if windows else b"ELF-eth")
        if not windows:
            (prebuilt / "bin" / "vehicle_sil_socketcan_bridge").write_bytes(b"ELF-can")
        else:
            (prebuilt / "licenses" / "msys2").mkdir(parents=True)
            (prebuilt / "licenses" / "msys2" / "PACKAGES.txt").write_text("gcc-libs\n", encoding="utf-8")
        (prebuilt / "compiler.txt").write_text("GNU\n", encoding="ascii")
        for rel in package_mod.GRPC_LICENSES:
            p = prebuilt / "licenses" / "grpc" / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(f"license {rel}\n", encoding="utf-8")

        web_parent = self.dir / f"web-dist-{suffix}"
        web = web_parent / "browser"
        web.mkdir(parents=True)
        (web / "index.html").write_text("<app-root></app-root>\n", encoding="utf-8")
        (web_parent / "3rdpartylicenses.txt").write_text("MIT\n", encoding="utf-8")

        client_dir = self.dir / f"client-{suffix}"
        client_dir.mkdir(parents=True)
        client = client_dir / "vehicle-sil-gui-client.mjs"
        client.write_text("export const createClient = () => {};\n", encoding="utf-8")
        (client_dir / "vehicle-sil-gui-elements.mjs").write_text("export {};\n", encoding="utf-8")
        client_pkg = client_dir / f"vehicle-sil-gui-client-{version}.tgz"
        client_pkg.write_bytes(b"tgz")

        recorded: list[list[str]] = []

        def fake_run(step_self, command, cwd=None, env=None, check=True, capture=False, timeout=None):
            cmd = [str(c) for c in command]
            recorded.append(cmd)
            prog = Path(cmd[0]).name
            stdout = ""
            if prog == "uv" and len(cmd) >= 2 and cmd[1] == "export":
                if "--package" in cmd and "vehicle-sil-control" in cmd:
                    stdout = "grpcio==1.85.1 \\\n    --hash=sha256:aaa\nprotobuf==7.36.2 \\\n    --hash=sha256:bbb\n"
                elif "--package" in cmd and "vehicle-sil-gui" in cmd:
                    stdout = "aiohttp==3.14.5 \\\n    --hash=sha256:ccc\ngrpcio==1.85.1 \\\n    --hash=sha256:aaa\n"
                else:
                    stdout = "grpcio-tools==1.85.1 \\\n    --hash=sha256:ddd\n"
            elif prog == "uv" and len(cmd) >= 2 and cmd[1] == "run":
                out_idx = cmd.index("--out") + 1
                gen_dir = Path(cmd[out_idx]) / "vehicle_sil" / "control" / "v1"
                gen_dir.mkdir(parents=True, exist_ok=True)
                (gen_dir / "control_pb2.py").write_text("# stub\n", encoding="utf-8")
            elif prog == f"vehicle_sil_launch{exe}" and "version" in cmd:
                stdout = f'{{"vehicle_sil": "{version}", "features": ["control-server"]}}\n'
            elif prog == "readelf":
                stdout = " 0x0000000000000001 (NEEDED) Shared library: [libc.so.6]\n"
            elif prog == "objdump":
                if windows:
                    stdout = "        DLL Name: api-ms-win-crt-runtime-l1-1-0.dll\n"
                else:
                    stdout = "0000000000000000 DF *UND* 0000000000000000 GLIBC_2.34 main\n"
            else:
                raise AssertionError(f"unexpected command in stubbed Step.run: {cmd}")
            return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr="")

        args = argparse.Namespace(
            build_dir=str(build),
            no_build=False,
            build_only=None,
            prebuilt=str(prebuilt),
            web=str(web),
            client=str(client),
            client_package=str(client_pkg),
            out=str(self.dir / f"dist-{suffix}"),
            jobs=1,
            cmake_arg=None,
            allow_missing_licenses=False,
            shared_runtime=False,
            tag=f"test-control-reqs-{suffix}",
        )

        stdout = io.StringIO()
        with (
            mock.patch.object(package_mod, "WINDOWS", windows),
            mock.patch.object(package_mod, "PLATFORM", platform),
            mock.patch.object(package_mod, "EXE", exe),
            mock.patch.object(package_mod, "LAUNCHER", f"vehicle_sil_launch{exe}"),
            mock.patch.object(package_mod, "ETHERNET_BRIDGE", f"vehicle_sil_ethernet_bridge{exe}"),
            mock.patch.object(package_mod, "LOG_DIR", self.log_dir),
            mock.patch.object(package_mod, "need", lambda tool, why: f"/usr/bin/{tool}"),
            mock.patch.object(package_mod, "check_bridge_alone", lambda s, b, v: "eth ok"),
            mock.patch.object(package_mod, "check_socketcan_bridge_alone", lambda s, b, v: "can ok"),
            mock.patch.object(package_mod, "stage_licenses", lambda *a, **kw: None),
            mock.patch.object(package_mod.Step, "run", fake_run),
            contextlib.redirect_stdout(stdout),
        ):
            top, _ = package_mod.names(version)
            rc = package_mod.tools_command(args)

        self.assertEqual(rc, 0, stdout.getvalue())
        return build / "stage" / top, recorded

    def test_tools_prebuilt_exports_stages_control_requirements_and_renders_readme(self) -> None:
        stage, recorded = self._run_prebuilt_tools(windows=False)
        self.assertTrue(
            any(
                Path(cmd[0]).name == "uv"
                and "export" in cmd
                and "--package" in cmd
                and "vehicle-sil-control" in cmd
                and "--extra" in cmd
                and "grpc" in cmd
                for cmd in recorded
            ),
            recorded,
        )

        gui_reqs = (stage / "python" / "requirements.txt").read_text(encoding="utf-8")
        ctl_reqs = (stage / "python" / "requirements-control.txt").read_text(encoding="utf-8")
        self.assertIn("aiohttp==3.14.5", gui_reqs)
        self.assertIn("grpcio==1.85.1", ctl_reqs)
        self.assertIn("protobuf==7.36.2", ctl_reqs)
        self.assertNotIn("aiohttp", ctl_reqs)

        readme = (stage / "README.md").read_text(encoding="utf-8")
        self.assertIn("requirements-control.txt", readme)
        self.assertIn("PYTHONPATH=<tools>/python uv run --no-project --no-build", readme)
        self.assertIn(f"--python '{package_mod.python_versions()}'", readme)
        self.assertIn("PYTHONPATH=<tools>/python python3 -S your_launcher_script.py", readme)
        self.assertNotIn('set "PYTHONPATH=', readme)
        self.assertIn("Launch", readme)
        self.assertIn("Serve", readme)
        self.assertIn("control-metrics.schema.json", readme)
        staged_cm = stage / "schema" / "control-metrics.schema.json"
        self.assertTrue(staged_cm.is_file(), "control-metrics.schema.json should be staged in <tools>/schema/")
        self.assertEqual(
            staged_cm.read_text(encoding="utf-8"),
            (ROOT / "schema" / "control-metrics.schema.json").read_text(encoding="utf-8"),
        )
        self.assertFalse(
            (ROOT / "template" / "schema" / "control-metrics.schema.json").exists(),
            "control-metrics.schema.json is a launcher output schema and must not be in template/schema/",
        )

    def test_tools_prebuilt_windows_renders_cmd_python_commands(self) -> None:
        stage, _ = self._run_prebuilt_tools(windows=True)
        readme = (stage / "README.md").read_text(encoding="utf-8")
        py_ver = package_mod.python_versions()
        self.assertIn('set "PYTHONPATH=<tools>\\python"', readme)
        self.assertIn(
            f'uv run --no-project --no-build --managed-python --python "{py_ver}" '
            r"--with-requirements <tools>\python\requirements-control.txt python -P your_script.py",
            readme,
        )
        self.assertIn("python -S your_launcher_script.py", readme)
        self.assertNotIn("python3 -S", readme)
        self.assertNotIn(f"--python '{py_ver}'", readme)
        self.assertNotIn("PYTHONPATH=<tools>/python uv run", readme)

    def test_verify_command_checks_control_requirements_and_stdlib_import(self) -> None:
        source = PACKAGE.read_text(encoding="utf-8")
        self.assertIn('"python/requirements-control.txt"', source)
        self.assertIn('"schema/control-metrics.schema.json"', source)
        self.assertIn('tools / "python" / "requirements-control.txt"', source)
        self.assertIn('sys.executable, "-S", "-P", "-c"', source)
        self.assertIn("from vehicle_sil_control import Serve, RunHandle, ServeError, Launch, LaunchError, control_address", source)
        self.assertIn('for gui_only in ("aiohttp", "pydantic", "pyyaml"):', source)

    def test_gui_wrappers_enable_utf8_mode_and_clear_cmd_errorlevel(self) -> None:
        self.assertTrue(package_mod.SH_WRAPPER.isascii())
        self.assertTrue(package_mod.CMD_WRAPPER.isascii())
        self.assertIn('python -X utf8 -P -m vehicle_sil_gui "$@"', package_mod.SH_WRAPPER)
        self.assertIn('"$python" -X utf8 -P -c \'import aiohttp, pydantic, yaml\'', package_mod.SH_WRAPPER)
        self.assertIn('exec "$python" -X utf8 -P -m vehicle_sil_gui "$@"', package_mod.SH_WRAPPER)

        cmd_lines = package_mod.CMD_WRAPPER.splitlines()
        setlocal_idx = cmd_lines.index("setlocal")
        self.assertEqual(cmd_lines[setlocal_idx + 1], 'set "ERRORLEVEL="')
        self.assertIn("python -X utf8 -P -m vehicle_sil_gui %*", package_mod.CMD_WRAPPER)
        self.assertIn('"%VEHICLE_SIL_PYTHON%" -X utf8 -P -c "import aiohttp, pydantic, yaml"', package_mod.CMD_WRAPPER)
        self.assertIn('"%VEHICLE_SIL_PYTHON%" -X utf8 -P -m vehicle_sil_gui %*', package_mod.CMD_WRAPPER)

        stage_win, _ = self._run_prebuilt_tools(windows=True)
        staged_cmd = (stage_win / "bin" / "vehicle-sil-gui.cmd").read_bytes()
        self.assertIn(b'set "ERRORLEVEL="\r\n', staged_cmd)
        self.assertIn(b"python -X utf8 -P -m vehicle_sil_gui %*\r\n", staged_cmd)
        staged_sh = (stage_win / "bin" / "vehicle-sil-gui").read_text(encoding="ascii")
        self.assertIn('python -X utf8 -P -m vehicle_sil_gui "$@"', staged_sh)


class PackageVerifyChecksTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.log_dir = self.dir / "logs"
        self.addCleanup(self.tmp.cleanup)

    def _make_tgz(self, path: Path, entries: dict[str, bytes] | None = None, *, dir_member: str | None = None) -> None:
        import tarfile

        with tarfile.open(path, "w:gz") as tar:
            if dir_member is not None:
                info = tarfile.TarInfo(dir_member)
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                tar.addfile(info)
            for name, data in (entries or {}).items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = 0o644
                tar.addfile(info, io.BytesIO(data))

    def test_check_client_package_manifest_accepts_valid_and_rejects_malformed_with_step_failed(self) -> None:
        import json

        for ver in ("0.7.0-rc.1", "0.7.0"):
            valid = self.dir / f"vehicle-sil-gui-client-{ver}.tgz"
            self._make_tgz(valid, {"package/package.json": json.dumps({"name": "@vehicle-sil/gui-client", "version": ver}).encode()})
            package_mod.check_client_package_manifest(valid, ver)

        bad_cases: list[tuple[str, bytes | None, str | None]] = [
            ("missing_manifest", None, None),
            ("directory_manifest", None, "package/package.json"),
            ("non_dict_list", b"[]", None),
            ("non_dict_null", b"null", None),
            ("non_dict_str", b'"0.7.0"', None),
            ("non_dict_int", b"123", None),
            ("invalid_json", b"{not-json", None),
            ("invalid_utf8", b"\xff\xfe", None),
            ("wrong_name", json.dumps({"name": "other", "version": "0.7.0-rc.1"}).encode(), None),
            ("wrong_version", json.dumps({"name": "@vehicle-sil/gui-client", "version": "0.6.0"}).encode(), None),
            ("pep440_version", json.dumps({"name": "@vehicle-sil/gui-client", "version": "0.7.0rc1"}).encode(), None),
        ]
        for label, payload, dir_member in bad_cases:
            with self.subTest(label=label):
                tgz = self.dir / f"{label}.tgz"
                entries = {"package/package.json": payload} if payload is not None else {}
                self._make_tgz(tgz, entries, dir_member=dir_member)
                with self.assertRaises(package_mod.StepFailed):
                    package_mod.check_client_package_manifest(tgz, "0.7.0-rc.1")

        # Truncated and corrupt gzip payloads also raise StepFailed rather than EOFError/zlib.error.
        valid_tgz = self.dir / "valid-src.tgz"
        self._make_tgz(
            valid_tgz,
            {"package/package.json": json.dumps({"name": "@vehicle-sil/gui-client", "version": "0.7.0-rc.1"}).encode()},
        )
        raw = valid_tgz.read_bytes()
        truncated = self.dir / "truncated.tgz"
        truncated.write_bytes(raw[: max(10, len(raw) // 2)])
        with self.assertRaises(package_mod.StepFailed):
            package_mod.check_client_package_manifest(truncated, "0.7.0-rc.1")

        corrupt = self.dir / "corrupt.tgz"
        corrupt.write_bytes(b"not a gzip file")
        with self.assertRaises(package_mod.StepFailed):
            package_mod.check_client_package_manifest(corrupt, "0.7.0-rc.1")

    def test_verify_gui_checks_api_app_version_and_json_errors(self) -> None:
        import http.server
        import json
        import threading

        tools = self.dir / "tools"
        project = self.dir / "project"
        tools.mkdir()
        project.mkdir()
        starter = tools / "gui_starter"
        starter.mkdir()

        response_holder: dict[str, tuple[int, bytes]] = {
            "app": (200, json.dumps({"version": "0.7.0-rc.1", "launcher": {"usable": True}}).encode()),
        }

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802
                if self.path == "/":
                    body = b'<script src="main.js"></script><script src="/vsil/vehicle-sil-gui-elements.mjs"></script>'
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(body)
                elif self.path == "/vsil/vehicle-sil-gui-client.mjs":
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(b"export { createClient, takeToken };")
                elif self.path == "/vsil/vehicle-sil-gui-elements.mjs":
                    self.send_response(200)
                    self.end_headers()
                    self.wfile.write(b"vsil-run-controls")
                elif self.path == "/api/app":
                    code, body = response_holder["app"]
                    self.send_response(code)
                    self.end_headers()
                    self.wfile.write(body)
                else:
                    self.send_response(404)
                    self.end_headers()

            def log_message(self, format, *args):  # noqa: A002
                return

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        port = server.server_address[1]
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.shutdown)
        self.addCleanup(server.server_close)

        class FakeProc:
            def __init__(self):
                self.pid = 999999
                ready = json.dumps({"vehicle_sil_gui": 4, "port": port, "url": f"http://127.0.0.1:{port}/#token=t"})
                self.stdout = io.StringIO(ready + "\n")

            def poll(self):
                return 0

            def wait(self, timeout=None):
                return 0

        with (
            mock.patch.object(package_mod, "LOG_DIR", self.log_dir),
            mock.patch.object(package_mod.subprocess, "Popen", side_effect=lambda *a, **kw: FakeProc()),
        ):
            # Happy path with --web
            step_ok = package_mod.Step("test", "gui-ok")
            try:
                package_mod.verify_gui(step_ok, tools, project, ["vehicle-sil-gui"], "0.7.0-rc.1", web=starter)
            finally:
                step_ok.close()

            # Failure modes for both --web and default modes
            bad_payloads = [
                ("version_mismatch", json.dumps({"version": "0.6.0", "launcher": {"usable": True}}).encode()),
                ("version_missing", json.dumps({"launcher": {"usable": True}}).encode()),
                ("non_dict_json", b"[]"),
                ("invalid_json", b"{bad"),
            ]
            for label, payload in bad_payloads:
                response_holder["app"] = (200, payload)
                for web_arg in (starter, None):
                    with self.subTest(label=label, web=web_arg is not None):
                        step_bad = package_mod.Step("test", f"gui-{label}")
                        try:
                            with self.assertRaises(package_mod.StepFailed):
                                package_mod.verify_gui(
                                    step_bad, tools, project, ["vehicle-sil-gui"], "0.7.0-rc.1", web=web_arg
                                )
                        finally:
                            step_bad.close()


if __name__ == "__main__":
    unittest.main()
