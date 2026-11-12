"""Requests that race each other, launchers that misbehave at their end, and the server's own end: one run at a
time, stop and force, checks that wait their turn, a launcher whose descendants keep its output open, and an end
that is bounded whatever is connected or running."""

from __future__ import annotations

import asyncio
import os
import re
import signal
import socket
import sys
import time
from pathlib import Path

import pytest
from conftest import Client, alive, children, gui_server, is_ended, is_synced, run_async, wait_until

POSIX = sys.platform != "win32"
pytestmark = pytest.mark.skipif(not POSIX, reason="signals and /proc")


def event_messages(messages: list[dict], kind: str) -> list[dict]:
    return [m["event"] for m in messages if m["type"] == "event" and m["event"]["event"] == kind]


def stop_and_time(proc, how: int = signal.SIGTERM, limit: float = 60) -> float:
    start = time.monotonic()
    proc.send_signal(how)
    proc.wait(limit)
    return time.monotonic() - start


# ---------------------------------------------------------------- starting


@pytest.mark.parametrize("checked_first", [True, False], ids=["names-known", "names-unknown"])
def test_concurrent_runs_start_one_launcher(tmp_path: Path, checked_first: bool) -> None:
    # The reviewer's steps: (a check,) then four api/run at once. Only one may start; the others are refused.
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_CHECK_DELAY_MS": "200"}) as gui:
        async def main(c: Client) -> list[int]:
            if checked_first:
                assert (await c.post("api/check", {"file": "forever.yaml"}))[0] == 200
            replies = await asyncio.gather(*(c.post("api/run", {"file": "forever.yaml"}) for _ in range(4)))
            assert sorted(s for s, _ in replies) == [200, 409, 409, 409], replies
            assert all(b["error"]["code"] == "already-running" for s, b in replies if s == 409)
            await asyncio.sleep(1.0)
            launchers = children(gui.proc.pid)
            assert len(launchers) == 1, launchers
            status, body = await c.get("api/session")
            assert body["session"]["id"] == 1 and body["session"]["launcher"]["pid"] == launchers[0]
            return launchers
        launchers = run_async(main, gui)
        assert stop_and_time(gui.proc) < 10
        assert wait_until(lambda: not any(alive(p) for p in launchers), 5)


# ---------------------------------------------------------------- stopping a run


def test_stop_is_idempotent_and_answers_with_the_summary(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_TICK_MS": "50"}) as gui:
        async def main(c: Client) -> None:
            status, body = await c.post("api/stop")  # nothing runs: not an error
            assert status == 200 and body == {"session": None}
            sock = await c.ws()
            await sock.until(is_synced)
            status, body = await c.post("api/run", {"file": "forever.yaml"})
            assert status == 200 and body["session"]["stop"] == "none" and body["session"]["launcher"]["running"]
            await sock.until(lambda m: m.get("event", {}).get("event") == "time")
            replies = await asyncio.gather(*(c.post("api/stop", {"force": False}) for _ in range(3)))
            assert all(s == 200 and b["session"]["stop"] == "requested" for s, b in replies), replies
            # A stop without a body and without Content-Type (what a plain fetch sends) is a stop too.
            async with c.http.post(f"{gui.base}/api/stop", headers=gui.headers()) as r:
                assert r.status == 200 and (await r.json())["session"]["stop"] == "requested"
            got = await sock.until(is_ended)
            assert len(event_messages(sock.received, "command")) == 1  # one stop reached the launcher
            end = got[-1]["session"]
            assert end["launcher"] == {"pid": end["launcher"]["pid"], "running": False, "exit_code": 130,
                                       "signal": None}
            status, body = await c.post("api/stop", {"force": True})  # ended: the summary, nothing sent
            assert status == 200 and body["session"] == end
            for bad in ({"force": "yes"}, {"force": 1}):
                status, body = await c.post("api/stop", bad)
                assert status == 400 and body["error"]["code"] == "invalid-field"
        run_async(main, gui)


def test_force_stops_a_launcher_that_waits(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_TICK_MS": "50"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "ignore-stop.yaml"})
            await sock.until(lambda m: m.get("event", {}).get("event") == "time")
            status, body = await c.post("api/stop", {})
            assert body["session"]["stop"] == "requested"
            await sock.until(lambda m: m.get("event", {}).get("command") == "stop")
            await asyncio.sleep(0.3)
            status, body = await c.get("api/session")
            assert body["session"]["state"] == "running"  # this launcher waits after the first stop
            replies = await asyncio.gather(*(c.post("api/stop", {"force": True}) for _ in range(3)))
            assert all(b["session"]["stop"] == "forced" for _, b in replies)
            got = await sock.until(is_ended)
            assert [p["state"] for p in got[-1]["session"]["participants"]] == ["killed"] * 3
            assert len(event_messages(sock.received, "command")) == 2
        run_async(main, gui)


def test_force_without_a_stop_first_sends_two(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_TICK_MS": "50"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "ignore-stop.yaml"})
            await sock.until(lambda m: m.get("event", {}).get("event") == "time")
            status, body = await c.post("api/stop", {"force": True})
            assert body["session"]["stop"] == "forced"
            got = await sock.until(is_ended)
            assert got[-1]["session"]["result"]["result"] == "interrupted"
            assert len(event_messages(sock.received, "command")) == 2
        run_async(main, gui)


def test_a_stop_while_the_run_starts_stops_it_once_started(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_TICK_MS": "50", "FAKE_LAUNCHER_CHECK_DELAY_MS": "1500"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            run = asyncio.create_task(c.post("api/run", {"file": "forever.yaml"}))  # checks the names first
            await asyncio.sleep(0.5)
            status, body = await c.post("api/stop", {"force": False})
            assert status == 200 and body == {"session": None}  # no run yet: the one starting gets the stop
            status, body = await run
            assert status == 200 and body["session"]["stop"] == "requested", body
            got = await sock.until(is_ended, timeout=15)
            assert got[-1]["session"]["result"]["result"] == "interrupted"
            assert len(event_messages(sock.received, "command")) == 1
            # The next run does not inherit the stop.
            status, body = await c.post("api/run", {"file": "forever.yaml"})
            assert status == 200 and body["session"]["stop"] == "none"
            await c.post("api/stop", {"force": True})
            await sock.until(is_ended, timeout=15)
        run_async(main, gui)


# ---------------------------------------------------------------- checks


def test_concurrent_checks_wait_their_turn(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_CHECK_DELAY_MS": "300"}) as gui:
        async def main(c: Client) -> None:
            replies = await asyncio.gather(*(c.post("api/check", {"file": "simulation.yaml"}) for _ in range(3)))
            assert [s for s, _ in replies] == [200, 200, 200], replies
            # Too many waiting: the ones over the limit are told so, not queued for minutes.
            replies = await asyncio.gather(*(c.post("api/check", {"file": "simulation.yaml"}) for _ in range(10)))
            statuses = sorted(s for s, _ in replies)
            assert statuses.count(200) >= 2 and 503 in statuses, statuses
            assert all(b["error"]["code"] == "check-busy" for s, b in replies if s == 503)
        run_async(main, gui)


# ---------------------------------------------------------------- the launcher's end


def test_a_launcher_whose_descendant_keeps_its_output_open_still_ends(tmp_path: Path) -> None:
    # The reviewer's steps: a launcher that writes its result and exits, leaving a process in a session of its
    # own that holds the launcher's standard output and error.
    orphans: list[int] = []
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_ORPHAN_S": "60"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            status, body = await c.post("api/run", {"file": "orphan.yaml"})
            assert status == 200
            for e in event_messages(await sock.until(lambda m: "left a process behind" in str(m)), "log"):
                if m := re.search(r"pid (\d+)", e["line"]):
                    orphans.append(int(m.group(1)))
            got = await sock.until(is_ended, timeout=10)
            end = got[-1]["session"]
            assert end["result"]["result"] == "passed"
            assert end["launcher"]["running"] is False and end["launcher"]["exit_code"] == 0
            assert "output-left-open" in [p["code"] for p in end["problems"]]
            status, body = await c.post("api/run", {"file": "simulation.yaml"})  # the next run may start
            assert status == 200, body
            await sock.until(is_ended)
        try:
            run_async(main, gui)
            assert orphans and all(alive(p) for p in orphans)  # the GUI cannot reach it (another session)
        finally:
            for pid in orphans:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


def test_the_summary_says_when_the_launcher_has_ended_without_a_result(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "launcher-crash.yaml"})
            got = await sock.until(is_ended)
            sessions = [m["session"] for m in got if m["type"] == "session"]
            assert [s["state"] for s in sessions] == ["running", "ended"]  # the start, then the end
            end = sessions[-1]
            assert end["state"] == "ended" and end["result"] is None
            assert end["launcher"] == {"pid": end["launcher"]["pid"], "running": False, "exit_code": 70,
                                       "signal": None}
            assert [p["code"] for p in end["problems"]][-1] == "no-result"
        run_async(main, gui)


# ---------------------------------------------------------------- the server's end


def test_no_new_run_or_check_once_the_server_is_ending(tmp_path: Path) -> None:
    with gui_server(tmp_path, args=("--stop-timeout", "3")) as gui:
        async def main(c: Client) -> list[int]:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "ignore-stop.yaml"})
            await sock.until(lambda m: m.get("event", {}).get("event") == "time")
            launchers = children(gui.proc.pid)
            gui.proc.send_signal(signal.SIGTERM)  # waits up to 3 s for this launcher, which ignores the end
            await asyncio.sleep(0.5)
            for path, body in (("api/run", {"file": "simulation.yaml"}), ("api/check", {"file": "simulation.yaml"})):
                status, reply = await c.post(path, body)
                assert status == 503 and reply["error"]["code"] == "shutting-down", (path, reply)
            return launchers
        launchers = run_async(main, gui)
        assert gui.proc.wait(30) == 0
        assert wait_until(lambda: not any(alive(p) for p in launchers), 5)


def unauthenticated_websocket(port: int) -> socket.socket:
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nOrigin: http://127.0.0.1:{port}\r\n"
              "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
              "Sec-WebSocket-Version: 13\r\n\r\n".encode())
    assert s.recv(100).startswith(b"HTTP/1.1 101")
    return s


def half_sent_request(port: int) -> socket.socket:
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(b"GET /api/app HTTP/1.1\r\nHost: 127.0.0.1")
    return s


def idle_keep_alive(port: int) -> socket.socket:
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n\r\n".encode())
    s.recv(100)
    return s


@pytest.mark.parametrize("connect", [unauthenticated_websocket, half_sent_request, idle_keep_alive])
def test_open_connections_do_not_delay_the_end(tmp_path: Path, connect) -> None:
    with gui_server(tmp_path) as gui:
        s = connect(gui.port)
        try:
            time.sleep(0.3)
            assert stop_and_time(gui.proc) < 4
            if connect is unauthenticated_websocket:  # told why: going away (1001), not just cut
                s.settimeout(2)
                rest = b""
                while chunk := s.recv(4096):
                    rest += chunk
                assert b"\x88" in rest and b"\x03\xe9" in rest.split(b"\r\n\r\n", 1)[-1], rest
        finally:
            s.close()


def test_a_hanging_check_does_not_delay_the_end(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_CHECK_DELAY_MS": "60000"}) as gui:
        async def main(c: Client) -> list[int]:
            check = asyncio.create_task(c.post("api/check", {"file": "simulation.yaml"}))
            assert await asyncio.to_thread(wait_until, lambda: children(gui.proc.pid), 5)
            checks = children(gui.proc.pid)
            elapsed = await asyncio.to_thread(stop_and_time, gui.proc)
            assert elapsed < 5
            status, body = await check  # answered, not cut: the check was stopped by the server's end
            assert status == 503 and body["error"]["code"] == "shutting-down", body
            return checks
        checks = run_async(main, gui)
        assert wait_until(lambda: not any(alive(p) for p in checks), 5)


def test_the_end_is_bounded_by_the_stop_timeout(tmp_path: Path) -> None:
    # A launcher that ignores the end of its input and SIGTERM: input closed, --stop-timeout, SIGTERM, a wait,
    # then SIGKILL. With a WebSocket connected and another one not yet authenticated.
    with gui_server(tmp_path, args=("--stop-timeout", "1")) as gui:
        async def main(c: Client) -> list[int]:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "ignore-stop.yaml"})
            got = await sock.until(lambda m: m.get("event", {}).get("event") == "time")
            return [*children(gui.proc.pid),
                    *(e["pid"] for e in event_messages(sock.received, "participant") if e["state"] == "started")]
        pids = run_async(main, gui)
        s = unauthenticated_websocket(gui.port)
        try:
            assert stop_and_time(gui.proc) < 1 + 15
        finally:
            s.close()
        assert wait_until(lambda: not any(alive(p) for p in pids), 5)
