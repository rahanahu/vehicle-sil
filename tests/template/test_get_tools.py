# SPDX-License-Identifier: MIT
"""Tests of template/get_tools.py and of `scripts/package.py template` and `version`, without the network:
archives made here stand in for the release's.

    python tests/template/test_get_tools.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import hashlib
import io
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GET_TOOLS = ROOT / "template" / "get_tools.py"
PACKAGE = ROOT / "scripts" / "package.py"
VERSION = re.search(r'(?m)^VERSION = "([^"]+)"', GET_TOOLS.read_text(encoding="utf-8")).group(1)
LINUX = f"vehicle-sil-tools-{VERSION}-linux-x86_64"
WINDOWS = f"vehicle-sil-tools-{VERSION}-windows-x86_64"
failures: list[str] = []


def check(condition: bool, what: str) -> None:
    if not condition:
        failures.append(what)
        print(f"FAILED: {what}")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tar_archive(path: Path, top: str, extra: list[tarfile.TarInfo] | None = None, version: str = VERSION) -> Path:
    with tarfile.open(path, "w:gz") as t:
        for name, data, mode in [(f"{top}/VERSION", f"{version}\n".encode(), 0o644),
                                 (f"{top}/bin/vehicle_sil_launch", b"#!/bin/sh\n", 0o755)]:
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), mode
            t.addfile(info, io.BytesIO(data))
        for info in extra or []:
            t.addfile(info, io.BytesIO(b"x" * info.size) if info.isfile() else None)
    return path


def zip_archive(path: Path, top: str, extra: list[str] | None = None) -> Path:
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(f"{top}/VERSION", f"{VERSION}\n")
        z.writestr(f"{top}/bin/vehicle_sil_launch.exe", b"MZ")
        for name in extra or []:
            z.writestr(name, b"x")
    return path


def get_tools(script: Path, *args: str | Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(script), *map(str, args)], capture_output=True, text=True, encoding="utf-8", errors="replace")


def test_fetch(tmp: Path) -> None:
    archive = tar_archive(tmp / f"{LINUX}.tar.gz", LINUX)
    tools = tmp / "tools"
    common = ["--platform", "linux-x86_64", "--dir", tools, "--archive", archive]

    p = get_tools(GET_TOOLS, *common)
    check(p.returncode == 2 and "--sha256" in p.stderr,
          f"without a SHA-256 in the script or --sha256, exit code 2 and how to give one ({p.returncode}: {p.stderr})")
    check(not (tools / LINUX).exists(), "without a SHA-256, nothing is unpacked")

    p = get_tools(GET_TOOLS, *common, "--sha256", "0" * 64)
    check(p.returncode == 1 and "SHA-256" in p.stderr and "not" in p.stderr,
          f"a wrong SHA-256: exit code 1 and both values ({p.returncode}: {p.stderr})")
    check(not (tools / LINUX).exists(), "a wrong SHA-256 unpacks nothing")

    p = get_tools(GET_TOOLS, *common, "--sha256", "12ab")
    check(p.returncode == 2, f"--sha256 that is not one: exit code 2 ({p.returncode}: {p.stderr})")

    p = get_tools(GET_TOOLS, *common, "--sha256", sha256(archive).upper())
    check(p.returncode == 0, f"the right SHA-256 (in capitals too): exit code 0 ({p.returncode}: {p.stderr})")
    check((tools / LINUX / "VERSION").read_text(encoding="utf-8").strip() == VERSION, "VERSION unpacked")
    launcher = tools / LINUX / "bin" / "vehicle_sil_launch"
    check(launcher.is_file() and (sys.platform == "win32" or launcher.stat().st_mode & 0o100 != 0),
          "the launcher unpacked, executable")
    check(f"{tools / LINUX / 'bin'}" in p.stdout and "PATH" in p.stdout, f"says how to put bin on PATH: {p.stdout}")
    check(not any(x.name.startswith(".get-tools-") for x in tools.iterdir()), "no work directory left behind")

    p = get_tools(GET_TOOLS, "--platform", "linux-x86_64", "--dir", tools)
    check(p.returncode == 0 and "already there" in p.stdout,
          f"a second time: already there, no download ({p.returncode}: {p.stdout} {p.stderr})")

    (tmp / "wrong").mkdir()
    wrong = tar_archive(tmp / "wrong" / f"{LINUX}.tar.gz", LINUX, version="9.9.9")
    p =get_tools(GET_TOOLS, "--platform", "linux-x86_64", "--dir", tmp / "tools-wrong", "--archive", wrong,
                  "--sha256", sha256(wrong))
    check(p.returncode == 1 and "VERSION" in p.stderr, f"another version inside: exit code 1 ({p.returncode}: {p.stderr})")


def test_broken(tmp: Path) -> None:
    archive = tmp / f"{LINUX}.tar.gz"
    archive.write_bytes(b"not a gzip file")
    p = get_tools(GET_TOOLS, "--platform", "linux-x86_64", "--dir", tmp / "tools", "--archive", archive,
                  "--sha256", sha256(archive))
    check(p.returncode == 1 and "cannot be read" in p.stderr and "Traceback" not in p.stderr,
          f"an archive that cannot be read: exit code 1, no traceback ({p.returncode}: {p.stderr})")


def test_unsafe(tmp: Path) -> None:
    outside = tmp / "evil"
    for what, extra in [
        ("a path out of the directory", [tarfile.TarInfo("../evil")]),
        ("an absolute path", [tarfile.TarInfo("/tmp/evil-vehicle-sil")]),
        ("a symbolic link", [tarfile.TarInfo(f"{LINUX}/bin/link")]),
    ]:
        for info in extra:
            if what == "a symbolic link":
                info.type, info.linkname = tarfile.SYMTYPE, "/etc/passwd"
            else:
                info.size = 1
        d = tmp / f"unsafe-{len(what)}"
        d.mkdir()
        archive = tar_archive(d / f"{LINUX}.tar.gz", LINUX, extra)
        p = get_tools(GET_TOOLS, "--platform", "linux-x86_64", "--dir", d / "tools", "--archive", archive,
                      "--sha256", sha256(archive))
        check(p.returncode == 1 and "do not use it" in p.stderr, f"{what}: refused ({p.returncode}: {p.stderr})")
        check(not (d / "tools" / LINUX).exists(), f"{what}: nothing unpacked")
    check(not outside.exists(), "nothing written outside")

    d = tmp / "unsafe-zip"
    d.mkdir()
    archive = zip_archive(d / f"{WINDOWS}.zip", WINDOWS, ["../evil.txt"])
    p = get_tools(GET_TOOLS, "--platform", "windows-x86_64", "--dir", d / "tools", "--archive", archive,
                  "--sha256", sha256(archive))
    check(p.returncode == 1 and "unsafe path" in p.stderr, f"a zip with ../: refused ({p.returncode}: {p.stderr})")


def test_template_archive(tmp: Path) -> None:
    """package.py template fills the SHA-256 of both tools archives into get_tools.py; that copy then needs no --sha256."""
    dist = tmp / "dist"
    dist.mkdir()
    linux = tar_archive(dist / f"{LINUX}.tar.gz", LINUX)
    windows = zip_archive(dist / f"{WINDOWS}.zip", WINDOWS)
    client = dist / f"vehicle-sil-gui-client-{VERSION}.tgz"
    p = subprocess.run([sys.executable, str(PACKAGE), "template", "--out", str(dist)], capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(p.returncode == 2 and client.name in p.stderr,
          f"the client library's package missing: exit code 2 ({p.returncode}: {p.stderr})")
    client.write_bytes(b"stands for the npm package of the client library")
    p = subprocess.run([sys.executable, str(PACKAGE), "template", "--out", str(dist)], capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(p.returncode == 0, f"package.py template: exit code 0 ({p.returncode}: {p.stdout} {p.stderr})")
    if p.returncode != 0:
        return
    template = dist / f"vehicle-sil-template-{VERSION}.zip"
    sums = (dist / "SHA256SUMS").read_text(encoding="utf-8")
    for path in (linux, windows, template, client):
        check(f"{sha256(path)}  {path.name}\n" in sums, f"SHA256SUMS lists {path.name}")
    unpacked = tmp / "unpacked"
    with zipfile.ZipFile(template) as z:
        names = z.namelist()
        z.extractall(unpacked)
    top = unpacked / f"vehicle-sil-template-{VERSION}"
    for name in ["LICENSE", "CMakeLists.txt", "CMakePresets.json", "README.md", ".gitignore", "get_tools.py", "simulation.yaml",
                 "engine/main.cpp", "dashboard/main.cpp", "scenarios/throttle_step.yaml"]:
        check((top / name).is_file(), f"the template archive has {name}")
    check(not any(re.search(r"/(build|bin|tools|__pycache__)/", n) for n in names), f"no build output in it: {names}")
    check((top / "LICENSE").read_text(encoding="utf-8").startswith("MIT No Attribution"),
          "the template archive's LICENSE is MIT-0")
    script = (top / "get_tools.py").read_text(encoding="utf-8")
    check(f'"linux-x86_64": "{sha256(linux)}"' in script and f'"windows-x86_64": "{sha256(windows)}"' in script,
          "get_tools.py of the template archive has both SHA-256")
    p = get_tools(top / "get_tools.py", "--platform", "linux-x86_64", "--archive", linux)
    check(p.returncode == 0 and (top / "tools" / LINUX / "VERSION").is_file(),
          f"its get_tools.py checks the archive without --sha256 ({p.returncode}: {p.stderr})")

    linux.unlink()
    p = subprocess.run([sys.executable, str(PACKAGE), "template", "--out", str(dist)], capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(p.returncode == 2 and LINUX in p.stderr, f"a tools archive missing: exit code 2 ({p.returncode}: {p.stderr})")


def test_version() -> None:
    p = subprocess.run([sys.executable, str(PACKAGE), "version"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(p.returncode == 0 and p.stdout.strip() == VERSION,
          f"package.py version: the version agrees everywhere ({p.returncode}: {p.stdout} {p.stderr})")
    p = subprocess.run([sys.executable, str(PACKAGE), "version", "--expect", "v0.0.0-not"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(p.returncode == 1 and "tag" in p.stderr, f"package.py version --expect another tag: exit code 1 ({p.stderr})")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        for test in (test_fetch, test_broken, test_unsafe, test_template_archive):
            d = Path(tmp) / test.__name__
            d.mkdir()
            test(d)
    test_version()
    print(f"{'FAILED' if failures else 'passed'}: {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
