#!/usr/bin/env python3
"""Say whether this computer has what building and testing vehicle-sil needs. Reads only, changes nothing.

  scripts/doctor.py           one line per item: ok, MISSING (needed for every build), or absent (needed for one part)
  scripts/doctor.py --json    the same as JSON, for a script or an AI

Each line that is not ok says what the item is for and how to get it. Exit codes: 0 everything needed for a
build and its tests is there, 1 something of that is missing, 2 usage error. Items for one part only (the GUI,
the sanitizer presets, ...) do not change the exit code. Requires Python 3.11+ (standard library only); runs on
Linux and in MSYS2 on Windows.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WINDOWS = os.name == "nt" or sys.platform in ("msys", "cygwin")


def run(*command: str) -> tuple[int, str]:
    try:
        proc = subprocess.run(command, cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=30)
    except (OSError, subprocess.TimeoutExpired) as error:
        return 127, str(error)
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def version_of(text: str) -> tuple[int, ...]:
    found = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    return tuple(int(part) for part in found.groups() if part is not None) if found else ()


def shown(version: tuple[int, ...]) -> str:
    return ".".join(str(part) for part in version)


class Report:
    def __init__(self) -> None:
        self.items: list[dict[str, object]] = []

    def add(self, name: str, ok: bool, detail: str, *, needed: bool, purpose: str = "", fix: str = "") -> None:
        self.items.append({"name": name, "ok": ok, "needed": needed, "detail": detail,
                           "for": purpose, "fix": "" if ok else fix})

    def tool(self, name: str, command: str, *, at_least: tuple[int, ...] = (), needed: bool, purpose: str,
             fix: str) -> bool:
        path = shutil.which(command)
        if path is None:
            self.add(name, False, f"{command} is not on the PATH", needed=needed, purpose=purpose, fix=fix)
            return False
        code, text = run(path, "--version")
        version = version_of(text)
        if code != 0 or not version:
            self.add(name, False, f"{path} --version did not answer with a version", needed=needed,
                     purpose=purpose, fix=fix)
            return False
        if at_least and version < at_least:
            self.add(name, False, f"{shown(version)} at {path}, but {shown(at_least)} or newer is needed",
                     needed=needed, purpose=purpose, fix=fix)
            return False
        self.add(name, True, f"{shown(version)} at {path}", needed=needed, purpose=purpose)
        return True


def compiler(report: Report) -> str | None:
    """The C++ compiler CMake would take: $CXX, else g++, else clang++."""
    names = [os.environ["CXX"]] if os.environ.get("CXX") else ["g++", "clang++"]
    fix = ("install GCC or Clang (Fedora: dnf install gcc-c++; Ubuntu: apt install g++; "
           "MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-gcc)")
    for name in names:
        path = shutil.which(name)
        if path is None:
            continue
        code, text = run(path, "--version")
        first = text.splitlines()[0] if text else ""
        if code != 0:
            continue
        # C++20 is what the code needs: ask the compiler itself instead of comparing version numbers.
        code, _ = run(path, "-std=c++20", "-fsyntax-only", "-x", "c++", os.devnull)
        if code != 0:
            report.add("C++ compiler", False, f"{first} at {path} does not take -std=c++20", needed=True,
                       purpose="every build", fix=fix)
            return None
        report.add("C++ compiler", True, f"{first} at {path}", needed=True, purpose="every build")
        return path
    report.add("C++ compiler", False, f"none of {', '.join(names)} is on the PATH", needed=True,
               purpose="every build", fix=fix)
    return None


def links(report: Report, cxx: str | None, name: str, flags: list[str], *, purpose: str, fix: str) -> None:
    """Whether <cxx> links an empty program with <flags>: the only sure sign that the libraries are there."""
    if cxx is None:
        return
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "empty.cpp"
        source.write_text("int main() { return 0; }\n", encoding="utf-8")
        code, text = run(cxx, *flags, str(source), "-o", str(Path(directory) / "empty"))
    last = text.splitlines()[-1] if text else ""
    report.add(name, code == 0, f"{Path(cxx).name} {' '.join(flags)} links" if code == 0 else
               f"{Path(cxx).name} {' '.join(flags)} does not link: {last}", needed=False, purpose=purpose, fix=fix)


def submodules(report: Report) -> None:
    fix = "git submodule update --init --recursive"
    if not (ROOT / ".git").exists():
        present = (ROOT / "external" / "sil-kit" / "CMakeLists.txt").is_file()
        report.add("SIL Kit sources", present, "external/sil-kit" if present else "external/sil-kit is empty",
                   needed=True, purpose="every build",
                   fix="this is not a git checkout: put the SIL Kit sources of the pinned commit "
                       "(docs/dependencies.md) in external/sil-kit")
        return
    code, text = run("git", "submodule", "status", "--recursive")
    if code != 0:
        report.add("submodules", False, "git submodule status failed", needed=True, purpose="every build", fix=fix)
        return
    lines = [line for line in text.splitlines() if line.strip()]
    absent = [line.split()[1] for line in lines if line.startswith("-")]
    moved = [line.split()[1] for line in lines if line.startswith("+")]
    if absent or not lines:
        report.add("submodules", False, "not checked out: " + (", ".join(absent) or "external/sil-kit"),
                   needed=True, purpose="every build", fix=fix)
    elif moved:
        report.add("submodules", True, "checked out, but not at the commit the repository records: " +
                   ", ".join(moved) + f" (to go back: {fix})", needed=True, purpose="every build")
    else:
        report.add("submodules", True, f"{len(lines)} checked out", needed=True, purpose="every build")


def check() -> Report:
    report = Report()
    report.add("Python", sys.version_info >= (3, 11), f"{sys.version.split()[0]} at {sys.executable}", needed=True,
               purpose="the scripts in scripts/", fix="install Python 3.11 or newer")
    report.tool("git", "git", needed=True, purpose="the SIL Kit submodule and its patches", fix="install git")
    report.tool("CMake", "cmake", at_least=(3, 21), needed=True, purpose="every build",
                fix="install CMake 3.21 or newer (Fedora: dnf install cmake; Ubuntu: apt install cmake; "
                    "MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-cmake)")
    report.tool("Ninja", "ninja", needed=True, purpose="every preset in CMakePresets.json",
                fix="install Ninja (Fedora: dnf install ninja-build; Ubuntu: apt install ninja-build; "
                    "MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-ninja)")
    cxx = compiler(report)
    submodules(report)

    if WINDOWS:
        system = os.environ.get("MSYSTEM", "")
        report.add("MSYS2 shell", system == "UCRT64", f"MSYSTEM={system or '(not set)'}", needed=True,
                   purpose="the windows-mingw-* presets",
                   fix="start the 'MSYS2 UCRT64' shell: the presets expect the UCRT64 toolchain")

    report.tool("uv", "uv", needed=False, purpose="scripts/verify-python.py, the GUI server and its tests",
                fix="install uv (https://docs.astral.sh/uv/)")
    report.tool("Node.js", "node", at_least=(24, 15), needed=False, purpose="the GUI screen and client library (gui/)",
                fix="install Node.js 24.15 or newer (see gui/web/package.json)")
    report.tool("npm", "npm", at_least=(11, 12), needed=False, purpose="the GUI screen and client library (gui/)",
                fix="npm comes with Node.js; update it with: npm install -g npm")
    report.tool("clang++", "clang++", needed=False,
                purpose="the linux-clang-* presets, three of the five that scripts/verify.py --linux runs",
                fix="install Clang (Fedora: dnf install clang; Ubuntu: apt install clang)")
    if not WINDOWS:
        links(report, shutil.which("g++"), "libstdc++ (static)", ["-static-libstdc++", "-static-libgcc"],
              purpose="the linux-gcc-static preset", fix="Fedora: dnf install libstdc++-static (Ubuntu has it with g++)")
        links(report, shutil.which("g++"), "GCC sanitizers", ["-fsanitize=address,undefined"],
              purpose="the linux-gcc-asan preset", fix="Fedora: dnf install libasan libubsan (Ubuntu has them with g++)")
        links(report, shutil.which("g++"), "GCC ThreadSanitizer", ["-fsanitize=thread"],
              purpose="the linux-gcc-tsan preset", fix="Fedora: dnf install libtsan (Ubuntu has it with g++)")
        links(report, shutil.which("clang++"), "Clang sanitizers", ["-fsanitize=address,undefined"],
              purpose="the linux-clang-asan preset, one that scripts/verify.py --linux runs",
              fix="Fedora: dnf install compiler-rt (Ubuntu: apt install libclang-rt-dev)")
        links(report, shutil.which("clang++"), "Clang ThreadSanitizer", ["-fsanitize=thread"],
              purpose="the linux-clang-tsan preset, one that scripts/verify.py --linux runs",
              fix="Fedora: dnf install compiler-rt (Ubuntu: apt install libclang-rt-dev)")
    return report


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--json", action="store_true", help="write the report as JSON")
    args = parser.parse_args(argv)

    report = check()
    missing = [item for item in report.items if item["needed"] and not item["ok"]]
    if args.json:
        print(json.dumps({"vehicle_sil_doctor": 1, "result": "missing" if missing else "ok",
                          "items": report.items}, indent=2))
        return 1 if missing else 0

    width = max(len(str(item["name"])) for item in report.items)
    for item in report.items:
        state = "ok" if item["ok"] else ("MISSING" if item["needed"] else "absent")
        print(f"{str(item['name']).ljust(width)}  {state.ljust(7)}  {item['detail']}")
        if not item["ok"]:
            print(f"{' ' * width}           for: {item['for']}")
            print(f"{' ' * width}           fix: {item['fix']}")
    absent = sum(1 for item in report.items if not item["needed"] and not item["ok"])
    if missing:
        print(f"doctor: {len(missing)} missing: no build is possible yet")
    else:
        print("doctor: ok" + (f" ({absent} absent: each is needed for one part only)" if absent else ""))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
