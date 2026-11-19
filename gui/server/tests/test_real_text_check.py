"""The check of an unsaved text (POST api/check with text) against the real launcher: it starts no process, and the
check of the saved file that saves the descriptions before it (and POST api/describe) starts each executable once.

The participants are a small Python program that says who it is to --describe and marks each start in a file, so
only the launcher is needed (no example ECU). Runs only when VEHICLE_SIL_GUI_LAUNCHER names a real vehicle_sil_launch
(scripts/verify-gui.py --launcher <path>); otherwise skipped, with the reason.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest
from conftest import Client, gui_server, run_async

LAUNCHER = os.environ.get("VEHICLE_SIL_GUI_LAUNCHER")
REASON = ("no real launcher: set VEHICLE_SIL_GUI_LAUNCHER to build/<preset>/tools/launch/vehicle_sil_launch "
          "(scripts/verify-gui.py --launcher <path>)" if not LAUNCHER or not Path(LAUNCHER).is_file() else None)
pytestmark = pytest.mark.skipif(REASON is not None, reason=REASON or "")

ECU = '''import json, sys
with open(sys.argv[1], "a", encoding="utf-8") as f:
    f.write("started\\n")
if "--describe" in sys.argv:
    print(json.dumps({"vehicle_sil_ecu": 1, "name": sys.argv[2], "step_ns": 1000000,
                      "networks": [{"name": "CAN1", "kind": "can", "use": "raw", "nodes": []}]}))
'''


def participant(name: str, ecu: Path, mark: Path) -> str:
    return (f"  - name: {name}\n    command: [{sys.executable.replace(chr(92), '/')!r}, "
            f"{ecu.as_posix()!r}, {mark.as_posix()!r}, {name}]\n")


def test_a_text_check_starts_nothing(tmp_path: Path) -> None:
    root = tmp_path / "root"
    root.mkdir()
    ecu, mark = tmp_path / "ecu.py", tmp_path / "started.txt"
    ecu.write_text(ECU, encoding="utf-8")
    saved = ("vehicle_sil_simulation: 1\nnetworks:\n  - name: CAN1\n    kind: can\nparticipants:\n"
             + participant("Engine", ecu, mark) + participant("Dashboard", ecu, mark))
    (root / "sim.yaml").write_text(saved, encoding="utf-8")

    def starts() -> int:
        return mark.read_text(encoding="utf-8").count("started\n") if mark.exists() else 0

    def statuses(reply: dict) -> dict[str, str]:
        return {p["name"]: p["status"] for p in reply["check"]["participants"]}

    async def talk(c: Client) -> None:
        # No descriptions yet: the saved file is checked first (each executable describes itself once)
        status, reply = await c.post("api/check", {"file": "sim.yaml", "text": saved})
        assert status == 200 and reply["check"]["result"] == "passed", reply
        assert statuses(reply) == {"Engine": "described", "Dashboard": "described"}
        assert starts() == 2
        # An edited text with a participant that the saved file lacks: nothing starts, and that one is not described
        edited = (saved + participant("Gateway", ecu, mark)).replace("\n", "\r\n")
        status, reply = await c.post("api/check", {"file": "sim.yaml", "text": edited})
        assert status == 200, reply
        assert statuses(reply) == {"Engine": "described", "Dashboard": "described", "Gateway": "not_described"}
        assert "not-described" in {f["code"] for f in reply["check"]["findings"]}
        assert starts() == 2
        # Describing again starts each executable of the saved file once more
        status, reply = await c.post("api/describe", {"file": "sim.yaml"})
        assert status == 200 and reply["check"]["result"] == "passed", reply
        assert starts() == 4
        status, reply = await c.post("api/check", {"file": "sim.yaml", "text": saved})
        assert status == 200 and statuses(reply) == {"Engine": "described", "Dashboard": "described"}, reply
        assert starts() == 4

    with gui_server(tmp_path, root=root, launcher=Path(LAUNCHER)) as gui:
        run_async(talk, gui)
