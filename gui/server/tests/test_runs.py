"""Runs through the API and the WebSocket with the stand-in launcher's recordings, reconnection, and stopping the
server with a run going (no process may be left)."""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import signal
import sys
import time
from collections.abc import Callable
from pathlib import Path

import pytest
from conftest import (CONFIGS, REPLAY, Client, Gui, alive, gui_server, is_ended, is_synced, run_async, scaled, wait_until,
                      without_variables)

POSIX = sys.platform != "win32"


def events(messages: list[dict], kind: str | None = None) -> list[dict]:
    return [m["event"] for m in messages if m["type"] == "event" and (kind is None or m["event"]["event"] == kind)]


def started_pids(messages: list[dict]) -> list[int]:
    return [e["pid"] for e in events(messages, "participant") if e["state"] == "started"]


async def run_to_end(c: Client, body: dict) -> list[dict]:
    sock = await c.ws()
    await sock.until(is_synced)
    status, reply = await c.post("api/run", body)
    assert status == 200, reply
    got = await sock.until(is_ended)
    await sock.close()
    return got


SPEED = {"REPLAY_FEATURES": "events,stdin-commands,check-variables,speed"}
OLD = {"REPLAY_FEATURES": "events,stdin-commands,check-variables"}


def is_time(**fields) -> Callable[[dict], bool]:
    """A time event with these values."""
    return lambda m: m["type"] == "event" and m["event"]["event"] == "time" and \
        all(m["event"].get(k) == v for k, v in fields.items())


def is_command(name: str, accepted: bool) -> Callable[[dict], bool]:
    return lambda m: m["type"] == "event" and m["event"]["event"] == "command" and \
        m["event"]["command"] == name and m["event"]["accepted"] is accepted


def test_a_run_that_passes(tmp_path: Path) -> None:
    async def main(c: Client) -> None:
        status, app = await c.get("api/app")
        assert app["launcher"]["usable"] and app["initial_file"] is None
        got = await run_to_end(c, {"file": "simulation.yaml", "disable": ["Logger"], "speed": 2})
        seqs = [e["seq"] for e in events(got)]
        assert seqs == list(range(len(seqs)))
        hello = events(got, "hello")[0]
        assert hello["arguments"] == ["run", "--events", "--stdin-commands", "--disable", "Logger", "--speed", "2",
                                      "--", "simulation.yaml"]
        end = got[-1]["session"]
        assert end["launcher"]["exit_code"] == 0 and end["result"]["result"] == "passed" and end["problems"] == []
        assert {p["name"]: p["state"] for p in end["participants"]} == \
            {"Engine": "ended", "Dashboard": "ended", "Logger": "disabled"}
        assert [p["name"] for p in end["check"]["participants"]] == ["Engine", "Dashboard", "Logger"]
        assert all(not alive(pid) for pid in started_pids(got))
        status, body = await c.get("api/session")
        assert without_variables(body["session"]) == end and body["session"]["variables"]["state"] == "off"
    with gui_server(tmp_path, env=SPEED) as gui:
        run_async(main, gui)


def test_a_launcher_that_writes_nothing_for_a_while_is_reported(tmp_path: Path) -> None:
    env = {"REPLAY_FAULT": "silent", "REPLAY_SILENT_S": "4"}
    with gui_server(tmp_path, env=env, args=("--silent-after", "1.5")) as gui:
        async def main(c: Client) -> None:
            got = await run_to_end(c, {"file": "simulation.yaml"})
            summaries = [m["session"] for m in got if m["type"] == "session"]
            silent = [s["launcher"]["silent_s"] for s in summaries]
            assert any(s is not None and s >= 1.5 for s in silent), silent
            assert silent[-1] is None, "it wrote again: no longer silent"
            problems = [m for m in got if m["type"] == "error"]
            assert [p["code"] for p in problems] == ["launcher-silent"], problems
            assert got[-1]["session"]["result"]["result"] == "passed"
        run_async(main, gui)


def test_lines_the_launcher_could_not_write_are_counted(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_FAULT": "log-dropped"}) as gui:
        async def main(c: Client) -> None:
            got = await run_to_end(c, {"file": "simulation.yaml", "disable": ["Logger"]})
            assert len(events(got, "log_dropped")) == 1
            log = got[-1]["session"]["log"]
            assert log["Engine"]["dropped"] == 120 and log["vehicle_sil_launch"]["dropped"] == 5
            sock = await c.ws()  # a screen that connects later gets the marker in its place
            replay = await sock.until(is_synced)
            assert [e["seq"] for e in events(replay, "log_dropped")] == [events(got, "log_dropped")[0]["seq"]]
            await sock.close()
        run_async(main, gui)


def test_a_participant_that_the_user_starts(tmp_path: Path) -> None:
    """manual: given to the launcher as --manual; it is not started, its command is in the summary; stopped while
    the run waits for it, the simulation does not start. A launcher without the feature: refused."""
    async def main(c: Client) -> None:
        status, body = await c.post("api/check", {"file": "simulation.yaml", "manual": ["Dashboard"]})
        assert status == 200 and body["manual"] == ["Dashboard"]
        assert [p["manual"] for p in body["check"]["participants"]] == [False, True, False]
        sock = await c.ws()
        await sock.until(is_synced)
        status, reply = await c.post("api/run", {"file": "simulation.yaml", "manual": ["Dashboard"]})
        assert status == 200 and reply["session"]["manual"] == ["Dashboard"]
        got = await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "participant"
                               and m["event"]["state"] == "manual")
        assert "--manual" in events(got, "hello")[0]["arguments"]
        await c.post("api/stop", {})
        got = await sock.until(is_ended)
        end = got[-1]["session"]
        dashboard = next(p for p in end["participants"] if p["name"] == "Dashboard")
        assert dashboard["state"] == "manual" and "--name Dashboard" in dashboard["command_line"]
        assert dashboard["working_directory"] and "pid" not in dashboard
        assert end["result"]["result"] == "interrupted" and end["result"]["simulation"] == "not_started"
        assert [p["manual"] for p in end["result"]["participants"]] == [False, True, False]
    with gui_server(tmp_path) as gui:
        run_async(main, gui)
    with gui_server(tmp_path, env=OLD) as gui:
        async def old(c: Client) -> None:
            for path in ("api/check", "api/run"):
                status, body = await c.post(path, {"file": "simulation.yaml", "manual": ["Dashboard"]})
                assert status == 400 and body["error"]["code"] == "unsupported", path
                assert "manual-participants" in body["error"]["message"]
        run_async(old, gui)


def test_the_environment_to_start_a_participant_with_reaches_the_screen(gui: Gui) -> None:
    """A registry that is not on a loopback address (network.yaml): the check's note registry-on-network, and the
    environment of the manual event (VEHICLE_SIL_LISTEN_ON_NETWORK=1) in the event and in the summary, as the
    launcher wrote them."""
    async def main(c: Client) -> None:
        status, body = await c.post("api/check", {"file": "network.yaml"})
        assert status == 200
        notes = {f["code"]: f for f in body["check"]["findings"]}
        assert notes["registry-on-network"]["severity"] == "note"
        assert "VEHICLE_SIL_LISTEN_ON_NETWORK=1" in notes["participant-manual"]["message"]
        sock = await c.ws()
        await sock.until(is_synced)
        status, _ = await c.post("api/run", {"file": "network.yaml"})
        assert status == 200
        got = await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "participant"
                               and m["event"]["state"] == "manual")
        assert events(got, "participant")[-1]["environment"] == {"VEHICLE_SIL_LISTEN_ON_NETWORK": "1"}
        await c.post("api/stop", {})
        end = (await sock.until(is_ended))[-1]["session"]
        dashboard = next(p for p in end["participants"] if p["name"] == "Dashboard")
        assert dashboard["environment"] == {"VEHICLE_SIL_LISTEN_ON_NETWORK": "1"}
        engine = next(p for p in end["participants"] if p["name"] == "Engine")
        assert "environment" not in engine
    run_async(main, gui)


def test_check_then_run_with_a_participant_left_out(gui: Gui) -> None:
    async def main(c: Client) -> None:
        status, body = await c.post("api/check", {"file": "simulation.yaml", "disable": ["Dashboard"]})
        assert status == 200 and body["exit_code"] == 0
        dashboard = next(p for p in body["check"]["participants"] if p["name"] == "Dashboard")
        assert dashboard["status"] == "disabled" and dashboard["variables"] is None
        engine = next(p for p in body["check"]["participants"] if p["name"] == "Engine")
        # every ECU has ecu.connected first (builtin: vehicle-sil adds it), then its own
        assert [v["name"] for v in engine["variables"]] == ["ecu.connected", "engine.demand", "engine.speed",
                                                             "engine.mode"]
        assert [v["builtin"] for v in engine["variables"]] == [True, False, False, False]
        status, body = await c.post("api/check", {"file": "check-error.yaml"})
        assert status == 200 and body["exit_code"] == 1 and body["check"]["result"] == "failed"
        got = await run_to_end(c, {"file": "check-error.yaml"})
        assert got[-1]["session"]["result"]["result"] == "check_failed" and not started_pids(got)
    run_async(main, gui)


def test_one_run_at_a_time_and_stop(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "50"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            status, _ = await c.post("api/run", {"file": "forever.yaml"})
            assert status == 200
            status, body = await c.post("api/run", {"file": "simulation.yaml"})
            assert status == 409 and body["error"]["code"] == "already-running"
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            status, body = await c.post("api/stop")
            assert status == 200 and body["session"]["stop"] == "requested"
            got = await sock.until(is_ended)
            end = got[-1]["session"]
            assert end["launcher"]["exit_code"] == 130 and end["result"]["result"] == "interrupted"
            assert all(not alive(pid) for pid in started_pids(sock.received))
            status, _ = await c.post("api/run", {"file": "simulation.yaml"})  # the next run may start
            assert status == 200
            got = await sock.until(is_ended)
            assert got[0]["type"] == "session" and got[0]["session"]["id"] == 2
        run_async(main, gui)


def test_a_second_stop_does_not_wait(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "50"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "ignore-stop.yaml"})
            await sock.until(lambda m: m.get("event", {}).get("event") == "time")
            await c.post("api/stop")
            await sock.until(lambda m: m.get("event", {}).get("command") == "stop")
            await asyncio.sleep(0.3)
            status, body = await c.get("api/session")
            assert body["session"]["state"] == "running"  # this launcher waits after the first stop
            status, body = await c.post("api/stop", {"force": True})
            assert body["session"]["stop"] == "forced"
            got = await sock.until(is_ended)
            # Engine and Dashboard end when asked; Logger (it ignores SIGTERM) is killed by the second stop
            assert [p["state"] for p in got[-1]["session"]["participants"]] == ["ended", "ended", "killed"]
        run_async(main, gui)


def test_problems_of_the_launcher_are_shown(gui: Gui) -> None:
    async def main(c: Client) -> None:
        got = await run_to_end(c, {"file": "participant-crash.yaml"})
        end = got[-1]["session"]
        assert end["result"]["result"] == "failed" and end["findings"][0]["code"] == "participant-failed"
        assert {p["name"]: p.get("exit_code") for p in end["participants"]}["Dashboard"] == 3
        got = await run_to_end(c, {"file": "launcher-crash.yaml"})
        end = got[-1]["session"]
        assert end["launcher"]["exit_code"] == 70
        assert [p["code"] for p in end["problems"]] == ["launcher-stderr", "no-result"]
        if POSIX:  # what the dead launcher left in its process group is killed
            assert wait_until(lambda: all(not alive(pid) for pid in started_pids(got)), 5)
        got = await run_to_end(c, {"file": "bad-lines.yaml"})
        assert [m["code"] for m in got if m["type"] == "error"] == ["launcher-output", "launcher-output", "seq-gap"]
        assert events(got, "mystery")  # an unknown event is passed on
        assert any(e.get("colour") == "blue" for e in events(got, "log"))  # unknown fields are passed on
        assert got[-1]["session"]["result"]["result"] == "passed"
        got = await run_to_end(c, {"file": "future-events.yaml"})
        assert [m["code"] for m in got if m["type"] == "error"] == ["events-format"]
    run_async(main, gui)


def test_a_launcher_that_runs_on_after_its_result_can_be_stopped(gui: Gui) -> None:
    async def main(c: Client) -> None:
        sock = await c.ws()
        await sock.until(is_synced)
        assert (await c.post("api/run", {"file": "result-then-wait.yaml"}))[0] == 200
        await sock.until(lambda m: m.get("event", {}).get("event") == "result")
        await asyncio.sleep(0.5)
        status, body = await c.get("api/session")
        summary = body["session"]
        assert summary["result"]["result"] == "passed" and summary["phase"] == "ended"
        assert summary["launcher"]["running"] and summary["state"] == "running"  # so the screen offers Stop
        status, body = await c.post("api/stop", {})
        assert status == 200 and body["session"]["stop"] == "requested"
        got = await sock.until(is_ended)
        end = got[-1]["session"]
        assert end["launcher"]["running"] is False and end["launcher"]["exit_code"] == 0
        # the launcher writes nothing after its result, not even the command event of this stop
        assert end["result"]["result"] == "passed" and end["last_command"] is None
    run_async(main, gui)


def test_reconnecting_replays_the_kept_log(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_FLOOD": "2500"}) as gui:
        async def main(c: Client) -> None:
            live = await run_to_end(c, {"file": "flood.yaml"})
            again = await c.ws()
            got = await again.until(is_synced)
            hello = got[0]
            assert hello["type"] == "hello" and without_variables(hello["session"]) == live[-1]["session"]
            assert hello["replay"] == {"from_seq": None}
            live_engine = [e for e in events(live, "log") if e["source"] == "Engine"]
            assert got[1] == {"type": "dropped", "session": 1, "reason": "not-kept",
                              "log": {"Engine": len(live_engine) - 2000}, "at_least": [], "events": 0}
            assert hello["session"]["log"]["Engine"] == {"kept": 2000, "dropped": len(live_engine) - 2000}
            assert events(got) == [e for e in events(live, "log") if e not in live_engine[:-2000]]
            assert got[-1] == {"type": "synced", "session": 1, "last_seq": events(live)[-1]["seq"]}
            from_seq = events(live, "log")[-5]["seq"]
            resumed = await c.ws({"session": 1, "from_seq": from_seq})
            got = await resumed.until(is_synced)
            assert got[0]["replay"] == {"from_seq": from_seq} and events(got) == events(live, "log")[-5:]
            other = await c.ws({"session": 99, "from_seq": 10_000})  # an old session: everything again
            got = await other.until(is_synced)
            assert got[0]["replay"]["from_seq"] is None and len(events(got)) > 2000
        run_async(main, gui)


def test_several_screens_see_the_same_run(gui: Gui) -> None:
    async def main(c: Client) -> None:
        a, b = await c.ws(), await c.ws()
        await a.until(is_synced)
        await b.until(is_synced)
        await c.post("api/run", {"file": "simulation.yaml"})
        got_a, got_b = await asyncio.gather(a.until(is_ended), b.until(is_ended))
        assert events(got_a) == events(got_b)
    run_async(main, gui)


def test_checks_and_configs(tmp_path: Path) -> None:
    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)
    (root / "nested" / "deeper").mkdir(parents=True)
    shutil.copy(CONFIGS / "simulation.yaml", root / "nested" / "deeper" / "n.yml")
    (root / ".git").mkdir()
    (root / ".git" / "x.yaml").write_text("vehicle_sil_simulation: 1\n")
    with gui_server(tmp_path, root=root, env={"REPLAY_CHECK_DELAY_MS": "700"},
                    args=(str(root / "nested" / "deeper" / "n.yml"),)) as gui:
        async def main(c: Client) -> None:
            status, app = await c.get("api/app")
            assert app["initial_file"] == "nested/deeper/n.yml" and app["root"] == str(root.resolve())
            status, body = await c.get("api/configs")
            paths = [f["path"] for f in body["files"]]
            assert "nested/deeper/n.yml" in paths and not any(p.startswith(".git") for p in paths)
            results = await asyncio.gather(*(c.post("api/check", {"file": "simulation.yaml"}) for _ in range(3)))
            assert [s for s, _ in results] == [200, 200, 200]  # they wait their turn
        run_async(main, gui)


# ---------------------------------------------------------------- the server's end


def start_run(gui: Gui, file: str, wait_for: str = "time") -> list[int]:
    """Starts a run and returns the pids of the launcher and its participants once it is running."""
    async def main(c: Client) -> list[int]:
        sock = await c.ws()
        await sock.until(is_synced)
        status, body = await c.post("api/run", {"file": file})
        assert status == 200, body
        await sock.until(lambda m: m.get("event", {}).get("event") == wait_for)
        await sock.close()
        return [body["session"]["launcher"]["pid"], *started_pids(sock.received)]
    return run_async(main, gui)


@pytest.mark.skipif(not POSIX, reason="signals and /proc")
@pytest.mark.parametrize("how", [signal.SIGTERM, signal.SIGINT, signal.SIGHUP])
def test_stopping_the_server_stops_the_run(tmp_path: Path, how: int) -> None:
    with gui_server(tmp_path) as gui:
        pids = start_run(gui, "forever.yaml")
        assert len(pids) == 4 and all(alive(p) for p in pids)
        start = time.monotonic()
        gui.proc.send_signal(how)
        assert gui.proc.wait(scaled(20)) == 0
        assert time.monotonic() - start < 5  # in order, through the end of input: no waiting for a timeout
        assert wait_until(lambda: not any(alive(p) for p in pids), 5)


@pytest.mark.skipif(not POSIX, reason="signals and /proc")
def test_a_launcher_that_does_not_stop_is_killed(tmp_path: Path) -> None:
    with gui_server(tmp_path, args=("--stop-timeout", "0.5")) as gui:
        pids = start_run(gui, "ignore-stop.yaml")
        start = time.monotonic()
        gui.proc.send_signal(signal.SIGTERM)
        assert gui.proc.wait(scaled(20)) == 0
        assert time.monotonic() - start < 10
        assert wait_until(lambda: not any(alive(p) for p in pids), 5)


@pytest.mark.skipif(not POSIX, reason="signals and /proc")
def test_a_second_signal_skips_the_waiting(tmp_path: Path) -> None:
    with gui_server(tmp_path, args=("--stop-timeout", "60")) as gui:
        pids = start_run(gui, "ignore-stop.yaml")
        gui.proc.send_signal(signal.SIGINT)
        time.sleep(0.5)
        assert gui.proc.poll() is None  # waiting for the launcher, which ignores the first stop
        start = time.monotonic()
        gui.proc.send_signal(signal.SIGINT)
        assert gui.proc.wait(scaled(20)) == 0
        assert time.monotonic() - start < 10
        assert wait_until(lambda: not any(alive(p) for p in pids), 5)


@pytest.mark.skipif(not POSIX, reason="signals and /proc")
def test_a_killed_server_leaves_no_run(tmp_path: Path) -> None:
    # The server cannot clean up; the launcher's input ends, and the launcher stops (the contract).
    with gui_server(tmp_path) as gui:
        pids = start_run(gui, "forever.yaml")
        gui.proc.kill()
        gui.proc.wait()
        assert wait_until(lambda: not any(alive(p) for p in pids), 10)


# ---------------------------------------------------------------- the command line


@pytest.mark.parametrize("env,args,expected", [
    ({"REPLAY_FEATURES": "events"}, (), "has no 'stdin-commands'"),
    ({"REPLAY_FEATURES": ""}, (), "has no 'events'"),
    ({}, ("--port", "70000"), "--port"),
    ({}, ("--port", "80"), "--port 80"),
    ({}, ("/nonexistent/x.yaml",), "no such file"),
])
def test_start_errors_exit_with_2(tmp_path: Path, env: dict, args: tuple, expected: str) -> None:
    with gui_server(tmp_path, env=env, args=args, expect_start=False) as gui:
        assert gui.proc.returncode == 2
        assert expected in gui.stderr_text()


def test_launcher_that_is_not_found_or_too_old(tmp_path: Path) -> None:
    with gui_server(tmp_path, launcher=tmp_path / "missing", expect_start=False) as gui:
        assert gui.proc.returncode == 2 and "no such file" in gui.stderr_text()
    old = tmp_path / "old_launcher.py"
    old.write_text("import sys\nprint('usage: vehicle_sil_launch check|run', file=sys.stderr)\nsys.exit(4)\n")
    with gui_server(tmp_path, launcher=old, expect_start=False) as gui:
        assert gui.proc.returncode == 2 and "older than this GUI" in gui.stderr_text()


def test_a_config_outside_the_root_is_refused_at_start(tmp_path: Path) -> None:
    outside = tmp_path / "elsewhere.yaml"
    shutil.copy(CONFIGS / "simulation.yaml", outside)
    with gui_server(tmp_path, args=(str(outside),), expect_start=False) as gui:
        assert gui.proc.returncode == 2 and "--root" in gui.stderr_text()


def test_the_url_line(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
        assert gui.url == f"http://127.0.0.1:{gui.port}/#token={gui.token}" and len(gui.token) >= 40
    with gui_server(tmp_path) as other:
        assert other.token != gui.token


@pytest.mark.skipif(not POSIX, reason="an executable script")
def test_a_launcher_in_the_current_directory_is_not_taken_from_path(tmp_path: Path) -> None:
    # Windows looks in the current directory before PATH: a folder received from someone could run its own
    # vehicle_sil_launch. The server does not take a launcher found there; --launcher still can name it.
    here = tmp_path / "received"
    here.mkdir()
    planted = here / "vehicle_sil_launch"
    planted.write_text(f"#!{sys.executable}\n" + REPLAY.read_text())
    planted.chmod(0o755)
    env = {"PATH": os.pathsep.join([".", os.environ.get("PATH", "")]), "VEHICLE_SIL_LAUNCH": ""}
    with gui_server(tmp_path, launcher=None, env=env, cwd=here, expect_start=False) as gui:
        assert gui.proc.returncode == 2 and "current directory" in gui.stderr_text()
    with gui_server(tmp_path, launcher=planted, env=env, cwd=here) as gui:
        pass


def test_pause_step_resume_and_speed(tmp_path: Path) -> None:
    """Started paused: a step (rounded up to the controller's time step), a new speed, resume, pause, stop. Each
    command goes to the launcher at once; what it did comes as its events, and the summary keeps the last."""
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "200"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            status, body = await c.post("api/run", {"file": "forever.yaml", "disable": ["Logger"], "speed": "max",
                                                    "start_paused": True})
            assert status == 200 and body["session"]["start_paused"] is True, body
            await sock.until(is_time(sim_ms=0, paused=True))
            hello = events(sock.received, "hello")[0]
            assert hello["arguments"] == ["run", "--events", "--stdin-commands", "--disable", "Logger", "--speed",
                                          "max", "--start-paused", "--", "forever.yaml"]
            status, body = await c.post("api/time", {"command": "step", "ms": 25})
            assert status == 200 and body == {"session_id": 1, "command": {"command": "step", "ms": 25}}, body
            got = await sock.until(is_time(sim_ms=30, paused=True))
            step = next(e for e in events(got, "command"))
            assert step["accepted"] and step["ms"] == 30  # whole steps of 10 ms
            status, body = await c.post("api/time", {"command": "speed", "value": 2})
            assert status == 200 and body["command"] == {"command": "speed", "value": 2.0}
            await sock.until(is_time(speed=2, paused=True))
            await c.post("api/time", {"command": "resume"})
            await sock.until(is_time(paused=False))
            status, body = await c.post("api/time", {"command": "step", "ms": 10})
            assert status == 200  # the server passes it on; the launcher refuses it
            got = await sock.until(is_command("step", False))
            assert "paused" in got[-1]["event"]["message"]
            await c.post("api/time", {"command": "pause"})
            await sock.until(is_time(paused=True))
            status, body = await c.get("api/session")
            summary = body["session"]
            assert summary["time"]["paused"] is True and summary["time"]["speed"] == 2
            assert summary["last_command"]["command"] == "pause" and summary["last_command"]["accepted"] is True
            await c.post("api/stop")
            got = await sock.until(is_ended)
            end = got[-1]["session"]
            assert end["result"]["result"] == "interrupted" and end["time"]["paused"] is False
            timing = end["result"]["timing"]
            assert timing["speed"] == 2 and timing["time_step_ms"] == 10 and timing["paused_ms"] > 0
            status, body = await c.post("api/time", {"command": "resume"})
            assert status == 409 and body["error"]["code"] == "not-running"
        run_async(main, gui)


def test_time_commands_are_checked(tmp_path: Path) -> None:
    async def main(c: Client) -> None:
        status, body = await c.post("api/time", {"command": "pause"})
        assert status == 409 and body["error"]["code"] == "not-running", body
        for bad in ({}, {"command": "stop"}, {"command": "Pause"}, {"command": "step"}, {"command": "step", "ms": 0},
                    {"command": "step", "ms": 1.5}, {"command": "step", "ms": True}, {"command": "step", "ms": "5"},
                    {"command": "step", "ms": 10 ** 12 + 1}, {"command": "speed"}, {"command": "speed", "value": 0},
                    {"command": "speed", "value": "fast"}, {"command": "speed", "value": 10 ** 7},
                    {"command": "pause", "ms": 5}):
            status, body = await c.post("api/time", bad)
            assert status == 400 and body["error"]["code"] == "invalid-field", (bad, body)
        status, body = await c.post("api/time", raw=b'{"command": "speed", "value": NaN}')
        assert status == 400
        sock = await c.ws()
        await sock.until(is_synced)
        await c.post("api/run", {"file": "forever.yaml"})
        await sock.until(is_time(sim_ms=0))
        status, body = await c.post("api/time", {"command": "step", "ms": 10 ** 12})
        assert status == 200
        await sock.until(is_command("step", False))  # not paused
        await c.post("api/stop")
        await sock.until(is_ended)
    with gui_server(tmp_path) as gui:
        run_async(main, gui)


def test_a_launcher_without_time_commands(tmp_path: Path) -> None:
    async def main(c: Client) -> None:
        status, app = await c.get("api/app")
        assert "time-commands" not in app["launcher"]["version"]["features"]
        # The server does not pass an argument the launcher cannot take, nor a command it cannot read.
        status, body = await c.post("api/run", {"file": "forever.yaml", "start_paused": True})
        assert status == 400 and body["error"]["code"] == "unsupported" and "start-paused" in body["error"]["message"]
        sock = await c.ws()
        await sock.until(is_synced)
        await c.post("api/run", {"file": "forever.yaml"})
        await sock.until(is_time(sim_ms=0))
        status, body = await c.post("api/time", {"command": "pause"})
        assert status == 400 and body["error"]["code"] == "unsupported" and "time-commands" in body["error"]["message"]
        await c.post("api/stop")
        await sock.until(is_ended)
    with gui_server(tmp_path, env=OLD) as gui:
        run_async(main, gui)


def test_lateness_reaches_the_screen_and_the_summary(tmp_path: Path) -> None:
    async def main(c: Client) -> None:
        sock = await c.ws()
        await sock.until(is_synced)
        await c.post("api/run", {"file": "late.yaml"})
        got = await sock.until(is_time(late_steps=1, late_ms=0))
        stalled = [e for e in events(got, "time") if e["late_ms"] > 0]
        assert stalled and stalled[0]["late_ms"] == 200 and stalled[0]["slipped_ms"] == 200
        status, body = await c.get("api/session")
        assert body["session"]["time"]["late_steps"] == 1 and body["session"]["time"]["max_late_ms"] == 200
        await c.post("api/stop")
        got = await sock.until(is_ended)
        end = got[-1]["session"]
        # The real launcher's order: warnings before notes
        assert [f["code"] for f in end["result"]["findings"]] == ["behind-real-time", "interrupted"]
        assert [e["code"] for e in events(got, "finding")] == ["behind-real-time", "interrupted"]
        timing = end["result"]["timing"]
        assert (timing["late_steps"], timing["max_late_ms"], timing["slipped_ms"], timing["speed"]) == (1, 200, 200, 1)
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "50"}) as gui:
        run_async(main, gui)


def test_lateness_within_the_tolerance_is_not_a_late_step(tmp_path: Path) -> None:
    """late_ms comes and goes while each step's synchronisation takes a little longer than its real time; only a
    step later than the tolerance counts (late_steps), and only that makes the warning."""
    async def main(c: Client) -> None:
        sock = await c.ws()
        await sock.until(is_synced)
        await c.post("api/run", {"file": "jitter.yaml"})
        # a second of virtual time (the real launcher's time events come about every 100 ms, not on round times)
        got = await sock.until(lambda m: is_time()(m) and m["event"]["sim_ms"] >= 1000)
        times = events(got, "time")
        assert {t["late_ms"] for t in times} == {0, 3} and {t["late_steps"] for t in times} == {0}
        await c.post("api/stop")
        got = await sock.until(is_ended)
        end = got[-1]["session"]
        assert [f["code"] for f in end["result"]["findings"]] == ["interrupted"]
        assert end["result"]["timing"]["late_steps"] == 0
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "20"}) as gui:
        run_async(main, gui)


@pytest.mark.skipif(sys.platform != "linux", reason="the stand-in makes its input pipe small with F_SETPIPE_SZ")
def test_a_launcher_that_does_not_read_its_input_holds_no_request(tmp_path: Path) -> None:
    """Commands to a launcher that reads nothing fill its input; a request then returns after about
    STDIN_DRAIN_S (2 s) instead of waiting, and the launcher's events go on reaching the screens meanwhile."""
    async def main(c: Client) -> None:
        sock = await c.ws()
        await sock.until(is_synced)
        await c.post("api/run", {"file": "deaf.yaml"})
        await sock.until(is_time(sim_ms=0))
        body = {"command": "speed", "value": 0.000001}  # the longest line
        for _ in range(200):  # until the input pipe and the server's own buffer (64 KiB) are full
            began = time.monotonic()
            replies = await asyncio.gather(*(c.post("api/time", body) for _ in range(100)))
            took = time.monotonic() - began
            assert all(status == 200 for status, _ in replies), replies[0]
            if took >= 1.5:
                break
        assert 1.5 <= took < 5, took
        logs = len(events(sock.received, "log"))
        await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "log")
        await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "log")
        assert len(events(sock.received, "log")) >= logs + 2
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "100"}, args=("--stop-timeout", "1")) as gui:
        run_async(main, gui)


@pytest.mark.skipif(not POSIX, reason="file modes")
def test_time_commands_follow_the_launcher_of_the_run(tmp_path: Path) -> None:
    """A launcher built again while a run goes: the commands the running one can take decide, not the new one's."""
    launcher = tmp_path / "vehicle_sil_launch"

    def write(features: str) -> None:  # another size each time: the server asks the new one for its version
        launcher.write_text(f"#!{sys.executable}\n# {features}\nimport os, runpy\n"
                            f"os.environ['REPLAY_FEATURES'] = {features!r}\n"
                            f"runpy.run_path({str(REPLAY)!r}, run_name='__main__')\n")
        launcher.chmod(0o755)

    write("events,stdin-commands,check-variables")
    with gui_server(tmp_path, launcher=launcher) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "forever.yaml"})
            await sock.until(is_time(sim_ms=0))
            write("events,stdin-commands,check-variables,speed,start-paused,time-commands")
            status, app = await c.get("api/app")
            assert "time-commands" in app["launcher"]["version"]["features"]
            status, body = await c.post("api/time", {"command": "pause"})
            assert status == 400 and body["error"]["code"] == "unsupported", body
            await c.post("api/stop")
            await sock.until(is_ended)
        run_async(main, gui)
