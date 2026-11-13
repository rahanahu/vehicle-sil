"""The ECUs' variables through the GUI server with the real vehicle_sil_launch (built with its control server) and
the ecu_main example ECUs: the schema, a write before the first step, the values while the simulation stands still.

Runs only when VEHICLE_SIL_GUI_LAUNCHER names such a launcher (scripts/verify-gui.py --launcher <path>) with the
example ECUs built next to it; otherwise every test here is skipped, with the reason.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest
from conftest import Client, gui_server, is_ended, is_synced, is_variables, run_async
from real_configs import examples_dir, usable, write

LAUNCHER = os.environ.get("VEHICLE_SIL_GUI_LAUNCHER")
EXAMPLES = examples_dir(LAUNCHER)


def _reason() -> str | None:
    if not LAUNCHER or not Path(LAUNCHER).is_file():
        return "no real launcher: scripts/verify-gui.py --launcher <path>"
    if not usable(EXAMPLES):
        return f"no ecu_main example executables in {EXAMPLES}"
    version = subprocess.run([LAUNCHER, "version", "--json"], capture_output=True, text=True, timeout=30)
    if "control-server" not in json.loads(version.stdout).get("features", []):
        return "the launcher has no control-server: build it with -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON"
    return None


REASON = _reason()
pytestmark = pytest.mark.skipif(REASON is not None, reason=REASON or "")
THROTTLE = {"peer": "Engine", "name": "engine.throttle"}
SPEED = {"peer": "Engine", "name": "engine.speed"}


def is_values(watch: int, **fields):
    return lambda m: m["type"] == "values" and m["id"] == watch and all(m.get(k) == v for k, v in fields.items())


def is_operation(seq: int, state: str):
    return lambda m: m["type"] == "operation" and m["seq"] == seq and m["state"] == state


def test_write_before_the_start_and_read_while_it_stands_still(tmp_path: Path) -> None:
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        async def talk(c: Client) -> None:
            _, app = await c.get("api/app")
            assert app["variables"] == {"available": True, "reason": None}
            ws = await c.ws()
            await ws.until(is_synced)
            status, started = await c.post("api/run", {"file": "forever.yaml", "start_paused": True})
            sid = started["session"]["id"]
            assert status == 200 and started["session"]["variables"]["state"] == "connecting"
            # Ready comes when the control server has been read; the ECUs join it one by one after that, and
            # each time the summary is told again.
            got = await ws.until(is_variables("ready"))
            got += await ws.until(lambda m: m["type"] == "variables" and len(m["variables"]["peers"]) == 2
                                  and all(p["connected"] and p["variables"] for p in m["variables"]["peers"]))
            # The launcher got the credentials as a file, which is gone once its control server listens.
            hello = next(m["event"] for m in got if m["type"] == "event" and m["event"]["event"] == "hello")
            path = Path(hello["arguments"][hello["arguments"].index("--control-credentials") + 1])
            assert not path.exists()
            schema = {p["name"]: {v["name"]: v for v in p["variables"]} for p in got[-1]["variables"]["peers"]}
            assert set(schema) == {"Engine", "Dashboard"}
            throttle, speed = schema["Engine"]["engine.throttle"], schema["Engine"]["engine.speed"]
            assert (throttle["type"], throttle["role"], throttle["writable"], throttle["unit"]) == \
                ("real", "input", True, "%")
            assert (throttle["min"], throttle["max"]) == (0, 100)
            assert (speed["role"], speed["writable"], speed["unit"]) == ("output", False, "rpm")

            await ws.send({"type": "watch", "id": 1, "variables": [THROTTLE, SPEED], "period_ms": 20})
            first = (await ws.until(is_values(1, paused=True)))[-1]
            assert first["sim_ms"] == 0  # standing still before the first step
            status, body = await c.post("api/write", {"session": sid, "writes": {"Engine": {"engine.throttle": 50}}})
            assert status == 200, body
            seq = body["operation"]
            await ws.until(is_operation(seq, "queued"))
            for bad, code in (({"Engine": {"engine.speed": 1}}, "not-writable"),
                              ({"Engine": {"engine.throttle": 101}}, "invalid-field"),
                              ({"Engine": {"engine.nothing": 1}}, "unknown-variable")):
                status, body = await c.post("api/write", {"session": sid, "writes": bad})
                assert (status, body["error"]["code"]) == (400, code)

            # The engine runs 50 rpm per 10 ms step towards 800 rpm + 20 rpm per percent: 1800 rpm at 500 ms.
            await c.post("api/time", {"command": "step", "ms": 500})
            got = await ws.until(is_values(1, paused=True, sim_ms=500))
            applied = next(m for m in ws.received if is_operation(seq, "applied")(m))
            assert applied["peers"]["Engine"]["applied_at_ms"] == 0
            engine = got[-1]["peers"]["Engine"]
            assert engine["values"] == {"engine.throttle": 50.0, "engine.speed": 1800}, engine
            assert 490 <= engine["sample_ms"] <= 500

            # Standing still at 500: a write for 500 itself. The engine has run its step at 500 (its sample is
            # there), so the write is too late; the engine finds that out at its next step. Whether the step at
            # the standstill has run is not promised for every ECU (docs/control-api.md says to write for a time
            # after the standstill), so the other outcome, made in the step at 500, is taken too. The value
            # written is the one the variable has, so the steps below read the same either way.
            status, body = await c.post("api/write", {"session": sid, "writes": {"Engine": {"engine.throttle": 50}}, "at_ms": 500})
            assert status == 200, body
            boundary = body["operation"]
            await ws.until(lambda m: m["type"] == "operation" and m["seq"] == boundary
                           and m["state"] in ("queued", "late"))

            # Step by step: each time it stands still, the values are those of the steps before that time. The
            # engine's step is 10 ms; its speed falls 50 rpm per step towards 800 rpm once the throttle is 0.
            status, body = await c.post("api/write", {"session": sid, "writes": {"Engine": {"engine.throttle": 0}}, "at_ms": 501})
            assert status == 200, body
            for n in range(1, 13):
                await c.post("api/time", {"command": "step", "ms": 10})
                got = await ws.until(is_values(1, paused=True, sim_ms=500 + 10 * n))
                engine = got[-1]["peers"]["Engine"]
                at = engine["sample_ms"]
                assert 500 + 10 * n - 10 <= at <= 500 + 10 * n, (n, engine)
                steps_with_zero = (at - 510) // 10 + 1  # the write took effect in the step at 510
                assert engine["values"]["engine.speed"] == 1800 - 50 * max(steps_with_zero, 0), (n, engine)
                # Nothing sent while it runs claims to stand still at another time.
                assert all(m["sim_ms"] in (500 + 10 * n - 10, 500 + 10 * n) or not m["paused"]
                           for m in got if m["type"] == "values")

            end = next(m for m in reversed(ws.received) if m["type"] == "operation" and m["seq"] == boundary)
            assert end["done"] and (end["state"], end["peers"]["Engine"]["applied_at_ms"]) in (
                ("late", None), ("applied", 500)), end
            await c.post("api/stop")
            got = await ws.until(is_ended)
            assert [m["variables"]["state"] for m in got if m["type"] == "variables"][-1] == "ended"
            assert got[-1]["session"]["result"]["result"] == "interrupted"
            # The last values stay for a watch that comes after the end.
            await ws.send({"type": "watch", "id": 2, "variables": [SPEED]})
            last = (await ws.until(is_values(2)))[-1]
            assert last["paused"] is False and "engine.speed" in last["peers"]["Engine"]["values"]
            text = json.dumps(ws.received)
            assert gui.token not in text and "operator vehicle-sil-gui" not in text
            await ws.close()
        run_async(talk, gui)
        assert gui.stderr_text() == ""


def test_a_run_that_passes_has_its_last_values_after_the_end(tmp_path: Path) -> None:
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            await ws.send({"type": "watch", "id": 1, "variables": "all", "period_ms": 20})  # before any run
            await c.post("api/run", {"file": "simulation.yaml", "speed": 1})
            got = await ws.until(is_ended)
            assert got[-1]["session"]["result"]["result"] == "passed"
            states = [m["variables"]["state"] for m in got if m["type"] == "variables"]
            assert states[0] == "connecting" and states[-1] == "ended" and "ready" in states
            values = [m for m in got if m["type"] == "values"]
            times = [m["peers"]["Engine"]["sample_ms"] for m in values if "Engine" in m["peers"]]
            assert len(times) >= 5 and times == sorted(times)
            # The run stops at 1000 ms: the values read last are those of its end.
            await ws.send({"type": "watch", "id": 2, "variables": [SPEED]})
            last = (await ws.until(is_values(2)))[-1]
            assert last["peers"]["Engine"]["sample_ms"] >= 900, last
            await ws.close()
        run_async(talk, gui)
        assert gui.stderr_text() == ""


def test_a_run_started_paused_stands_before_its_first_step_and_is_resumed(tmp_path: Path) -> None:
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            await c.post("api/run", {"file": "simulation.yaml", "start_paused": True})
            await ws.until(is_variables("ready"))
            await ws.send({"type": "watch", "id": 1, "variables": "all", "period_ms": 20})
            first = (await ws.until(is_values(1, paused=True)))[-1]
            # Before the first step: the phase is still waiting, no time event has come, and no step was read.
            _, body = await c.get("api/session")
            assert (body["session"]["phase"], body["session"]["time"]) == ("waiting", None)
            assert (first["sim_ms"], first["peers"]) == (0, {})
            status, _ = await c.post("api/time", {"command": "resume"})
            assert status == 200
            got = await ws.until(is_ended)
            assert got[-1]["session"]["result"]["result"] == "passed"
            command = next(m["event"] for m in got if m["type"] == "event" and m["event"]["event"] == "command")
            assert (command["command"], command["source"], command["accepted"]) == ("resume", "stdin", True)
            assert got[-1]["session"]["time"]["sim_ms"] == 1000
            # After the resume nothing was called paused any more.
            assert not [m for m in got if m["type"] == "values" and m["paused"]]
            await ws.close()
        run_async(talk, gui)
        assert gui.stderr_text() == ""
