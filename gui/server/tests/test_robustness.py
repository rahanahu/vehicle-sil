"""Input that a correct client or launcher would not send: events with fields of the wrong type, odd requests
that must not become a 500, connections that never authenticate, and the parts of the checks that aiohttp does
(fixed here so that an upgrade that changes them is noticed)."""

from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import aiohttp
import pytest
from conftest import Client, Gui, gui_server, is_ended, is_synced, raw_http, run_async

POSIX = sys.platform != "win32"


def event_messages(messages: list[dict], kind: str) -> list[dict]:
    return [m["event"] for m in messages if m["type"] == "event" and m["event"]["event"] == kind]


# ---------------------------------------------------------------- events of the wrong shape


def test_events_with_wrong_types_do_not_stop_the_reading(gui: Gui) -> None:
    async def main(c: Client) -> None:
        sock = await c.ws()
        await sock.until(is_synced)
        assert (await c.post("api/run", {"file": "bad-types.yaml"}))[0] == 200
        got = await sock.until(is_ended, timeout=15)
        end = got[-1]["session"]
        assert end["result"]["result"] == "passed" and end["launcher"]["exit_code"] == 0
        codes = [m["code"] for m in got if m["type"] == "error"]
        assert codes.count("bad-event") == 9, codes
        assert end["check"]["participants"][0]["name"] == "Engine"  # the good check was kept, not the bad one
        assert [p["state"] for p in end["participants"]] == ["ended"] * 3
        assert not any(e.get("source") is None for e in event_messages(got, "log"))  # bad events are not passed
    run_async(main, gui)


# ---------------------------------------------------------------- requests that were a 500


def test_odd_input_is_a_4xx_with_the_usual_headers(gui: Gui) -> None:
    host = f"127.0.0.1:{gui.port}".encode()
    good = b"Host: " + host + b"\r\nOrigin: http://" + host + b"\r\nAuthorization: Bearer " + gui.token.encode() + \
        b"\r\nContent-Type: application/json\r\n"

    def post(path: bytes, body: bytes, headers: bytes = good) -> bytes:
        return raw_http(gui.port, b"POST " + path + b" HTTP/1.1\r\n" + headers + b"Content-Length: " +
                        str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)

    answers = {
        "token bytes that are not UTF-8": raw_http(gui.port, b"GET /api/app HTTP/1.1\r\nHost: " + host +
                                                   b"\r\nAuthorization: Bearer \xff\xfe\r\nConnection: close\r\n\r\n"),
        "a speed of 401 digits": post(b"/api/run", b'{"file":"simulation.yaml","speed":1' + b"0" * 400 + b"}"),
        "a lone surrogate in file": post(b"/api/run", b'{"file":"sim\\ud800.yaml"}'),
        "a lone surrogate in a name": post(b"/api/check", b'{"file":"simulation.yaml","disable":["\\udc80"]}'),
    }
    for what, answer in answers.items():
        status = int(answer.split(b" ", 2)[1])
        assert 400 <= status < 500, (what, answer[:200])
        assert b"Content-Security-Policy:" in answer and b"nosniff" in answer, what
        assert b'"error"' in answer, what
    assert "Traceback" not in gui.stderr_text()

    async def main(c: Client) -> None:
        ws = await c.http.ws_connect(f"ws://127.0.0.1:{gui.port}/ws", headers={"Origin": gui.origin})
        await ws.send_str('{"type":"auth","token":"\\ud800"}')
        msg = await ws.receive()
        assert msg.type == aiohttp.WSMsgType.TEXT and json.loads(msg.data)["code"] == "unauthorized"
        msg = await ws.receive()
        assert msg.type == aiohttp.WSMsgType.CLOSE and msg.data == 4401
    run_async(main, gui)


# ---------------------------------------------------------------- connections


def test_unauthenticated_websockets_count_toward_the_limit(gui: Gui) -> None:
    async def main(c: Client) -> None:
        socks, refused = [], None
        for n in range(40):
            try:
                socks.append(await c.http.ws_connect(f"ws://127.0.0.1:{gui.port}/ws", headers={"Origin": gui.origin}))
            except aiohttp.WSServerHandshakeError as e:
                refused = (n, e.status)
                break
        assert refused == (32, 503), refused
        msg = await socks[0].receive(timeout=10)  # no auth in time: closed
        assert msg.type == aiohttp.WSMsgType.CLOSE and msg.data == 4401 and msg.extra == "auth-timeout"
        for s in socks:
            await s.close()
    run_async(main, gui)


def test_auth_fields_must_be_counts(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "simulation.yaml"})
            await sock.until(is_ended)
            for auth in ({"session": True, "from_seq": 5}, {"session": 1, "from_seq": -3},
                         {"session": 1, "from_seq": 2.5}):
                again = await c.ws(auth)
                got = await again.until(is_synced)
                assert got[0]["replay"]["from_seq"] is None, auth
        run_async(main, gui)


# ---------------------------------------------------------------- what aiohttp checks for this server


def test_the_checks_that_aiohttp_makes(gui: Gui) -> None:
    host = f"127.0.0.1:{gui.port}".encode()
    auth = b"Authorization: Bearer " + gui.token.encode() + b"\r\n"

    def status(request: bytes) -> int:
        return int(raw_http(gui.port, request + b"Connection: close\r\n\r\n").split(b" ", 2)[1])

    assert status(b"GET /api/app HTTP/1.0\r\n" + auth) == 403  # no Host
    assert status(b"GET /api/app HTTP/1.1\r\n" + auth) == 400  # no Host (aiohttp)
    assert status(b"GET /api/app HTTP/1.1\r\n" + auth + b"Host: " + host + b"\r\nHost: evil.example\r\n") == 400
    assert status(b"GET /api/app HTTP/1.1\r\n" + auth + b"Host: evil.example\r\nHost: " + host + b"\r\n") == 400
    # The Host header decides, not the request line (a browser cannot forge Host).
    assert status(b"GET http://evil.example/api/app HTTP/1.1\r\n" + auth + b"Host: " + host + b"\r\n") == 200
    assert status(b"GET http://" + host + b"/api/app HTTP/1.1\r\n" + auth + b"Host: evil.example\r\n") == 403
    for path in (b"/%61pi/app", b"/api%2Fapp", b"/api/app/", b"/api/app;x", b"/api/../api/app"):
        assert status(b"GET " + path + b" HTTP/1.1\r\nHost: " + host + b"\r\n") in (401, 404), path
    for path in (b"//api/app", b"/x/../api/app", b"/./api/app", b"/api"):
        assert status(b"GET " + path + b" HTTP/1.1\r\nHost: " + host + b"\r\n") == 404, path


# ---------------------------------------------------------------- names that look like options


def test_a_participant_named_like_an_option_is_a_value(gui: Gui) -> None:
    async def main(c: Client) -> None:
        status, body = await c.post("api/check", {"file": "dash-name.yaml", "disable": ["--json"]})
        assert status == 200 and body["disable"] == ["--json"], body
        sock = await c.ws()
        await sock.until(is_synced)
        status, body = await c.post("api/run", {"file": "dash-name.yaml", "disable": ["--json"]})
        assert status == 200, body
        got = await sock.until(is_ended)
        hello = event_messages(got, "hello")[0]
        assert hello["arguments"][-4:] == ["--disable", "--json", "--", "dash-name.yaml"]
        assert got[-1]["session"]["result"]["result"] == "passed"
    run_async(main, gui)


# ---------------------------------------------------------------- lines the server no longer has


def test_a_reconnection_is_told_how_many_lines_were_dropped(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_FLOOD": "5000"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "flood.yaml"})
            live = await sock.until(is_ended)
            engine = [e for e in event_messages(live, "log") if e["source"] == "Engine"]
            for from_seq in (50, engine[-2500]["seq"], engine[-100]["seq"]):
                again = await c.ws({"session": 1, "from_seq": from_seq})
                got = await again.until(is_synced)
                missing = [e for e in engine[:-2000] if e["seq"] >= from_seq]
                dropped = [m for m in got if m["type"] == "dropped"]
                if missing:
                    assert got[1]["type"] == "dropped" and got[1]["reason"] == "not-kept"
                    assert dropped[0]["log"]["Engine"] == len(missing) and dropped[0]["at_least"] == []
                else:
                    assert not dropped
                replayed = [e for e in event_messages(got, "log") if e["source"] == "Engine"]
                assert replayed == [e for e in engine[-2000:] if e["seq"] >= from_seq]
                assert all(m["event"]["event"] == "log" for m in got if m["type"] == "event")
                await again.close()
        run_async(main, gui)
