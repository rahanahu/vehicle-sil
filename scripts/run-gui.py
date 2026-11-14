#!/usr/bin/env python3
"""Start the browser GUI from this repository: the built example of examples/ecu_main, at the speed of a clock.

  scripts/run-gui.py              find what the GUI needs, start the GUI server, print the URL to open
  scripts/run-gui.py --dry-run    only say what it found and the command it would run

It builds nothing. It looks for a vehicle_sil_launch with the Control Server under build/ (or takes --launcher),
the configuration next to it (examples/ecu_main/simulation.live.yaml of that build: no stop_at_ms, speed 1), the
built screen (gui/web/dist) and uv, and then runs `uv run vehicle-sil-gui --launcher <launcher> <configuration>`.
For each thing that is missing it prints the commands that make it. Ctrl+C stops the server.

Exit codes: 1 something is not built or not installed (the commands to run are on stderr), 2 usage error (an
argument, or a --launcher or a configuration that is not there); otherwise the GUI server's (0 when it was
stopped). Requires Python 3.11+ (standard library only).

Known limit on Windows: Ctrl+C stops everything, but ending only this process (a tool's terminate(), "End
task") leaves uv, the GUI server and a running simulation behind. A tool stops the whole process tree
(taskkill /T /PID <pid>) or sends Ctrl+Break.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parent.parent
WINDOWS = os.name == "nt" or sys.platform in ("msys", "cygwin")
LAUNCHER = "vehicle_sil_launch.exe" if WINDOWS else "vehicle_sil_launch"
EXAMPLE = Path("examples") / "ecu_main"
CONFIG = "simulation.live.yaml"
CONTROL = "control-server"
NAME = "run-gui"

Probe = Callable[[Path], "list[str] | None"]


class Missing(Exception):
    """Something the GUI needs is not there: the message says what, and the commands that make it."""


class Usage(Exception):
    """An argument names a file that is not there: the message says which, and what to give."""


def features_of(launcher: Path) -> list[str] | None:
    """The features of `<launcher> version --json`, or None if it cannot be run or says something else."""
    try:
        proc = subprocess.run([str(launcher), "version", "--json"], stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=30)
        features = json.loads(proc.stdout).get("features") if proc.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired, ValueError, AttributeError):
        return None
    return [f for f in features if isinstance(f, str)] if isinstance(features, list) else None


def shown(path: Path, root: Path) -> str:
    """The path as a person types it from the repository's directory."""
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def build_commands(windows: bool = WINDOWS) -> str:
    if windows:
        return ("  cmake --preset windows-mingw-debug -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON\n"
                "  cmake --build --preset windows-mingw-debug\n"
                "(in the MSYS2 UCRT64 shell. The Control Server needs gRPC: CMake takes an installed one, "
                "'pacman -S mingw-w64-ucrt-x86_64-grpc'; where it finds none, the first build downloads and "
                "builds gRPC, which takes a while)")
    return ("  cmake --preset linux-gcc-control\n"
            "  cmake --build --preset linux-gcc-control\n"
            "(the Control Server needs gRPC: CMake takes an installed one; where it finds none, the first build "
            "downloads and builds gRPC, which takes a while)")


def find_launcher(root: Path, probe: Probe = features_of, name: str = LAUNCHER, windows: bool = WINDOWS) -> Path:
    """The newest build/*/tools/launch/vehicle_sil_launch that has the Control Server."""
    found = sorted(root.glob(f"build/*/tools/launch/{name}"), key=lambda p: p.stat().st_mtime, reverse=True)
    without: list[str] = []
    for launcher in found:
        features = probe(launcher)
        if features is not None and CONTROL in features:
            return launcher
        without.append(f"  {shown(launcher, root)}: " +
                       ("built without the Control Server" if features is not None else "'version --json' failed"))
    if not found:
        raise Missing(f"no {name} under build/: nothing is built yet. Build with the Control Server (the GUI reads "
                      f"and writes the ECUs' variables through it), then run this again:\n{build_commands(windows)}")
    raise Missing(f"no {name} with the Control Server under build/ (the GUI reads and writes the ECUs' variables "
                  "through it). Found:\n" + "\n".join(without) + "\nBuild one with the Control Server, then run this "
                  f"again:\n{build_commands(windows)}\nTo use one of the above anyway (the screen then says that "
                  "variables are unavailable), pass it with --launcher.")


def find_config(launcher: Path, root: Path) -> Path:
    """examples/ecu_main/simulation.live.yaml of the launcher's build, with the two ECUs next to it."""
    build = launcher.parent.parent.parent  # <build>/tools/launch/vehicle_sil_launch
    example = build / EXAMPLE
    config = example / CONFIG
    suffix = ".exe" if launcher.suffix.lower() == ".exe" else ""
    ecus = [example / "engine" / f"vehicle_sil_engine_ecu{suffix}", example / "dashboard" / f"vehicle_sil_dashboard_ecu{suffix}"]
    absent = [p for p in [config, *ecus] if not p.is_file()]
    if absent and not (build / "CMakeCache.txt").is_file():
        # A launcher from somewhere else (unpacked tools, an installation): there is no build to finish.
        raise Usage(f"--launcher {shown(launcher, root)} is not in a build directory of this repository, so there "
                    "is no example next to it. Give the configuration to show: "
                    f"scripts/run-gui.py --launcher {shlex.quote(shown(launcher, root))} <simulation.yaml>")
    if absent:
        raise Missing(f"{shown(absent[0], root)} is not there: the example of examples/ecu_main is not built next "
                      f"to {shown(launcher, root)}. Build it, then run this again:\n"
                      f"  cmake --build {shlex.quote(shown(build, root))}\n"
                      "Or give a configuration of your own: scripts/run-gui.py <simulation.yaml>")
    return config


def find_web(root: Path) -> Path:
    index = root / "gui" / "web" / "dist" / "browser" / "index.html"
    if not index.is_file():
        raise Missing(f"the screen is not built ({shown(index, root)} is not there). Build it, then run this again:\n"
                      "  npm --prefix gui/web ci\n  npm --prefix gui/web run build\n"
                      "(needs Node.js and npm; scripts/doctor.py says whether this computer has the versions)")
    return index.parent


def find_uv() -> str:
    uv = shutil.which("uv")
    if uv is None:
        raise Missing("uv is not on the PATH (it runs the GUI server and installs its Python packages). "
                      "Install uv (https://docs.astral.sh/uv/), then run this again")
    return uv


def plan(args: argparse.Namespace, root: Path = ROOT, probe: Probe = features_of, uv: Callable[[], str] = find_uv,
         cwd: Path | None = None, windows: bool = WINDOWS) -> tuple[list[str], list[str]]:
    """The command to run and the lines to say first. Raises Missing with everything that is not there, or
    Usage (with the same text) when one of those is a file that an argument names."""
    problems: list[str] = []
    usage = False
    cwd = Path.cwd() if cwd is None else cwd
    notes: list[str] = []
    launcher: Path | None = None
    config: Path | None = None
    if args.launcher:
        launcher = Path(args.launcher).resolve()
        if windows and not launcher.is_file() and launcher.with_name(launcher.name + ".exe").is_file():
            launcher = launcher.with_name(launcher.name + ".exe")  # (the name as the docs of Linux write it)
        features = probe(launcher) if launcher.is_file() else None
        if features is None:
            usage = usage or not launcher.is_file()
            problems.append(f"--launcher {args.launcher}: " + ("no such file" if not launcher.is_file() else
                            "'version --json' did not say its features") + ". Give the path of a built "
                            "vehicle_sil_launch (build/<preset>/tools/launch/), or leave --launcher out to let "
                            "this script look under build/")
            launcher = None
        elif CONTROL not in features:
            notes.append(f"warning: {shown(launcher, root)} is built without the Control Server: the screen will "
                         "say that variables are unavailable (build with the preset linux-gcc-control, or with "
                         "-DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON)")
    else:
        try:
            launcher = find_launcher(root, probe, windows=windows)
        except Missing as e:
            problems.append(str(e))
    if args.config:
        config = Path(args.config).resolve()
        if not config.is_file():
            usage = True
            problems.append(f"{args.config}: no such file. Give a simulation configuration (a .yaml file), or leave "
                            f"it out for the example ({(EXAMPLE / CONFIG).as_posix()} of the launcher's build)")
    elif launcher is not None:
        try:
            config = find_config(launcher, root)
        except Usage as e:
            usage = True
            problems.append(str(e))
        except Missing as e:
            problems.append(str(e))
    try:
        find_web(root)
    except Missing as e:
        problems.append(str(e))
    runner = ""
    try:
        runner = uv()
    except Missing as e:
        problems.append(str(e))
    if problems:
        lines = [f"{NAME}: {p}" for p in problems]
        if cwd.resolve() != root.resolve() and any("\n  " in p for p in problems):
            # The commands and the paths are written from the repository's directory.
            lines.insert(0, f"{NAME}: run the commands below in {root} (cd {shlex.quote(str(root))})")
        raise (Usage if usage else Missing)("\n".join(lines))
    assert launcher is not None and config is not None
    command = [runner, "run", "--project", str(root), "vehicle-sil-gui", "--launcher", str(launcher)]
    if args.port is not None:
        command += ["--port", str(args.port)]
    command += [flag for flag, on in (("--no-browser", args.no_browser), ("--json", args.json)) if on]
    command.append(str(config))
    notes.insert(0, f"launcher {shown(launcher, root)}, configuration {shown(config, root)}")
    return command, notes


def parse(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="scripts/run-gui.py", description=__doc__.split("\n\n")[0],
                                     epilog="examples:\n"
                                            "  scripts/run-gui.py                       # the example, in the browser\n"
                                            "  scripts/run-gui.py --dry-run             # what is missing, what would run\n"
                                            "  scripts/run-gui.py --no-browser --json   # for a tool: one JSON line with the URL\n"
                                            "  scripts/run-gui.py --launcher build/linux-gcc-control/tools/launch/vehicle_sil_launch my/simulation.yaml",
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("config", nargs="?", help="a simulation configuration to show first (default: "
                        f"{(EXAMPLE / CONFIG).as_posix()} of the launcher's build)")
    parser.add_argument("--launcher", metavar="PATH", help="the vehicle_sil_launch to use (default: the newest "
                        "build/*/tools/launch/vehicle_sil_launch that has the Control Server)")
    parser.add_argument("--port", type=int, help="the GUI server's port on 127.0.0.1 (default: a free one)")
    parser.add_argument("--no-browser", action="store_true", help="do not open the browser (the URL is still printed)")
    parser.add_argument("--json", action="store_true", help="the GUI server prints the URL as one JSON line "
                        '({"vehicle_sil_gui": 1, "url": ..., "port": ...}) instead of the bare URL')
    parser.add_argument("--dry-run", action="store_true", help="start nothing: say what was found and the command, "
                        "or what is missing (exit code 0, 1 or 2)")
    return parser.parse_args(argv)


def main(argv: list[str], root: Path = ROOT, probe: Probe = features_of, uv: Callable[[], str] = find_uv) -> int:
    args = parse(argv)
    try:
        command, notes = plan(args, root, probe, uv)
    except Usage as e:
        print(e, file=sys.stderr)
        return 2
    except Missing as e:
        print(e, file=sys.stderr)
        return 1
    for note in notes:
        print(f"{NAME}: {note}", file=sys.stderr)
    if args.dry_run:
        print(f"{NAME}: would run: {shlex.join(command)}", file=sys.stderr)
        return 0
    print(f"{NAME}: starting the GUI server; it prints the URL to open (the first start installs its Python "
          "packages). Ctrl+C stops it", file=sys.stderr, flush=True)
    if os.name != "nt":
        # The GUI server takes this process's place: Ctrl+C and SIGTERM reach it, and nothing is left behind.
        os.execv(command[0], command)
    # Windows has no exec that keeps the console: wait for the server, which gets the Ctrl+C itself and ends.
    # Known limit: when only this process is ended (not with Ctrl+C), the server and what it started stay.
    with subprocess.Popen(command) as server:
        while True:
            try:
                return server.wait()
            except KeyboardInterrupt:
                pass


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
