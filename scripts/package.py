#!/usr/bin/env python3
"""Make the prebuilt tools archive of vehicle-sil, check it as a user would use it, and make the template archive.

Commands:

  tools     the tools archive of this platform, build/dist/vehicle-sil-tools-<version>-<platform>.tar.gz (Linux) or .zip
            (Windows), and its .sha256. Steps:
              version    the version agrees everywhere (see the command version)
              configure  a Release build of vehicle_sil_launch with its control server (gRPC) in build/package-<platform>,
                         with VEHICLE_SIL_STATIC=ON: SIL Kit, the C++ runtime and gRPC (built here at the pinned
                         version, never a system one) linked into the launcher
              build      cmake --build --target vehicle_sil_launch
              compiler   the build was made with GCC (MinGW-w64 included): the archive this repository hands out is
                         not made with another compiler (anyone may build so for themselves)
              python     python/requirements.txt (uv export of the GUI server with gRPC, pinned with hashes from uv.lock)
                         and the gRPC stubs of the Control API (uv run ... python -m vehicle_sil_control.generate)
              stage      bin/ (the launcher, the libraries it needs, vehicle-sil-gui), python/ (vehicle_sil_gui with the
                         built screen, vehicle_sil_control with the stubs), client/ (the bundled client library and
                         its npm package), gui_starter/ (examples/gui_starter with the bundled library: a screen of
                         your own, served with vehicle-sil-gui --web),
                         schema/, LICENSE, THIRD_PARTY_NOTICES.md, licenses/ (the license texts of what is inside),
                         README.md, VERSION. Windows: every DLL the executables load that is not Windows' own, found
                         with objdump, and the MSYS2 license files of their packages
              portable   the staged launcher runs with a PATH of the system's directories only, is this version, and
                         its version --json lists control-server. Linux: the executables need only the C library (no libstdc++, no other .so); prints the newest
                         glibc symbol version they use. Windows: every DLL they load is in bin/ or Windows' own
              archive    the archive and its .sha256
            The screen and the client library are built by scripts/verify-web.py (or in gui/web: npm ci, npm run build,
            npm run build:client, npm run pack:client); --web, --client and --client-package name them.
  verify    checks a tools archive as a user uses it, in a new empty directory: copies template/ there, unpacks the
            archive with the template's get_tools.py (--archive, its SHA-256), runs the launcher with a PATH of the
            system's directories only (nothing else may be needed), builds the template against this working tree
            (FETCHCONTENT_SOURCE_DIR_VEHICLE_SIL: no network for vehicle-sil), runs check, run and run --scenario with
            the archive's launcher, runs examples/launch_control/write_before_start.py with the archive's Python client
            against `run --control` (gRPC: write before the start, step, read back), and starts the archive's
            vehicle-sil-gui (its Python packages: uv run, which may download them; on Windows both the .cmd and the
            sh entry) and asks it: the launcher usable, the variables available, the built screen, a check of
            simulation.yaml; then starts it with --web gui_starter (the archive's starter screen) and gets its page
            and its client library, and api/app with the token. Keeps the directory when a step fails (--keep:
            always).
  template  build/dist/vehicle-sil-template-<version>.zip: template/ with the SHA-256 of both tools archives there
            filled into get_tools.py, and build/dist/SHA256SUMS of the archives and of the client library's npm
            package build/dist/vehicle-sil-gui-client-<version>.tgz (put there from build/gui-client), which the
            release carries on its own (npm install <its URL>).
  version   prints the version and checks that every place that names it agrees: CMakeLists.txt (project VERSION, the
            source), pyproject.toml, gui/server/pyproject.toml, template/get_tools.py (VERSION), template/CMakeLists.txt
            (GIT_TAG v<version>), CHANGELOG.md (a heading "## [<version>] - Unreleased" or "## [<version>] -
            YYYY-MM-DD"). --expect v<version> also checks a tag, and that the heading has the release date.

The full output of each step goes to build/logs/<tag>-<step>.log; the summary shows one line per step and, for a
failed step, its first error lines and its log. Stops at the first failed step.

Exit codes: 0 done, 1 a step failed, 2 usage error (a missing tool or input). Requires Python 3.11+ (standard
library only), CMake, Ninja, a C++ compiler, uv, and on Windows the MSYS2 UCRT64 environment (gcc, objdump, pacman).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import queue
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zipfile
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / "build" / "logs"
DIST = ROOT / "build" / "dist"
WINDOWS = sys.platform == "win32"
PLATFORM = "windows-x86_64" if WINDOWS else "linux-x86_64"
EXE = ".exe" if WINDOWS else ""
LINE_CHARS = 300
MAX_ERROR_LINES = 12
ERROR = re.compile(r"\berror:|Error|FAILED|fatal|not found|cannot |Traceback|No such file")

# The license files of the gRPC source tree that FetchContent builds (Linux): gRPC and what it compiles in.
GRPC_LICENSES = [
    "LICENSE", "NOTICE.txt",
    "third_party/abseil-cpp/LICENSE",
    "third_party/protobuf/LICENSE",
    "third_party/protobuf/third_party/utf8_range/LICENSE",
    "third_party/utf8_range/LICENSE",
    "third_party/re2/LICENSE",
    "third_party/cares/cares/LICENSE.md",
    "third_party/boringssl-with-bazel/LICENSE",
    "third_party/boringssl-with-bazel/src/third_party/fiat/LICENSE",
    "third_party/zlib/LICENSE",
    "third_party/address_sorting/LICENSE",
    "third_party/xxhash/LICENSE",
]
# SIL Kit and the code it compiles in (external/sil-kit/ThirdParty/LICENSES.rst lists them).
SILKIT_LICENSES = [
    "LICENSE",
    "ThirdParty/LICENSES.rst",
    "ThirdParty/asio/asio/LICENSE_1_0.txt",
    "ThirdParty/fmt/LICENSE",
    "ThirdParty/spdlog/LICENSE",
]
# rapidyaml is one amalgamated header; the notices of what it holds (rapidyaml, c4core, fast_float, debugbreak, ...)
# are comments in it.
RAPIDYAML = "ThirdParty/rapidyaml/rapidyaml.hpp"
# What a Linux executable of the archive may need besides the libraries in bin/.
LINUX_SYSTEM_LIBRARIES = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
                          "ld-linux-x86-64.so.2"}


class Usage(Exception):
    """A usage error: a missing tool or input (exit code 2)."""


class StepFailed(Exception):
    pass


# --- steps and logs ---------------------------------------------------------------------------------------------


class Step:
    """One step: its log, the commands it runs (output to the log), notes for the summary."""

    def __init__(self, tag: str, name: str):
        self.name = name
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        self.log = LOG_DIR / f"{tag}-{name}.log"
        self.file = self.log.open("w", encoding="utf-8", errors="backslashreplace")
        self.notes: list[str] = []

    def write(self, text: str) -> None:
        self.file.write(text if text.endswith("\n") else text + "\n")
        self.file.flush()

    def run(self, command: list[str], cwd: Path | None = None, env: dict | None = None, check: bool = True,
            capture: bool = False, timeout: float | None = None) -> subprocess.CompletedProcess:
        self.write(f"$ {' '.join(str(c) for c in command)}" + (f"   (in {cwd})" if cwd else ""))
        try:
            p = subprocess.run([str(c) for c in command], cwd=cwd, env=env, timeout=timeout, text=True,
                               encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE if capture else self.file, stderr=subprocess.STDOUT)
        except FileNotFoundError:
            raise StepFailed(f"{command[0]} not found: install it, or put it on PATH")
        except subprocess.TimeoutExpired:
            raise StepFailed(f"{Path(str(command[0])).name} did not end within {timeout:.0f} s")
        if capture and p.stdout:
            self.write(p.stdout)
        if check and p.returncode != 0:
            raise StepFailed(f"{Path(str(command[0])).name} {' '.join(str(c) for c in command[1:3])} ... ended with "
                             f"exit code {p.returncode}")
        return p

    def close(self) -> None:
        self.file.close()


def first_errors(log: Path) -> list[str]:
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    picked = [line for line in lines if ERROR.search(line)] or lines[-MAX_ERROR_LINES:]
    return [line[:LINE_CHARS] for line in picked[:MAX_ERROR_LINES]]


def run_steps(tag: str, steps: list[tuple[str, object]]) -> bool:
    for name, body in steps:
        step = Step(tag, name)
        start = time.monotonic()
        try:
            body(step)
            ok, why = True, ""
        except StepFailed as e:
            ok, why = False, str(e)
            step.write(f"FAILED: {why}")
        except Usage:
            step.close()
            raise
        finally:
            if not step.file.closed:
                step.close()
        seconds = time.monotonic() - start
        if ok:
            note = f" ({'; '.join(step.notes)})" if step.notes else ""
            print(f"  {name:<10} ok, {seconds:.0f} s{note}", flush=True)
            continue
        print(f"  {name:<10} FAILED after {seconds:.0f} s: {why}", flush=True)
        for line in first_errors(step.log):
            print(f"    {line}")
        print(f"    log: {step.log.relative_to(ROOT) if step.log.is_relative_to(ROOT) else step.log}")
        return False
    return True


# --- version --------------------------------------------------------------------------------------------------


def project_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\(\s*vehicle_sil\s+VERSION\s+(\d+\.\d+\.\d+)", text)
    if not m:
        raise Usage("CMakeLists.txt has no project(vehicle_sil VERSION x.y.z): the version comes from there")
    return m.group(1)


def version_problems(version: str, release: bool = False) -> list[str]:
    """Where the version disagrees with CMakeLists.txt, and what to change. `release`: for the tag being made."""
    checks = [
        ("pyproject.toml", r'(?m)^version = "([^"]*)"', '{}'),
        ("gui/server/pyproject.toml", r'(?m)^version = "([^"]*)"', '{}'),
        ("template/get_tools.py", r'(?m)^VERSION = "([^"]*)"', '{}'),
        ("template/CMakeLists.txt", r"GIT_TAG\s+(\S+)", 'v{}'),
    ]
    problems = []
    for name, pattern, form in checks:
        path = ROOT / name
        m = re.search(pattern, path.read_text(encoding="utf-8")) if path.is_file() else None
        want = form.format(version)
        if not m:
            problems.append(f"{name}: no version found (expected {want})")
        elif m.group(1) != want:
            problems.append(f"{name} says {m.group(1)}, CMakeLists.txt says {version}: make it {want}")
    readme = (ROOT / "template" / "README.md").read_text(encoding="utf-8")
    others = sorted(set(re.findall(r"(?<![\d.])\d+\.\d+\.\d+(?![\d.])", readme)) - {version})
    if others:
        problems.append(f"template/README.md names version {', '.join(others)}, CMakeLists.txt says {version}: make "
                        f"it {version}")
    changelog = ROOT / "CHANGELOG.md"
    text = changelog.read_text(encoding="utf-8") if changelog.is_file() else ""
    problems += changelog_problems(text, version, release)
    return problems


def changelog_problems(text: str, version: str, release: bool = False) -> list[str]:
    """What is wrong with the heading of `version` in CHANGELOG.md (Keep a Changelog: "## [0.1.0] - 2026-01-31", or
    "## [0.1.0] - Unreleased" before the release). `release`: the tag is being made, so the heading needs the date."""
    v = re.escape(version)
    want = f"\"## [{version}] - Unreleased\" (\"## [{version}] - YYYY-MM-DD\" in the tagged commit)"
    m = re.search(rf"(?m)^## \[{v}\] - (Unreleased|\d{{4}}-\d{{2}}-\d{{2}})[ \t]*$", text)
    if m is None:
        old = re.search(rf"(?m)^##[ \t]+\[?v?{v}(?![\d.]).*$", text)
        if old:
            return [f"CHANGELOG.md has the heading \"{old.group(0).strip()}\", which is not in the form of Keep a "
                    f"Changelog: write it as {want}"]
        return [f"CHANGELOG.md has no heading {want}: add the changes of {version} under it"]
    if m.group(1) == "Unreleased":
        if release:
            return [f"CHANGELOG.md says \"## [{version}] - Unreleased\": write the release date in its place "
                    f"(\"## [{version}] - YYYY-MM-DD\") in the commit that is tagged v{version}"]
        return []
    try:
        date.fromisoformat(m.group(1))
    except ValueError:
        return [f"CHANGELOG.md: the date in \"{m.group(0).strip()}\" is not a date: write it as YYYY-MM-DD"]
    return []


def check_version(step: Step) -> str:
    version = project_version()
    problems = version_problems(version)
    for p in problems:
        step.write(f"error: {p}")
    if problems:
        raise StepFailed(problems[0] + (f" (and {len(problems) - 1} more)" if len(problems) > 1 else ""))
    step.notes.append(version)
    return version


# Windows: uv's own CPython, never MSYS2's MinGW Python (PyPI has no wheels of the packages for it).
UV_MANAGED = ["--managed-python"] if WINDOWS else []


def python_versions() -> str:
    """The Pythons the pinned packages are used with: from the lowest supported (3.11) to the one of
    .python-version, which the tests run on. A newer one may have no wheels of them yet (uv would then try to
    compile them, and fail without a C compiler)."""
    m = re.match(r"3\.(\d+)", (ROOT / ".python-version").read_text(encoding="utf-8").strip())
    if not m:
        raise Usage(".python-version does not name a Python 3.x")
    return f">=3.11,<3.{int(m.group(1)) + 1}"


def client_package_path(version: str) -> Path:
    """The client library's npm package that gui/web's npm run pack:client makes."""
    return ROOT / "build" / "gui-client" / f"vehicle-sil-gui-client-{version}.tgz"


def names(version: str) -> tuple[str, str]:
    """The tools archive's top directory and file name for this platform."""
    top = f"vehicle-sil-tools-{version}-{PLATFORM}"
    return top, top + (".zip" if WINDOWS else ".tar.gz")


# --- tools ----------------------------------------------------------------------------------------------------


def need(tool: str, why: str) -> str:
    found = shutil.which(tool)
    if not found:
        raise Usage(f"{tool} not found ({why}): install it, or put it on PATH")
    return found


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def copy_tree(src: Path, dst: Path) -> None:
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "_generated"))


def license_comments(path: Path) -> str:
    """The comment blocks of a source file that state a copyright or a license (the notices of amalgamated code)."""
    blocks, current, in_block = [], [], False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if in_block or s.startswith("/*"):
            current.append(line)
            in_block = "*/" not in s
            continue
        if s.startswith("//"):
            current.append(line)
            continue
        if current:
            blocks.append("\n".join(current))
            current = []
    keep, seen = [], set()
    for b in blocks:
        if re.search(r"copyright|license", b, re.IGNORECASE) and b.count("\n") >= 3 and b not in seen:
            seen.add(b)
            keep.append(b)
    return "\n\n".join(keep) + "\n"


SH_WRAPPER = r"""#!/bin/sh
# vehicle-sil-gui: the browser GUI's server (docs/gui.md of vehicle-sil) with the vehicle_sil_launch of these
# tools. Its Python packages are pinned in python/requirements.txt: with uv, `uv run` installs them (once, in its
# cache); without uv, install them into a Python 3.11+ yourself and name it with VEHICLE_SIL_PYTHON (Linux).
set -e
self=$(readlink -f -- "$0" 2>/dev/null || printf '%s' "$0")
tools=$(CDPATH= cd -- "$(dirname -- "$self")/.." && pwd)
python_dir="$tools/python"
launch="$tools/bin/vehicle_sil_launch"
uv_python=""
if [ -f "$launch.exe" ]; then launch="$launch.exe"; fi
if command -v cygpath >/dev/null 2>&1; then
  # MSYS2: the launcher and the Python are Windows programs. uv's own CPython, not MSYS2's MinGW Python, for
  # which PyPI has no wheels of these packages.
  python_dir=$(cygpath -w "$python_dir")
  launch=$(cygpath -w "$launch")
  uv_python="--managed-python"
fi
export VEHICLE_SIL_LAUNCH="$launch"
export VEHICLE_SIL_CONTROL_STUBS="$python_dir/vehicle_sil_control/_generated"
export PYTHONPATH="$python_dir"
if command -v uv >/dev/null 2>&1; then
  exec uv run --quiet --no-project --no-build $uv_python --python '@PYTHON@' \
    --with-requirements "$python_dir/requirements.txt" python -P -m vehicle_sil_gui "$@"
fi
if [ -n "$uv_python" ]; then
  echo "vehicle-sil-gui: uv is not installed in this MSYS2 shell. Install it with  pacman -S mingw-w64-ucrt-x86_64-uv" \
       "(MSYS2's Python cannot install the GUI server's packages: PyPI has no MinGW wheels of them)." >&2
  exit 2
fi
python=${VEHICLE_SIL_PYTHON:-python3}
if ! "$python" -P -c 'import aiohttp, yaml' 2>/dev/null; then
  echo "vehicle-sil-gui: uv is not installed, and $python (VEHICLE_SIL_PYTHON) is not a Python 3.11+ that has the" \
       "GUI server's packages. Install uv (https://docs.astral.sh/uv/), or install the packages into a virtual" \
       "environment with  python3 -m venv <dir> && <dir>/bin/python -m pip install --require-hashes -r" \
       "'$python_dir/requirements.txt'  and set VEHICLE_SIL_PYTHON=<dir>/bin/python" >&2
  exit 2
fi
exec "$python" -P -m vehicle_sil_gui "$@"
"""

CMD_WRAPPER = r"""@echo off
rem vehicle-sil-gui: the browser GUI's server (docs/gui.md of vehicle-sil) with the vehicle_sil_launch of these
rem tools. Its Python packages are pinned in python\requirements.txt: with uv, "uv run" installs them (once, in its
rem cache); without uv, install them into a Python 3.11+ yourself and name it with VEHICLE_SIL_PYTHON.
setlocal
set "VSIL_TOOLS=%~dp0.."
set "VEHICLE_SIL_LAUNCH=%VSIL_TOOLS%\bin\vehicle_sil_launch.exe"
set "VEHICLE_SIL_CONTROL_STUBS=%VSIL_TOOLS%\python\vehicle_sil_control\_generated"
set "PYTHONPATH=%VSIL_TOOLS%\python"
where uv >nul 2>nul || goto no_uv
uv run --quiet --no-project --no-build --managed-python --python "@PYTHON@" --with-requirements "%VSIL_TOOLS%\python\requirements.txt" python -P -m vehicle_sil_gui %*
exit /b %ERRORLEVEL%
:no_uv
if not defined VEHICLE_SIL_PYTHON set "VEHICLE_SIL_PYTHON=python"
"%VEHICLE_SIL_PYTHON%" -P -c "import aiohttp, yaml" 2>nul || goto no_packages
"%VEHICLE_SIL_PYTHON%" -P -m vehicle_sil_gui %*
exit /b %ERRORLEVEL%
:no_packages
echo vehicle-sil-gui: uv is not installed, and %VEHICLE_SIL_PYTHON% (VEHICLE_SIL_PYTHON) is not a Python 3.11+ that has the GUI server's packages. Install uv (https://docs.astral.sh/uv/), or, with a Windows Python (python.org, not MSYS2's), install the packages into a virtual environment with  python -m venv DIR  and  DIR\Scripts\python -m pip install --require-hashes -r "%VSIL_TOOLS%\python\requirements.txt"  and set VEHICLE_SIL_PYTHON=DIR\Scripts\python.exe 1>&2
exit /b 2
"""

README = """# vehicle-sil {version} tools ({platform})

Prebuilt tools of vehicle-sil {version}: the launcher and the browser GUI. Build your ECUs with the same
vehicle-sil version (the ECU project template: GIT_TAG v{version}).

| Path | What |
|---|---|
| `bin/vehicle_sil_launch{exe}` | checks and runs a simulation configuration (`check`, `run`, `run --scenario`, `run --control`) |
| `bin/vehicle-sil-gui`{cmd} | the browser GUI's server, with the launcher above (needs Python 3.11+; with uv, `uv run` installs its packages) |
| `python/` | the GUI server (`vehicle_sil_gui`, with the built screen) and the Control API client (`vehicle_sil_control`, with its gRPC stubs); `requirements.txt` pins their packages with hashes |
| `client/vehicle-sil-gui-client.mjs` | the GUI client library as one ES module (for a screen without a build: import it with a relative path) |
| `client/vehicle-sil-gui-client-{version}.tgz` | the same library as an npm package with its types: `npm install <this file>`, then `import ... from '@vehicle-sil/gui-client'` |
| `gui_starter/` | a screen of your own to start from (plain HTML and JavaScript, the library next to it): `vehicle-sil-gui --web gui_starter simulation.yaml` |
| `schema/` | the JSON Schemas of the configuration (`simulation.schema.json`), of a scenario (`scenario.schema.json`) and of `<configuration>.dashboard.yaml` (`dashboard.schema.json`): an editor with a YAML language server completes and checks a file whose first line is `# yaml-language-server: $schema=<path of the schema>`. `get_tools.py` of the project template copies them to the project's `schema/` |
| `LICENSE`, `THIRD_PARTY_NOTICES.md`, `licenses/` | the license of vehicle-sil and the notices and license texts of what these tools contain |

Put `bin` on PATH, then, next to your simulation.yaml:

    vehicle_sil_launch check simulation.yaml
    vehicle_sil_launch run simulation.yaml --scenario scenarios/<file>.yaml
    vehicle-sil-gui simulation.yaml
    vehicle-sil-gui --web <your built screen, or gui_starter> simulation.yaml

The Control API client from Python: put `python` on PYTHONPATH and use a Python with the packages of
`python/requirements.txt` (for example `uv run --with-requirements python/requirements.txt python your_script.py`).

Documentation: https://github.com/rahanahu/vehicle-sil/tree/v{version} (README.md, docs/howto/first-ecu.md,
docs/gui.md, docs/control-api.md).
"""


def built_with(build: Path) -> str | None:
    """The C++ compiler of a configured build directory, as CMake names it (GNU, Clang, ...), or None."""
    for found in sorted(build.glob("CMakeFiles/*/CMakeCXXCompiler.cmake")):
        m = re.search(r'(?m)^set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)', found.read_text(encoding="utf-8", errors="replace"))
        if m:
            return m.group(1)
    return None


def tools_command(args: argparse.Namespace) -> int:
    version = project_version()
    top, archive_name = names(version)
    build = Path(args.build_dir).resolve() if args.build_dir else ROOT / "build" / f"package-{PLATFORM}"
    web = Path(args.web).resolve()
    client = Path(args.client).resolve()
    client_package = Path(args.client_package).resolve() if args.client_package else client_package_path(version)
    out = Path(args.out).resolve()
    stage_root = build / "stage"
    stage = stage_root / top
    launcher = build / "tools" / "launch" / f"vehicle_sil_launch{EXE}"
    for tool, why in [("cmake", "configure and build"), ("uv", "the Python packages and stubs")] + (
            [] if args.no_build else [("ninja", "the build")]) + (
            [("objdump", "the DLLs of the executables (MSYS2: binutils)"), ("pacman", "the licenses of the DLLs")]
            if WINDOWS else [("readelf", "the libraries of the executables (binutils)"),
                            ("objdump", "the glibc versions the executables need (binutils)")]):
        need(tool, why)
    if not WINDOWS and not args.shared_runtime and not args.no_build:
        cxx = os.environ.get("CXX") or "g++"
        found = subprocess.run([cxx, "-print-file-name=libstdc++.a"], capture_output=True, text=True).stdout.strip()
        if not Path(found).is_absolute():
            raise Usage(f"{cxx} has no static libstdc++ (libstdc++.a), which the archive links so that it runs on "
                        "other distributions: install it (Fedora: libstdc++-static; Debian and Ubuntu: it is in "
                        "libstdc++-<n>-dev), or pass --shared-runtime for an archive that needs the system's "
                        "libstdc++ (for a local try, not for a release)")
    if not (web / "index.html").is_file() or not client.is_file():
        raise Usage(f"the built screen ({web}/index.html) or the client library ({client}) is missing: build them "
                    "with scripts/verify-web.py --skip-e2e (or in gui/web: npm ci, npm run build, npm run build:client),"
                    " or name them with --web and --client")
    if client_package.name != client_package_path(version).name or not client_package.is_file():
        raise Usage(f"the client library's npm package {client_package_path(version).name} is missing ({client_package}):"
                    " make it with scripts/verify-web.py --skip-e2e (or in gui/web: npm run pack:client), or name it"
                    " with --client-package")
    web_licenses = web.parent / "3rdpartylicenses.txt"
    if not web_licenses.is_file():
        raise Usage(f"{web_licenses} is missing: the screen's build writes the licenses of the packages it bundles "
                    "there (Angular's extractLicenses); build the screen again")

    def configure(s: Step) -> None:
        command = ["cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                   "-DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON", "-DVEHICLE_SIL_BUILD_TESTS=OFF",
                   "-DVEHICLE_SIL_BUILD_EXAMPLES=OFF"]
        # Always the pinned gRPC built here and linked statically, never a system one (MSYS2's is DLLs that
        # need libstdc++-6.dll: a second C++ runtime next to the static one, and DLLs a user's PATH may mix up).
        command += ["-DCMAKE_DISABLE_FIND_PACKAGE_gRPC=ON"]
        if WINDOWS:
            # BoringSSL without its assembly (it would need NASM): the control server's TLS is not a hot path.
            command += ["-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++", "-DOPENSSL_NO_ASM=ON"]
        else:
            command += ["-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON", "-DCMAKE_INSTALL_RPATH=$ORIGIN",
                        "-DgRPC_USE_SYSTEMD=OFF"]  # no systemd library
        # One executable: SIL Kit and the C++ runtime linked statically (VEHICLE_SIL_STATIC).
        command.append(f"-DVEHICLE_SIL_STATIC={'OFF' if args.shared_runtime else 'ON'}")
        command += args.cmake_arg or []
        s.run(command)

    def build_step(s: Step) -> None:
        s.run(["cmake", "--build", build, "--target", "vehicle_sil_launch", "--parallel", str(args.jobs)])

    python_dir = build / "python"

    def python_step(s: Step) -> None:
        shutil.rmtree(python_dir, ignore_errors=True)
        python_dir.mkdir(parents=True)
        # What the GUI server and the client need, with gRPC; then what the stubs are made with.
        export = ["uv", "export", "--frozen", "--no-dev", "--no-emit-workspace", "--no-header", "--quiet"]
        requirements = s.run(export + ["--package", "vehicle-sil-gui", "--extra", "variables"],
                             cwd=ROOT, capture=True).stdout
        (python_dir / "requirements.txt").write_text(
            "# The Python packages of the GUI server (vehicle_sil_gui) and of the Control API client\n"
            "# (vehicle_sil_control), pinned with hashes: uv run --with-requirements requirements.txt ...\n"
            "# or python -m pip install --require-hashes -r requirements.txt\n" + requirements, encoding="utf-8")
        generator = s.run(export + ["--only-group", "control"], cwd=ROOT, capture=True).stdout
        (build / "generator-requirements.txt").write_text(generator, encoding="utf-8")
        env = dict(os.environ, PYTHONPATH=str(ROOT / "python"))
        s.run(["uv", "run", "--quiet", "--no-project", "--no-build", *UV_MANAGED, "--python", python_versions(),
               "--with-requirements", build / "generator-requirements.txt", "python", "-P", "-m", "vehicle_sil_control.generate",
               "--out", python_dir / "_generated"], cwd=build, env=env)
        s.notes.append(f"{requirements.count('==')} packages pinned")

    def stage_step(s: Step) -> None:
        shutil.rmtree(stage_root, ignore_errors=True)
        (stage / "bin").mkdir(parents=True)
        shutil.copy2(launcher, stage / "bin")
        if WINDOWS:
            bundled = stage_windows_dlls(s, launcher.parent, stage / "bin")
            (stage / "bin" / "vehicle-sil-gui.cmd").write_text(
                CMD_WRAPPER.replace("@PYTHON@", python_versions()).replace("\n", "\r\n"), encoding="ascii", newline="")
        else:
            bundled = stage_linux_libraries(s, launcher, build, stage / "bin")
        wrapper = stage / "bin" / "vehicle-sil-gui"
        wrapper.write_text(SH_WRAPPER.replace("@PYTHON@", python_versions()), encoding="ascii", newline="\n")
        wrapper.chmod(0o755)
        py = stage / "python"
        copy_tree(ROOT / "gui" / "server" / "src" / "vehicle_sil_gui", py / "vehicle_sil_gui")
        shutil.copytree(web, py / "vehicle_sil_gui" / "web")
        copy_tree(ROOT / "python" / "vehicle_sil_control", py / "vehicle_sil_control")
        shutil.copytree(python_dir / "_generated", py / "vehicle_sil_control" / "_generated")
        shutil.copy2(python_dir / "requirements.txt", py / "requirements.txt")
        (stage / "client").mkdir()
        shutil.copy2(client, stage / "client" / "vehicle-sil-gui-client.mjs")
        shutil.copy2(client_package, stage / "client" / client_package.name)
        shutil.copytree(ROOT / "examples" / "gui_starter", stage / "gui_starter",
                        ignore=shutil.ignore_patterns(".*"))
        shutil.copy2(client, stage / "gui_starter" / "vehicle-sil-gui-client.mjs")
        (stage / "schema").mkdir()
        shutil.copy2(ROOT / "gui" / "protocol" / "dashboard.schema.json", stage / "schema")
        shutil.copy2(ROOT / "schema" / "simulation.schema.json", stage / "schema")
        shutil.copy2(ROOT / "schema" / "scenario.schema.json", stage / "schema")
        shutil.copy2(ROOT / "LICENSE", stage)
        shutil.copy2(ROOT / "THIRD_PARTY_NOTICES.md", stage)
        stage_licenses(s, build, stage / "licenses", web_licenses, bundled, args.allow_missing_licenses)
        (stage / "README.md").write_text(README.format(version=version, platform=PLATFORM, exe=EXE,
                                                       cmd=" (`.cmd` from the Command Prompt)" if WINDOWS else ""),
                                         encoding="utf-8")
        (stage / "VERSION").write_text(version + "\n", encoding="ascii")

    def compiler_step(s: Step) -> None:
        # What this repository hands out is built with GCC (MinGW-w64 included): the C++ runtime linked into
        # the launcher is passed on under the GCC Runtime Library Exception, which was checked for GCC only.
        # Anyone may build with another compiler; the released archive is the thing that must not be.
        found = built_with(build)
        if found != "GNU":
            raise StepFailed(f"the build in {build} was made with {found or 'an unknown compiler'}, and the tools "
                             "archive is made with GCC only (its license notices are written for GCC's runtime): "
                             "configure again with -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ "
                             "(--cmake-arg), in a new --build directory")

    def portable_step(s: Step) -> None:
        # The staged launcher, with a PATH of the system's directories only: it finds its libraries next to it.
        p = s.run([stage / "bin" / launcher.name, "version", "--json"], env=dict(os.environ, PATH=minimal_path()),
                  capture=True)
        info = json.loads(p.stdout)
        if info.get("vehicle_sil") != version:
            raise StepFailed(f"the launcher says vehicle-sil {info.get('vehicle_sil')}, not {version}: configure again")
        if "control-server" not in info.get("features", []):
            raise StepFailed("the launcher has no control server (features without control-server): gRPC was not "
                             "built in; see the configure log")
        if WINDOWS:
            missing = [f"{pe.name} needs {dll}" for pe in sorted((stage / "bin").glob("*.[de][lx][le]"))
                       for dll in pe_imports(s, pe)
                       if not (stage / "bin" / dll).is_file() and not windows_system_dll(dll)]
            if missing:
                raise StepFailed("DLLs neither in bin/ nor Windows' own: " + ", ".join(missing))
            dlls = sorted(f.name for f in (stage / "bin").glob("*.dll"))
            if dlls:
                # Users put bin/ on PATH: their ECUs would load these instead of their own toolchain's.
                raise StepFailed(f"bin/ holds DLLs ({', '.join(dlls)}): the launcher is to be one executable "
                                 "(VEHICLE_SIL_STATIC, gRPC built here), since bin/ goes on the users' PATH")
            s.notes.append("no DLL in bin/")
            return
        newest = (0, 0)
        for elf in [stage / "bin" / "vehicle_sil_launch", *sorted((stage / "bin").glob("*.so*"))]:
            needed = elf_needed(s, elf)
            allowed = LINUX_SYSTEM_LIBRARIES | ({"libstdc++.so.6", "libgcc_s.so.1"} if args.shared_runtime else set())
            extra = [n for n in needed if n not in allowed and not (stage / "bin" / n).is_file()]
            if extra:
                raise StepFailed(f"{elf.name} needs {', '.join(extra)}, which the archive does not have and a Linux "
                                 "system may not either (libstdc++ and libgcc are to be linked statically)")
            symbols = s.run(["objdump", "-T", elf], capture=True).stdout
            for major, minor in re.findall(r"GLIBC_(\d+)\.(\d+)", symbols):
                newest = max(newest, (int(major), int(minor)))
        s.notes.append(f"needs glibc {newest[0]}.{newest[1]} or newer"
                       + (", and the system's libstdc++ (--shared-runtime)" if args.shared_runtime else ""))

    def archive_step(s: Step) -> None:
        out.mkdir(parents=True, exist_ok=True)
        path = out / archive_name
        if WINDOWS:
            with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
                for f in sorted(stage.rglob("*")):
                    z.write(f, f.relative_to(stage_root).as_posix())
        else:
            def normal(info: tarfile.TarInfo) -> tarfile.TarInfo:
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mode = 0o755 if info.isdir() or info.mode & 0o111 else 0o644
                return info
            with tarfile.open(path, "w:gz") as t:
                t.add(stage, arcname=top, filter=normal)
        digest = sha256_of(path)
        (out / (archive_name + ".sha256")).write_text(f"{digest}  {archive_name}\n", encoding="ascii")
        s.notes.append(f"{path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}, "
                       f"{path.stat().st_size / 1e6:.1f} MB, sha256 {digest[:12]}...")

    steps = [("version", check_version)]
    if not args.no_build:
        steps += [("configure", configure), ("build", build_step)]
    elif not launcher.is_file():
        raise Usage(f"--no-build, and there is no {launcher}: build it first (without --no-build)")
    steps += [("compiler", compiler_step), ("python", python_step), ("stage", stage_step), ("portable", portable_step),
              ("archive", archive_step)]
    print(f"package: tools {version} for {PLATFORM} (build {build})", flush=True)
    if not run_steps(args.tag, steps):
        print("package: FAILED")
        return 1
    print(f"package: ok {out / archive_name}")
    return 0


def elf_needed(s: Step, path: Path) -> list[str]:
    out = s.run(["readelf", "-d", path], capture=True).stdout
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)


def stage_linux_libraries(s: Step, launcher: Path, build: Path, bin_dir: Path) -> list[str]:
    """Copies the vehicle-sil libraries the launcher needs (libSilKit.so) from the build tree next to it."""
    copied = []
    for name in elf_needed(s, launcher):
        if name in LINUX_SYSTEM_LIBRARIES or name.startswith(("libstdc++.so", "libgcc_s.so")):
            continue
        found = sorted(build.glob(f"**/{name}"))
        if not found:
            raise StepFailed(f"vehicle_sil_launch needs {name}, which is neither a C library nor in {build}")
        shutil.copy2(found[0], bin_dir / name)
        copied.append(name)
    s.notes.append("bundled " + ", ".join(copied) if copied else "the launcher needs no library of its own")
    return copied


def pe_imports(s: Step, path: Path) -> list[str]:
    out = s.run(["objdump", "-p", path], capture=True).stdout
    return re.findall(r"DLL Name: (\S+)", out)


def windows_system_dll(name: str) -> bool:
    system = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    return name.lower().startswith(("api-ms-win-", "ext-ms-")) or (system / name).is_file()


def stage_windows_dlls(s: Step, build_bin: Path, bin_dir: Path) -> list[Path]:
    """Copies every DLL the launcher loads, directly or through other DLLs, that is not Windows' own: from the
    launcher's build directory (SilKit.dll and the imported libraries' DLLs) or the MinGW bin directory (the GCC
    runtime, gRPC and its dependencies from MSYS2). Returns the DLLs taken from MinGW (for their licenses)."""
    mingw_bin = Path(need("gcc", "the MinGW bin directory")).resolve().parent
    from_mingw: list[Path] = []
    todo = [bin_dir / f"vehicle_sil_launch{EXE}"]
    seen: set[str] = set()
    while todo:
        for dll in pe_imports(s, todo.pop()):
            key = dll.lower()
            if key in seen:
                continue
            seen.add(key)
            for where in (mingw_bin, build_bin):
                if (where / dll).is_file():
                    shutil.copy2(where / dll, bin_dir / dll)
                    todo.append(bin_dir / dll)
                    if where == mingw_bin:
                        from_mingw.append(where / dll)
                    break
            else:
                if not windows_system_dll(dll):
                    raise StepFailed(f"a DLL that the launcher loads is nowhere: {dll} (not in {build_bin}, "
                                     f"{mingw_bin} or Windows' System32)")
    s.notes.append(f"{len(from_mingw)} DLLs from {mingw_bin}")
    return from_mingw


def stage_licenses(s: Step, build: Path, licenses: Path, web_licenses: Path, bundled: list,
                   allow_missing: bool) -> None:
    licenses.mkdir()
    silkit = ROOT / "external" / "sil-kit"
    for rel in SILKIT_LICENSES:
        target = licenses / "sil-kit" / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(silkit / rel, target)
    (licenses / "sil-kit" / "ThirdParty" / "rapidyaml-notices.txt").write_text(
        "The notices in rapidyaml.hpp, the amalgamated rapidyaml of SIL Kit (also used by vehicle_sil_launch):\n\n"
        + license_comments(silkit / RAPIDYAML), encoding="utf-8")
    web_dir = licenses / "web"
    web_dir.mkdir()
    shutil.copy2(web_licenses, web_dir / "3rdpartylicenses.txt")
    if WINDOWS:
        stage_msys2_licenses(s, licenses / "msys2", bundled, allow_missing)
    grpc = build / "_deps" / "grpc-src"
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    given = re.search(r"(?m)^FETCHCONTENT_SOURCE_DIR_GRPC:[A-Z]+=(.+)$", cache)
    if given:  # a gRPC source tree given to the configure
        grpc = Path(given.group(1).strip())
    missing = [rel for rel in GRPC_LICENSES if not (grpc / rel).is_file()]
    if missing:
        raise StepFailed(f"license files missing in {grpc}: {', '.join(missing)} (the gRPC version changed: update "
                         "GRPC_LICENSES in scripts/package.py and THIRD_PARTY_NOTICES.md)")
    for rel in GRPC_LICENSES:
        target = licenses / "grpc" / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(grpc / rel, target)


# The MinGW libraries every executable of the archive has inside (linked statically) or loads.
MINGW_STATIC = {"libstdc++.a": "the C++ library", "libgcc.a": "the GCC runtime", "libwinpthread.a": "winpthreads",
                "libmingwex.a": "the mingw-w64 C runtime", "crt2.o": "the mingw-w64 start-up code"}


def stage_msys2_licenses(s: Step, into: Path, dlls: list[Path], allow_missing: bool) -> None:
    """The license files that MSYS2 installs for each package whose code is in the archive: the packages of the
    DLLs taken from MinGW, and of the runtime libraries linked statically into every MinGW executable. A package
    without license files fails the step (allow_missing: only noted), so no code goes out without its text."""
    into.mkdir()
    parts: list[tuple[Path, str]] = [(dll, dll.name) for dll in dlls]
    for name, what in MINGW_STATIC.items():
        found = s.run(["gcc", f"-print-file-name={name}"], capture=True).stdout.strip()
        if not Path(found).is_absolute():
            raise StepFailed(f"gcc does not find {name} ({what}): is this the MSYS2 UCRT64 shell?")
        parts.append((Path(found), f"{name} ({what}, linked in)"))
    packages: dict[str, list[str]] = {}
    for path, label in parts:
        posix = s.run(["cygpath", "-u", path], capture=True).stdout.strip()
        owner = s.run(["pacman", "-Qqo", posix], capture=True).stdout.strip()
        packages.setdefault(owner, []).append(label)
    missing = []
    lines = ["The MinGW code in this archive, by MSYS2 (UCRT64) package, and the license files MSYS2 installs for it:",
             ""]
    for package, labels in sorted(packages.items()):
        listed = s.run(["pacman", "-Qlq", package], capture=True).stdout.splitlines()
        files = [f for f in listed if "/share/licenses/" in f and not f.endswith("/")]
        short = re.sub(r"^mingw-w64-ucrt-x86_64-", "", package)
        if files:
            (into / short).mkdir()
            for f in files:
                native = s.run(["cygpath", "-w", f], capture=True).stdout.strip()
                shutil.copy2(native, into / short / Path(f).name)
            lines.append(f"{package}: {', '.join(sorted(labels))} -> licenses/msys2/{short}/")
        else:
            missing.append(package)
            lines.append(f"{package}: {', '.join(sorted(labels))} -> NO LICENSE FILES (MSYS2 installs none)")
    (into / "PACKAGES.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    if missing and not allow_missing:
        raise StepFailed(f"MSYS2 installs no license files for {', '.join(missing)}, whose code would be in the "
                         "archive: find the license text of each and add it (stage_msys2_licenses in "
                         "scripts/package.py), or keep the code out; --allow-missing-licenses only for a local try")
    s.notes.append(f"licenses of {len(packages)} MSYS2 packages" + (f"; MISSING for {', '.join(missing)}"
                                                                     if missing else ""))


# --- verify ---------------------------------------------------------------------------------------------------


def copy_template(into: Path) -> None:
    def ignore(directory: str, entries: list[str]) -> list[str]:
        top = Path(directory).resolve() == (ROOT / "template").resolve()
        return [e for e in entries if e == "__pycache__" or (top and e in ("build", "bin", "tools"))]
    shutil.copytree(ROOT / "template", into, ignore=ignore)


def minimal_path() -> str:
    """A PATH of the system's directories only: what a user's PATH has for sure."""
    if WINDOWS:
        root = os.environ.get("SystemRoot", r"C:\Windows")
        return os.pathsep.join([str(Path(root) / "System32"), root, str(Path(root) / "System32" / "Wbem")])
    return "/usr/bin:/bin"


def verify_command(args: argparse.Namespace) -> int:
    archive = Path(args.archive).resolve()
    if not archive.is_file():
        raise Usage(f"{archive}: no such file (make it with scripts/package.py tools)")
    need("cmake", "the template's build")
    need("uv", "the GUI server's Python packages")
    version = project_version()
    top, name = names(version)
    if archive.name != name:
        raise Usage(f"{archive.name} is not the tools archive of vehicle-sil {version} for {PLATFORM} ({name})")
    work = Path(tempfile.mkdtemp(prefix="vehicle-sil-verify-"))
    project = work / "project"
    tools = project / "tools" / top
    preset = "windows" if WINDOWS else "linux"
    launch = tools / "bin" / f"vehicle_sil_launch{EXE}"
    print(f"package: verify {archive.name} in {work}", flush=True)

    def unpack(s: Step) -> None:
        copy_template(project)
        s.run([sys.executable, "-I", project / "get_tools.py", "--archive", archive, "--sha256", sha256_of(archive)],
              cwd=project)
        for path in ["VERSION", "LICENSE", "THIRD_PARTY_NOTICES.md", f"bin/vehicle_sil_launch{EXE}",
                     "bin/vehicle-sil-gui", "python/requirements.txt", "python/vehicle_sil_gui/web/index.html",
                     "python/vehicle_sil_control/_generated/vehicle_sil/control/v1/control_pb2.py",
                     "client/vehicle-sil-gui-client.mjs", f"client/{client_package_path(version).name}",
                     "gui_starter/index.html", "gui_starter/vehicle-sil-gui-client.mjs",
                     "schema/dashboard.schema.json", "schema/simulation.schema.json",
                     "schema/scenario.schema.json"] + (
                ["bin/vehicle-sil-gui.cmd"] if WINDOWS else []):
            if not (tools / path).is_file():
                raise StepFailed(f"the archive has no {top}/{path}")

    def alone(s: Step) -> None:
        env = dict(os.environ, PATH=minimal_path())
        p = s.run([launch, "version", "--json"], env=env, capture=True)
        info = json.loads(p.stdout)
        if info.get("vehicle_sil") != version or "control-server" not in info.get("features", []):
            raise StepFailed(f"the archive's launcher is not vehicle-sil {version} with its control server: {info}")
        s.notes.append(f"PATH={env['PATH']}")

    def build(s: Step) -> None:
        s.run(["cmake", "--preset", preset, f"-DFETCHCONTENT_SOURCE_DIR_VEHICLE_SIL={ROOT}"] + (args.cmake_arg or []),
              cwd=project)
        s.run(["cmake", "--build", "--preset", preset], cwd=project)

    def simulate(s: Step) -> None:
        for command in (["check", "simulation.yaml"], ["run", "simulation.yaml"],
                        ["run", "simulation.yaml", "--scenario", "scenarios/throttle_step.yaml"]):
            s.run([launch] + command, cwd=project, timeout=300)

    def control(s: Step) -> None:
        # The launcher's control server through gRPC, with the archive's Python client: one write before the start,
        # a step, a read (examples/launch_control/write_before_start.py on the template's ECUs).
        env = dict(os.environ, PYTHONPATH=str(tools / "python"))
        s.run(["uv", "run", "--quiet", "--no-project", "--no-build", *UV_MANAGED, "--python", python_versions(),
               "--with-requirements", tools / "python" / "requirements.txt", "python", "-P",
               ROOT / "examples" / "launch_control" / "write_before_start.py", "--launch", launch, "simulation.yaml"],
              cwd=project, env=env, timeout=600)
        s.notes.append("run --control: written before the start, stepped, read back")

    def gui(s: Step) -> None:
        verify_gui(s, tools, project, [str(tools / "bin" / "vehicle-sil-gui.cmd")] if WINDOWS else
                   ["/bin/sh", str(tools / "bin" / "vehicle-sil-gui")])
        # A screen of one's own: the archive's starter, served with --web.
        verify_gui(s, tools, project, [str(tools / "bin" / "vehicle-sil-gui.cmd")] if WINDOWS else
                   ["/bin/sh", str(tools / "bin" / "vehicle-sil-gui")], web=tools / "gui_starter")
        if WINDOWS:  # the sh entry, as the MSYS2 UCRT64 shell starts it
            verify_gui(s, tools, project, [need("bash", "the sh entry in MSYS2"), str(tools / "bin" / "vehicle-sil-gui")])

    ok = run_steps(args.tag, [("unpack", unpack), ("alone", alone), ("build", build), ("simulate", simulate),
                              ("control", control), ("gui", gui)])
    if ok and not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    elif not ok:
        print(f"package: kept {work} for a look")
    print("package: ok" if ok else "package: FAILED", archive.name if ok else "")
    return 0 if ok else 1


def verify_gui(s: Step, tools: Path, project: Path, entry: list[str], web: Path | None = None) -> None:
    """Starts the archive's vehicle-sil-gui (entry: how to start it) on the project and asks it what a screen would
    ask first. With web: started with --web web (a screen of one's own); asks for its page, its client library and
    api/app."""
    command = entry + ["--json", "--no-browser", *(["--web", str(web)] if web else []), "simulation.yaml"]
    s.write(f"$ {' '.join(command)}   (in {project})")
    flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    p = subprocess.Popen(command, cwd=project, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                         start_new_session=not WINDOWS, creationflags=flags)
    lines: queue.Queue = queue.Queue()

    def read() -> None:
        for line in p.stdout:
            s.write(line)
            lines.put(line)
        lines.put(None)
    threading.Thread(target=read, daemon=True).start()
    try:
        info = None
        deadline = time.monotonic() + 600  # the first start may download Python and the packages
        while info is None:
            try:
                line = lines.get(timeout=max(0.1, deadline - time.monotonic()))
            except queue.Empty:
                raise StepFailed("vehicle-sil-gui wrote no {\"vehicle_sil_gui\": ...} line within 600 s")
            if line is None:
                raise StepFailed(f"vehicle-sil-gui ended (exit code {p.wait()}) before it said where it listens")
            if line.startswith("{"):
                info = json.loads(line)
        base = f"http://127.0.0.1:{info['port']}/"
        token = info["url"].split("#token=", 1)[1]
        headers = {"Authorization": f"Bearer {token}"}

        def get(path: str, body: dict | None = None, token: bool = True) -> bytes:
            h = dict(headers) if token else {"Sec-Fetch-Mode": "navigate", "Accept": "text/html"}
            data = None
            if body is not None:
                h.update({"Origin": base.rstrip("/"), "Content-Type": "application/json"})
                data = json.dumps(body).encode()
            request = urllib.request.Request(base + path, data=data, headers=h)
            try:
                with urllib.request.urlopen(request, timeout=180) as response:
                    return response.read()
            except urllib.error.HTTPError as e:
                raise StepFailed(f"{path}: HTTP {e.code} {e.read()[:300]!r}") from None

        if web is not None:
            page = get("", token=False).decode("utf-8", "replace")
            if 'src="main.js"' not in page:
                raise StepFailed(f"vehicle-sil-gui --web {web} does not serve the starter's index.html: {page[:300]!r}")
            library = get("vehicle-sil-gui-client.mjs", token=False)
            if b"createClient" not in library or b"takeToken" not in library:
                raise StepFailed(f"vehicle-sil-gui --web {web} does not serve the client library next to the page")
            app = json.loads(get("api/app"))
            if not app.get("launcher", {}).get("usable"):
                raise StepFailed(f"vehicle-sil-gui --web {web}: api/app says the launcher is not usable: {app}")
            s.notes.append(f"{Path(entry[-1]).name} --web gui_starter: page and library served, api/app answered")
            return
        app = json.loads(get("api/app"))
        s.write(json.dumps(app, indent=1)[:3000])
        launcher = app.get("launcher", {})
        if not launcher.get("usable") or Path(launcher.get("path", "")).resolve() != (
                tools / "bin" / f"vehicle_sil_launch{EXE}").resolve():
            raise StepFailed(f"the GUI server does not use the archive's launcher: {launcher}")
        if not app.get("variables", {}).get("available"):
            raise StepFailed(f"the GUI server cannot read the variables: {app.get('variables')}")
        page = get("").decode("utf-8", "replace")
        if "<app-root" not in page:
            raise StepFailed("the GUI server does not serve the built screen (no <app-root> in its page)")
        checked = json.loads(get("api/check", {"file": "simulation.yaml"}))
        if checked.get("exit_code") != 0:
            raise StepFailed(f"api/check of simulation.yaml ended with {checked.get('exit_code')}")
        s.notes.append(f"{Path(entry[-1]).name}: launcher usable, variables available, screen served, check passed")
    finally:
        stop(p)


def stop(p: subprocess.Popen) -> None:
    if p.poll() is not None:
        return
    if WINDOWS:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], capture_output=True)
    else:
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
    try:
        p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        if not WINDOWS:
            os.killpg(p.pid, signal.SIGKILL)
        p.wait()


# --- template -------------------------------------------------------------------------------------------------


def template_command(args: argparse.Namespace) -> int:
    version = project_version()
    out = Path(args.out).resolve()
    problems = version_problems(version)
    if problems:
        print("package: FAILED, the version disagrees:\n  " + "\n  ".join(problems))
        return 1
    sums: dict[str, str] = {}
    source = (ROOT / "template" / "get_tools.py").read_text(encoding="utf-8")
    for plat, suffix in (("linux-x86_64", ".tar.gz"), ("windows-x86_64", ".zip")):
        name = f"vehicle-sil-tools-{version}-{plat}{suffix}"
        path = out / name
        if not path.is_file():
            raise Usage(f"{path} is missing: the template archive pins the SHA-256 of both tools archives; put them "
                        "in the --out directory first (scripts/package.py tools on each platform)")
        sums[name] = sha256_of(path)
        source, n = re.subn(rf'(?m)^(    "{plat}": )""', rf'\1"{sums[name]}"', source)
        if n != 1:
            raise Usage(f'template/get_tools.py has no line `    "{plat}": "",` in SHA256 to fill in')
    client_package = out / client_package_path(version).name
    if not client_package.is_file():
        raise Usage(f"{client_package} is missing: the release carries the client library's npm package; copy it "
                    f"there from build/gui-client/ (scripts/verify-web.py, or in gui/web: npm run pack:client)")
    sums[client_package.name] = sha256_of(client_package)
    top = f"vehicle-sil-template-{version}"
    path = out / f"{top}.zip"
    with tempfile.TemporaryDirectory() as tmp:
        copy_template(Path(tmp) / top)
        (Path(tmp) / top / "get_tools.py").write_text(source, encoding="utf-8", newline="\n")
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            for f in sorted((Path(tmp) / top).rglob("*")):
                z.write(f, f.relative_to(tmp).as_posix())
    sums[path.name] = sha256_of(path)
    (out / "SHA256SUMS").write_text("".join(f"{h}  {n}\n" for n, h in sorted(sums.items())), encoding="ascii")
    print(f"package: ok {path} and {out / 'SHA256SUMS'} ({len(sums)} files)")
    return 0


def version_command(args: argparse.Namespace) -> int:
    version = project_version()
    problems = version_problems(version, release=bool(args.expect))
    if args.expect and args.expect != f"v{version}":
        problems.append(f"the tag is {args.expect}, the version {version}: tag v{version}, or change the version "
                        "(CMakeLists.txt and the places this command checks)")
    print(version)
    for p in problems:
        print(f"error: {p}", file=sys.stderr)
    return 1 if problems else 0


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(
        prog="scripts/package.py", description=__doc__.split("\n\n")[0],
        epilog=__doc__.split("\n\n", 1)[1] + "\nExamples:\n"
        "  scripts/verify-web.py --skip-e2e && scripts/package.py tools\n"
        "  scripts/package.py verify build/dist/vehicle-sil-tools-0.1.0-linux-x86_64.tar.gz\n"
        "  scripts/package.py template            # after both tools archives are in build/dist/\n"
        "  scripts/package.py version --expect v0.1.0\n",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    tools = sub.add_parser("tools", help="build and pack the tools archive of this platform")
    tools.add_argument("--build-dir", help=f"the build directory (default: build/package-{PLATFORM})")
    tools.add_argument("--no-build", action="store_true", help="pack the launcher already built there")
    tools.add_argument("--web", default=str(ROOT / "gui" / "web" / "dist" / "browser"),
                       help="the built screen (default: gui/web/dist/browser)")
    tools.add_argument("--client", default=str(ROOT / "build" / "gui-client" / "vehicle-sil-gui-client.mjs"),
                       help="the bundled client library (default: build/gui-client/vehicle-sil-gui-client.mjs)")
    tools.add_argument("--client-package",
                       help="the client library's npm package (default: build/gui-client/vehicle-sil-gui-client-"
                            "<version>.tgz)")
    tools.add_argument("--out", default=str(DIST), help="where the archive goes (default: build/dist)")
    tools.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="build jobs (default: the CPUs)")
    tools.add_argument("--cmake-arg", action="append", metavar="ARG", help="one more configure argument (repeatable)")
    tools.add_argument("--allow-missing-licenses", action="store_true",
                       help="Windows: only note a package without license files instead of failing (a local try only)")
    tools.add_argument("--shared-runtime", action="store_true",
                       help="Linux: VEHICLE_SIL_STATIC=OFF, libSilKit.so next to the launcher and the system's libstdc++ "
                            "(for a local try where no static libstdc++ is installed, not for a release)")
    tools.add_argument("--tag", default="package", help="prefix of the log names (default: package)")
    verify = sub.add_parser("verify", help="check a tools archive as a user uses it")
    verify.add_argument("archive", help="the tools archive of this platform")
    verify.add_argument("--keep", action="store_true", help="keep the directory it worked in")
    verify.add_argument("--cmake-arg", action="append", metavar="ARG",
                        help="one more configure argument for the template (repeatable)")
    verify.add_argument("--tag", default="package-verify", help="prefix of the log names (default: package-verify)")
    template = sub.add_parser("template", help="the template archive with the tools' SHA-256, and SHA256SUMS")
    template.add_argument("--out", default=str(DIST), help="where both tools archives are (default: build/dist)")
    version = sub.add_parser("version", help="print the version; check that every place agrees")
    version.add_argument("--expect", metavar="TAG", help="also check that the tag is v<version>")
    args = parser.parse_args()
    if platform.machine().lower() not in ("x86_64", "amd64") and args.command in ("tools", "verify"):
        print(f"package: error: only x86_64 is packaged (this is {platform.machine()})", file=sys.stderr)
        return 2
    try:
        return {"tools": tools_command, "verify": verify_command, "template": template_command,
                "version": version_command}[args.command](args)
    except Usage as e:
        print(f"package: error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
