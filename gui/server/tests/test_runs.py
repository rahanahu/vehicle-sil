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
from conftest import (CONFIGS, REPLAY, Client, Gui, alive, gui_server, is_ended, is_state, is_synced, run_async, scaled, wait_until,
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




# The features that every launcher of this GUI's version has and that the server requires at its start.
REQUIRED = ("events,stdin-commands,request,time-commands,stdin-run-until,state,"
            "speed,start-paused,scenario,manual-participants")


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
        got = await run_to_end(c, {"file": "simulation.yaml", "disable": ["Logger"], "speed": 2, "stop_at_ms": 1000})
        seqs = [e["seq"] for e in events(got)]
        assert seqs == list(range(len(seqs)))
        hello = events(got, "hello")[0]
        assert hello["arguments"] == ["run", "--events", "--stdin-commands", "--disable", "Logger", "--speed", "2", "--stop-at-ms", "1000",
                                      "--", "simulation.yaml"]
        end = got[-1]["session"]
        assert end["launcher"]["exit_code"] == 0 and end["result"]["result"] == "passed" and end["problems"] == []
        assert {p["name"]: p["state"] for p in end["participants"]} == \
            {"Engine": "ended", "Dashboard": "ended", "Logger": "disabled"}
        assert [p["name"] for p in end["check"]["participants"]] == ["Engine", "Dashboard", "Logger"]
        assert all(not alive(pid) for pid in started_pids(got))
        status, body = await c.get("api/session")
        assert without_variables(body["session"]) == end and body["session"]["variables"]["state"] == "off"
    with gui_server(tmp_path) as gui:
        run_async(main, gui)


def test_a_launcher_that_writes_nothing_for_a_while_is_reported(tmp_path: Path) -> None:
    env = {"REPLAY_FAULT": "silent", "REPLAY_SILENT_S": "4"}
    with gui_server(tmp_path, env=env, args=("--silent-after", "1.5")) as gui:
        async def main(c: Client) -> None:
            got = await run_to_end(c, {"file": "simulation.yaml", "stop_at_ms": 1000})
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
            got = await run_to_end(c, {"file": "simulation.yaml", "disable": ["Logger"], "stop_at_ms": 1000})
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
    the run waits for it, the simulation does not start."""
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
        assert end["result"]["result"] == "stopped" and end["result"]["simulation"] == "not_started"
        assert [p["manual"] for p in end["result"]["participants"]] == [False, True, False]
    with gui_server(tmp_path) as gui:
        run_async(main, gui)


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
    run_async(main, gui)


def test_a_run_whose_check_fails_is_refused_with_the_findings_of_the_check(gui: Gui) -> None:
    """No run is started: 422 check-failed, and detail.findings are the findings that api/check returns."""
    async def main(c: Client) -> None:
        req = {"file": "check-error.yaml", "disable": []}
        status, checked = await c.post("api/check", req)
        assert status == 200 and checked["check"]["result"] == "failed"
        status, body = await c.post("api/run", req)
        assert status == 422, body
        error = body["error"]
        assert error["code"] == "check-failed" and "check-error.yaml" in error["message"]
        assert error["detail"] == {"result": "failed", "findings": checked["check"]["findings"]}
        assert any(f["severity"] == "error" for f in error["detail"]["findings"])
        status, body = await c.get("api/session")
        assert body["session"] is None  # nothing was started, and the next start is not blocked
        got = await run_to_end(c, {"file": "simulation.yaml", "stop_at_ms": 1000})
        assert got[-1]["session"]["result"]["result"] == "passed"
    run_async(main, gui)


def test_a_variant_goes_to_the_check_and_the_run(gui: Gui) -> None:
    async def main(c: Client) -> None:
        status, body = await c.post("api/check", {"file": "variants.yaml", "variant": "small"})
        assert status == 200 and body["check"]["result"] == "passed", body
        status, body = await c.post("api/check", {"file": "variants.yaml", "variant": "quiet", "disable": []})
        assert status == 200 and body["check"]["result"] == "passed", body
        # without a variant, the launcher's check says to choose one: the run is refused with that finding
        status, body = await c.post("api/run", {"file": "variants.yaml"})
        assert (status, body["error"]["code"]) == (422, "check-failed"), body
        got = await run_to_end(c, {"file": "variants.yaml", "variant": "small", "stop_at_ms": 1000})
        assert got[-1]["session"]["result"]["result"] == "passed"
        assert events(got, "hello")[0]["arguments"][-6:] == ["--variant", "small", "--stop-at-ms", "1000", "--", "variants.yaml"]
    run_async(main, gui)


def test_a_start_checks_the_file_once_and_asks_for_the_names_only_for_disable_or_manual(tmp_path: Path) -> None:
    """The participant names are asked for (a check without the request) only when the request names participants
    and the names of the file as it is now are not known; the run's own check is the one check otherwise."""
    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)
    calls = tmp_path / "calls.jsonl"

    def checks() -> int:
        lines = calls.read_text(encoding="utf-8").splitlines() if calls.exists() else []
        return sum(1 for line in lines if json.loads(line)[:1] == ["check"])

    async def main(c: Client) -> None:
        got = await run_to_end(c, {"file": "simulation.yaml", "stop_at_ms": 1000})
        assert got[-1]["session"]["result"]["result"] == "passed"
        assert checks() == 1  # the run's check: no names asked for
        got = await run_to_end(c, {"file": "simulation.yaml", "disable": ["Logger"], "stop_at_ms": 1000})
        assert got[-1]["session"]["result"]["result"] == "passed"
        assert checks() == 2  # the names come from the last check of the same file
        stat = (root / "simulation.yaml").stat()
        os.utime(root / "simulation.yaml", ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000))
        got = await run_to_end(c, {"file": "simulation.yaml", "disable": ["Logger"], "stop_at_ms": 1000})
        assert got[-1]["session"]["result"]["result"] == "passed"
        assert checks() == 4  # the file changed since: the names are asked for again, then the run's check
    with gui_server(tmp_path, root=root, env={"REPLAY_CALLS": str(calls)}) as gui:
        run_async(main, gui)


@pytest.mark.skipif(not POSIX, reason="file modes")
def test_the_intent_goes_to_the_launcher_in_a_request_file_that_is_removed_afterwards(tmp_path: Path) -> None:
    """What the browser chose is written to a file of this server's (readable by this user only) and given as
    --request; the scenario is written from the directory of the configuration file; the files are gone once the
    check or the run is done."""
    seen = tmp_path / "seen"
    seen.mkdir()
    launcher = tmp_path / "vehicle_sil_launch"
    launcher.write_text(f"#!{sys.executable}\nimport json, os, runpy, stat, sys\n"
                        f"a = sys.argv[1:]\nif '--request' in a:\n    p = a[a.index('--request') + 1]\n"
                        f"    json.dump({{'argv': a, 'path': p, 'mode': stat.S_IMODE(os.stat(p).st_mode), "
                        f"'text': open(p).read()}}, open('{seen}/' + str(len(os.listdir('{seen}'))) + '.json', 'w'))\n"
                        f"runpy.run_path({str(REPLAY)!r}, run_name='__main__')\n")
    launcher.chmod(0o755)

    def made() -> list[dict]:
        return [json.loads(f.read_text()) for f in sorted(seen.iterdir(), key=lambda f: int(f.stem))]

    async def main(c: Client) -> None:
        status, body = await c.post("api/check", {"file": "simulation.yaml", "scenario": "scenarios/throttle.yaml",
                                                  "vehicle_sil_run_request": 1})
        assert status == 200, body
        check = made()[-1]
        assert check["argv"][:2] == ["check", "--json"] and check["mode"] == 0o600
        assert json.loads(check["text"]) == {"vehicle_sil_run_request": 1, "scenario": "scenarios/throttle.yaml"}
        assert not os.path.exists(check["path"])
        got = await run_to_end(c, {"file": "simulation.yaml", "speed": 2, "disable": ["Logger"], "stop_at_ms": 1000})
        assert got[-1]["session"]["result"]["result"] == "passed"
        run = made()[-1]
        assert run["argv"][:3] == ["run", "--events", "--stdin-commands"] and run["argv"][-2:] == ["--", "simulation.yaml"]
        assert not {"--disable", "--speed", "--scenario", "--variant"} & set(run["argv"])  # the intent is not in them
        assert json.loads(run["text"]) == {"vehicle_sil_run_request": 1, "disable": ["Logger"], "speed": 2.0}
        assert run["mode"] == 0o600 and not os.path.exists(run["path"])
    with gui_server(tmp_path, launcher=launcher) as gui:
        run_async(main, gui)


@pytest.mark.skipif(not POSIX, reason="file modes")
def test_the_launchers_words_do_not_name_the_temporary_request_file(tmp_path: Path) -> None:
    """The launcher says where in its request file a value is wrong, naming the file that this server made: the
    answer says "the request" instead (a file that is gone before the answer, and not one the sender can open)."""
    launcher = tmp_path / "vehicle_sil_launch"
    launcher.write_text(f"#!{sys.executable}\nimport json, runpy, sys\na = sys.argv[1:]\n"
                        "if a[:1] == ['check'] and '--request' in a:\n    p = a[a.index('--request') + 1]\n"
                        "    print(json.dumps({'vehicle_sil_check': 1, 'result': 'usage', 'participants': [], "
                        "'findings': [{'severity': 'error', 'code': 'usage', 'file': p, 'line': 5, "
                        "'message': p + ', line 5: params: a name is letters', 'participant': None, "
                        "'network': None}]}))\n    sys.stderr.write('error: ' + p + ', line 5: bad\\n')\n"
                        "    sys.exit(2)\n"
                        f"runpy.run_path({str(REPLAY)!r}, run_name='__main__')\n")
    launcher.chmod(0o755)

    async def main(c: Client) -> None:
        status, body = await c.post("api/check", {"file": "simulation.yaml", "params": {"1a": 1}})
        assert status == 200 and "vehicle-sil-gui-request" not in json.dumps(body), body
        finding = body["check"]["findings"][0]
        assert (finding["file"], finding["line"]) == ("the request", None)
        assert finding["message"] == "the request: params: a name is letters"
        status, body = await c.post("api/run", {"file": "simulation.yaml", "params": {"1a": 1}})
        assert (status, body["error"]["code"]) == (422, "check-failed"), body
        assert "vehicle-sil-gui-request" not in json.dumps(body), body
        assert body["error"]["detail"]["findings"][0]["message"] == "the request: params: a name is letters"
    with gui_server(tmp_path, launcher=launcher) as gui:
        run_async(main, gui)


def test_variant_and_params_of_the_wrong_shape_are_refused(gui: Gui) -> None:
    async def main(c: Client) -> None:
        scenario = "scenarios/throttle.yaml"
        for path in ("api/check", "api/run"):
            for body, code in (({"variant": ""}, "invalid-field"), ({"variant": 1}, "invalid-field"),
                               ({"variant": None}, "invalid-field"), ({"scenario": None}, "invalid-field"),
                               ({"params": ["a"]}, "invalid-field"), ({"params": None}, "invalid-field"),
                               ({"params": {"a": [1]}, "scenario": scenario}, "invalid-field"),
                               ({"params": {"a": None}, "scenario": scenario}, "invalid-field"),
                               ({"enable": "A"}, "invalid-field"), ({"enable": [1]}, "invalid-field"),
                               ({"strict": 1}, "invalid-field"), ({"strict": None}, "invalid-field"),
                               ({"vehicle_sil_run_request": 2}, "invalid-field"),
                               ({"connect": ["ETH=x"]}, "invalid-field"), ({"connect": {"ETH": 1}}, "invalid-field"),
                               ({"connect": {"": "none"}}, "invalid-field"), ({"connect": None}, "invalid-field"),
                               ({"time_policy": ""}, "invalid-field"), ({"time_policy": True}, "invalid-field"),
                               ({"time_policy": None}, "invalid-field")):
                status, reply = await c.post(path, {"file": "simulation.yaml", **body})
                assert (status, reply["error"]["code"]) == (400, code), (path, body, reply)
        status, body = await c.get("api/session")
        assert body["session"] is None
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
            assert end["launcher"]["exit_code"] == 0 and end["result"]["result"] == "stopped"
            assert (end["result"]["end"], end["result"]["ok"]) == ("stopped", True) and end["result"]["message"]
            assert all(not alive(pid) for pid in started_pids(sock.received))
            status, _ = await c.post("api/run", {"file": "simulation.yaml", "stop_at_ms": 1000})  # the next run may start
            assert status == 200
            got = [m for m in await sock.until(is_ended) if m["type"] != "allowed"]  # (`starting` comes first)
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
        got = await run_to_end(c, {"file": "launcher-crash.yaml", "stop_at_ms": 1000})
        end = got[-1]["session"]
        assert end["launcher"]["exit_code"] == 70
        assert [p["code"] for p in end["problems"]] == ["launcher-stderr", "no-result"]
        if POSIX:  # what the dead launcher left in its process group is killed
            assert wait_until(lambda: all(not alive(pid) for pid in started_pids(got)), 5)
        got = await run_to_end(c, {"file": "bad-lines.yaml", "stop_at_ms": 1000})
        assert [m["code"] for m in got if m["type"] == "error"] == ["launcher-output", "launcher-output", "seq-gap"]
        assert events(got, "mystery")  # an unknown event is passed on
        assert any(e.get("colour") == "blue" for e in events(got, "log"))  # unknown fields are passed on
        assert got[-1]["session"]["result"]["result"] == "passed"
        got = await run_to_end(c, {"file": "future-events.yaml", "stop_at_ms": 1000})
        assert [m["code"] for m in got if m["type"] == "error"] == ["events-format"]
    run_async(main, gui)


def test_a_launcher_that_runs_on_after_its_result_can_be_stopped(gui: Gui) -> None:
    async def main(c: Client) -> None:
        sock = await c.ws()
        await sock.until(is_synced)
        assert (await c.post("api/run", {"file": "result-then-wait.yaml", "stop_at_ms": 300}))[0] == 200
        await sock.until(lambda m: m.get("event", {}).get("event") == "result")
        await asyncio.sleep(0.5)
        status, body = await c.get("api/session")
        summary = body["session"]
        assert summary["result"]["result"] == "passed" and summary["phase"] == "ended"
        assert summary["launcher"]["running"] and summary["state"] == "running"  # so the screen offers Stop
        assert summary["launcher_state"]["allowed"]["pause"]["reason"] == "not-running"
        # a time command is refused with the launcher's own sentence (and not written: the replay has none)
        status, body = await c.post("api/time", {"command": "resume"})
        assert status == 409 and body["error"]["code"] == "not-running", body
        assert body["error"]["message"] == summary["launcher_state"]["allowed"]["resume"]["message"], body
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
            live = await run_to_end(c, {"file": "flood.yaml", "stop_at_ms": 1000})
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
        await c.post("api/run", {"file": "simulation.yaml", "stop_at_ms": 1000})
        got_a, got_b = await asyncio.gather(a.until(is_ended), b.until(is_ended))
        assert events(got_a) == events(got_b)
    run_async(main, gui)


def test_checks_and_configs(tmp_path: Path) -> None:
    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)
    (root / "nested" / "deeper").mkdir(parents=True)
    shutil.copy(CONFIGS / "simulation.yaml", root / "nested" / "deeper" / "n.yml")
    (root / ".git").mkdir()
    (root / ".git" / "x.yaml").write_text("vehicle_sil_simulation: 2\n")
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
        assert time.monotonic() - start < scaled(10)
        assert wait_until(lambda: not any(alive(p) for p in pids), 5)


@pytest.mark.parametrize("value", ["nan", "inf", "-inf", "-1"])
def test_stop_timeout_rejects_non_finite_or_negative_values(tmp_path: Path, value: str) -> None:
    with gui_server(tmp_path, args=(f"--stop-timeout={value}",), expect_start=False) as gui:
        assert gui.proc.returncode == 2
        assert "--stop-timeout must be a finite number of 0 or more seconds" in gui.stderr_text()


@pytest.mark.skipif(not POSIX, reason="signals and /proc")
def test_a_second_signal_skips_the_waiting(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
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
    *[({"REPLAY_FEATURES": ",".join(f for f in REQUIRED.split(",") if f != missing)}, (),
       f"has no '{missing}' (its features: ")
      for missing in ("request", "time-commands", "stdin-run-until", "state", "speed", "start-paused", "scenario",
                 "manual-participants")],
    ({"REPLAY_FEATURES": "events"}, (), "use the vehicle_sil_launch of the same vehicle-sil version"),
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


@pytest.mark.parametrize("theirs", ["0.1.0", "0.2.0", "0.2.0-rc.2", "0.1.0-rc.1"])
def test_launcher_of_another_version_is_refused_at_start(tmp_path: Path, theirs: str) -> None:
    from vehicle_sil_gui import __version__
    shown = theirs if theirs != __version__ else theirs + "-x"  # never the GUI's own version
    with gui_server(tmp_path, env={"REPLAY_VERSION": shown}, expect_start=False) as gui:
        assert gui.proc.returncode == 2
        assert f"is vehicle-sil {shown}, this GUI is {__version__}" in gui.stderr_text()


def test_launcher_of_the_version_of_the_gui_is_used(tmp_path: Path) -> None:
    from vehicle_sil_gui import __version__
    with gui_server(tmp_path, env={"REPLAY_VERSION": __version__}):
        pass


def test_a_launcher_replaced_by_another_version_is_refused(tmp_path: Path) -> None:
    from vehicle_sil_gui import __version__
    launcher = tmp_path / "vehicle_sil_launch"

    def write(version: str) -> None:  # another size each time: the server asks the new one for its version
        launcher.write_text(f"#!{sys.executable}\n# {version}\nimport os, runpy\n"
                            f"os.environ['REPLAY_VERSION'] = {version!r}\n"
                            f"runpy.run_path({str(REPLAY)!r}, run_name='__main__')\n")
        launcher.chmod(0o755)

    write(__version__)
    with gui_server(tmp_path, launcher=launcher) as gui:
        async def main(c: Client) -> None:
            other = await c.ws()
            assert (await other.until(is_synced))[0]["allowed"]["start"]["ok"]
            write(__version__ + "-rc.9")
            await c.get("api/app")
            got = await other.until(lambda m: m["type"] == "allowed" and not m["allowed"]["start"]["ok"])
            assert got[-1]["allowed"]["start"]["reason"] == "launcher-unusable"
            assert "this GUI is " + __version__ in got[-1]["allowed"]["start"]["message"]
        run_async(main, gui)


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
            await sock.until_stands(0)
            hello = events(sock.received, "hello")[0]
            assert hello["arguments"] == ["run", "--events", "--stdin-commands", "--disable", "Logger", "--speed",
                                          "max", "--start-paused", "--", "forever.yaml"]
            status, body = await c.post("api/time", {"command": "step", "ms": 25})
            assert status == 200 and body == {"session_id": 1, "command": {"command": "step", "ms": 25}}, body
            await sock.until_stands(30)
            step = next(e for e in events(sock.received, "command"))
            assert step["accepted"] and step["ms"] == 30  # whole steps of 10 ms
            status, body = await c.post("api/time", {"command": "speed", "value": 2, "id": 7})
            assert status == 200 and body["command"] == {"command": "speed", "value": 2.0, "id": 7}
            await sock.until_stands(speed=2)
            # the launcher returns the id in the answer to that line, and null in the others
            answers = {e["command"]: e["id"] for e in events(sock.received, "command")}
            assert answers == {"step": None, "speed": 7}, answers
            await c.post("api/time", {"command": "resume"})
            await sock.until(is_state(paused=False))
            status, body = await c.post("api/time", {"command": "step", "ms": 10})
            assert status == 200  # the server passes it on; the launcher refuses it
            got = await sock.until(is_command("step", False))
            assert "paused" in got[-1]["event"]["message"]
            await c.post("api/time", {"command": "pause"})
            await sock.until_stands()
            status, body = await c.get("api/session")
            summary = body["session"]
            assert "paused" not in summary["time"] and "speed" not in summary["time"]
            assert summary["launcher_state"]["paused"] is True and summary["launcher_state"]["speed"] == 2
            assert summary["last_command"]["command"] == "pause" and summary["last_command"]["accepted"] is True
            await c.post("api/stop")
            got = await sock.until(is_ended)
            end = got[-1]["session"]
            assert end["result"]["result"] == "stopped" and end["launcher_state"]["paused"] is False
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
                    {"command": "pause", "ms": 5}, {"command": "run_until"}, {"command": "run_until", "at_ms": -1},
                    {"command": "run_until", "at_ms": 1.5}, {"command": "run_until", "at_ms": 10 ** 12 + 1},
                    {"command": "run_until", "at_ms": 5, "ms": 5}, {"command": "pause", "id": -1}):
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


def test_allowed_follows_the_run_and_the_summary_keeps_the_last_state(tmp_path: Path) -> None:
    def start(allowed: dict) -> object:
        return allowed["start"]["reason"]

    async def main(c: Client) -> None:
        status, body = await c.get("api/session")
        assert body["session"] is None and start(body["allowed"]) is None
        assert body["allowed"]["stop"]["reason"] == body["allowed"]["force"]["reason"] == "not-running"
        sock = await c.ws()
        got = await sock.until(is_synced)
        assert got[0]["session"] is None and start(got[0]["allowed"]) is None  # hello says it with no run
        status, body = await c.post("api/run", {"file": "forever.yaml"})
        assert status == 200
        allowed = body["session"]["allowed"]
        assert allowed["start"]["reason"] == "already-running" and allowed["stop"]["ok"] and allowed["force"]["ok"]
        assert isinstance(allowed["start"]["message"], str) and allowed["stop"]["message"] is None
        await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "state")
        status, body = await c.get("api/session")
        assert body["allowed"] == body["session"]["allowed"]
        assert isinstance(body["session"]["launcher_state"]["rev"], int)
        status, body = await c.post("api/stop")
        assert body["session"]["allowed"]["stop"]["reason"] == "already-stopping"
        assert body["session"]["allowed"]["force"]["ok"]
        got = await sock.until(is_ended)
        end = got[-1]["session"]
        assert end["allowed"]["start"]["ok"] and end["allowed"]["stop"]["reason"] == "not-running"
        assert end["launcher_state"]["phase"] in ("ended", "stopping", "running")
        states = events(sock.received, "state")  # many came; the summary has only the last
        assert len(states) > 1 and end["launcher_state"]["rev"] == states[-1]["rev"] and end["launcher_state"]["seq"] == states[-1]["seq"]
        assert "event" not in end["launcher_state"] and "t_ms" not in end["launcher_state"]
        status, body = await c.post("api/stop", {"force": True})  # nothing to stop: the same answer again
        assert status == 200 and body["session"]["allowed"]["force"]["reason"] == "not-running"
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "50"}) as gui:
        run_async(main, gui)


def test_the_probe_that_changes_the_launcher_is_pushed_to_the_other_screens(tmp_path: Path) -> None:
    launcher = tmp_path / "vehicle_sil_launch"

    def write(features: str) -> None:  # another size each time: the server asks the new one for its version
        launcher.write_text(f"#!{sys.executable}\n# {features}\nimport os, runpy\n"
                            f"os.environ['REPLAY_FEATURES'] = {features!r}\n"
                            f"runpy.run_path({str(REPLAY)!r}, run_name='__main__')\n")
        launcher.chmod(0o755)

    write(REQUIRED)
    with gui_server(tmp_path, launcher=launcher) as gui:
        async def main(c: Client) -> None:
            other = await c.ws()
            hello = (await other.until(is_synced))[0]["allowed"]
            assert hello["start"]["ok"]
            write("check-variables")  # no events: this launcher cannot be used
            status, app = await c.get("api/app")
            got = await other.until(lambda m: m["type"] == "allowed" and not m["allowed"]["start"]["ok"])
            unusable = got[-1]["allowed"]
            assert unusable["start"]["reason"] == "launcher-unusable" and unusable["rev"] > hello["rev"]
            # a screen that reads api/session now makes no new number: the other screens already have it
            status, body = await c.get("api/session")
            assert body["allowed"]["rev"] == unusable["rev"]
            write(REQUIRED + ",check-variables")  # usable again
            await c.get("api/app")
            got = await other.until(lambda m: m["type"] == "allowed" and m["allowed"]["start"]["ok"])
            assert got[-1]["allowed"]["rev"] > unusable["rev"]
        run_async(main, gui)


def test_a_start_that_is_starting_says_so_to_the_second_request(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_CHECK_DELAY_MS": "300"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            first = asyncio.ensure_future(c.post("api/run", {"file": "forever.yaml"}))
            await asyncio.sleep(0.1)
            status, body = await c.post("api/run", {"file": "forever.yaml"})
            assert status == 409 and body["error"]["code"] == "starting", body
            assert (await first)[0] == 200
            status, body = await c.post("api/run", {"file": "forever.yaml"})
            assert status == 409 and body["error"]["code"] == "already-running", body
            await sock.until(is_time(sim_ms=0))
            await c.post("api/stop")
            await sock.until(is_ended)
        run_async(main, gui)


def test_allowed_alone_is_pushed_only_when_no_session_message_carries_it(tmp_path: Path) -> None:
    """A start beginning, and a start that fails, change `allowed` with no session message to carry it: a message
    `allowed` says so, numbered. A start that works is carried by its session message: nothing else is pushed."""
    def pushed(sock) -> list[dict]:
        return [m for m in sock.received if m["type"] == "allowed"]

    async def main(c: Client) -> None:
        sock = await c.ws()
        got = await sock.until(is_synced)
        hello = got[0]["allowed"]
        assert hello["start"]["ok"] and isinstance(hello["rev"], int)
        status, body = await c.post("api/run", {"file": "check-error.yaml"})
        assert status == 422
        await sock.until(lambda m: m["type"] == "allowed" and m["allowed"]["start"]["ok"])
        starting, back = pushed(sock)
        assert starting["session"] is None and starting["allowed"]["start"]["reason"] == "starting"
        assert starting["allowed"]["start"]["message"] and back["allowed"]["start"]["ok"]
        assert hello["rev"] < starting["allowed"]["rev"] < back["allowed"]["rev"]
        status, body = await c.get("api/session")
        assert body["allowed"]["rev"] == back["allowed"]["rev"]  # nothing changed since: the same number
        sock.received.clear()
        status, body = await c.post("api/run", {"file": "forever.yaml"})
        assert status == 200
        carried = body["session"]["allowed"]
        assert carried["start"]["reason"] == "already-running" and carried["rev"] > back["allowed"]["rev"]
        await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
        assert [m["allowed"]["start"]["reason"] for m in pushed(sock)] == ["starting"]  # and no more after it
        await c.post("api/stop")
        await sock.until(is_ended)
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "50"}) as gui:
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
        assert [f["code"] for f in end["result"]["findings"]] == ["behind-real-time", "stopped"]
        assert [e["code"] for e in events(got, "finding")] == ["behind-real-time", "stopped"]
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
        assert [f["code"] for f in end["result"]["findings"]] == ["stopped"]
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
