"""The ECUs' variables through the GUI server: the schema, watches, writes and their results, with the fake
launcher and its stand-in for the control server (fake_control.py), and what a run without one looks like."""

from __future__ import annotations

import asyncio
import json
import os
import re
from pathlib import Path

import pytest
from conftest import Client, gui_server, is_ended, is_synced, is_variables, run_async

ENGINE = {"peer": "Engine", "name": "engine.demand"}


def is_values(watch: int = 1, **fields):
    return lambda m: m["type"] == "values" and m["id"] == watch and all(m.get(k) == v for k, v in fields.items())


def is_time(**fields):
    return lambda m: m["type"] == "event" and m["event"]["event"] == "time" and \
        all(m["event"].get(k) == v for k, v in fields.items())


def is_operation(seq: int, state: str):
    return lambda m: m["type"] == "operation" and m["seq"] == seq and m["state"] == state


async def start(c: Client, file: str = "forever.yaml", **body) -> tuple:
    ws = await c.ws()
    await ws.until(is_synced)
    status, started = await c.post("api/run", {"file": file, **body})
    assert status == 200, started
    return ws, started["session"]


def test_a_launcher_without_a_control_server_has_no_variables_and_says_why(gui) -> None:
    async def talk(c: Client) -> None:
        _, app = await c.get("api/app")
        assert app["variables"]["available"] is False and "control-server" in app["variables"]["reason"]
        ws, session = await start(c)
        assert session["variables"]["state"] == "off" and session["variables"]["reason"] == app["variables"]["reason"]
        assert session["variables"]["peers"] == []
        await ws.send({"type": "watch", "id": 1, "variables": "all", "period_ms": 20})
        status, body = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 1}}})
        assert (status, body["error"]["code"]) == (400, "unsupported")
        await c.post("api/stop")
        got = await ws.until(is_ended)
        # Told once, right after the run's first session message.
        assert [m["type"] for m in got[:2]] == ["session", "variables"] and got[1]["variables"]["state"] == "off"
        assert not [m for m in got[2:] if m["type"] in ("values", "variables", "operation", "error")]
        assert "variables" not in got[-1]["session"]  # the session messages never carry them
        _, body = await c.get("api/session")
        assert body["session"]["variables"]["state"] == "off"
        status, body = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 1}}})
        assert (status, body["error"]["code"]) == (409, "not-running")
    run_async(talk, gui)


def test_watch_write_and_result(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env=control_env) as gui:
        async def talk(c: Client) -> None:
            _, app = await c.get("api/app")
            assert app["variables"] == {"available": True, "reason": None}
            ws, session = await start(c, start_paused=True)
            assert session["variables"]["state"] == "connecting"
            got = await ws.until(is_variables("ready"))
            schema = got[-1]["variables"]
            assert [p["name"] for p in schema["peers"]] == ["Engine", "Dashboard"]
            demand, speed, mode = schema["peers"][0]["variables"]
            assert demand == {"name": "engine.demand", "type": "real", "role": "input", "writable": True, "unit": "%",
                              "min": 0, "max": 100, "step": None, "description": "requested level", "enum": None}
            assert (speed["role"], speed["writable"], speed["type"]) == ("output", False, "integer")
            assert mode["enum"] == [{"name": "off", "value": 0}, {"name": "on", "value": 1}]
            # Standing still before the first step: no sample yet, and that is complete.
            await ws.send({"type": "watch", "id": 1, "variables": "all", "period_ms": 20})
            first = (await ws.until(is_values(paused=True)))[-1]
            assert first == {"type": "values", "id": 1, "session": session["id"], "sim_ms": 0, "paused": True,
                             "peers": {}}
            # A write before the start, a parameter too: queued, then applied in the first step.
            status, body = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 40, "engine.mode": "on"}}})
            assert status == 200 and body == {"session_id": session["id"], "operation": 1}
            await ws.until(is_operation(1, "queued"))
            # It stands still before the first step: the phase is still waiting, and no time event has come.
            _, body = await c.get("api/session")
            assert (body["session"]["phase"], body["session"]["time"]) == ("waiting", None)
            status, _ = await c.post("api/time", {"command": "step", "ms": 200})
            assert status == 200
            got = await ws.until(is_values(paused=True, sim_ms=200))
            applied = next(m for m in ws.received if is_operation(1, "applied")(m))
            assert applied["done"] is True and applied["peers"] == {
                "Engine": {"state": "applied", "applied_at_ms": 0, "late": False, "reason": None}}
            # Standing still at 200: every step that began before it is in (and the one that begins there has
            # run too), for every peer.
            assert got[-1]["peers"] == {
                "Engine": {"sample_ms": 200, "values": {"engine.demand": 40.0, "engine.speed": 600, "engine.mode": 1}},
                "Dashboard": {"sample_ms": 200, "values": {"dashboard.demand": 0.0, "dashboard.speed": 200,
                                                           "dashboard.mode": 0}}}
            # Nothing that was sent on the way called another time paused.
            assert {(m["sim_ms"]) for m in ws.received if m["type"] == "values" and m["paused"]} == {0, 200}
            # A check meanwhile starts another process: the connection to the control server goes on.
            status, body = await c.post("api/check", {"file": "simulation.yaml", "disable": []})
            assert status == 200 and body["check"]["result"] == "passed"
            # A watch of one variable, replaced under the same id; a second watch.
            await ws.send({"type": "watch", "id": 1, "variables": [ENGINE], "period_ms": 50})
            await ws.send({"type": "watch", "id": 2, "variables": [{"peer": "Dashboard", "name": "dashboard.speed"},
                                                                   {"peer": "Nobody", "name": "x"}]})
            one = (await ws.until(is_values(1)))[-1]
            two = (await ws.until(is_values(2)))[-1]
            assert one["peers"] == {"Engine": {"sample_ms": 200, "values": {"engine.demand": 40.0}}}
            assert two["peers"] == {"Dashboard": {"sample_ms": 200, "values": {"dashboard.speed": 200}}}
            await ws.send({"type": "unwatch", "id": 2})
            # A screen opened now has the schema and the operation in hello.
            late = await c.ws()
            hello = (await late.until(is_synced))[0]["session"]["variables"]
            assert hello["state"] == "ready"
            assert hello["operations"] == [{k: v for k, v in applied.items() if k not in ("type", "session")}]
            await late.close()
            await c.post("api/stop")
            got = await ws.until(is_ended)
            assert [m["variables"]["state"] for m in got if m["type"] == "variables"] == ["ended"]
            assert not [m for m in got if m["type"] == "values" and m["id"] == 2]
            # After the end the last values are still there for a new watch, once.
            await ws.send({"type": "watch", "id": 3, "variables": [ENGINE]})
            last = (await ws.until(is_values(3)))[-1]
            assert last["paused"] is False and last["peers"]["Engine"]["values"] == {"engine.demand": 40.0}
            await ws.close()
        run_async(talk, gui)
        assert gui.stderr_text() == ""


# ---------------------------------------------------------------- the token, the limits, what goes wrong


def collapsed(states: list[str]) -> list[str]:
    """The states without the ones that only repeat the one before (the same state told again with other peers)."""
    return [x for i, x in enumerate(states) if i == 0 or x != states[i - 1]]


def test_the_token_of_the_control_server_stays_in_its_file(tmp_path: Path, control_env: dict) -> None:
    temp = tmp_path / "temp"  # the server's own temporary directory: what is left in it is this server's
    temp.mkdir()
    env = {**control_env, "TMPDIR": str(temp), "TEMP": str(temp), "TMP": str(temp)}
    with gui_server(tmp_path, env=env) as gui:
        async def talk(c: Client) -> None:
            ws, session = await start(c, start_paused=True)
            got = await ws.until(is_variables("ready"))
            hello = next(m["event"] for m in got if m["type"] == "event" and m["event"]["event"] == "hello")
            at = hello["arguments"].index("--control-credentials")
            assert hello["arguments"][at - 2:at] == ["--control", "0"]
            path = Path(hello["arguments"][at + 1])
            # The launcher read one credential from a file only this user can read; the file is gone once the
            # control server listens, long before the run ends.
            lines = [m["event"]["line"] for m in got if m["type"] == "event" and m["event"]["event"] == "log"]
            said = next(line for line in lines if line.startswith("control credentials: "))
            assert said.startswith("control credentials: 1 credential(s), file mode ")
            if os.name == "posix":
                assert said.endswith("file mode 600")
            else:  # no mode bits there: the file was in the user's own temporary directory, and nowhere else
                assert path.parent.resolve() == temp.resolve()
            assert path.parent.resolve() == temp.resolve() and path.name.startswith("vehicle-sil-gui-control-")
            assert not path.exists() and not list(temp.glob("vehicle-sil-gui-control-*"))
            server = next(m["event"] for m in got if m["type"] == "event" and m["event"]["event"] == "control_server")
            assert re.fullmatch(r"127\.0\.0\.1:\d+", server["address"]) and server["credentials"] is True
            await c.post("api/stop")
            got += await ws.until(is_ended)
            # Whatever the screens were sent has neither token.
            text = json.dumps(ws.received) + json.dumps((await c.get("api/session"))[1])
            assert gui.token not in text and "operator" not in text
            assert collapsed([m["variables"]["state"] for m in got if m["type"] == "variables"]) == \
                ["connecting", "ready", "ended"]
        run_async(talk, gui)
        assert gui.stderr_text() == ""
    assert not list(temp.glob("vehicle-sil-gui-control-*"))


@pytest.mark.parametrize("trouble, wait_for, states, reason", [
    ("wrong-token", "failed", ["connecting", "failed"], "refused this GUI server (UNAUTHENTICATED"),
    ("bad-address", "failed", ["connecting", "failed"], "an address that is not 127.0.0.1:<port>"),
    ("unreachable", "connecting", ["connecting", "failed"], "cannot be reached until the run ended"),
    ("closes", "connecting", ["connecting", "ready", "connecting", "ended"], "was lost before the run ended"),
])
def test_trouble_with_the_control_server_leaves_the_run_alone(tmp_path: Path, control_env: dict, trouble: str,
                                                              wait_for: str, states: list[str], reason: str) -> None:
    # Each state is waited for before the next thing is done: the order is the test's, not the clock's.
    with gui_server(tmp_path, env={**control_env, "FAKE_LAUNCHER_CONTROL": trouble}) as gui:
        async def talk(c: Client) -> None:
            ws, session = await start(c, start_paused=True)
            await ws.send({"type": "watch", "id": 1, "variables": "all"})
            if trouble == "closes":  # the control server ends after the third step
                await ws.until(is_variables("ready"))
                await c.post("api/time", {"command": "resume"})
            # the trouble shows: the state, with a reason
            got = await ws.until(lambda m: m["type"] == "variables" and m["variables"]["state"] == wait_for
                                 and m["variables"]["reason"])
            assert gui.token not in got[-1]["variables"]["reason"]
            if trouble != "closes":
                status, body = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 1}}})
                assert (status, body["error"]["code"]) == (409, "variables-not-ready")
                await c.post("api/time", {"command": "resume"})
            # the run itself goes on as without variables
            await ws.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time" and m["event"]["sim_ms"] > 300)
            await c.post("api/stop")
            got = await ws.until(is_ended)
            assert got[-1]["session"]["result"]["result"] == "interrupted"
            told = [m["variables"] for m in ws.received if m["type"] == "variables"]
            assert collapsed([v["state"] for v in told]) == states
            assert reason in told[-1]["reason"] and gui.token not in json.dumps(ws.received)
            if trouble != "closes":
                assert not [m for m in ws.received if m["type"] == "values"]
            status, body = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 1}}})
            assert (status, body["error"]["code"]) == (409, "not-running")
            _, app = await c.get("api/app")
            assert app["variables"]["available"] is True
        run_async(talk, gui)
        assert gui.stderr_text() == ""


def test_a_write_while_the_variables_cannot_be_reached_is_refused_with_the_reason(tmp_path: Path, control_env) -> None:
    with gui_server(tmp_path, env={**control_env, "FAKE_LAUNCHER_CONTROL": "unreachable"}) as gui:
        async def talk(c: Client) -> None:
            ws, session = await start(c)
            await ws.until(lambda m: m["type"] == "variables" and m["variables"]["reason"])
            status, body = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 1}}})
            assert (status, body["error"]["code"]) == (409, "variables-not-ready")
            assert "cannot be reached" in body["error"]["message"]
            await c.post("api/stop")
            await ws.until(is_ended)
        run_async(talk, gui)


def test_the_launcher_dying_ends_the_variables_and_the_open_operations(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env={**control_env, "FAKE_LAUNCHER_SCENARIO": "launcher-crash"}) as gui:
        async def talk(c: Client) -> None:
            ws, session = await start(c, start_paused=True)
            await ws.until(is_variables("ready"))
            status, body = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 5}}, "at_ms": 100000})
            assert status == 200
            await ws.until(is_operation(body["operation"], "queued"))
            await c.post("api/time", {"command": "resume"})
            got = await ws.until(is_ended)
            end = next(m for m in got if m["type"] == "operation" and m["done"])
            assert end["state"] == "unknown" and end["peers"]["Engine"]["reason"] == "the run ended before the result came"
            last = [m["variables"] for m in got if m["type"] == "variables"][-1]
            assert last["state"] == "ended" and last["operations"][-1]["state"] == "unknown"
            assert got[-1]["session"]["launcher"]["exit_code"] == 70
            status, _ = await c.get("api/app")  # the server goes on
            assert status == 200
        run_async(talk, gui)


def test_writes_that_are_wrong_are_refused_before_they_reach_the_launcher(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env=control_env) as gui:
        async def talk(c: Client) -> None:
            ws, session = await start(c, start_paused=True)
            await ws.until(is_variables("ready"))
            for raw, code in ((b'{"writes": {"Engine": {"engine.demand": NaN}}}', "invalid-field"),
                              (b'{"writes": {"Engine": {"engine.demand": Infinity}}}', "invalid-field"),
                              (b'{"writes": {"Engine": {"engine.demand": 1e999}}}', "invalid-field"),
                              (b'{"writes": {"Engine": {"engine.demand": 1' + b"0" * 400 + b'}}}', "invalid-field"),
                              (b'{"writes": {"Engine": {"engine.demand": 100.01}}}', "invalid-field"),
                              (b'{"writes": {"Engine": {"engine.speed": 1}}}', "not-writable"),
                              (b'{"writes": {"Engine": {"engine.nothing": 1}}}', "unknown-variable"),
                              (b'{"writes": {"Nobody": {"x": 1}}}', "unknown-variable"),
                              (b'{"writes": {"Engine": {"engine.mode": "sideways"}}}', "invalid-field"),
                              (b'{"writes": {"Engine": {"engine.demand": 1}}, "at_ms": -5}', "invalid-field"),
                              (b'[1]', "invalid-json"), (b'{"writes"', "invalid-json")):
                status, body = await c.post("api/write", raw=raw)
                assert (status, body["error"]["code"]) == (400, code), raw[:60]
                assert len(body["error"]["message"]) < 400
            # None of them became an operation; and the checks of every API hold here too.
            _, body = await c.get("api/session")
            assert body["session"]["variables"]["operations"] == []
            ok = {"writes": {"Engine": {"engine.demand": 1}}}
            assert (await c.post("api/write", ok, token="wrong"))[0] == 401
            assert (await c.post("api/write", ok, origin="http://evil.example"))[0] == 403
            assert (await c.post("api/write", ok, origin=None))[0] == 403
            assert (await c.post("api/write", ok, host="evil.example"))[0] == 403
            assert (await c.post("api/write", ok, content_type="text/plain"))[0] == 415
            assert (await c.get("api/write"))[0] == 404
            status, body = await c.post("api/write", raw=b'{"writes": {"Engine": {"' + b"x" * 70000 + b'": 1}}}')
            assert (status, body["error"]["code"]) == (413, "too-large")
            # A parameter after the first step: the control server's answer, as the operation's result.
            await c.post("api/time", {"command": "step", "ms": 100})
            await ws.until(is_time(sim_ms=100, paused=True))
            status, body = await c.post("api/write", {"writes": {"Engine": {"engine.mode": 1}}})
            assert status == 200
            got = await ws.until(is_operation(body["operation"], "invalid"))
            assert "parameter" in got[-1]["peers"]["Engine"]["reason"]

            # Standing still at 100, the step that begins at 100 has run: a write for 100 (or earlier) is too
            # late: at the next step it is refused, or made when asked so; a write for 101 is in time.
            status, late = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 2}}, "at_ms": 100})
            status, apply = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 3}}, "at_ms": 0,
                                                       "late": "apply"})
            status, timely = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 9}}, "at_ms": 101})
            await ws.until(is_operation(timely["operation"], "queued"))
            await c.post("api/time", {"command": "step", "ms": 100})
            await ws.until(is_operation(timely["operation"], "applied"))
            done = {m["seq"]: m["peers"]["Engine"] for m in ws.received if m["type"] == "operation" and m["done"]}
            assert done[late["operation"]] == {"state": "late", "applied_at_ms": None, "late": True,
                                               "reason": "its time had passed"}
            assert done[apply["operation"]] == {"state": "applied", "applied_at_ms": 200, "late": True, "reason": None}
            assert done[timely["operation"]] == {"state": "applied", "applied_at_ms": 200, "late": False,
                                                 "reason": None}
            await c.post("api/stop")
            await ws.until(is_ended)
        run_async(talk, gui)


def test_watches_that_are_wrong_get_an_error_and_the_connection_goes_on(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env=control_env) as gui:
        async def talk(c: Client) -> None:
            ws, session = await start(c, start_paused=True)
            await ws.until(is_variables("ready"))
            for bad, code in (({"type": "watch", "id": 1, "variables": "all", "period_ms": 5}, "invalid-watch"),
                              ({"type": "watch", "variables": "all"}, "invalid-watch"),
                              ({"type": "watch", "id": 1, "variables": [["Engine", "engine.demand"]]}, "invalid-watch"),
                              ({"no": "type"}, "invalid-message"), ([1, 2], "invalid-message")):
                await ws.ws.send_str(json.dumps(bad))
                got = await ws.until(lambda m: m["type"] == "error")
                assert got[-1]["code"] == code and got[-1]["session"] is None, bad
            await ws.ws.send_str("not json")
            assert (await ws.until(lambda m: m["type"] == "error"))[-1]["code"] == "invalid-message"
            await ws.send({"type": "something-new", "id": 1})  # a type from a newer screen: ignored
            await ws.send({"type": "unwatch", "id": 77})  # not there: nothing
            for n in range(16):
                await ws.send({"type": "watch", "id": n, "variables": "all", "period_ms": 1000})
            await ws.send({"type": "watch", "id": 99, "variables": "all"})
            got = await ws.until(lambda m: m["type"] == "error")
            assert got[-1]["code"] == "too-many-watches" and got[-1]["detail"] == {"id": 99}
            seen = {m["id"] for m in got if m["type"] == "values"}
            while seen != set(range(16)):  # each of the 16 gets its values
                seen.add((await ws.until(lambda m: m["type"] == "values"))[-1]["id"])
            await ws.send({"type": "watch", "id": 3, "variables": [ENGINE], "period_ms": 20})  # replacing is not one more
            await ws.send({"type": "unwatch", "id": 4})
            await ws.send({"type": "watch", "id": 99, "variables": [ENGINE]})
            await ws.until(is_values(99))
            assert not [m for m in ws.received if m["type"] == "error" and m["code"] == "too-many-watches"][1:]
            # The watches of a connection end with it.
            await ws.close()
            other = await c.ws()
            await other.until(is_synced)
            await c.post("api/time", {"command": "step", "ms": 100})
            await c.post("api/stop")
            got = await other.until(is_ended)
            assert not [m for m in got if m["type"] == "values"]
        run_async(talk, gui)
        assert gui.stderr_text() == ""


def test_a_screen_that_does_not_read_gets_the_latest_values_when_it_reads_again(tmp_path: Path, control_env) -> None:
    # A run at full speed: the values change with every step, many thousand times while the slow screen sleeps.
    with gui_server(tmp_path, env={**control_env, "FAKE_LAUNCHER_TICK_MS": "0"}) as gui:
        async def talk(c: Client) -> None:
            slow = await c.ws()
            await slow.until(is_synced)
            fast = await c.ws()
            await fast.until(is_synced)
            await c.post("api/run", {"file": "forever.yaml", "speed": "max"})
            for ws in (slow, fast):
                await ws.send({"type": "watch", "id": 1, "variables": "all", "period_ms": 20})
            await slow.until(is_values(1))
            # The fast screen gets them about every period all the while (the slow one holds nobody up).
            started = asyncio.get_running_loop().time()
            got = []
            while asyncio.get_running_loop().time() - started < 1.5:
                got += await fast.until(is_values(1))
            values = [m for m in got if m["type"] == "values"]
            assert len(values) >= 5, len(values)
            times = [m["peers"]["Engine"]["sample_ms"] for m in values]
            assert times == sorted(times) and times[-1] > times[0]
            newest = times[-1]
            # The slow screen now reads. What waits for it is what its connection's buffers took before they
            # were full, not one message per period: the server made no message while it could not send one.
            got = await slow.until(lambda m: m["type"] == "values" and m["peers"]["Engine"]["sample_ms"] >= newest)
            backlog = [m for m in got if m["type"] == "values"]
            assert len(backlog) < len(values), f"{len(backlog)} values messages had piled up"
            status, body = await c.post("api/write", {"writes": {"Engine": {"engine.demand": 7}}})
            assert status == 200
            for ws in (slow, fast):  # a result is never dropped, for either
                await ws.until(is_operation(body["operation"], "applied"))
            await c.post("api/stop")
            await fast.until(is_ended)
        run_async(talk, gui)
        assert gui.stderr_text() == ""


# ---------------------------------------------------------------- before the first step


def test_a_run_that_stands_before_its_first_step_is_resumed_from_the_screen(tmp_path: Path, control_env) -> None:
    with gui_server(tmp_path, env=control_env) as gui:
        async def talk(c: Client) -> None:
            ws, session = await start(c, file="simulation.yaml", start_paused=True)
            got = await ws.until(is_variables("ready"))
            lines = [m["event"]["line"] for m in got if m["type"] == "event" and m["event"]["event"] == "log"]
            assert any(line.startswith("standing still before the first step") for line in lines)
            # A pause changes nothing; the phase stays waiting, and there is still no time event.
            await c.post("api/time", {"command": "pause"})
            await ws.until(lambda m: m["type"] == "event" and m["event"]["event"] == "command")
            _, body = await c.get("api/session")
            assert (body["session"]["phase"], body["session"]["time"]) == ("waiting", None)
            assert body["session"]["last_command"]["accepted"] is True
            status, _ = await c.post("api/time", {"command": "resume"})
            assert status == 200
            got = await ws.until(is_ended)
            assert got[-1]["session"]["result"]["result"] == "passed"
            phases = [m["event"]["phase"] for m in ws.received if m["type"] == "event" and m["event"]["event"] == "phase"]
            assert phases[phases.index("waiting"):] == ["waiting", "running", "stopping", "ended"]
        run_async(talk, gui)
        assert gui.stderr_text() == ""


# ---------------------------------------------------------------- without the variables


def test_no_variables_when_the_server_is_told_so_or_cannot_load_the_stubs(tmp_path: Path, control_env) -> None:
    empty = tmp_path / "no-stubs"
    empty.mkdir()
    for args, env, why in ((("--no-variables",), control_env, "--no-variables"),
                           ((), {**control_env, "VEHICLE_SIL_CONTROL_STUBS": str(empty)}, "has no ")):
        with gui_server(tmp_path, env=env, args=args) as gui:
            async def talk(c: Client) -> None:
                _, app = await c.get("api/app")
                assert app["variables"]["available"] is False and why in app["variables"]["reason"]
                ws, session = await start(c, file="simulation.yaml", start_paused=True)
                assert session["variables"] == {**session["variables"], "state": "off",
                                                "reason": app["variables"]["reason"]}
                # Without the control server the launcher stands still after its first step, as before.
                await ws.until(is_time(paused=True))
                hello = next(m["event"] for m in ws.received if m["type"] == "event" and m["event"]["event"] == "hello")
                assert "--control" not in hello["arguments"]
                await c.post("api/time", {"command": "resume"})
                got = await ws.until(is_ended)
                assert got[-1]["session"]["result"]["result"] == "passed"
            run_async(talk, gui)
            assert gui.stderr_text() == ""


def test_a_watch_before_the_auth_is_not_taken(gui) -> None:
    async def talk(c: Client) -> None:
        ws = await c.ws(auth=False)
        await ws.send({"type": "watch", "id": 1, "variables": "all"})
        got = await ws.until(lambda m: m["type"] == "error")
        assert got[-1]["code"] == "unauthorized"
        with pytest.raises(ConnectionError):
            await ws.until(lambda m: False, timeout=5)
    run_async(talk, gui)


def test_a_message_larger_than_the_limit_ends_the_connection(tmp_path: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env=control_env) as gui:
        async def talk(c: Client) -> None:
            ws, session = await start(c, start_paused=True)
            await ws.until(is_variables("ready"))
            names = [{"peer": "Engine", "name": "n" * 250 + str(n)} for n in range(1100)]  # more than 256 KiB
            await ws.send({"type": "watch", "id": 1, "variables": names})
            try:
                await ws.until(lambda m: False, timeout=10)
                closed = None
            except ConnectionError as e:
                closed = str(e)
            assert closed is not None and "1009" in closed, closed
            # The server and the run go on; a screen that connects again is served.
            again = await c.ws()
            await again.until(is_synced)
            await again.send({"type": "watch", "id": 1, "variables": "all"})
            await again.until(is_values(1))
            await c.post("api/stop")
            await again.until(is_ended)
        run_async(talk, gui)
        assert gui.stderr_text() == ""
