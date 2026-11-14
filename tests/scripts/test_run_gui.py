# SPDX-License-Identifier: MIT
"""Tests of scripts/run-gui.py: which launcher it takes, what it says for each thing that is missing, and
the command it runs. Nothing is built or started: a made-up repository in a temporary directory.

    python tests/scripts/test_run_gui.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "run-gui.py"
spec = importlib.util.spec_from_file_location("run_gui", SCRIPT)
run_gui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_gui)
NAME = run_gui.LAUNCHER
ALL = ["events", "stdin-commands", "control-server"]
failures: list[str] = []


def check(condition: bool, what: str) -> None:
    if not condition:
        failures.append(what)
        print(f"FAILED: {what}")


def touch(path: Path, age_s: int = 0) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("", encoding="utf-8")
    if age_s:
        old = path.stat().st_mtime - age_s
        os.utime(path, (old, old))
    return path


def build(root: Path, preset: str, example: bool = True, age_s: int = 0) -> Path:
    """A build directory as CMake leaves it: the launcher and, with `example`, the example next to it."""
    launcher = touch(root / "build" / preset / "tools" / "launch" / NAME, age_s)
    if example:
        suffix = launcher.suffix
        directory = root / "build" / preset / "examples" / "ecu_main"
        touch(directory / "simulation.live.yaml")
        touch(directory / "engine" / f"vehicle_sil_engine_ecu{suffix}")
        touch(directory / "dashboard" / f"vehicle_sil_dashboard_ecu{suffix}")
    return launcher


def web(root: Path) -> None:
    touch(root / "gui" / "web" / "dist" / "browser" / "index.html")


def probe(features: dict[str, list[str] | None]):
    return lambda launcher: features.get(launcher.parts[-4])


def arguments(*argv: str) -> argparse.Namespace:
    return run_gui.parse(list(argv))


def planned(root: Path, features: dict[str, list[str] | None], *argv: str, uv=lambda: "/bin/uv", cwd: Path | None = None,
            windows: bool = False, kind: type | None = None):
    """(command, notes) or the text of what is missing. `kind`: the exception that text must come as."""
    try:
        return run_gui.plan(arguments(*argv), root, probe(features), uv, cwd or root, windows)
    except (run_gui.Missing, run_gui.Usage) as e:
        if kind is not None:
            check(type(e) is kind, f"{' '.join(argv)}: {kind.__name__}, not {type(e).__name__} ({e})")
        return str(e)


def test_takes_the_newest_launcher_with_the_control_server(root: Path) -> None:
    web(root)
    build(root, "linux-gcc-debug")  # the newest, but without the Control Server
    build(root, "old-control", age_s=200)
    wanted = build(root, "linux-gcc-control", age_s=100)
    build(root, "broken", age_s=50)
    features = {"linux-gcc-debug": ["events"], "old-control": ALL, "linux-gcc-control": ALL, "broken": None}
    result = planned(root, features)
    check(isinstance(result, tuple), f"everything is there: a command, not {result}")
    command, notes = result
    config = root / "build" / "linux-gcc-control" / "examples" / "ecu_main" / "simulation.live.yaml"
    check(command == ["/bin/uv", "run", "--project", str(root), "vehicle-sil-gui", "--launcher", str(wanted), str(config)],
          f"the command: uv run vehicle-sil-gui with the newest launcher that has the Control Server and its example ({command})")
    check(notes == ["launcher build/linux-gcc-control/tools/launch/" + NAME +
                    ", configuration build/linux-gcc-control/examples/ecu_main/simulation.live.yaml"],
          f"one line that says which launcher and configuration ({notes})")
    command, _ = planned(root, features, "--port", "8765", "--no-browser", "--json")
    check(command[-5:] == ["--port", "8765", "--no-browser", "--json", str(config)],
          f"--port, --no-browser and --json go to the GUI server, the configuration last ({command})")


def test_nothing_built(root: Path) -> None:
    web(root)
    said = planned(root, {}, kind=run_gui.Missing)
    check(isinstance(said, str) and "nothing is built yet" in said, f"no build at all: it says so ({said})")
    check(isinstance(said, str) and said.startswith("run-gui: no "), f"from the repository's directory: no line about where ({said})")
    # From another directory: where to type the commands, first.
    said = planned(root, {}, cwd=root / "build", kind=run_gui.Missing)
    check(isinstance(said, str) and said.startswith(f"run-gui: run the commands below in {root} (cd {root})\nrun-gui: no "),
          f"from another directory: the directory to run the commands in comes first ({said})")
    for windows, lines in ((False, ["  cmake --preset linux-gcc-control\n", "  cmake --build --preset linux-gcc-control\n"]),
                           (True, ["  cmake --preset windows-mingw-debug -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON\n",
                                   "  cmake --build --preset windows-mingw-debug\n"])):
        try:
            run_gui.find_launcher(root, probe({}), NAME, windows)
            check(False, "find_launcher raises when nothing is built")
        except run_gui.Missing as e:
            check(all(line in str(e) for line in lines), f"the commands to type, each on a line (windows={windows}): {e}")
            check("downloads and builds gRPC" in str(e) and ("pacman -S mingw-w64-ucrt-x86_64-grpc" in str(e)) == windows,
                  f"where gRPC comes from; the MSYS2 package on Windows only (windows={windows}): {e}")


def test_no_control_server(root: Path) -> None:
    web(root)
    build(root, "linux-gcc-debug")
    build(root, "linux-gcc-release", age_s=10)
    said = planned(root, {"linux-gcc-debug": ["events"], "linux-gcc-release": None})
    check(isinstance(said, str) and "run-gui: no " + NAME + " with the Control Server under build/" in said,
          f"launchers without the Control Server: it says so ({said})")
    check(f"  build/linux-gcc-debug/tools/launch/{NAME}: built without the Control Server\n" in said and
          f"  build/linux-gcc-release/tools/launch/{NAME}: 'version --json' failed\n" in said,
          f"it lists what it found and why each does not do ({said})")
    check("cmake --build --preset" in said and "--launcher" in said, f"it says how to build one, or to pass one anyway ({said})")
    # Given by hand, such a launcher is taken, with a warning.
    result = planned(root, {"linux-gcc-debug": ["events"]}, "--launcher", str(root / "build" / "linux-gcc-debug" / "tools" / "launch" / NAME))
    check(isinstance(result, tuple) and len(result[1]) == 2 and result[1][1].startswith("warning: ") and
          "built without the Control Server" in result[1][1], f"--launcher without the Control Server: a warning ({result})")


def test_example_not_built(root: Path) -> None:
    web(root)
    build(root, "linux-gcc-control", example=False)
    touch(root / "build" / "linux-gcc-control" / "CMakeCache.txt")
    said = planned(root, {"linux-gcc-control": ALL}, kind=run_gui.Missing)
    check(isinstance(said, str) and said.startswith("run-gui: build/linux-gcc-control/examples/ecu_main/simulation.live.yaml is not there")
          and "\n  cmake --build build/linux-gcc-control\n" in said, f"the example is not built: the command that builds it ({said})")
    # The configuration alone (copied when CMake configured) is not enough: its ECUs are looked for too.
    launcher = build(root, "half")
    touch(root / "build" / "half" / "CMakeCache.txt")
    (root / "build" / "half" / "examples" / "ecu_main" / "engine" / f"vehicle_sil_engine_ecu{launcher.suffix}").unlink()
    said = planned(root, {"half": ALL, "linux-gcc-control": None})
    check(isinstance(said, str) and f"run-gui: build/half/examples/ecu_main/engine/vehicle_sil_engine_ecu{launcher.suffix} is not there"
          in said and "\n  cmake --build build/half\n" in said, f"an ECU of the example is not built ({said})")
    # A configuration of one's own needs no example.
    mine = touch(root / "mine" / "simulation.yaml")
    result = planned(root, {"linux-gcc-control": ALL}, str(mine))
    check(isinstance(result, tuple) and result[0][-1] == str(mine), f"a configuration given by hand is the one shown ({result})")
    said = planned(root, {"linux-gcc-control": ALL}, str(root / "nowhere.yaml"), kind=run_gui.Usage)
    check(isinstance(said, str) and "nowhere.yaml: no such file" in said, f"a configuration that is not there ({said})")
    # A launcher that is not in a build directory (unpacked tools): no cmake --build, the configuration is asked for.
    tools = touch(root / "tools" / "vehicle-sil" / "bin" / NAME)
    said = planned(root, {"tools": ALL}, "--launcher", str(tools), kind=run_gui.Usage)
    check(isinstance(said, str) and said == f"run-gui: --launcher tools/vehicle-sil/bin/{NAME} is not in a build directory of this "
          "repository, so there is no example next to it. Give the configuration to show: scripts/run-gui.py --launcher "
          f"tools/vehicle-sil/bin/{NAME} <simulation.yaml>", f"a launcher outside a build directory, no configuration ({said})")
    result = planned(root, {"tools": ALL}, "--launcher", str(tools), str(mine))
    check(isinstance(result, tuple) and result[0][-1] == str(mine), f"that launcher with a configuration ({result})")


def test_everything_missing_is_said_at_once(root: Path) -> None:
    build(root, "linux-gcc-control")

    def no_uv() -> str:
        raise run_gui.Missing("uv is not on the PATH")

    said = planned(root, {"linux-gcc-control": ALL}, uv=no_uv, kind=run_gui.Missing)
    check(isinstance(said, str) and "run-gui: the screen is not built (gui/web/dist/browser/index.html is not there)" in said
          and "  npm --prefix gui/web ci\n  npm --prefix gui/web run build\n" in said and "cd gui/web" not in said,
          f"the screen is not built: commands that leave the directory as it is ({said})")
    check("\nrun-gui: uv is not on the PATH" in said, f"and uv, in the same run ({said})")
    said = planned(root, {}, "--launcher", str(root / "no" / NAME), kind=run_gui.Usage)
    check(isinstance(said, str) and "--launcher " in said and "no such file" in said and "the screen is not built" in said,
          f"--launcher that is not there, and the screen ({said})")
    # uv alone has no command to type: no line about the directory, even from elsewhere.
    web(root)
    said = planned(root, {"linux-gcc-control": ALL}, uv=no_uv, cwd=root / "build", kind=run_gui.Missing)
    check(said == "run-gui: uv is not on the PATH", f"only uv is missing, from another directory ({said})")


def test_windows_launcher_without_exe(root: Path) -> None:
    """--launcher as the docs of Linux write it: on Windows the .exe next to that name is taken."""
    web(root)
    exe = touch(root / "build" / "windows-mingw-debug" / "tools" / "launch" / "vehicle_sil_launch.exe")
    mine = touch(root / "mine" / "simulation.yaml")
    given = str(exe.with_suffix(""))
    result = planned(root, {"windows-mingw-debug": ALL}, "--launcher", given, str(mine), windows=True)
    check(isinstance(result, tuple) and result[0][6] == str(exe), f"Windows: the .exe of the given name ({result})")
    said = planned(root, {"windows-mingw-debug": ALL}, "--launcher", given, str(mine), windows=False, kind=run_gui.Usage)
    check(isinstance(said, str) and "no such file" in said, f"elsewhere the name is taken as given ({said})")


def test_command_line(root: Path) -> None:
    """The script as a process: the exit code and the text on stderr; --help; a usage error is 2."""
    def run(*argv: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-B", str(SCRIPT), *argv], capture_output=True, text=True, encoding="utf-8",
                              errors="replace", stdin=subprocess.DEVNULL)

    p = run("--help")
    check(p.returncode == 0 and "--dry-run" in p.stdout, f"--help ({p.returncode}: {p.stderr})")
    p = run("--no-such-argument")
    check(p.returncode == 2, f"an unknown argument: exit code 2 ({p.returncode})")
    p = run("--dry-run", "--launcher", str(root / "no" / NAME))
    check(p.returncode == 2 and p.stdout == "" and "run-gui: --launcher " in p.stderr,
          f"a --launcher that is not there: exit code 2, the text on stderr, nothing on stdout ({p.returncode}: {p.stdout} {p.stderr})")
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        code = run_gui.main(["--dry-run"], root, probe=lambda launcher: None, uv=lambda: "/bin/uv")
    check(code == 1 and "nothing is built yet" in err.getvalue(), f"nothing built: exit code 1 ({code}: {err.getvalue()})")
    # main() with everything there and --dry-run: the command on stderr, nothing started.
    web(root)
    build(root, "linux-gcc-control")
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        code = run_gui.main(["--dry-run"], root, probe=lambda launcher: ALL, uv=lambda: "/bin/uv")
    check(code == 0 and "run-gui: would run: /bin/uv run --project " in err.getvalue() and
          "run-gui: launcher build/linux-gcc-control/tools/launch/" in err.getvalue(), f"--dry-run ({code}: {err.getvalue()})")


def test_features_of(root: Path) -> None:
    check(run_gui.features_of(root / "no-such-launcher") is None, "a launcher that cannot be run has no features")
    if os.name == "nt":
        return  # (a made-up launcher is a shell script)
    fake = root / "vehicle_sil_launch"
    fake.write_text('#!/bin/sh\necho \'{"vehicle_sil_launch": 1, "features": ["events", "control-server"]}\'\n', encoding="utf-8")
    fake.chmod(0o755)
    check(run_gui.features_of(fake) == ["events", "control-server"], "the features of version --json")
    fake.write_text("#!/bin/sh\necho nonsense\n", encoding="utf-8")
    check(run_gui.features_of(fake) is None, "output that is not the version's JSON: no features")
    fake.write_text("#!/bin/sh\nexit 4\n", encoding="utf-8")
    check(run_gui.features_of(fake) is None, "a launcher that fails: no features")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        for test in (test_takes_the_newest_launcher_with_the_control_server, test_nothing_built, test_no_control_server,
                     test_example_not_built, test_everything_missing_is_said_at_once, test_windows_launcher_without_exe,
                     test_command_line, test_features_of):
            d = Path(tmp).resolve() / test.__name__
            d.mkdir()
            test(d)
    print(f"{'FAILED' if failures else 'passed'}: {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
