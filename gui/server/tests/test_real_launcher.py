"""The recordings that replay_launcher.py plays, against the real launcher: record_launcher.py --compare records
every case again with the real vehicle_sil_launch and the ecu_main example ECUs and compares them with recordings/
(the events in order without time, log and heartbeat, their keys and JSON types, the commands, the exit code, what
check --json and version --json print). When it differs, record again and read the diff (record_launcher.py says
how). Also the YAML of a saved scenario, read by the real launcher's check.

Runs only when VEHICLE_SIL_GUI_LAUNCHER names a real vehicle_sil_launch (scripts/verify-gui.py --launcher
<path>) and the ecu_main example ECUs are built next to it (or VEHICLE_SIL_GUI_EXAMPLES names their
directory); otherwise every test here is skipped, with the reason. Linux only (record_launcher.py is).
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest
from real_configs import NETWORKS, examples_dir, executables, usable
from vehicle_sil_gui.recording import scenario_yaml

TESTS = Path(__file__).resolve().parent
LAUNCHER = os.environ.get("VEHICLE_SIL_GUI_LAUNCHER")
EXAMPLES = examples_dir(LAUNCHER)
REASON = ("no real launcher: set VEHICLE_SIL_GUI_LAUNCHER to build/<preset>/tools/launch/vehicle_sil_launch "
          "(scripts/verify-gui.py --launcher <path>)" if not LAUNCHER or not Path(LAUNCHER).is_file()
          else f"no ecu_main example executables in {EXAMPLES}: build them, or set VEHICLE_SIL_GUI_EXAMPLES"
          if not usable(EXAMPLES) else "Linux only" if sys.platform != "linux" else None)
pytestmark = pytest.mark.skipif(REASON is not None, reason=REASON or "")

RUN_TIMEOUT_S = 60


def test_the_recordings_are_what_the_real_launcher_writes() -> None:
    p = subprocess.run([sys.executable, str(TESTS / "record_launcher.py"), "--launcher", LAUNCHER, "--examples",
                        str(EXAMPLES), "--compare"], capture_output=True, text=True, timeout=900)
    assert p.returncode == 0, f"{p.stdout}{p.stderr[-6000:]}"


def test_a_saved_scenario_with_names_beyond_ascii_passes_the_real_check(tmp_path: Path) -> None:
    """The YAML of GET api/recording, read by the real launcher's YAML reader: names with characters beyond ASCII
    (U+0080 to U+00FF among them, which must not be written as \\xNN) name the participant they are."""
    engine, dashboard = (p.as_posix() for p in executables(EXAMPLES))
    name = "Mot\u00f6r\u00b0\u00b5\u00e9\u4e00"
    (tmp_path / "sim.yaml").write_text(
        f"vehicle_sil_simulation: 1\nstop_at_ms: 1000\n{NETWORKS}participants:\n"
        f"  - name: {name}\n    command: ['{engine}']\n    step_ms: 10\n"
        f"  - name: Dashboard\n    command: ['{dashboard}']\n    step_ms: 5\n", encoding="utf-8")
    text, steps, _ = scenario_yaml([(0, 1, name, "engine.throttle", 10.0), (20, 2, name, "ecu.connected", True)],
                                   "recorded from s\u00edm.yaml (run 1)", {}, 0, 500)
    assert steps == 2
    (tmp_path / "rec.yaml").write_text(text, encoding="utf-8")
    p = subprocess.run([LAUNCHER, "check", "--json", "--scenario", "rec.yaml", "--", "sim.yaml"], cwd=tmp_path,
                       capture_output=True, timeout=RUN_TIMEOUT_S)
    check = json.loads(p.stdout)
    assert check["result"] == "passed", check["findings"]
    assert check["scenario"] == {"file": "rec.yaml", "name": "recorded from s\u00edm.yaml (run 1)", "steps": 2}
