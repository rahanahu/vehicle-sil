#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""Fetches the vehicle-sil tools (vehicle_sil_launch, the browser GUI) of this project's vehicle-sil
version into tools/, after checking the archive's SHA-256.

    python get_tools.py                      download from the vehicle-sil release, check, unpack
    python get_tools.py --archive FILE       use an archive you downloaded yourself (checked the same way)

The tools come only from the release named below (VERSION, URL), never "the latest". The template
archive of a release (vehicle-sil-template-<version>.zip) has the SHA-256 of each tools archive filled
in. A copy of the template from the vehicle-sil repository has none: give --sha256 with the value
from SHA256SUMS on the release page. To move to another vehicle-sil version, take the template
archive of that release (or change VERSION, SHA256 and GIT_TAG in CMakeLists.txt together).

Exit codes: 0 the tools are there, 1 the download, the check or the unpacking failed, 2 usage error
(no tools for this platform, no checksum). Python 3.9 or newer, standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import os
import platform
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

VERSION = "0.1.0"
URL = "https://github.com/rahanahu/vehicle-sil/releases/download/v{version}/{name}"
RELEASE_PAGE = "https://github.com/rahanahu/vehicle-sil/releases/tag/v{version}"
ARCHIVES = {  # platform: the archive's name on the release
    "linux-x86_64": "vehicle-sil-tools-{version}-linux-x86_64.tar.gz",
    "windows-x86_64": "vehicle-sil-tools-{version}-windows-x86_64.zip",
}
# The SHA-256 of each archive of VERSION (filled in by the release's template archive).
SHA256 = {
    "linux-x86_64": "",
    "windows-x86_64": "",
}
HERE = Path(__file__).resolve().parent


class Failure(Exception):
    """What went wrong and what to do (exit code 1)."""


class Usage(Exception):
    """A usage error (exit code 2)."""


def this_platform() -> str:
    machine = platform.machine().lower()
    if machine in ("x86_64", "amd64"):
        if sys.platform.startswith("linux"):
            return "linux-x86_64"
        if sys.platform == "win32":
            return "windows-x86_64"
    raise Usage(f"there are no prebuilt vehicle-sil tools for {sys.platform} {platform.machine()}: build "
                "vehicle_sil_launch from the vehicle-sil sources (README.md of vehicle-sil)")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url: str, to: Path) -> None:
    print(f"get_tools: downloading {url}", flush=True)
    try:
        with urllib.request.urlopen(url, timeout=60) as response, to.open("wb") as out:
            shutil.copyfileobj(response, out)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise Failure(f"{url} does not exist (HTTP 404): vehicle-sil v{VERSION} may not be published yet, or has "
                          f"no tools for this platform; see {RELEASE_PAGE.format(version=VERSION)}. Until it is, "
                          "build vehicle-sil from its sources (README.md of vehicle-sil)") from None
        raise Failure(f"cannot download {url} (HTTP {e.code} {e.reason}): try again later, or download the file in "
                      "a browser and pass it with --archive") from None
    except (OSError, http.client.HTTPException) as e:  # URLError is an OSError; IncompleteRead is neither
        raise Failure(f"cannot download {url} ({e}): check the network (a proxy: set HTTPS_PROXY), or download "
                      "the file in a browser and pass it with --archive") from None


def safe_member(name: str) -> PurePosixPath:
    path = PurePosixPath(name.replace("\\", "/"))
    if path.is_absolute() or ".." in path.parts or (path.parts and ":" in path.parts[0]):
        raise Failure(f"the archive has an unsafe path {name!r}: it is not a vehicle-sil tools archive; "
                      "do not use it")
    return path


def unpack(archive: Path, into: Path) -> None:
    """Unpacks into the directory `into` (new and empty); refuses paths that leave it and links."""
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                safe_member(info.filename)
                if (info.external_attr >> 16) & 0o170000 == 0o120000:
                    raise Failure(f"the archive has a symbolic link {info.filename!r}: it is not a vehicle-sil "
                                  "tools archive; do not use it")
            z.extractall(into)
        return
    with tarfile.open(archive, "r:gz") as t:
        members = t.getmembers()
        for m in members:
            safe_member(m.name)
            if not (m.isfile() or m.isdir()):
                raise Failure(f"the archive has {m.name!r}, which is not a file or a directory: it is not a "
                              "vehicle-sil tools archive; do not use it")
        if hasattr(tarfile, "data_filter"):
            t.extractall(into, members=members, filter="data")
        else:  # Python before 3.11.4 / 3.12: the checks above stand in for the filter
            t.extractall(into, members=members)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="get_tools.py", description=__doc__.split("\n\n")[0],
        epilog=f"version {VERSION}; the tools go to {HERE / 'tools'}",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--archive", type=Path, metavar="FILE",
                        help="use this archive instead of downloading it (its SHA-256 is checked the same way)")
    parser.add_argument("--sha256", metavar="HEX",
                        help="the archive's SHA-256, when this copy of the template has none (SHA256SUMS of the release)")
    parser.add_argument("--platform", choices=sorted(ARCHIVES), help="the tools of another platform (default: this one)")
    parser.add_argument("--dir", type=Path, default=HERE / "tools", help="where the tools go (default: %(default)s)")
    args = parser.parse_args(argv)
    try:
        return fetch(args)
    except (tarfile.TarError, zipfile.BadZipFile) as e:
        print(f"get_tools: error: the archive cannot be read ({e}): download it again", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"get_tools: error: {e.strerror or e} ({e.filename or ''}): check that the directory {args.dir} "
              "is writable and that nothing in it is in use", file=sys.stderr)
        return 1
    except Usage as e:
        print(f"get_tools: error: {e}", file=sys.stderr)
        return 2
    except Failure as e:
        print(f"get_tools: error: {e}", file=sys.stderr)
        return 1


def fetch(args: argparse.Namespace) -> int:
    plat = args.platform or this_platform()
    name = ARCHIVES[plat].format(version=VERSION)
    top = name.removesuffix(".zip").removesuffix(".tar.gz")
    target = args.dir / top
    if (target / "VERSION").is_file() and (target / "VERSION").read_text(encoding="utf-8").strip() == VERSION:
        report(target, plat, "already there")
        return 0
    expected = (args.sha256 or SHA256[plat]).strip().lower()
    if not expected:
        raise Usage(f"this copy of the template has no SHA-256 for {name}: pass --sha256 with the value from "
                    f"SHA256SUMS of the release v{VERSION}, or use the template archive of the release "
                    f"(vehicle-sil-template-{VERSION}.zip), which has it")
    if len(expected) != 64 or any(c not in "0123456789abcdef" for c in expected):
        raise Usage(f"--sha256 {args.sha256!r} is not a SHA-256 (64 hexadecimal digits)")

    args.dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=".get-tools-", dir=args.dir))
    try:
        if args.archive:
            archive = args.archive
            if not archive.is_file():
                raise Usage(f"--archive {archive}: no such file")
        else:
            archive = work / name
            download(URL.format(version=VERSION, name=name), archive)
        actual = sha256_of(archive)
        if actual != expected:
            raise Failure(f"{archive} has SHA-256 {actual}, not {expected} (the one of {name}): the file is "
                          "damaged or not the release's; download it again, and do not use it if it still differs")
        unpacked = work / "unpacked"
        unpacked.mkdir()
        unpack(archive, unpacked)
        found = unpacked / top
        version = found / "VERSION"
        if not version.is_file() or version.read_text(encoding="utf-8").strip() != VERSION:
            raise Failure(f"{archive} does not hold {top}/VERSION with {VERSION}: it is not the tools archive "
                          f"of vehicle-sil {VERSION}")
        if target.exists():
            shutil.rmtree(target)
        os.replace(found, target)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    report(target, plat, "unpacked")
    return 0


def report(target: Path, plat: str, what: str) -> None:
    bin_dir = target / "bin"
    print(f"get_tools: vehicle-sil {VERSION} tools {what} in {target}")
    print("get_tools: put their bin directory on PATH, in each new shell:")
    if plat.startswith("windows"):
        print(f"  MSYS2 shell:        export PATH=\"$(cygpath -u '{bin_dir}'):$PATH\"")
        print(f"  Command Prompt:     set \"PATH={bin_dir};%PATH%\"")
        print(f"  PowerShell:         $env:PATH = \"{bin_dir};$env:PATH\"")
    else:
        print(f"  export PATH=\"{bin_dir}:$PATH\"")


if __name__ == "__main__":
    sys.exit(main())
