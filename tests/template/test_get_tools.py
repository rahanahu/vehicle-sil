# SPDX-License-Identifier: MIT
"""Tests of template/get_tools.py and of `scripts/package.py template` and `version`, without the network:
archives made here stand in for the release's.

    python tests/template/test_get_tools.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import hashlib
import importlib.util
import io
import re
import shutil
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
                 "simulation.live.yaml", "simulation.live.dashboard.yaml",
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


def test_schemas(tmp: Path) -> None:
    """get_tools.py copies the JSON Schemas of the tools over the project's, and only those."""
    def archive_with(directory: str, schemas: dict[str, bytes]) -> Path:
        (tmp / directory).mkdir()
        path = tmp / directory / f"{LINUX}.tar.gz"
        with tarfile.open(path, "w:gz") as t:
            for name, data in [("VERSION", f"{VERSION}\n".encode()), *((f"schema/{n}", d) for n, d in schemas.items())]:
                info = tarfile.TarInfo(f"{LINUX}/{name}")
                info.size, info.mode = len(data), 0o644
                t.addfile(info, io.BytesIO(data))
        return path

    new = {"simulation.schema.json": b'{"title": "new simulation"}\n', "scenario.schema.json": b'{"title": "new scenario"}\n'}
    archive = archive_with("new", {**new, "notes.txt": b"not a schema\n"})
    tools, schema = tmp / "tools", tmp / "project" / "schema"
    schema.mkdir(parents=True)
    (schema / "simulation.schema.json").write_bytes(b'{"title": "old simulation"}\n')
    (schema / "own.schema.json").write_bytes(b'{"title": "the project\'s own"}\n')
    common = ["--platform", "linux-x86_64", "--dir", tools, "--schema-dir", schema]

    def contents() -> dict[str, bytes]:
        return {p.name: p.read_bytes() for p in sorted(schema.iterdir())}

    def lines(p: subprocess.CompletedProcess) -> list[str]:
        return [line for line in p.stdout.splitlines() if "JSON Schemas" in line]

    p = get_tools(GET_TOOLS, *common, "--archive", archive, "--sha256", sha256(archive))
    check(p.returncode == 0, f"an archive with schema/: exit code 0 ({p.returncode}: {p.stderr})")
    check(contents() == {**new, "own.schema.json": b'{"title": "the project\'s own"}\n'},
          f"the schemas of the tools replace the old one and add the missing one; the project's own file and the "
          f"file that is not .json stay out of it ({contents()})")
    said = lines(p)
    check(len(said) == 1 and "scenario.schema.json, simulation.schema.json" in said[0] and str(schema) in said[0]
          and "own.schema.json" not in said[0], f"one line says which schemas were copied and where: {p.stdout}")

    p = get_tools(GET_TOOLS, *common)
    check(p.returncode == 0 and "already there" in p.stdout and lines(p) == [],
          f"a second time, the schemas are the same: nothing copied, nothing said ({p.returncode}: {p.stdout} {p.stderr})")

    (schema / "scenario.schema.json").write_bytes(b"{}\n")
    p = get_tools(GET_TOOLS, *common)
    said = lines(p)
    check(p.returncode == 0 and contents()["scenario.schema.json"] == new["scenario.schema.json"] and len(said) == 1
          and "scenario.schema.json" in said[0] and "simulation.schema.json" not in said[0],
          f"tools already there, one schema differs: that one is written again and named ({p.returncode}: {p.stdout} {p.stderr})")

    absent = tmp / "absent" / "schema"
    p = get_tools(GET_TOOLS, "--platform", "linux-x86_64", "--dir", tools, "--schema-dir", absent)
    check(p.returncode == 0 and {x.name for x in absent.iterdir()} == set(new),
          f"a project without schema/: the directory is made ({p.returncode}: {p.stdout} {p.stderr})")

    plain = tar_archive(tmp / f"{LINUX}.tar.gz", LINUX)
    none = tmp / "none" / "schema"
    p = get_tools(GET_TOOLS, "--platform", "linux-x86_64", "--dir", tmp / "tools-plain", "--schema-dir", none,
                  "--archive", plain, "--sha256", sha256(plain))
    check(p.returncode == 0 and not none.exists() and lines(p) == [],
          f"tools without schema/: nothing copied, no directory made ({p.returncode}: {p.stdout} {p.stderr})")

    blocked = tmp / "blocked"
    blocked.write_bytes(b"a file where the directory would be")
    p = get_tools(GET_TOOLS, "--platform", "linux-x86_64", "--dir", tools, "--schema-dir", blocked)
    check(p.returncode == 1 and "--schema-dir" in p.stderr and "Traceback" not in p.stderr,
          f"schema/ cannot be written: exit code 1 and what to do, no traceback ({p.returncode}: {p.stderr})")

    # What the tools archive must hold (package.py verify looks for these) covers every schema of the template, so
    # that none of a project's copies stays at the old version.
    template = {x.name for x in (ROOT / "template" / "schema").iterdir()}
    packaged = set(re.findall(r'"schema/([a-z_]+\.schema\.json)"', PACKAGE.read_text(encoding="utf-8")))
    check(template and template <= packaged,
          f"every schema of template/schema is in the tools archive's schema/ (only in the template: "
          f"{sorted(template - packaged)}): add it to the tools archive in scripts/package.py (tools and verify)")


def test_version() -> None:
    p = subprocess.run([sys.executable, str(PACKAGE), "version"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(p.returncode == 0 and p.stdout.strip() == VERSION,
          f"package.py version: the version agrees everywhere ({p.returncode}: {p.stdout} {p.stderr})")
    p = subprocess.run([sys.executable, str(PACKAGE), "version", "--expect", "v0.0.0-not"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(p.returncode == 1 and "tag" in p.stderr, f"package.py version --expect another tag: exit code 1 ({p.stderr})")


def test_changelog() -> None:
    """The CHANGELOG.md heading of a version (Keep a Changelog), and the release notes that release.yml cuts."""
    spec = importlib.util.spec_from_file_location("package", PACKAGE)
    package = importlib.util.module_from_spec(spec)
    # package.py imports the modules next to it (as a script, its directory is on the path).
    sys.path.insert(0, str(PACKAGE.parent))
    try:
        spec.loader.exec_module(package)
    finally:
        sys.path.remove(str(PACKAGE.parent))
    head = "# Changelog\n\nIntro.\n\n"
    body = "The first version.\n\n### Added\n\n#### Launcher\n\n- a\n\n### Fixed\n\n- b\n"
    older = "\n## [0.0.9] - 2026-01-02\n\n### Added\n\n- old\n"
    links = "\n[0.1.0]: https://example.invalid/v0.1.0\n"
    cases = [  # (text, release, what the problem says, or None)
        (head + "## [0.1.0] - Unreleased\n\n" + body, False, None),
        (head + "## [0.1.0] - 2026-02-03\n\n" + body, True, None),
        (head + "## [Unreleased]\n\n## [0.1.0] - 2026-02-03\n\n" + body, True, None),
        (head + "## [0.1.0] - Unreleased\n\n" + body, True, 'write the release date in its place ("## [0.1.0] - YYYY-MM-DD") in the commit that is tagged v0.1.0'),
        (head + "## 0.1.0 (unreleased)\n\n" + body, False, '"## 0.1.0 (unreleased)", which is not in the form'),
        (head + "## [0.1.0]\n\n" + body, False, 'write it as "## [0.1.0] - Unreleased"'),
        (head + "## [0.1.0] - soon\n\n" + body, False, "which is not in the form of Keep a Changelog"),
        (head + "## [0.1.0] - 2026-02-30\n\n" + body, True, "is not a date"),
        (head + "## [0.1.01] - 2026-02-03\n\n" + body, False, 'no heading "## [0.1.0] - Unreleased"'),
        (head, False, 'no heading "## [0.1.0] - Unreleased"'),
    ]
    for text, release, want in cases:
        got = package.changelog_problems(text, "0.1.0", release)
        first = text.splitlines()[4] if len(text.splitlines()) > 4 else "(no heading)"
        if want is None:
            check(got == [], f"changelog {first!r} (release {release}): accepted ({got})")
        else:
            check(len(got) == 1 and want in got[0], f"changelog {first!r} (release {release}): refused with {want!r} ({got})")
    check(package.changelog_problems((ROOT / "CHANGELOG.md").read_text(encoding="utf-8"), VERSION) == [],
          "CHANGELOG.md has the heading of the version")

    # The release notes: the awk program of release.yml on a made-up CHANGELOG.md.
    workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
    m = re.search(r"awk -v h=\"## \[\$version\] - \" '([^']*)'", workflow)
    check(m is not None, "release.yml cuts the release notes with awk -v h=\"## [$version] - \" '<program>'")
    awk = shutil.which("awk")
    if m is None or awk is None:
        print("note: no awk here, the release notes are not tried")
        return
    for text in (head + "## [0.1.0] - 2026-02-03\n\n" + body + older + links,
                 head + "## [Unreleased]\n\n- next\n\n## [0.1.0] - 2026-02-03\n\n" + body + links):
        p = subprocess.run([awk, "-v", "h=## [0.1.0] - ", m.group(1)], input=text, capture_output=True, text=True,
                           encoding="utf-8")
        check(p.returncode == 0 and p.stdout.strip() == body.strip(),
              f"release.yml's awk cuts the body of \"## [0.1.0] - <date>\" ({p.returncode}: {p.stdout!r} {p.stderr})")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        for test in (test_fetch, test_broken, test_unsafe, test_template_archive, test_schemas):
            d = Path(tmp) / test.__name__
            d.mkdir()
            test(d)
    test_version()
    test_changelog()
    print(f"{'FAILED' if failures else 'passed'}: {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
