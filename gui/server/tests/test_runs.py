"""Runs through the API and the WebSocket with the fake launcher's scenarios, reconnection, and stopping the
server with a run going (no process may be left)."""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import signal
import sys
import time
from pathlib import Path

import pytest
from conftest import CONFIGS, FAKE, Client, Gui, alive, gui_server, is_ended, is_synced, run_async, wait_until

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


def test_a_run_that_passes(gui: Gui) -> None:
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
        assert end["exit_code"] == 0 and end["result"]["result"] == "passed" and end["problems"] == []
        assert [p["state"] for p in end["participants"]] == ["ended", "ended", "disabled"]
        assert all(not alive(pid) for pid in started_pids(got))
        status, body = await c.get("api/session")
        assert body["session"] == end
    run_async(main, gui)


def test_check_then_run_with_a_participant_left_out(gui: Gui) -> None:
    async def main(c: Client) -> None:
        status, body = await c.post("api/check", {"file": "simulation.yaml", "disable": ["Dashboard"]})
        assert status == 200 and body["exit_code"] == 0
        dashboard = next(p for p in body["check"]["participants"] if p["name"] == "Dashboard")
        assert dashboard["status"] == "disabled" and dashboard["variables"] is None
        engine = next(p for p in body["check"]["participants"] if p["name"] == "Engine")
        assert [v["name"] for v in engine["variables"]] == ["engine.demand", "engine.speed", "engine.mode"]
        status, body = await c.post("api/check", {"file": "check-error.yaml"})
        assert status == 200 and body["exit_code"] == 1 and body["check"]["result"] == "failed"
        got = await run_to_end(c, {"file": "check-error.yaml"})
        assert got[-1]["session"]["result"]["result"] == "check_failed" and not started_pids(got)
    run_async(main, gui)


def test_one_run_at_a_time_and_stop(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_TICK_MS": "50"}) as gui:
        async def main(c: Client) -> None:
            status, body = await c.post("api/stop")
            assert status == 409 and body["error"]["code"] == "not-running"
            sock = await c.ws()
            await sock.until(is_synced)
            status, _ = await c.post("api/run", {"file": "forever.yaml"})
            assert status == 200
            status, body = await c.post("api/run", {"file": "simulation.yaml"})
            assert status == 409 and body["error"]["code"] == "session-running"
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            status, body = await c.post("api/stop")
            assert status == 200 and body["session"]["stop_requests"] == 1
            got = await sock.until(is_ended)
            end = got[-1]["session"]
            assert end["exit_code"] == 130 and end["result"]["result"] == "interrupted"
            assert all(not alive(pid) for pid in started_pids(sock.received))
            status, _ = await c.post("api/run", {"file": "simulation.yaml"})  # the next run may start
            assert status == 200
            got = await sock.until(is_ended)
            assert got[0]["type"] == "session" and got[0]["session"]["id"] == 2
        run_async(main, gui)


def test_a_second_stop_does_not_wait(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_TICK_MS": "50"}) as gui:
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
            status, body = await c.post("api/stop")
            assert body["session"]["stop_requests"] == 2
            got = await sock.until(is_ended)
            assert [p["state"] for p in got[-1]["session"]["participants"]] == ["killed"] * 3
        run_async(main, gui)


def test_problems_of_the_launcher_are_shown(gui: Gui) -> None:
    async def main(c: Client) -> None:
        got = await run_to_end(c, {"file": "participant-crash.yaml"})
        end = got[-1]["session"]
        assert end["result"]["result"] == "failed" and end["findings"][0]["code"] == "participant-failed"
        assert end["participants"][1]["exit_code"] == 3
        got = await run_to_end(c, {"file": "launcher-crash.yaml"})
        end = got[-1]["session"]
        assert end["exit_code"] == 70 and [p["code"] for p in end["problems"]] == ["launcher-stderr", "no-result"]
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


def test_reconnecting_replays_what_is_kept(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_FLOOD": "2500"}) as gui:
        async def main(c: Client) -> None:
            live = await run_to_end(c, {"file": "flood.yaml"})
            again = await c.ws()
            got = await again.until(is_synced)
            hello = got[0]
            assert hello["type"] == "hello" and hello["session"]["state"] == "ended"
            omitted = hello["replay"]["omitted"]["log"]
            replayed = events(got)
            assert len(events(got, "log")) <= 2000 * 4 and omitted["Engine"] > 0
            live_engine = [e for e in events(live, "log") if e["source"] == "Engine"]
            kept_engine = [e for e in events(got, "log") if e["source"] == "Engine"]
            assert len(kept_engine) == 2000 and kept_engine == live_engine[-2000:]
            assert omitted["Engine"] == len(live_engine) - 2000
            assert [e for e in replayed if e["event"] not in ("log", "time")] == \
                [e for e in events(live) if e["event"] not in ("log", "time")]
            assert events(got, "time") == events(live, "time")[-1:]
            assert got[-1] == {"type": "synced", "session": 1, "last_seq": events(live)[-1]["seq"]}
            resumed = await c.ws({"session": 1, "from_seq": events(live)[-3]["seq"]})
            got = await resumed.until(is_synced)
            assert events(got) == events(live)[-3:]
            other = await c.ws({"session": 99, "from_seq": 10_000})  # an old session: everything again
            got = await other.until(is_synced)
            assert got[0]["replay"]["from_seq"] is None and events(got)[0]["event"] == "hello"
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


def test_check_busy_and_configs(tmp_path: Path) -> None:
    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)
    (root / "nested" / "deeper").mkdir(parents=True)
    shutil.copy(CONFIGS / "simulation.yaml", root / "nested" / "deeper" / "n.yml")
    (root / ".git").mkdir()
    (root / ".git" / "x.yaml").write_text("vehicle_sil_simulation: 1\n")
    with gui_server(tmp_path, root=root, env={"FAKE_LAUNCHER_CHECK_DELAY_MS": "700"},
                    args=(str(root / "nested" / "deeper" / "n.yml"),)) as gui:
        async def main(c: Client) -> None:
            status, app = await c.get("api/app")
            assert app["initial_file"] == "nested/deeper/n.yml" and app["root"] == str(root.resolve())
            status, body = await c.get("api/configs")
            paths = [f["path"] for f in body["files"]]
            assert "nested/deeper/n.yml" in paths and not any(p.startswith(".git") for p in paths)
            results = await asyncio.gather(*(c.post("api/check", {"file": "simulation.yaml"}) for _ in range(3)))
            codes = sorted(s for s, _ in results)
            assert codes[0] == 200 and codes[-1] == 409
            assert all(b["error"]["code"] == "check-busy" for s, b in results if s == 409)
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
        return [body["session"]["launcher_pid"], *started_pids(sock.received)]
    return run_async(main, gui)


@pytest.mark.skipif(not POSIX, reason="signals and /proc")
@pytest.mark.parametrize("how", [signal.SIGTERM, signal.SIGINT, signal.SIGHUP])
def test_stopping_the_server_stops_the_run(tmp_path: Path, how: int) -> None:
    with gui_server(tmp_path) as gui:
        pids = start_run(gui, "forever.yaml")
        assert len(pids) == 4 and all(alive(p) for p in pids)
        start = time.monotonic()
        gui.proc.send_signal(how)
        assert gui.proc.wait(20) == 0
        assert time.monotonic() - start < 5  # in order, through the end of input: no waiting for a timeout
        assert wait_until(lambda: not any(alive(p) for p in pids), 5)


@pytest.mark.skipif(not POSIX, reason="signals and /proc")
def test_a_launcher_that_does_not_stop_is_killed(tmp_path: Path) -> None:
    with gui_server(tmp_path, args=("--stop-timeout", "0.5")) as gui:
        pids = start_run(gui, "ignore-stop.yaml")
        start = time.monotonic()
        gui.proc.send_signal(signal.SIGTERM)
        assert gui.proc.wait(20) == 0
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
        assert gui.proc.wait(20) == 0
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
    ({"FAKE_LAUNCHER_FEATURES": "events"}, (), "has no 'stdin-commands'"),
    ({"FAKE_LAUNCHER_FEATURES": ""}, (), "has no 'events'"),
    ({}, ("--port", "70000"), "--port"),
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
