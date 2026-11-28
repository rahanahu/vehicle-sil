"""The check of an unsaved text (POST api/check with text) against the real launcher: it starts no process, and the
check of the saved file that saves the descriptions before it (and POST api/describe) starts each executable once.
Saving a text (POST api/config): the launcher's check --compare-commands decides what is saved by default.

The participants are a small Python program that says who it is to --describe and marks each start in a file, so
only the launcher is needed (no example ECU). Runs only when VEHICLE_SIL_GUI_LAUNCHER names a real vehicle_sil_launch
(scripts/verify-gui.py --launcher <path>); otherwise skipped, with the reason.
"""

from __future__ import annotations

import hashlib
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
    print(json.dumps({"vehicle_sil_ecu": 1, "name": sys.argv[2], "step_ns": 1000000, "features": ["map-network"],
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
    saved = ("vehicle_sil_simulation: 2\nnetworks:\n  - name: CAN1\n    kind: can\nparticipants:\n"
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


def test_saving_keeps_the_commands_by_default(tmp_path: Path) -> None:
    """An edit of the networks and their mapping is saved by default; an edit of a command is refused, and saved
    with --allow-command-changes. After a save, the check of the text describes the saved file again and passes."""
    root = tmp_path / "root"
    root.mkdir()
    ecu, mark = tmp_path / "ecu.py", tmp_path / "started.txt"
    ecu.write_text(ECU, encoding="utf-8")
    saved = ("vehicle_sil_simulation: 2\nnetworks:\n  - name: CAN1\n    kind: can\nparticipants:\n"
             + participant("Engine", ecu, mark) + participant("Dashboard", ecu, mark))
    (root / "sim.yaml").write_text(saved, encoding="utf-8")
    # The networks and their mapping only: the simulation calls the bus BodyCAN, the executables still CAN1
    mapped = saved.replace("  - name: CAN1\n", "  - name: BodyCAN\n").replace(
        "Engine]\n", "Engine]\n    network_names: {BodyCAN: CAN1}\n").replace(
        "Dashboard]\n", "Dashboard]\n    network_names: {BodyCAN: CAN1}\n")
    assert mapped.count("network_names") == 2
    # A command that changes: the executable gets another argument
    commanded = mapped.replace("Engine]\n", "Engine, --verbose]\n")
    stamp = hashlib.sha256(saved.encode()).hexdigest()

    def starts() -> int:
        return mark.read_text(encoding="utf-8").count("started\n") if mark.exists() else 0

    async def by_default(c: Client) -> None:
        status, reply = await c.post("api/config", {"file": "sim.yaml", "text": mapped, "stamp": stamp})
        assert status == 200, reply
        assert (root / "sim.yaml").read_text(encoding="utf-8") == mapped
        assert starts() == 0  # the save started nothing
        status, reply = await c.post("api/check", {"file": "sim.yaml", "text": mapped})
        assert status == 200 and reply["check"]["result"] == "passed", reply
        assert starts() == 2  # the saved file was described first
        status, reply = await c.post("api/config", {"file": "sim.yaml", "text": commanded,
                                                    "stamp": reply_stamp(mapped)})
        assert status == 403 and reply["error"]["code"] == "commands-changed", reply
        assert reply["error"]["detail"]["commands"] == "changed"
        assert "participant 'Engine': command differs" in reply["error"]["message"]
        assert (root / "sim.yaml").read_text(encoding="utf-8") == mapped and starts() == 2

    async def allowed(c: Client) -> None:
        status, reply = await c.post("api/config", {"file": "sim.yaml", "text": commanded,
                                                    "stamp": reply_stamp(mapped)})
        assert status == 200 and reply["stamp"] == reply_stamp(commanded), reply
        status, reply = await c.post("api/check", {"file": "sim.yaml", "text": commanded})
        assert status == 200 and reply["check"]["result"] == "passed", reply
        assert starts() == 4  # the new command described itself

    with gui_server(tmp_path, root=root, launcher=Path(LAUNCHER)) as gui:
        run_async(by_default, gui)
    with gui_server(tmp_path, root=root, launcher=Path(LAUNCHER), args=("--allow-command-changes",)) as gui:
        run_async(allowed, gui)


def reply_stamp(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def test_saved_check_reuses_descriptions_and_falls_back_when_executable_changes(tmp_path: Path) -> None:
    root = tmp_path / "root"
    root.mkdir()
    ecu, mark = tmp_path / "ecu.py", tmp_path / "started.txt"
    ecu.write_text(ECU, encoding="utf-8")
    saved = ("vehicle_sil_simulation: 2\nnetworks:\n  - name: CAN1\n    kind: can\nparticipants:\n"
             + participant("Engine", ecu, mark) + participant("Dashboard", ecu, mark))
    (root / "sim.yaml").write_text(saved, encoding="utf-8")

    def starts() -> int:
        return mark.read_text(encoding="utf-8").count("started\n") if mark.exists() else 0

    async def talk(c: Client) -> None:
        status, reply = await c.post("api/check", {"file": "sim.yaml"})
        assert status == 200 and reply["check"]["result"] == "passed", reply
        assert starts() == 2

        # Second check on the unchanged saved file + YAML skips --describe.
        status, reply = await c.post("api/check", {"file": "sim.yaml"})
        assert status == 200 and reply["check"]["result"] == "passed", reply
        assert starts() == 2

        # Modifying the ECU executable (different size/mtime) without touching sim.yaml triggers fallback.
        ecu.write_text(ECU + "\n# rebuilt\n", encoding="utf-8")
        status, reply = await c.post("api/check", {"file": "sim.yaml"})
        assert status == 200 and reply["check"]["result"] == "passed", reply
        assert {p["status"] for p in reply["check"]["participants"]} == {"described"}
        assert starts() == 4

        # Next check reuses the refreshed descriptions.
        status, reply = await c.post("api/check", {"file": "sim.yaml"})
        assert status == 200 and starts() == 4, reply

    with gui_server(tmp_path, root=root, launcher=Path(LAUNCHER)) as gui:
        run_async(talk, gui)
