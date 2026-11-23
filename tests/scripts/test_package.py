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


if __name__ == "__main__":
    unittest.main()
