"""The client library and the parts for a screen of your own, served under vsil/ without a token: the two files of
the fixed table and nothing else, with the server's usual headers, before --web's own files, and what the server
says when they are not built."""

from __future__ import annotations

import asyncio
import json
import socket
from pathlib import Path

import aiohttp
import pytest
from aiohttp.test_utils import TestServer
from conftest import CONFIGS, REPLAY, REPOSITORY

from vehicle_sil_gui import client_files
from vehicle_sil_gui.launcher import Launcher
from vehicle_sil_gui.server import CSP, GuiServer, Options

CLIENT, ELEMENTS = "vehicle-sil-gui-client.mjs", "vehicle-sil-gui-elements.mjs"


def built(directory: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / CLIENT).write_text("export const connect = 1;\n")
    (directory / ELEMENTS).write_text("import './vehicle-sil-gui-client.mjs';\n")
    (directory / "secret.mjs").write_text("SECRETDATA")
    return directory


async def fetch(server: TestServer, path: str, **headers) -> tuple[int, bytes, dict]:
    async with aiohttp.ClientSession() as http:
        # raw_path: the request line is sent as written (no normalization of ".." or "%2e%2e" by the client)
        url = server.make_url("/").with_path(path, encoded=True)
        async with http.get(url, headers=headers, allow_redirects=False) as r:
            return r.status, await r.read(), dict(r.headers)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def serving(client_dir: Path | None, web_dir: Path | None = None, monkeypatch=None) -> tuple[GuiServer, TestServer]:
    """The server (its Host check wants the port it is given) and the test server that serves its app."""
    monkeypatch.setattr(client_files, "find_dir", lambda: client_dir)
    port = free_port()
    server = GuiServer(Options(root=CONFIGS, launcher=Launcher(REPLAY), port=port, token="t", web_dir=web_dir))
    return server, TestServer(server.app(), host="127.0.0.1", port=port)


def test_the_two_files_are_served_without_a_token_with_the_servers_headers(tmp_path: Path, monkeypatch) -> None:
    directory = built(tmp_path / "gui-client")
    _, ts = serving(directory, monkeypatch=monkeypatch)

    async def main() -> None:
        async with ts:
            for name in (CLIENT, ELEMENTS):
                status, body, headers = await fetch(ts, f"/vsil/{name}")
                assert status == 200 and body == (directory / name).read_bytes(), name
                assert headers["Content-Type"] == "text/javascript; charset=utf-8"
                assert headers["Content-Security-Policy"] == CSP and headers["X-Content-Type-Options"] == "nosniff"

    asyncio.run(main())


@pytest.mark.parametrize("path", [
    "/vsil", "/vsil/", "/vsil/secret.mjs", "/vsil/missing.mjs", "/vsil/index.html",
    "/vsil/Vehicle-Sil-Gui-Client.mjs", "/vsil/VEHICLE-SIL-GUI-ELEMENTS.MJS",  # another case: another name
    "/vsil/vehicle-sil-gui-client.mjs/", "/vsil/vehicle-sil-gui-client.mjs.map", "/vsil/x/vehicle-sil-gui-client.mjs",
    "/vsil/../secret.mjs", "/vsil/%2e%2e/secret.mjs", "/vsil/..%2fsecret.mjs", "/vsil/%2e%2e%2fsecret.mjs",
    "/vsil/..\\secret.mjs", "/vsil/%5c..%5csecret.mjs", "/vsil/vehicle-sil-gui-client.mjs%00",
    "/vsil/vehicle-sil-gui-client.mjs%20", "/vsil/vehicle-sil-gui-client.mjs.", "/vsil//vehicle-sil-gui-client.mjs",
    "/vsil/%76ehicle-sil-gui-client.mjs/..", "/vsil/./secret.mjs",
])
def test_no_other_name_is_served(tmp_path: Path, monkeypatch, path: str) -> None:
    directory = built(tmp_path / "gui-client")
    _, ts = serving(directory, monkeypatch=monkeypatch)

    async def main() -> None:
        async with ts:
            status, body, _ = await fetch(ts, path)
            assert status == 404, (path, body[:100])
            assert b"SECRETDATA" not in body

    asyncio.run(main())


def test_the_servers_files_win_over_a_vsil_directory_of_the_screen(tmp_path: Path, monkeypatch) -> None:
    directory = built(tmp_path / "gui-client")
    web = tmp_path / "screen"
    (web / "vsil").mkdir(parents=True)
    (web / "index.html").write_text("<!doctype html>")
    (web / "vsil" / CLIENT).write_text("the screen's own copy")
    (web / "vsil" / "own.mjs").write_text("the screen's own file")
    _, ts = serving(directory, web, monkeypatch)

    async def main() -> None:
        async with ts:
            status, body, _ = await fetch(ts, f"/vsil/{CLIENT}")
            assert status == 200 and body == (directory / CLIENT).read_bytes()
            status, _, _ = await fetch(ts, "/vsil/own.mjs")  # the directory is not the screen's to fill
            assert status == 404

    asyncio.run(main())


def test_the_host_is_checked_as_for_the_screen(tmp_path: Path, monkeypatch) -> None:
    _, ts = serving(built(tmp_path / "gui-client"), monkeypatch=monkeypatch)

    async def main() -> None:
        async with ts:
            status, body, _ = await fetch(ts, f"/vsil/{CLIENT}", Host="localhost:1")
            assert status == 403 and json.loads(body)["error"]["code"] == "forbidden-host"

    asyncio.run(main())


def test_a_missing_build_says_what_to_run(tmp_path: Path, monkeypatch) -> None:
    _, ts = serving(None, monkeypatch=monkeypatch)

    async def main() -> None:
        async with ts:
            status, body, _ = await fetch(ts, f"/vsil/{CLIENT}")
            error = json.loads(body)["error"]
            assert status == 404 and error["code"] == "not-found"
            assert "npm run build:client" in error["message"] and "gui/web" in error["message"], error
            assert CLIENT in error["message"] and ELEMENTS in error["message"]

    asyncio.run(main())


def test_a_missing_build_is_said_once_on_stderr_and_only_when_a_file_is_asked_for(tmp_path: Path, monkeypatch,
                                                                                  caplog) -> None:
    _, missing = serving(None, monkeypatch=monkeypatch)
    _, present = serving(built(tmp_path / "gui-client"), monkeypatch=monkeypatch)

    async def main() -> None:
        async with missing:
            for path in ("/vsil/other.mjs", "/vsil/", "/"):  # not one of the two names: nothing to say
                await fetch(missing, path)
            assert caplog.records == []
            for _ in range(2):
                status, _, _ = await fetch(missing, f"/vsil/{CLIENT}")
                assert status == 404
            await fetch(missing, f"/vsil/{ELEMENTS}")
            assert [r.getMessage() for r in caplog.records] == [client_files.missing_message()]
        caplog.clear()
        async with present:
            for name in (CLIENT, ELEMENTS, "other.mjs"):
                await fetch(present, f"/vsil/{name}")
            assert caplog.records == []

    asyncio.run(main())


def test_a_file_that_disappears_is_a_404_that_says_how_to_build_it(tmp_path: Path, monkeypatch) -> None:
    directory = built(tmp_path / "gui-client")
    _, ts = serving(directory, monkeypatch=monkeypatch)
    (directory / ELEMENTS).unlink()

    async def main() -> None:
        async with ts:
            status, body, _ = await fetch(ts, f"/vsil/{ELEMENTS}")
            assert status == 404 and "npm run build:client" in json.loads(body)["error"]["message"]

    asyncio.run(main())


@pytest.mark.parametrize("name", [CLIENT, ELEMENTS])
def test_the_directory_is_found_only_with_both_files(tmp_path: Path, monkeypatch, name: str) -> None:
    import vehicle_sil_gui.client_files as module
    fake = tmp_path / "vehicle_sil_gui"
    (fake / "client").mkdir(parents=True)
    for other in (CLIENT, ELEMENTS):
        if other != name:
            (fake / "client" / other).write_text("x")
    monkeypatch.setattr(module, "__file__", str(fake / "client_files.py"))
    assert module.find_dir() is None  # neither the packaged one (one file missing) nor a repository above tmp_path
    (fake / "client" / name).write_text("x")
    assert module.find_dir() == fake / "client"


def test_the_repository_is_searched_in_its_build_directory() -> None:
    # The build of gui/web (npm run build:client) is where the server finds the files from the repository.
    here = Path(client_files.__file__).resolve().parent
    assert here.parents[3] == REPOSITORY
    found = client_files.find_dir()
    assert found is None or found in (here / "client", REPOSITORY / "build" / "gui-client")
