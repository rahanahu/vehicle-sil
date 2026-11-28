"""The ECUs' variables through the GUI server with the real vehicle_sil_launch (built with its control server) and
the ecu_main example ECUs: the schema, a write before the first step, the values while the simulation stands still.

Runs only when VEHICLE_SIL_GUI_LAUNCHER names such a launcher (scripts/verify-gui.py --launcher <path>) with the
example ECUs built next to it; otherwise every test here is skipped, with the reason.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest
import yaml
from conftest import Client, gui_server, is_ended, is_synced, is_variables, run_async
from real_configs import examples_dir, usable, write
from vehicle_sil_gui import __version__

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
    import tempfile
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    with tempfile.TemporaryDirectory(prefix="vsil-") as temp_dir:
        temp = Path(temp_dir)
        cred_paths: list[Path] = []
        env = {"TMPDIR": str(temp), "TEMP": str(temp), "TMP": str(temp)}
        with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER), env=env) as gui:
            async def talk(c: Client) -> None:
                _, app = await c.get("api/app")
                assert app["version"] == __version__
                assert app["launcher"]["version"]["vehicle_sil"] == __version__
                assert app["variables"] == {"available": True, "reason": None}
                ws = await c.ws()
                await ws.until(is_synced)
                status, started = await c.post("api/run", {"file": "forever.yaml", "start_paused": True})
                assert status == 200, started
                sid, instance = started["run_id"], started["instance"]
                _, s_init = await c.get("api/session")
                assert s_init["session"]["variables"]["state"] in ("connecting", "ready")
                # Ready comes when the control server has been read; the ECUs join it one by one after that, and
                # each time the summary is told again.
                if s_init["session"]["variables"]["state"] != "ready":
                    got = await ws.until(is_variables("ready"))
                else:
                    got = list(ws.received)
                if not any(
                    m["type"] == "variables"
                    and len(m["variables"]["peers"]) == 2
                    and all(p["connected"] and p["variables"] for p in m["variables"]["peers"])
                    for m in ws.received
                ):
                    got += await ws.until(lambda m: m["type"] == "variables" and len(m["variables"]["peers"]) == 2
                                          and all(p["connected"] and p["variables"] for p in m["variables"]["peers"]))
                else:
                    got = list(ws.received)
                # Persistent `serve` retains credentials until shutdown.
                found_creds = list(temp.glob("vehicle-sil-gui-control-*"))
                assert len(found_creds) == 1 and found_creds[0].exists()
                cred_paths.extend(found_creds)
                var_msgs = [m for m in ws.received if m["type"] == "variables" and len(m["variables"]["peers"]) == 2]
                schema = {p["name"]: {v["name"]: v for v in p["variables"]} for p in var_msgs[-1]["variables"]["peers"]}
                assert set(schema) == {"Engine", "Dashboard"}
                throttle, speed = schema["Engine"]["engine.throttle"], schema["Engine"]["engine.speed"]
                assert (throttle["type"], throttle["role"], throttle["writable"], throttle["builtin"],
                        throttle["unit"]) == ("real", "input", True, False, "%")
                assert (throttle["min"], throttle["max"]) == (0, 100)
                assert (speed["role"], speed["writable"], speed["builtin"], speed["unit"]) == \
                    ("output", False, False, "rpm")
                assert schema["Engine"]["ecu.connected"]["builtin"] is True
                builtin_prefixes = ("ecu.", "peer.", "j1939.", "can.", "lin.", "eth.")
                engine_vars = schema["Engine"].values()
                assert any(v["name"].startswith("peer.") and v["builtin"] is True for v in engine_vars)
                for v in engine_vars:
                    assert v["builtin"] is v["name"].startswith(builtin_prefixes), v
                _, check_body = await c.post("api/check", {"file": "forever.yaml"})
                check_engine = next(p for p in check_body["check"]["participants"] if p["name"] == "Engine")
                for cv in check_engine["variables"]:
                    assert schema["Engine"][cv["name"]]["builtin"] is cv["builtin"], cv

                await ws.send({"type": "watch", "id": 1, "variables": [THROTTLE, SPEED], "period_ms": 20})
                first = (await ws.until(is_values(1, paused=True)))[-1]
                assert first["sim_ms"] == 0  # standing still before the first step
                status, body = await c.post("api/write", {"run_id": sid, "writes": {"Engine": {"engine.throttle": 50}}})
                assert status == 200, body
                seq = body["operation"]
                await ws.until(is_operation(seq, "queued"))
                for bad, code in (({"Engine": {"engine.speed": 1}}, "not-writable"),
                                  ({"Engine": {"engine.throttle": 101}}, "invalid-field"),
                                  ({"Engine": {"engine.nothing": 1}}, "unknown-variable")):
                    status, body = await c.post("api/write", {"run_id": sid, "writes": bad})
                    assert (status, body["error"]["code"]) == (400, code)

                # The engine runs 50 rpm per 10 ms step towards 800 rpm + 20 rpm per percent: 1800 rpm at 500 ms.
                await c.post("api/time", {"command": "step", "ms": 500})
                got = await ws.until(is_values(1, paused=True, sim_ms=500))
                applied = next(m for m in ws.received if is_operation(seq, "applied")(m))
                assert applied["peers"]["Engine"]["applied_at_ms"] == 0
                assert (applied["peers"]["Engine"]["applied_writes"], applied["peers"]["Engine"]["failed_write"]) == (1, None)
                engine = got[-1]["peers"]["Engine"]
                assert engine["values"] == {"engine.throttle": 50.0, "engine.speed": 1800}, engine
                assert 490 <= engine["sample_ms"] <= 500

                # Standing still at 500: a write for 500 itself. The engine has run its step at 500 (its sample is
                # there), so the write is too late; the engine finds that out at its next step. Whether the step at
                # the standstill has run is not promised for every ECU (docs/control-api.md says to write for a time
                # after the standstill), so the other outcome, made in the step at 500, is taken too. The value
                # written is the one the variable has, so the steps below read the same either way.
                status, body = await c.post("api/write", {"run_id": sid, "writes": {"Engine": {"engine.throttle": 50}}, "at_ms": 500})
                assert status == 200, body
                boundary = body["operation"]
                await ws.until(lambda m: m["type"] == "operation" and m["seq"] == boundary
                               and m["state"] in ("queued", "late"))

                # Step by step: each time it stands still, the values are those of the steps before that time. The
                # engine's step is 10 ms; its speed falls 50 rpm per step towards 800 rpm once the throttle is 0.
                status, body = await c.post("api/write", {"run_id": sid, "writes": {"Engine": {"engine.throttle": 0}}, "at_ms": 501})
                assert status == 200, body
                for n in range(1, 13):
                    await c.post("api/time", {"command": "step", "ms": 10})
                    got = await ws.until(is_values(1, paused=True, sim_ms=500 + 10 * n))
                    engine = got[-1]["peers"]["Engine"]
                    at = engine["sample_ms"]
                    assert 500 + 10 * n - 10 <= at <= 500 + 10 * n, (n, engine)
                    steps_with_zero = (at - 510) // 10 + 1  # the write took effect in the step at 510
                    assert engine["values"]["engine.speed"] == 1800 - 50 * max(steps_with_zero, 0), (n, engine)
                    assert engine["values"]["engine.throttle"] == (0.0 if at >= 510 else 50.0), (n, engine)
                    if n == 1 and at < 510:
                        # Stopped, written, stepped: the step at the standstill itself (the one that took the write)
                        # arrives while it stands still. The same time is then called paused once more, with the
                        # values of that step.
                        more = await ws.until(lambda m: is_values(1)(m) and m["peers"]["Engine"]["sample_ms"] == 510)
                        assert (more[-1]["paused"], more[-1]["sim_ms"]) == (True, 510), more[-1]
                        assert more[-1]["peers"]["Engine"]["values"] == {"engine.throttle": 0.0, "engine.speed": 1750}
                        got += more
                    # Nothing sent while it runs claims to stand still at another time.
                    assert all(m["sim_ms"] in (500 + 10 * n - 10, 500 + 10 * n) or not m["paused"]
                               for m in got if m["type"] == "values")

                end = next(m for m in reversed(ws.received) if m["type"] == "operation" and m["seq"] == boundary)
                assert end["done"] and (end["state"], end["peers"]["Engine"]["applied_at_ms"]) in (
                    ("late", None), ("applied", 500)), end
                await c.post("api/stop-session", {"run_id": sid, "instance": instance})
                got = await ws.until(is_ended)
                assert [m["variables"]["state"] for m in got if m["type"] == "variables"][-1] == "ended"
                assert got[-1]["session"]["result"]["result"] == "stopped"
                # The last values stay for a watch that comes after the end.
                await ws.send({"type": "watch", "id": 2, "variables": [SPEED]})
                last = (await ws.until(is_values(2)))[-1]
                assert last["paused"] is False and "engine.speed" in last["peers"]["Engine"]["values"]
                text = json.dumps(ws.received)
                assert gui.token not in text and "operator vehicle-sil-gui" not in text
                await ws.close()
            run_async(talk, gui)
            assert gui.stderr_text() == ""
        assert cred_paths and not cred_paths[0].exists()


def test_a_run_that_passes_has_its_last_values_after_the_end(tmp_path: Path) -> None:
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            await ws.send({"type": "watch", "id": 1, "variables": "all", "period_ms": 20})  # before any run
            await c.post("api/run", {"file": "simulation.yaml", "speed": 1, "stop_at_ms": 1000})
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
            await c.post("api/run", {"file": "simulation.yaml", "start_paused": True, "stop_at_ms": 1000})
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


def test_the_recorded_writes_pass_the_launchers_check_and_play_back(tmp_path: Path) -> None:
    """GET api/recording is a scenario the real launcher takes: check --scenario passes, and a run with it passes
    (through the GUI server, as the screen saves it into scenarios/ and runs it)."""
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            _, started = await c.post("api/run", {"file": "forever.yaml", "start_paused": True})
            sid, instance = started["run_id"], started["instance"]
            await ws.until(lambda m: m["type"] == "variables" and len(m["variables"]["peers"]) == 2
                           and all(p["connected"] and p["variables"] for p in m["variables"]["peers"]))
            before = len(ws.received)
            status, body = await c.post("api/write", {"run_id": sid, "writes": {"Engine": {"engine.throttle": 40}}})
            assert status == 200, body
            # The time goes on by steps, not freely: a write is recorded with the virtual time it was applied at,
            # and at the speed "max" a free run would be anywhere by the second write (on a busy machine, beyond
            # the stop_at_ms of the simulation that the record is played back in below).
            async def step_and_wait(first: int, operation: int, stands_at: int) -> None:
                """Steps 100 ms; waits for the operation's end and for the standstill, whichever comes first, among
                the messages from ws.received[first] on (from before the write: standing still at a time, an ECU
                whose step at that time has not run yet applies a write there, before the next step command)."""
                await c.post("api/time", {"command": "step", "ms": 100})
                wanted = (lambda m: m["type"] == "operation" and m["seq"] == operation and m["done"],
                          lambda m: m["type"] == "event" and m["event"]["event"] == "time"
                          and m["event"]["sim_ms"] == stands_at,
                          # (the state of the standstill: each wait ends after the one of its own step)
                          lambda m: m["type"] == "event" and m["event"]["event"] == "state" and m["event"]["paused"])
                while not all(any(w(m) for m in ws.received[first:]) for w in wanted):
                    await ws.until(lambda m: True)
            await step_and_wait(before, body["operation"], 100)
            before = len(ws.received)
            status, body = await c.post("api/write", {"run_id": sid, "writes": {
                "Engine": {"engine.throttle": 12.5}, "Dashboard": {"ecu.connected": False}}, "late": "apply"})
            assert status == 200, body
            await step_and_wait(before, body["operation"], 200)
            await c.post("api/stop-session", {"run_id": sid, "instance": instance})
            await ws.until(is_ended)
            _, body = await c.get("api/recording")
            r = body["recording"]
            # Each ECU applies at its own step (Engine 10 ms, Dashboard 5 ms): one set per time
            assert r["available"] and r["writes"] == 3 and r["steps"] in (2, 3) and r["end_ms"] == 200, r
            assert yaml.safe_load(r["yaml"])["end_ms"] == 200
            (configs / "scenarios" / "recorded.yaml").write_text(r["yaml"], encoding="utf-8")
            status, body = await c.post("api/check", {"file": "simulation.yaml", "scenario": "scenarios/recorded.yaml"})
            assert status == 200 and body["check"]["result"] == "passed", body["check"]["findings"]
            assert body["check"]["scenario"]["steps"] == r["steps"]
            await c.post("api/run", {"file": "simulation.yaml", "scenario": "scenarios/recorded.yaml"})
            got = await ws.until(is_ended)
            assert got[-1]["session"]["scenario"]["status"] == "passed", got[-1]["session"]["scenario"]
            assert got[-1]["session"]["result"]["simulation"] == "ended"
            await ws.close()
        run_async(talk, gui)


def test_real_hot_runs_reuse_resident_and_isolate_variables(tmp_path: Path) -> None:
    """Two consecutive hot runs reuse one `serve` process, isolate Run variables, and clean up per run."""
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        async def talk(c: Client) -> None:
            _, app = await c.get("api/app")
            if not app["variables"]["available"]:
                pytest.skip(app["variables"]["reason"])
            ws = await c.ws()
            await ws.until(is_synced)

            status, prep = await c.post("api/prepare", {"file": "forever.yaml"})
            assert status == 200 and prep["instance"], prep
            _, s_prep = await c.get("api/session")
            assert s_prep["runtime"]["mode"] == "hot" and s_prep["runtime"]["state"] == "ready"

            status, r1 = await c.post("api/run", {"file": "forever.yaml", "speed": 1, "start_paused": True})
            assert status == 200, r1
            _, s1 = await c.get("api/session")
            pid1 = s1["session"]["launcher"]["pid"]
            await ws.until(lambda m: m["type"] == "variables" and len(m["variables"]["peers"]) == 2
                           and all(p["connected"] and p["variables"] for p in m["variables"]["peers"]))
            st, w1 = await c.post("api/write", {
                "run_id": r1["run_id"],
                "writes": {"Engine": {"engine.throttle": 25}},
            })
            assert st == 200, w1
            await c.post("api/stop-session", {"run_id": r1["run_id"], "instance": r1["instance"], "force": False})
            got1 = await ws.until(is_ended)
            assert got1[-1]["session"]["result"]["result"] == "stopped"

            status, r2 = await c.post("api/run", {"file": "forever.yaml", "speed": 1, "start_paused": True})
            assert status == 200, r2
            _, s2 = await c.get("api/session")
            assert s2["session"]["launcher"]["pid"] == pid1
            assert r2["run_id"] != r1["run_id"]
            await ws.until(lambda m: m["type"] == "variables" and len(m["variables"]["peers"]) == 2
                           and all(p["connected"] and p["variables"] for p in m["variables"]["peers"]))
            # Stale write referencing Run 1 must be rejected
            st, stale = await c.post("api/write", {
                "run_id": r1["run_id"],
                "writes": {"Engine": {"engine.throttle": 50}},
            })
            assert st == 409 and stale["error"]["code"] == "session-changed"
            st, w2 = await c.post("api/write", {
                "run_id": r2["run_id"],
                "writes": {"Engine": {"engine.throttle": 75}},
            })
            assert st == 200, w2
            await c.post("api/stop-session", {"run_id": r2["run_id"], "instance": r2["instance"], "force": False})
            got2 = await ws.until(is_ended)
            assert got2[-1]["session"]["result"]["result"] == "stopped"
            await ws.close()
        run_async(talk, gui)


def test_real_hot_execution_status_and_cold_fallback_runtime_and_allowed(tmp_path: Path) -> None:
    """Real vehicle_sil_launch transitions runtime and allowed consistently across prepare, run, stop, and Cold fallback."""
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    sim_text = (configs / "simulation.yaml").read_text(encoding="utf-8")
    (configs / "fixed.yaml").write_text(
        sim_text + "registry_uri: silkit://127.0.0.1:8500\n",
        encoding="utf-8",
    )
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)

            st, prep = await c.post("api/prepare", {"file": "forever.yaml"})
            assert st == 200 and prep["instance"], prep
            await ws.until(lambda m: m["type"] == "runtime" and m["runtime"]["state"] == "ready")
            _, s_prep = await c.get("api/session")
            assert s_prep["runtime"] == {
                "mode": "hot",
                "state": "ready",
                "config": "forever.yaml",
                "reason": None,
                "message": None,
            }

            st, started = await c.post("api/run", {"file": "forever.yaml", "speed": 1, "start_paused": True})
            assert st == 200, started
            await ws.until(lambda m: m["type"] == "runtime" and m["runtime"]["state"] == "running")
            _, mid = await c.get("api/session")
            assert mid["runtime"]["mode"] == "hot" and mid["runtime"]["state"] == "running"
            assert mid["allowed"]["start"]["reason"] == "already-running"
            assert mid["allowed"]["stop"]["ok"] is True
            assert mid["allowed"]["force"]["ok"] is True

            st, stopped = await c.post("api/stop-session", {"run_id": started["run_id"], "instance": started["instance"], "force": False})
            assert st == 200, stopped
            _, mid_stop = await c.get("api/session")
            assert mid_stop["allowed"]["stop"]["reason"] in ("already-stopping", "not-running")
            got = await ws.until(is_ended)
            assert got[-1]["session"]["result"]["result"] == "stopped"
            if not any(m["type"] == "runtime" and m["runtime"]["state"] == "ready" for m in got):
                await ws.until(lambda m: m["type"] == "runtime" and m["runtime"]["state"] == "ready")
            _, after = await c.get("api/session")
            assert after["runtime"]["state"] == "ready"
            assert after["allowed"]["start"]["ok"] is True
            assert after["allowed"]["stop"]["reason"] == "not-running"

            # Cold fallback via manual participant override:
            st, manual_prep = await c.post("api/prepare", {"file": "forever.yaml", "manual": ["Engine"]})
            assert st == 200 and manual_prep["instance"], manual_prep
            _, after_manual = await c.get("api/session")
            assert after_manual["runtime"]["mode"] == "cold"
            assert after_manual["runtime"]["state"] == "cold"
            assert after_manual["runtime"]["reason"] == "manual-participants"
            assert after_manual["allowed"]["start"]["ok"] is True

            # Cold fallback via fixed registry_uri:
            st, fixed_prep = await c.post("api/prepare", {"file": "fixed.yaml"})
            assert st == 200 and fixed_prep["instance"], fixed_prep
            _, after_fixed = await c.get("api/session")
            assert after_fixed["runtime"]["mode"] == "cold"
            assert after_fixed["runtime"]["reason"] == "fixed-registry"
            await ws.close()

        run_async(talk, gui)


def test_real_hot_pre_prepare_and_run_without_prepare_idle_projects_ready(tmp_path: Path) -> None:
    """Real vehicle_sil_launch Idle without prepare() (both pre-prepare and post-run) projects Hot ready, never Cold."""
    import asyncio
    import aiohttp
    from aiohttp.test_utils import TestServer
    from conftest import Gui, free_port
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.runtime import ServeRuntime
    from vehicle_sil_gui.server import GuiServer, Options

    configs = tmp_path / "configs"
    write(configs, EXAMPLES)

    async def run() -> None:
        launcher = Launcher(Path(LAUNCHER))
        await launcher.probe()

        # 1. Pre-prepare serve resident status -> Unprepared with eligible=False, reason=None, run.ok=True -> Hot ready.
        rt_direct = ServeRuntime(launcher, cwd=configs)
        try:
            res = await rt_direct._spawn_process("simulation.yaml", once=False, want_control=False)
            assert res is not None
            pre_status = rt_direct.execution_status()
            assert pre_status is not None
            assert pre_status["launcher_state"] == "unprepared"
            assert pre_status["eligibility"] == {"eligible": False, "reason": None, "message": None}
            assert pre_status["launcher_allowed"]["run"]["ok"] is True
            assert rt_direct.status() == {
                "mode": "hot",
                "state": "ready",
                "config": "simulation.yaml",
                "reason": None,
                "message": None,
            }
        finally:
            await rt_direct.shutdown(asyncio.Event(), 2.0)

        # 2. GuiServer run without prepare() -> post-run Unprepared -> execution_status() projects Hot ready.
        port = free_port()

        server = GuiServer(Options(
            root=configs,
            launcher=launcher,
            port=port,
            token="test-token",
            stop_timeout_s=2.0,
            variables=False,
        ))
        test_server = TestServer(server.app(), host="127.0.0.1", port=port)
        await test_server.start_server()
        http = aiohttp.ClientSession()
        gui = Gui(proc=None, port=port, token="test-token", url=f"http://127.0.0.1:{port}/#token=test-token",
                  stderr=tmp_path / ".err")
        c = Client(gui, http)
        try:
            ws = await c.ws()
            await ws.until(is_synced)

            st, r1 = await c.post("api/run", {"file": "simulation.yaml", "speed": 1, "stop_at_ms": 200})
            assert st == 200, r1
            got1 = await ws.until(is_ended)
            pid1 = got1[-1]["session"]["launcher"]["pid"]
            assert got1[-1]["session"]["result"]["result"] == "passed"

            rt = server.runs.runtime
            assert isinstance(rt, ServeRuntime)
            post_status = rt.execution_status()
            assert post_status is not None
            assert post_status["launcher_state"] in ("unprepared", "ready")
            assert post_status["eligibility"]["reason"] is None
            assert post_status["launcher_allowed"]["run"]["ok"] is True
            assert rt.status() == {
                "mode": "hot",
                "state": "ready",
                "config": "simulation.yaml",
                "reason": None,
                "message": None,
            }

            st2, r2 = await c.post("api/run", {"file": "simulation.yaml", "speed": 1, "stop_at_ms": 200})
            assert st2 == 200, r2
            got2 = await ws.until(is_ended)
            assert got2[-1]["session"]["launcher"]["pid"] == pid1
            assert got2[-1]["session"]["result"]["result"] == "passed"
            await ws.close()
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(run())


def test_real_hot_external_stop_does_not_escalate_on_graceful_api_stop(tmp_path: Path) -> None:
    """Real vehicle_sil_launch: POST api/stop-session {force:false} never escalates stop.requests=1 to 2; {force:true} does."""
    import asyncio
    import signal
    import sys
    import aiohttp
    from aiohttp.test_utils import TestServer
    from conftest import Gui, alive, free_port, until_async
    from vehicle_sil_gui.launcher import Launcher, stop_command_line
    from vehicle_sil_gui.runtime import ServeRuntime
    from vehicle_sil_gui.server import GuiServer, Options

    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    ready = tmp_path / "logger.ready"
    ignore_stop = tmp_path / "ignore_stop.py"
    ignore_stop.write_text(
        "import os, signal, time\n"
        "from pathlib import Path\n"
        "for name in ('SIGTERM', 'SIGINT', 'SIGBREAK'):\n"
        "    sig = getattr(signal, name, None)\n"
        "    if sig is not None:\n"
        "        signal.signal(sig, signal.SIG_IGN)\n"
        f"tmp = Path({str(ready)!r} + '.tmp')\n"
        "tmp.write_text(str(os.getpid()), encoding='utf-8')\n"
        f"os.replace(tmp, {str(ready)!r})\n"
        "while True:\n"
        "    time.sleep(10)\n",
        encoding="utf-8",
    )
    sim_text = (configs / "simulation.yaml").read_text(encoding="utf-8")
    (configs / "ignore-stop-not-joined.yaml").write_text(
        sim_text
        + "  - name: Logger\n"
        + f"    command: [{json.dumps(sys.executable)}, {json.dumps(str(ignore_stop))}]\n"
        + "    describe: false\n",
        encoding="utf-8",
    )

    async def run() -> None:
        launcher = Launcher(Path(LAUNCHER))
        await launcher.probe()

        port = free_port()

        server = GuiServer(Options(
            root=configs,
            launcher=launcher,
            port=port,
            token="test-token",
            stop_timeout_s=2.0,
            participant_start_timeout_ms=10000,
            participant_stop_timeout_ms=10000,
            variables=False,
        ))
        test_server = TestServer(server.app(), host="127.0.0.1", port=port)
        await test_server.start_server()
        http = aiohttp.ClientSession()
        gui = Gui(proc=None, port=port, token="test-token", url=f"http://127.0.0.1:{port}/#token=test-token",
                  stderr=tmp_path / ".err")
        c = Client(gui, http)
        logger_pid: int | None = None
        try:
            ws = await c.ws()
            await ws.until(is_synced)

            st, started = await c.post("api/run", {"file": "ignore-stop-not-joined.yaml"})
            assert st == 200, started
            gui_run_id, instance = started["run_id"], started["instance"]
            await ws.until(lambda m: m["type"] == "event" and m["event"]["event"] == "state")

            async def logger_ready() -> bool:
                return ready.exists()

            assert await until_async(logger_ready, timeout=10.0), "Logger did not install SIG_IGN handler"
            logger_pid = int(ready.read_text(encoding="utf-8"))

            rt = server.runs.runtime
            assert isinstance(rt, ServeRuntime)
            res = rt._resident
            assert res is not None and res.current_run is not None
            run_id = res.current_run.run_id
            assert run_id is not None and res.proc.stdin is not None

            # External graceful stop (force=False) raises C++ stop.requests to 1; Logger ignores SIGTERM so the run
            # stays in stopping state until participant_stop_timeout_ms or an explicit force=True (SIGKILL).
            res.proc.stdin.write(stop_command_line(run_id, force=False))
            await res.proc.stdin.drain()
            await ws.until(
                lambda m: m["type"] == "allowed" and m["allowed"]["stop"]["reason"] == "already-stopping"
            )

            # Graceful POST /api/stop-session {force: false} must NOT escalate C++ stop.requests to 2.
            st_grace, grace_body = await c.post("api/stop-session", {"run_id": gui_run_id, "instance": instance, "force": False})
            assert st_grace == 200, grace_body
            _, s_grace = await c.get("api/session")
            assert s_grace["allowed"]["stop"]["reason"] == "already-stopping"
            assert s_grace["allowed"]["force"]["ok"] is True

            # Verify direct ServeRunHandle.send_stop(force=False) also stays idempotent at requests=1 in C++.
            assert res.current_run.send_stop(force=False) is True
            await res.current_run.drain(1.0)
            queried = rt.execution_status()
            assert queried is not None and queried["run_state"] is not None
            assert queried["run_state"]["stop"]["requests"] == 1

            # Explicit POST /api/stop-session {force: true} escalates C++ stop.requests to 2 and ends the run.
            st_force, _ = await c.post("api/stop-session", {"run_id": gui_run_id, "instance": instance, "force": True})
            assert st_force == 200
            got = await ws.until(is_ended)
            assert got[-1]["session"]["result"]["result"] == "stopped"
            stop_states = [
                m["event"]["stop"]["requests"]
                for m in got
                if m["type"] == "event" and m["event"]["event"] == "state"
            ]
            assert 2 in stop_states
            if sys.platform == "linux":
                assert not alive(logger_pid), f"Logger pid {logger_pid} survived force stop"
            await ws.close()
        finally:
            await http.close()
            await server.shutdown()
            await test_server.close()
            if sys.platform == "linux" and logger_pid is not None and alive(logger_pid):
                try:
                    os.kill(logger_pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

    asyncio.run(run())


def test_real_hot_post_result_drain_stop_is_noop_without_spurious_problem(tmp_path: Path) -> None:
    """Real vehicle_sil_launch: POST api/stop-session during post-result drain leaves stop='none' and problems=[]."""
    import asyncio
    import aiohttp
    from aiohttp.test_utils import TestServer
    from conftest import Gui, free_port, scaled
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options

    configs = tmp_path / "configs"
    write(configs, EXAMPLES)

    async def run() -> None:
        launcher = Launcher(Path(LAUNCHER))
        await launcher.probe()

        port = free_port()

        server = GuiServer(Options(
            root=configs,
            launcher=launcher,
            port=port,
            token="test-token",
            stop_timeout_s=2.0,
            variables=False,
        ))
        entered = asyncio.Event()
        release = asyncio.Event()
        orig_close = server.runs._close_variables

        async def gated_close(run_obj) -> None:
            entered.set()
            await release.wait()
            await orig_close(run_obj)

        server.runs._close_variables = gated_close  # type: ignore[method-assign]
        test_server = TestServer(server.app(), host="127.0.0.1", port=port)
        await test_server.start_server()
        http = aiohttp.ClientSession()
        gui = Gui(proc=None, port=port, token="test-token", url=f"http://127.0.0.1:{port}/#token=test-token",
                  stderr=tmp_path / ".err")
        c = Client(gui, http)
        try:
            ws = await c.ws()
            await ws.until(is_synced)

            st, started = await c.post("api/run", {"file": "simulation.yaml", "speed": 1, "stop_at_ms": 200})
            assert st == 200, started
            sid = started["run_id"]

            await asyncio.wait_for(entered.wait(), timeout=scaled(15.0))
            assert server.runs.session is not None and server.runs.session.state == "running"
            assert server.runs.session.launcher_running is True

            for payload in (
                {"run_id": sid, "instance": server.instance, "force": False},
                {"run_id": sid, "instance": server.instance, "force": True},
            ):
                st_stop, stop_body = await c.post("api/stop-session", payload)
                assert st_stop == 200, (payload, stop_body)
                _, cur = await c.get("api/session")
                assert cur["session"]["stop"] == "none"
                assert cur["session"]["problems"] == []
                assert cur["allowed"]["stop"]["reason"] == "not-running"
                assert cur["allowed"]["force"]["reason"] == "not-running"

            release.set()
            got = await ws.until(is_ended)
            end_sess = got[-1]["session"]
            assert end_sess["state"] == "ended"
            assert end_sess["stop"] == "none"
            assert end_sess["problems"] == []
            assert end_sess["result"]["result"] == "passed"
            assert not any(m["type"] == "error" for m in got)
            await ws.close()
        finally:
            release.set()
            await http.close()
            await server.shutdown()
            await test_server.close()

    asyncio.run(run())


def test_real_recording_mid_run_post_run_end_ms_and_replay_parity(tmp_path: Path) -> None:
    """Real launcher + ecu_main: mid-run save omits end_ms, post-run save records authoritative end_ms,
    and unedited replay reaches the recorded end_ms and Dashboard value."""
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            status, started = await c.post("api/run", {"file": "forever.yaml", "start_paused": True})
            assert status == 200, started
            sid, inst = started["run_id"], started["instance"]
            await ws.until(lambda m: m["type"] == "variables" and len(m["variables"]["peers"]) == 2
                           and all(p["connected"] and p["variables"] for p in m["variables"]["peers"]))
            await ws.send({"type": "watch", "id": 1, "variables": [
                THROTTLE, SPEED, {"peer": "Dashboard", "name": "dashboard.engine_speed"},
            ], "period_ms": 20})
            await ws.until(is_values(1, paused=True, sim_ms=0))

            # Write at 0 ms, step 100 ms
            before = len(ws.received)
            st_w, w = await c.post("api/write", {"run_id": sid, "writes": {"Engine": {"engine.throttle": 50}}})
            assert st_w == 200, w
            await c.post("api/time", {"command": "step", "ms": 100})
            while not (
                any(m["type"] == "operation" and m["seq"] == w["operation"] and m["done"] for m in ws.received[before:])
                and any(is_values(1, paused=True, sim_ms=100)(m) for m in ws.received[before:])
            ):
                await ws.until(lambda m: True)

            # Mid-run save: applied write kept, end_ms omitted, no dummy writes or expect
            _, mid_rec = await c.get("api/recording")
            mr = mid_rec["recording"]
            assert mr["state"] == "running" and mr["end_ms"] is None and mr["writes"] == 1, mr
            mid_doc = yaml.safe_load(mr["yaml"])
            assert "end_ms" not in mid_doc
            assert "expect:" not in mr["yaml"] and "hold_for_ms:" not in mr["yaml"]

            # Advance 400 ms more to 500 ms without any further writes
            await c.post("api/time", {"command": "step", "ms": 400})
            vals_500 = (await ws.until(is_values(1, paused=True, sim_ms=500)))[-1]
            rec_dash_speed = vals_500["peers"]["Dashboard"]["values"]["dashboard.engine_speed"]
            assert rec_dash_speed > 0

            await c.post("api/stop-session", {"run_id": sid, "instance": inst})
            await ws.until(is_ended)

            # Post-run save: authoritative end_ms == 500
            _, end_rec = await c.get("api/recording")
            er = end_rec["recording"]
            assert er["state"] == "ended" and er["end_ms"] == 500, er
            end_doc = yaml.safe_load(er["yaml"])
            assert end_doc["end_ms"] == 500
            assert "expect:" not in er["yaml"] and "hold_for_ms:" not in er["yaml"]

            # Replay the saved YAML without editing: reaches end_ms 500 and matching Dashboard value
            (configs / "scenarios" / "recorded_end.yaml").write_text(er["yaml"], encoding="utf-8")
            before_replay = len(ws.received)
            st_r, started_r = await c.post("api/run", {
                "file": "simulation.yaml", "scenario": "scenarios/recorded_end.yaml", "speed": 0.5,
            })
            assert st_r == 200, started_r
            ready_pred = lambda m: m["type"] == "variables" and m["variables"]["state"] == "ready"
            if not any(ready_pred(m) for m in ws.received[before_replay:]):
                await ws.until(lambda m: m in ws.received[before_replay:] and ready_pred(m))
            await ws.send({"type": "watch", "id": 2, "variables": [
                {"peer": "Dashboard", "name": "dashboard.engine_speed"},
            ], "period_ms": 20})
            if not any(is_ended(m) for m in ws.received[before_replay:]):
                await ws.until(lambda m: m in ws.received[before_replay:] and is_ended(m))
            got_replay = ws.received[before_replay:]
            end_sess = next(m["session"] for m in reversed(got_replay) if is_ended(m))
            assert end_sess["result"]["timing"]["sim_ms"] == 500
            assert end_sess["scenario"]["status"] == "passed"
            assert end_sess["scenario"]["handed_over"] is False
            dash_samples = [
                m["peers"]["Dashboard"]["values"]["dashboard.engine_speed"]
                for m in ws.received[before_replay:]
                if m["type"] == "values" and m["id"] == 2 and "Dashboard" in m["peers"]
            ]
            assert dash_samples and max(dash_samples) == rec_dash_speed
            await ws.close()
        run_async(talk, gui)


def test_real_scenario_handover_holds_at_max_speed_and_unlocks_writes(tmp_path: Path) -> None:
    """Real launcher + ecu_main: after_scenario='pause' hands over at end_ms, holds at speed='max',
    keeps replay writes refused, and unlocks manual write + Step on the same Run."""
    import asyncio

    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    (configs / "scenarios" / "handover.yaml").write_text(
        "vehicle_sil_scenario: 1\nname: handover\nend_ms: 400\nsteps:\n"
        "  - at_ms: 0\n    set: { Engine.engine.throttle: 50 }\n"
        "  - after_ms: 200\n    expect: { Engine.engine.speed: { ge: 1000 } }\n",
        encoding="utf-8",
    )
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)
            st, started = await c.post("api/run", {
                "file": "simulation.yaml",
                "scenario": "scenarios/handover.yaml",
                "after_scenario": "pause",
                "speed": "max",
            })
            assert st == 200, started
            sid, inst = started["run_id"], started["instance"]
            await ws.until(lambda m: m["type"] == "variables" and len(m["variables"]["peers"]) == 2
                           and all(p["connected"] and p["variables"] for p in m["variables"]["peers"]))
            await ws.send({"type": "watch", "id": 1, "variables": [
                THROTTLE, SPEED, {"peer": "Dashboard", "name": "dashboard.engine_speed"},
            ], "period_ms": 20})

            ho_pred = lambda m: (
                m["type"] == "event" and m["event"]["event"] == "state" and m["event"].get("handed_over") is True
            )
            if not any(ho_pred(m) for m in ws.received):
                await ws.until(ho_pred)
            ho_state = next(m["event"] for m in reversed(ws.received) if ho_pred(m))
            assert ho_state["paused"] is True and ho_state["paused_at_ms"] == 400
            assert ho_state["scenario"] is True and ho_state["scenario_over_at_ms"] == 400
            assert ho_state["allowed"]["write"]["ok"] is True

            _, sess_ho = await c.get("api/session")
            assert sess_ho["session"]["scenario"]["handed_over"] is True
            assert sess_ho["session"]["scenario"]["status"] == "passed"
            assert sess_ho["session"]["scenario"]["over_at_ms"] == 400

            # Even at speed="max", virtual time holds at 400 ms until Step/Resume
            await asyncio.sleep(0.3)
            _, sess_held = await c.get("api/session")
            assert sess_held["session"]["time"]["sim_ms"] == 400
            assert sess_held["session"]["launcher_state"]["paused"] is True
            assert not any(
                m["type"] == "event" and m["event"]["event"] == "time" and m["event"]["sim_ms"] > 400
                for m in ws.received
            )

            # Manual write while paused & handed over, followed by Step on the same Run
            before_step = len(ws.received)
            st_w, w = await c.post("api/write", {"run_id": sid, "writes": {"Engine": {"engine.throttle": 0.0}}})
            assert st_w == 200, w
            await ws.until(lambda m: m in ws.received[before_step:] and is_operation(w["operation"], "queued")(m))
            st_step, _ = await c.post("api/time", {"command": "step", "ms": 100})
            assert st_step == 200
            while not (
                any(m["type"] == "operation" and m["seq"] == w["operation"] and m["state"] == "applied"
                    for m in ws.received[before_step:])
                and any(is_values(1, paused=True, sim_ms=500)(m) for m in ws.received[before_step:])
            ):
                await ws.until(lambda m: True)

            vals_500 = next(m for m in ws.received[before_step:] if is_values(1, paused=True, sim_ms=500)(m))
            assert vals_500["peers"]["Engine"]["values"]["engine.throttle"] == 0.0
            assert vals_500["peers"]["Engine"]["values"]["engine.speed"] < 1800

            await c.post("api/stop-session", {"run_id": sid, "instance": inst})
            got_end = await ws.until(is_ended)
            assert got_end[-1]["session"]["scenario"]["handed_over"] is True
            assert got_end[-1]["session"]["scenario"]["status"] == "passed"
            await ws.close()
        run_async(talk, gui)


def test_real_scenario_negative_cases_never_hand_over(tmp_path: Path) -> None:
    """Real launcher: validation rejects invalid after_scenario combinations; normal stop, stop_at_ms,
    incomplete stop, failed scenario, and participant crash never set handed_over=True."""
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    (configs / "scenarios" / "short_end.yaml").write_text(
        "vehicle_sil_scenario: 1\nname: short\nend_ms: 150\nsteps:\n"
        "  - at_ms: 0\n    set: { Engine.engine.throttle: 40 }\n",
        encoding="utf-8",
    )
    (configs / "crash.yaml").write_text(
        (configs / "simulation.yaml").read_text(encoding="utf-8")
        + f"  - name: Crasher\n    command: ['{sys.executable}', '-c', 'import time; time.sleep(0.15); raise SystemExit(2)']\n"
        + "    describe: false\n",
        encoding="utf-8",
    )
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        async def talk(c: Client) -> None:
            # 1. Invalid handover request combinations
            for bad in (
                {"file": "simulation.yaml", "after_scenario": "pause"},
                {"file": "simulation.yaml", "scenario": "scenarios/short_end.yaml",
                 "after_scenario": "pause", "stop_at_ms": 100},
            ):
                st, body = await c.post("api/run", bad)
                assert (st, body["error"]["code"]) == (400, "invalid-field"), (bad, st, body)

            ws = await c.ws()
            await ws.until(is_synced)

            # 2. Normal stop (after_scenario="stop"), stop_at_ms cutoff, failed scenario, participant crash
            for body, expected_res in (
                ({"file": "simulation.yaml", "scenario": "scenarios/short_end.yaml", "after_scenario": "stop"},
                 "passed"),
                ({"file": "simulation.yaml", "scenario": "scenarios/short_end.yaml",
                  "after_scenario": "stop", "stop_at_ms": 50}, "incomplete"),
                ({"file": "simulation.yaml", "scenario": "scenarios/fails.yaml", "after_scenario": "pause"},
                 "failed"),
                ({"file": "crash.yaml", "scenario": "scenarios/short_end.yaml", "after_scenario": "pause"},
                 "failed"),
            ):
                before = len(ws.received)
                st, started = await c.post("api/run", body)
                assert st == 200, (body, started)
                if not any(is_ended(m) for m in ws.received[before:]):
                    await ws.until(is_ended)
                run_msgs = ws.received[before:]
                end_msg = next(m for m in reversed(run_msgs) if is_ended(m))
                assert not any(
                    m["type"] == "event" and m["event"]["event"] == "state" and m["event"].get("handed_over") is True
                    for m in run_msgs
                ), body
                assert end_msg["session"]["result"]["result"] == expected_res
                assert end_msg["session"]["scenario"]["handed_over"] is False

            # 3. Incomplete user stop mid-scenario (plus write refusal during replay)
            before = len(ws.received)
            st, started = await c.post("api/run", {
                "file": "simulation.yaml",
                "scenario": "scenarios/throttle_step.yaml",
                "after_scenario": "pause",
                "speed": 0.01,
            })
            assert st == 200, started
            step2 = lambda m: m["type"] == "event" and m["event"]["event"] == "scenario_step" and m["event"]["index"] == 2
            if not any(step2(m) for m in ws.received[before:]):
                await ws.until(step2)
            st_w, w_err = await c.post("api/write", {
                "run_id": started["run_id"], "writes": {"Engine": {"engine.throttle": 0}},
            })
            assert (st_w, w_err["error"]["code"]) == (409, "scenario-running"), (st_w, w_err)
            await c.post("api/stop-session", {"run_id": started["run_id"], "instance": started["instance"]})
            if not any(is_ended(m) for m in ws.received[before:]):
                await ws.until(is_ended)
            end_inc = next(m for m in reversed(ws.received[before:]) if is_ended(m))
            assert end_inc["session"]["scenario"]["status"] == "incomplete"
            assert end_inc["session"]["scenario"]["handed_over"] is False
            await ws.close()
        run_async(talk, gui)


def test_running_never_exposes_ready_with_empty_peers(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env=control_env) as gui:
        async def talk(c: Client) -> None:
            _, app = await c.get("api/app")
            if not app["variables"]["available"]:
                pytest.skip(app["variables"]["reason"])
            ws = await c.ws()
            await ws.until(is_synced)
            status, started = await c.post("api/run", {"file": "forever.yaml", "speed": 1})
            assert status == 200, started
            await ws.until(lambda m: m["type"] == "event" and m["event"].get("event") == "phase"
                           and m["event"].get("phase") == "running")
            for m in ws.received:
                if m.get("type") == "variables" and m.get("run_id") == started["run_id"]:
                    v = m["variables"]
                    if v["state"] == "ready":
                        assert len(v["peers"]) > 0, v
            await ws.until(is_variables("ready"))
            ready_msgs = [m for m in ws.received if m.get("type") == "variables"
                          and m.get("run_id") == started["run_id"] and m["variables"]["state"] == "ready"]
            assert ready_msgs and len(ready_msgs[0]["variables"]["peers"]) == 2
            await c.post("api/stop-session", {"run_id": started["run_id"], "instance": started["instance"], "force": False})
            await ws.until(is_ended)
            await ws.close()
        run_async(talk, gui)
