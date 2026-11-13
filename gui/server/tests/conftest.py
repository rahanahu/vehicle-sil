"""Helpers of the GUI server's tests: the server as a real process (python -m vehicle_sil_gui) with the fake
launcher, and an HTTP and WebSocket client that sends what the screen sends."""

from __future__ import annotations

import asyncio
import json
import os
import signal
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

import aiohttp
import pytest

TESTS = Path(__file__).resolve().parent
FAKE = TESTS / "fake_launcher.py"
CONFIGS = TESTS / "configs"
START_TIMEOUT_S = 20


@dataclass
class Gui:
    proc: subprocess.Popen
    port: int
    token: str
    url: str
    stderr: Path

    @property
    def base(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    @property
    def origin(self) -> str:
        return self.base

    def headers(self, *, token: str | None = "", origin: str | None = "", host: str | None = None) -> dict:
        h = {}
        if token is not None:
            h["Authorization"] = f"Bearer {token or self.token}"
        if origin is not None:
            h["Origin"] = origin or self.origin
        if host is not None:
            h["Host"] = host
        return h

    def stderr_text(self) -> str:
        return self.stderr.read_text(encoding="utf-8", errors="replace")


@contextmanager
def gui_server(tmp_path: Path, *, root: Path = CONFIGS, env: dict | None = None, args: tuple = (),
               launcher: Path | None = FAKE, expect_start: bool = True, browser: bool = False,
               cwd: Path | None = None) -> Iterator[Gui]:
    environment = dict(os.environ)
    environment.setdefault("FAKE_LAUNCHER_TICK_MS", "10")
    environment.pop("FAKE_LAUNCHER_SCENARIO", None)
    environment.update(env or {})
    stderr = tmp_path / f"server-{time.monotonic_ns()}.err"
    with open(stderr, "wb") as err:
        proc = subprocess.Popen(
            [sys.executable, "-m", "vehicle_sil_gui", *([] if browser else ["--no-browser"]), "--json",
             *(["--launcher", str(launcher)] if launcher is not None else []), "--root", str(root), *args],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=err, env=environment, cwd=cwd)
    line = read_line(proc, START_TIMEOUT_S)
    if not expect_start:
        try:
            proc.wait(START_TIMEOUT_S)
            yield Gui(proc, 0, "", "", stderr)
        finally:
            stop_server(proc)
        return
    assert line, f"the server did not print its URL: {stderr.read_text()}"
    info = json.loads(line)
    gui = Gui(proc, info["port"], info["url"].split("#token=", 1)[1], info["url"], stderr)
    try:
        yield gui
    finally:
        stop_server(proc)


REPOSITORY = TESTS.parents[2]
CONTROL_FEATURES = ("events,stdin-commands,check-variables,speed,start-paused,time-commands,control-links,"
                    "scenario,manual-participants,control-server")


@pytest.fixture(scope="session")
def control_stubs(tmp_path_factory) -> Path:
    """The gRPC stubs of the Control API, made once for the tests that use the stand-in for the launcher's control
    server (fake_control.py). Skips when grpcio-tools is not installed (`uv sync` installs it); fails instead
    when VEHICLE_SIL_GUI_REQUIRE_CONTROL is 1 (scripts/verify-gui.py sets it)."""
    try:
        import grpc  # noqa: F401
        from grpc_tools import protoc
    except ImportError as e:
        # scripts/verify-gui.py asks for these tests: there, missing packages are a failure, not a skip.
        if os.environ.get("VEHICLE_SIL_GUI_REQUIRE_CONTROL") == "1":
            pytest.fail(f"grpcio and grpcio-tools are needed for the tests of the variables ({e}): uv sync")
        pytest.skip(f"grpcio or grpcio-tools is not installed ({e}): uv sync")
    out = tmp_path_factory.mktemp("control-stubs")
    proto = REPOSITORY / "proto"
    code = protoc.main(["protoc", f"-I{proto}", f"--python_out={out}", f"--grpc_python_out={out}",
                        str(proto / "vehicle_sil" / "control" / "v1" / "control.proto")])
    assert code == 0, "protoc failed"
    return out


@pytest.fixture
def control_env(control_stubs: Path, monkeypatch) -> dict:
    """The environment of a GUI server whose fake launcher has a control server (and of this process, for tests
    that start fake_control themselves)."""
    monkeypatch.setenv("VEHICLE_SIL_CONTROL_STUBS", str(control_stubs))
    return {"FAKE_LAUNCHER_FEATURES": CONTROL_FEATURES, "VEHICLE_SIL_CONTROL_STUBS": str(control_stubs)}


def without_variables(summary: dict) -> dict:
    """A summary of hello or of an HTTP answer as a `session` message carries it (those have no `variables`)."""
    return {k: v for k, v in summary.items() if k != "variables"}


def is_variables(state: str):
    return lambda m: m["type"] == "variables" and m["variables"]["state"] == state


def read_line(proc: subprocess.Popen, timeout: float) -> str:
    out: list[str] = []
    reader = threading.Thread(target=lambda: out.append(proc.stdout.readline().decode()), daemon=True)
    reader.start()
    reader.join(timeout)
    return out[0] if out else ""


def stop_server(proc: subprocess.Popen) -> int:
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    return proc.returncode


def alive(pid: int) -> bool:
    """Whether a process exists and is not a zombie (Linux)."""
    try:
        state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except (OSError, IndexError):
        return False
    return state != "Z"


def children(pid: int) -> list[int]:
    """The processes whose parent is pid and that are not zombies (Linux)."""
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
        except (OSError, IndexError):
            continue
        if fields[0] != "Z" and int(fields[1]) == pid:
            found.append(int(entry.name))
    return found


def raw_http(port: int, data: bytes, timeout: float = 3) -> bytes:
    """Sends bytes as they are (headers that a client library would not send) and returns the answer."""
    import socket
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(data)
        answer = b""
        try:
            while chunk := s.recv(65536):
                answer += chunk
                head, sep, body = answer.partition(b"\r\n\r\n")
                length = next((int(h.split(b":", 1)[1]) for h in head.split(b"\r\n")
                               if h.lower().startswith(b"content-length:")), None)
                if sep and (length is None or len(body) >= length):
                    break
        except TimeoutError:
            pass
    return answer


def wait_until(condition: Callable[[], bool], timeout: float = 10) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if condition():
            return True
        time.sleep(0.02)
    return condition()


class Client:
    """What the screen does: HTTP with the token and the server's Origin, and the WebSocket."""

    def __init__(self, gui: Gui, http: aiohttp.ClientSession):
        self.gui = gui
        self.http = http

    async def get(self, path: str, **headers) -> tuple[int, dict]:
        async with self.http.get(f"{self.gui.base}/{path}", headers=self.gui.headers(**headers)) as r:
            return r.status, await r.json(content_type=None)

    async def post(self, path: str, body: object = None, *, raw: bytes | None = None,
                   content_type: str = "application/json", **headers) -> tuple[int, dict]:
        h = self.gui.headers(**headers)
        h["Content-Type"] = content_type
        data = raw if raw is not None else json.dumps(body if body is not None else {}).encode()
        async with self.http.post(f"{self.gui.base}/{path}", data=data, headers=h) as r:
            return r.status, await r.json(content_type=None)

    async def ws(self, auth: dict | None = None, origin: str | None = "") -> "Socket":
        headers = {} if origin is None else {"Origin": origin or self.gui.origin}
        # no size limit on what it reads: the server relays events as large as the launcher writes them
        ws = await self.http.ws_connect(f"ws://127.0.0.1:{self.gui.port}/ws", headers=headers, max_msg_size=0)
        sock = Socket(ws)
        if auth is not False:
            message = {"type": "auth", "token": self.gui.token, **(auth or {})}
            await sock.send(message)
        return sock


class Socket:
    def __init__(self, ws: aiohttp.ClientWebSocketResponse):
        self.ws = ws
        self.sent: list[dict] = []
        self.received: list[dict] = []

    async def send(self, message: dict) -> None:
        self.sent.append(message)
        await self.ws.send_str(json.dumps(message))

    async def until(self, condition: Callable[[dict], bool], timeout: float = 20) -> list[dict]:
        """Receives until a message meets the condition; returns the messages received by this call."""
        got: list[dict] = []
        async with asyncio.timeout(timeout):
            while True:
                msg = await self.ws.receive()
                if msg.type != aiohttp.WSMsgType.TEXT:
                    raise ConnectionError(f"the WebSocket ended: {msg.type} {msg.data} {msg.extra}")
                m = json.loads(msg.data)
                got.append(m)
                self.received.append(m)
                if condition(m):
                    return got

    async def close(self) -> None:
        await self.ws.close()


def is_synced(m: dict) -> bool:
    return m["type"] == "synced"


def is_ended(m: dict) -> bool:
    return m["type"] == "session" and m["session"]["state"] == "ended"


def run_async(test: Callable[[Client], object], gui: Gui) -> object:
    async def main():
        async with aiohttp.ClientSession() as http:
            return await test(Client(gui, http))
    return asyncio.run(main())


@pytest.fixture
def gui(tmp_path: Path) -> Iterator[Gui]:
    with gui_server(tmp_path) as g:
        yield g
