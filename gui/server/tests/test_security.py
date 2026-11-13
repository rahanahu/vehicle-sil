"""The security requirements, against the real server: token, Host, Origin, no CORS, headers, paths, names."""

from __future__ import annotations

import json
import os
import shutil
import sys
from pathlib import Path
from urllib.parse import unquote

import aiohttp
import pytest
from conftest import CONFIGS, Client, Gui, gui_server, run_async, wait_until

API = [("GET", "api/app"), ("GET", "api/configs"), ("GET", "api/session"), ("POST", "api/check"),
       ("POST", "api/run"), ("POST", "api/stop"), ("POST", "api/time"), ("GET", "api/dashboard")]


async def request(c: Client, method: str, path: str, headers: dict, body: bytes = b"{}") -> aiohttp.ClientResponse:
    async with c.http.request(method, f"{c.gui.base}/{path}", headers=headers,
                              data=body if method == "POST" else None) as r:
        await r.read()
        return r


def test_every_api_needs_the_token(gui: Gui) -> None:
    async def main(c: Client) -> None:
        for method, path in API:
            for auth in (None, "Bearer", "Bearer wrong", f"Basic {gui.token}", f"Bearer {gui.token}x",
                         f"Bearer {gui.token[:-1]}"):
                headers = {"Origin": gui.origin, "Content-Type": "application/json"}
                if auth is not None:
                    headers["Authorization"] = auth
                r = await request(c, method, path, headers)
                assert r.status == 401, (method, path, auth)
                assert r.headers["WWW-Authenticate"] == "Bearer"
            # The token in the query string is not accepted (it would end up in logs and history).
            r = await request(c, method, f"{path}?token={gui.token}",
                              {"Origin": gui.origin, "Content-Type": "application/json"})
            assert r.status == 401
        status, _ = await c.get("api/app")
        assert status == 200
    run_async(main, gui)
    assert gui.token not in gui.stderr_text()


def test_only_the_servers_own_host_is_answered(gui: Gui) -> None:
    async def main(c: Client) -> None:
        # localhost is refused too: a browser may try ::1 first, where another program could listen.
        for host in ("evil.example", f"evil.example:{gui.port}", f"127.0.0.1:{gui.port + 1}", "127.0.0.1",
                     f"0.0.0.0:{gui.port}", f"[::1]:{gui.port}", f"localhost:{gui.port}", f"LOCALHOST:{gui.port}"):
            for path in ("api/app", "", "index.html"):
                r = await request(c, "GET", path, {**gui.headers(), "Host": host})
                assert r.status == 403, (host, path)
        r = await request(c, "GET", "api/app", {**gui.headers(origin=None), "Host": f"127.0.0.1:{gui.port}"})
        assert r.status == 200
    run_async(main, gui)


def test_changes_need_the_servers_own_origin(gui: Gui) -> None:
    async def main(c: Client) -> None:
        for origin in (None, "null", "http://evil.example", f"http://127.0.0.1:{gui.port + 1}",
                       f"https://127.0.0.1:{gui.port}", "http://127.0.0.1:4200", f"http://localhost:{gui.port}"):
            for path in ("api/check", "api/run", "api/stop", "api/time"):
                status, body = await c.post(path, {"file": "simulation.yaml"}, origin=origin)
                assert status == 403 and body["error"]["code"] == "forbidden-origin", (origin, path)
            if origin is not None:  # a GET from another page is refused too
                status, _ = await c.get("api/app", origin=origin)
                assert status == 403
        status, _ = await c.post("api/check", {"file": "simulation.yaml"})
        assert status == 200
        status, _ = await c.get("api/app", origin=None)  # same-origin GETs may come without Origin
        assert status == 200
    run_async(main, gui)


def test_no_cors_and_no_preflight(gui: Gui) -> None:
    async def main(c: Client) -> None:
        r = await request(c, "OPTIONS", "api/run", {"Origin": gui.origin, "Access-Control-Request-Method": "POST",
                                                     "Access-Control-Request-Private-Network": "true"})
        assert r.status == 403
        r = await request(c, "OPTIONS", "api/run", {"Origin": "http://evil.example"})
        assert r.status == 403
        for method, path in API:
            r = await request(c, method, path, {**gui.headers(), "Content-Type": "application/json"})
            assert not any(h.lower().startswith("access-control-") for h in r.headers), path
    run_async(main, gui)


def test_post_bodies_must_be_json(gui: Gui) -> None:
    async def main(c: Client) -> None:
        # A form post (what another page could send without a preflight) is refused.
        status, body = await c.post("api/check", raw=b"file=simulation.yaml",
                                    content_type="application/x-www-form-urlencoded")
        assert status == 415
        status, body = await c.post("api/check", raw=b"{not json")
        assert status == 400 and body["error"]["code"] == "invalid-json"
        status, body = await c.post("api/check", raw=b"[1]")
        assert status == 400 and body["error"]["code"] == "invalid-json"
        status, body = await c.post("api/check", raw=b'{"file": "' + b"x" * 100_000 + b'"}')
        assert status == 413
    run_async(main, gui)


def test_response_headers(gui: Gui) -> None:
    async def main(c: Client) -> None:
        for path, headers in (("api/app", gui.headers()), ("", {}), ("index.html", {}), ("run", {}),
                              ("api/nothing", gui.headers()), ("missing.js", {})):
            r = await request(c, "GET", path, headers)
            csp = r.headers.get("Content-Security-Policy", "")
            assert "script-src 'self'" in csp and "frame-ancestors 'none'" in csp and "unsafe-eval" not in csp
            assert r.headers["X-Content-Type-Options"] == "nosniff", path
            assert r.headers["Referrer-Policy"] == "no-referrer", path
            if path.startswith("api/"):
                assert r.headers["Cache-Control"] == "no-store", path
                assert r.headers["Content-Type"].startswith("application/json")
        r = await request(c, "GET", "api/nothing", gui.headers())
        assert r.status == 404
    run_async(main, gui)


def test_paths_outside_the_root_are_refused(tmp_path: Path) -> None:
    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)
    (tmp_path / "secret.yaml").write_text("vehicle_sil_simulation: 1\nparticipants:\n  - name: Secret\n")
    (tmp_path / "secretdir").mkdir()
    (tmp_path / "secretdir" / "s.yaml").write_text("vehicle_sil_simulation: 1\n")
    (root / "notes.txt").write_text("x")
    symlinks = sys.platform != "win32"
    if symlinks:
        os.symlink(tmp_path / "secret.yaml", root / "escape.yaml")
        os.symlink(tmp_path / "secretdir", root / "escapedir")
    with gui_server(tmp_path, root=root) as gui:
        async def main(c: Client) -> None:
            cases = {"../secret.yaml": "outside-root", "sub/../../secret.yaml": "outside-root",
                     str(tmp_path / "secret.yaml"): "outside-root", "..\\secret.yaml": "invalid-path",
                     "notes.txt": "not-a-configuration", "missing.yaml": "file-not-found", "": "invalid-path"}
            if symlinks:
                cases.update({"escape.yaml": "outside-root", "escapedir/s.yaml": "outside-root"})
            for file, code in cases.items():
                for path in ("api/check", "api/run"):
                    status, body = await c.post(path, {"file": file})
                    assert body["error"]["code"] == code and status in (400, 404), (path, file, body)
            status, body = await c.get("api/configs")
            listed = [f["path"] for f in body["files"]]
            assert "simulation.yaml" in listed and not any("escape" in p or "secret" in p for p in listed)
        run_async(main, gui)


def test_only_participants_of_the_file_can_be_disabled(gui: Gui) -> None:
    async def main(c: Client) -> None:
        for disable in (["Nobody"], ["--json"], ["Engine", "Nobody"], ["engine"]):
            for path in ("api/check", "api/run"):
                status, body = await c.post(path, {"file": "simulation.yaml", "disable": disable})
                assert status == 400 and body["error"]["code"] == "unknown-participant", (path, disable)
                assert "Engine, Dashboard, Logger" in body["error"]["message"]
        for disable in ("Engine", [1], ["Engine", "Engine"], {"Engine": True}):
            status, body = await c.post("api/check", {"file": "simulation.yaml", "disable": disable})
            assert status == 400 and body["error"]["code"] == "invalid-field", disable
        for speed in (True, 0, -1, 1e7, 1e-7, "fast", "Infinity", [2]):
            status, body = await c.post("api/run", {"file": "simulation.yaml", "speed": speed})
            assert status == 400 and body["error"]["code"] == "invalid-field", speed
        status, body = await c.post("api/run", raw=b'{"file": "simulation.yaml", "speed": NaN}')
        assert status == 400
        status, body = await c.post("api/run", {"file": "simulation.yaml", "start_paused": "yes"})
        assert status == 400 and body["error"]["code"] == "invalid-field"
        status, body = await c.get("api/session")
        assert body["session"] is None  # nothing was started by any of these
    run_async(main, gui)


def test_websocket_needs_origin_and_token(gui: Gui) -> None:
    async def main(c: Client) -> None:
        for origin in (None, "http://evil.example", f"http://127.0.0.1:{gui.port + 1}"):
            with pytest.raises(aiohttp.WSServerHandshakeError) as e:
                await c.ws(origin=origin)
            assert e.value.status == 403
        for auth in ({"token": "wrong"}, {"token": gui.token + "x"}, {"type": "hello", "token": gui.token},
                     {"token": None}):
            sock = await c.ws(auth=auth if "type" not in auth else False)
            if "type" in auth:
                await sock.send(auth)
            got = await sock.until(lambda m: True)
            assert got[0]["type"] == "error" and got[0]["code"] == "unauthorized"
            msg = await sock.ws.receive()
            assert msg.type == aiohttp.WSMsgType.CLOSE and msg.data == 4401
        # The token in the URL is not accepted either: the first message must carry it.
        ws = await c.http.ws_connect(f"ws://127.0.0.1:{gui.port}/ws?token={gui.token}",
                                     headers={"Origin": gui.origin})
        await ws.send_str("{}")
        msg = await ws.receive()
        assert '"unauthorized"' in msg.data
        await ws.close()
    run_async(main, gui)


def test_dev_origin_is_accepted_only_when_given(tmp_path: Path) -> None:
    dev = "http://127.0.0.1:4200"
    with gui_server(tmp_path, args=("--dev-origin", dev)) as gui:
        async def main(c: Client) -> None:
            # Through the dev server's proxy, Host and Origin are the dev server's.
            status, _ = await c.post("api/check", {"file": "simulation.yaml"}, origin=dev, host="127.0.0.1:4200")
            assert status == 200
            ws = await c.http.ws_connect(f"ws://127.0.0.1:{gui.port}/ws", headers={"Origin": dev})
            await ws.close()
            status, _ = await c.post("api/check", {"file": "simulation.yaml"}, origin="http://127.0.0.1:4201")
            assert status == 403
        run_async(main, gui)
    for bad in ("http://evil.example:4200", "https://127.0.0.1:4200", "http://127.0.0.1", "*",
                "http://127.0.0.1:4200/", "http://localhost:4200"):
        with gui_server(tmp_path, args=("--dev-origin", bad), expect_start=False) as gui:
            assert gui.proc.returncode == 2 and "--dev-origin" in gui.stderr_text()


def test_the_server_listens_on_loopback_only(gui: Gui) -> None:
    import socket
    addresses = {a[4][0] for a in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)} - {"127.0.0.1"}
    addresses = {a for a in addresses if not a.startswith("127.")}
    if not addresses:
        pytest.skip("no non-loopback IPv4 address to try")
    for address in addresses:
        with pytest.raises(OSError):
            socket.create_connection((address, gui.port), timeout=2).close()


@pytest.mark.skipif(sys.platform == "win32", reason="file modes, and BROWSER as a script")
def test_the_browser_is_not_given_the_token(tmp_path: Path) -> None:
    # Another user of the computer can read the command line of a process (/proc/<pid>/cmdline): the browser is
    # given a file that only this user can read, which forwards to the URL with the token.
    record = tmp_path / "browser.json"
    script = tmp_path / "browser.py"
    script.write_text(
        f"#!{sys.executable}\n"
        "import json, os, stat, sys\n"
        "from urllib.parse import unquote\n"
        "info = {'argv': sys.argv[1:]}\n"
        "path = unquote(sys.argv[1][len('file://'):]) if sys.argv[1].startswith('file://') else None\n"
        "if path and os.path.exists(path):\n"
        "    info['mode'] = stat.S_IMODE(os.stat(path).st_mode)\n"
        "    info['content'] = open(path, encoding='utf-8').read()\n"
        f"open({str(record)!r}, 'w').write(json.dumps(info))\n")
    script.chmod(0o755)
    with gui_server(tmp_path, browser=True, env={"BROWSER": str(script)}) as gui:
        assert wait_until(record.exists, 10)
        info = json.loads(record.read_text())
        assert all(gui.token not in a for a in info["argv"]), info["argv"]
        assert info["argv"][0].startswith("file://") and info["mode"] == 0o600
        assert gui.url in info["content"]
        page = Path(unquote(info["argv"][0][len("file://"):]))
    assert not page.exists()  # removed, at the latest when the server ends
