#!/usr/bin/env python3
"""Configurations for the tests against the real vehicle_sil_launch and the ecu_main example ECUs.

    real_configs.py <directory> [<examples>]

writes the files below into <directory> (made if missing). <examples> is the build directory of the ecu_main
example (build/<preset>/examples/ecu_main, with engine/ and dashboard/); by default the one next to the launcher
named by VEHICLE_SIL_GUI_LAUNCHER (build/<preset>/tools/launch/vehicle_sil_launch). The commands are absolute
paths, so the files work in any directory, and nothing is written into the build directory. Each file also has a
`# fake-launcher:` line, so that the fake launcher plays the same thing (test_real_launcher.py compares the two).
Standard library only; also used by the e2e of gui/web (e2e-real/).

  simulation.yaml          Engine and Dashboard, stop_at_ms 1000: passes
  forever.yaml             the same without stop_at_ms: runs until stopped
  missing-executable.yaml  Dashboard's executable does not exist: the check fails (executable)
  wrong-step.yaml          Engine's step_ms is 20, its executable says 10: the check fails (step-mismatch)
  not-joined.yaml          a required participant that never joins (sleep): the run fails (not-joined)
  broken.yaml              not YAML: the file cannot be read
  example.yaml             the same as forever.yaml, with the ecu_main example's dashboard file
                           (example.dashboard.yaml, from examples/ecu_main/simulation.dashboard.yaml) and its
                           panel (panels/rpm-bar.js)
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

EXAMPLE = Path(__file__).resolve().parents[3] / "examples" / "ecu_main"  # in the repository
ENGINE = "engine/vehicle_sil_engine_ecu"
DASHBOARD = "dashboard/vehicle_sil_dashboard_ecu"


def examples_dir(launcher: str | None = None) -> Path | None:
    """The ecu_main example's build directory: VEHICLE_SIL_GUI_EXAMPLES, or the one next to the launcher."""
    explicit = os.environ.get("VEHICLE_SIL_GUI_EXAMPLES")
    if explicit:
        return Path(explicit)
    launcher = launcher or os.environ.get("VEHICLE_SIL_GUI_LAUNCHER")
    if not launcher:
        return None
    return Path(launcher).resolve().parent.parent.parent / "examples" / "ecu_main"


def executables(examples: Path) -> tuple[Path, Path]:
    suffix = ".exe" if os.name == "nt" else ""
    return examples / (ENGINE + suffix), examples / (DASHBOARD + suffix)


def usable(examples: Path | None) -> bool:
    return examples is not None and all(p.is_file() for p in executables(examples))


def _participants(engine: str, dashboard: str, engine_step: int = 10) -> str:
    return (f"participants:\n"
            f"  - name: Engine\n    command: ['{engine}']\n    step_ms: {engine_step}\n"
            f"  - name: Dashboard\n    command: ['{dashboard}']\n    step_ms: 5\n")


NETWORKS = "networks:\n  - name: J1939_CAN\n    kind: can\n"


def write(directory: Path, examples: Path) -> list[Path]:
    engine, dashboard = (p.as_posix() for p in executables(examples))
    head = "vehicle_sil_simulation: 1\n"
    files = {
        "simulation.yaml": "# fake-launcher: pass\n" + head + "stop_at_ms: 1000\n" + NETWORKS
                           + _participants(engine, dashboard),
        "forever.yaml": "# fake-launcher: forever\n" + head + NETWORKS + _participants(engine, dashboard),
        "missing-executable.yaml": "# fake-launcher: check-error\n" + head + "stop_at_ms: 1000\n" + NETWORKS
                                   + _participants(engine, dashboard + "-missing"),
        "wrong-step.yaml": "# fake-launcher: check-error\n" + head + "stop_at_ms: 1000\n" + NETWORKS
                           + _participants(engine, dashboard, engine_step=20),
        "not-joined.yaml": "# fake-launcher: participant-crash\n" + head + "stop_at_ms: 1000\nstart_timeout_ms: 2500\n"
                           + NETWORKS + _participants(engine, dashboard)
                           + "  - name: Logger\n    command: [sleep, '100']\n    describe: false\n",
        "broken.yaml": "vehicle_sil_simulation: 1\nparticipants: [\n",
        "example.yaml": "# fake-launcher: forever\n" + head + NETWORKS + _participants(engine, dashboard),
        "example.dashboard.yaml": (EXAMPLE / "simulation.dashboard.yaml").read_text(encoding="utf-8"),
        "panels/rpm-bar.js": (EXAMPLE / "panels" / "rpm-bar.js").read_text(encoding="utf-8"),
    }
    directory.mkdir(parents=True, exist_ok=True)
    written = []
    for name, text in files.items():
        path = directory / name
        path.parent.mkdir(exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        written.append(path)
    return written


def main(argv: list[str]) -> int:
    if len(argv) not in (1, 2):
        print(__doc__, file=sys.stderr)
        return 2
    examples = Path(argv[1]) if len(argv) == 2 else examples_dir()
    if not usable(examples):
        print(f"real_configs.py: no ecu_main example executables in {examples}: build the example, or set "
              "VEHICLE_SIL_GUI_EXAMPLES to build/<preset>/examples/ecu_main", file=sys.stderr)
        return 1
    for path in write(Path(argv[0]), examples):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
