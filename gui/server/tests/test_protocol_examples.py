"""gui/protocol/examples: real conversations with the server and the fake launcher, compared with the files.

The values that change from run to run (process IDs, times, the instance, the paths of this computer, counts
that depend on timing) are replaced with fixed ones before the comparison. UPDATE_PROTOCOL_EXAMPLES=1 writes the
files instead. Every file in the directory must be one that these tests write (EXAMPLES below).
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import signal
import sys
from pathlib import Path

import pytest
from conftest import CONFIGS, FAKE, Client, gui_server, is_ended, is_synced, run_async

DIRECTORY = Path(__file__).resolve().parents[2] / "protocol" / "examples"
UPDATE = os.environ.get("UPDATE_PROTOCOL_EXAMPLES") == "1"
POSIX = sys.platform != "win32"

EXAMPLES = {
    "app", "configs", "session-none", "session-running", "session-ended", "check-passed", "check-disabled",
    "check-failed", "run", "stop", "stop-force", "stop-ended",
    "error-unauthorized", "error-forbidden-host", "error-forbidden-origin", "error-cors-not-allowed",
    "error-not-found", "error-method-not-allowed", "error-too-large", "error-unsupported-media-type",
    "error-invalid-json", "error-invalid-field", "error-invalid-path", "error-outside-root",
    "error-not-a-configuration", "error-unknown-participant", "error-unsupported", "error-file-not-found",
    "error-already-running", "error-check-busy", "error-check-timeout", "error-launcher-output",
    "error-launcher-unusable", "error-launcher-start", "error-shutting-down", "error-too-many-connections",
    "ws-pass", "ws-check-error", "ws-participant-crash", "ws-launcher-crash", "ws-bad-lines", "ws-bad-types",
    "ws-orphan", "ws-stop", "ws-force-stop", "ws-replay", "ws-resume", "ws-dropped", "ws-slow-client",
    "ws-resync", "ws-unauthorized",
}


class Normalizer:
    def __init__(self, gui, paths: dict[str, str] | None = None):
        self.pids: dict[int, int] = {}
        self.paths = {str(FAKE): "/path/to/vehicle_sil_launch", str(CONFIGS): "/path/to/root",
                      f"127.0.0.1:{gui.port}": "127.0.0.1:8765", **(paths or {})}
        self.token = gui.token

    def __call__(self, value, key=None):
        if isinstance(value, dict):
            out = {k: self(v, k) for k, v in value.items()}
            if isinstance(out.get("seq"), int) and "t_ms" in out:
                out["t_ms"] = out["seq"] * 10
            if isinstance(out.get("last_seq"), int) and "last_t_ms" in out:
                out["last_t_ms"] = out["last_seq"] * 10
            return out
        if isinstance(value, list):
            return [self(v) for v in value]
        if key == "pid" and isinstance(value, int):
            return self.pids.setdefault(value, 10001 + len(self.pids))
        if key == "instance" and isinstance(value, str):
            return "0123456789abcdef"
        if key in ("started_at", "ended_at") and isinstance(value, str):
            return "2026-01-01T00:00:00.000Z" if key == "started_at" else "2026-01-01T00:00:01.000Z"
        if key == "modified" and isinstance(value, str):
            return "2026-01-01T00:00:00Z"
        if isinstance(value, str):
            for real, fixed in self.paths.items():
                value = value.replace(real, fixed)
            value = value.replace(f"pid {self.pid_in_log(value)}", "pid 10099") if "pid " in value else value
            value = re.sub(r'"t_ms":\s*\d+', '"t_ms":0', value)  # a raw line in a problem's detail
            assert self.token not in value, "the token must not appear in any response"
        return value

    @staticmethod
    def pid_in_log(text: str) -> str:
        import re
        m = re.search(r"pid (\d+)", text)
        return m.group(1) if m else "-"


def check_example(name: str, example: dict, gui, paths: dict[str, str] | None = None) -> None:
    assert name in EXAMPLES, f"{name}: add it to EXAMPLES"
    example = Normalizer(gui, paths)(example)
    path = DIRECTORY / f"{name}.json"
    text = json.dumps(example, indent=2, ensure_ascii=False) + "\n"
    if UPDATE:
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        return
    assert path.is_file(), f"{path} is missing: run the tests with UPDATE_PROTOCOL_EXAMPLES=1"
    assert json.loads(path.read_text(encoding="utf-8")) == example, \
        f"{path} no longer matches what the server sends: if the change is intended, run the tests with " \
        "UPDATE_PROTOCOL_EXAMPLES=1 and update gui/web to the new shape"


def http(description: str, method: str, path: str, body, status: int, response: dict) -> dict:
    request = {"method": method, "path": path}
    if body is not None:
        request["body"] = body
    return {"description": description, "request": request, "status": status, "response": response}


def ws(description: str, client: list[dict], server: list[dict]) -> dict:
    return {"description": description, "client": [{**m, "token": "<token>"} if "token" in m else m for m in client],
            "server": server}


def test_no_example_is_left_over() -> None:
    present = {p.stem for p in DIRECTORY.glob("*.json")}
    assert present <= EXAMPLES, f"files that no test writes (remove them): {sorted(present - EXAMPLES)}"
    assert not [p.name for p in DIRECTORY.iterdir() if p.suffix != ".json"]


# ---------------------------------------------------------------- HTTP


def test_http_examples(tmp_path: Path) -> None:
    # Slow steps, so that the summary read right after the first step is always the same.
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_TICK_MS": "600"}) as gui:
        async def talk(c: Client) -> None:
            for name, path, text in (("app", "api/app", "The server, the launcher it found, and the limits."),
                                     ("configs", "api/configs", "The configuration files under the root."),
                                     ("session-none", "api/session", "No run since the server started.")):
                status, body = await c.get(path)
                check_example(name, http(text, "GET", path, None, status, body), gui)
            for name, req, text in (
                    ("check-passed", {"file": "simulation.yaml", "disable": []}, "A check that passed."),
                    ("check-disabled", {"file": "simulation.yaml", "disable": ["Dashboard"]},
                     "A check with one participant left out."),
                    ("check-failed", {"file": "check-error.yaml", "disable": []},
                     "A check that found an error (still status 200: the check ran)."),
            ):
                status, body = await c.post("api/check", req)
                check_example(name, http(text, "POST", "api/check", req, status, body), gui)
            for code, path, req, kw, text in (
                    ("unauthorized", "api/check", {"file": "simulation.yaml"}, {"token": "wrong"},
                     "No token, or a wrong one."),
                    ("forbidden-host", "api/check", {"file": "simulation.yaml"}, {"host": "localhost:8765"},
                     "A Host that is not 127.0.0.1:<port> (DNS rebinding, or localhost)."),
                    ("forbidden-origin", "api/run", {"file": "simulation.yaml"}, {"origin": "http://evil.example"},
                     "A request from another page."),
                    ("outside-root", "api/check", {"file": "../secret.yaml"}, {}, "A path that leaves the root."),
                    ("invalid-path", "api/check", {"file": "sub\\simulation.yaml"}, {},
                     "A path that is not root-relative with /."),
                    ("not-a-configuration", "api/check", {"file": "notes.txt"}, {}, "Not a .yaml or .yml file."),
                    ("file-not-found", "api/check", {"file": "missing.yaml"}, {}, "No such file under the root."),
                    ("unknown-participant", "api/check", {"file": "simulation.yaml", "disable": ["Nobody"]}, {},
                     "A participant name that the check of the file does not have."),
                    ("invalid-field", "api/run", {"file": "simulation.yaml", "speed": 0}, {},
                     "A field of the wrong type or out of range."),
                    ("unsupported", "api/run", {"file": "simulation.yaml", "start_paused": True}, {},
                     "start_paused true with a launcher that has no start-paused feature."),
            ):
                status, body = await c.post(path, req, **kw)
                assert body["error"]["code"] == code, body
                check_example(f"error-{code}", http(text, "POST", path, req, status, body), gui)
            status, body = await c.post("api/check", raw=b"{not json")
            check_example("error-invalid-json", http("A body that is not a JSON object.", "POST", "api/check",
                                                     "{not json", status, body), gui)
            status, body = await c.post("api/check", raw=b"file=simulation.yaml",
                                        content_type="application/x-www-form-urlencoded")
            check_example("error-unsupported-media-type", http("A body that is not JSON (a form).", "POST",
                                                               "api/check", "file=simulation.yaml", status, body), gui)
            status, body = await c.post("api/check", raw=b'{"file": "' + b"x" * 70_000 + b'"}')
            check_example("error-too-large", http("A body larger than 64 KiB.", "POST", "api/check",
                                                  "(70 kB)", status, body), gui)
            status, body = await c.get("api/nothing")
            check_example("error-not-found", http("No such API.", "GET", "api/nothing", None, status, body), gui)
            async with c.http.request("OPTIONS", f"{gui.base}/api/run", headers=gui.headers()) as r:
                check_example("error-cors-not-allowed", http("OPTIONS (a CORS preflight).", "OPTIONS", "api/run",
                                                             None, r.status, await r.json()), gui)
            async with c.http.post(f"{gui.base}/index.html", headers={"Origin": gui.origin}) as r:
                check_example("error-method-not-allowed", http("Not GET on a file of the screen.", "POST",
                                                               "index.html", None, r.status, await r.json()), gui)

            sock = await c.ws()
            await sock.until(is_synced)
            req = {"file": "forever.yaml", "disable": ["Logger"], "speed": None, "start_paused": False}
            status, body = await c.post("api/run", req)
            check_example("run", http("Start a run: the summary of the new run.", "POST", "api/run", req, status,
                                      body), gui)
            status, body = await c.post("api/run", req)
            check_example("error-already-running", http("Start while a run is starting or running.", "POST",
                                                        "api/run", req, status, body), gui)
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            status, body = await c.get("api/session")
            check_example("session-running", http("A run that is running (the summary).", "GET", "api/session",
                                                  None, status, body), gui)
            status, body = await c.post("api/stop", {})
            check_example("stop", http("Ask the run to stop in order (force false; again: nothing more).", "POST",
                                       "api/stop", {}, status, body), gui)
            status, body = await c.post("api/stop", {"force": True})
            check_example("stop-force", http("Ask the run to stop without waiting (a second stop to the launcher).",
                                             "POST", "api/stop", {"force": True}, status, body), gui)
            await sock.until(is_ended)
            await sock.close()
            status, body = await c.post("api/stop")
            check_example("stop-ended", http("Stop when the run has ended: the summary, not an error.", "POST",
                                             "api/stop", None, status, body), gui)
        run_async(talk, gui)


def test_errors_of_the_launcher_and_of_load(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_CHECK_DELAY_MS": "2000"}, args=("--check-timeout", "1")) as gui:
        async def talk(c: Client) -> None:
            req = {"file": "simulation.yaml"}
            status, body = await c.post("api/check", req)
            check_example("error-check-timeout", http("A check that did not end in time (--check-timeout).",
                                                      "POST", "api/check", req, status, body), gui)
        run_async(talk, gui)
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_CHECK_DELAY_MS": "300"}) as gui:
        async def talk(c: Client) -> None:
            req = {"file": "check-garbage.yaml"}
            status, body = await c.post("api/check", req)
            check_example("error-launcher-output", http("check --json wrote no check object.", "POST",
                                                        "api/check", req, status, body), gui)
            req = {"file": "simulation.yaml"}
            replies = await asyncio.gather(*(c.post("api/check", req) for _ in range(8)))
            status, body = next((s, b) for s, b in replies if s == 503)
            check_example("error-check-busy", http("Too many checks waiting for the one that runs.", "POST",
                                                   "api/check", req, status, body), gui)
            socks = [await c.ws(auth=False) for _ in range(32)]
            status, body = await c.get("ws")
            check_example("error-too-many-connections", http("A WebSocket over the limit (the handshake's answer).",
                                                             "GET", "ws", None, status, body), gui)
            for s in socks:
                await s.close()
        run_async(talk, gui)


@pytest.mark.skipif(not POSIX, reason="file modes and signals")
def test_errors_of_a_launcher_that_changed(tmp_path: Path) -> None:
    launcher = tmp_path / "vehicle_sil_launch"
    launcher.write_text(f"#!{sys.executable}\n" + FAKE.read_text())
    launcher.chmod(0o755)
    paths = {str(launcher): "/path/to/vehicle_sil_launch"}
    with gui_server(tmp_path, launcher=launcher) as gui:
        async def talk(c: Client) -> None:
            stat = launcher.stat()
            launcher.chmod(0o644)  # not executable any more; same time and size, so not asked again
            os.utime(launcher, ns=(stat.st_atime_ns, stat.st_mtime_ns))
            req = {"file": "simulation.yaml"}
            status, body = await c.post("api/check", req)
            check_example("error-launcher-start", http("The launcher could not be started.", "POST", "api/check",
                                                       req, status, body), gui, paths)
            launcher.write_text(f"#!{sys.executable}\nimport json\n"
                                "print(json.dumps({'vehicle_sil_launch': 1, 'features': ['events']}))\n")
            launcher.chmod(0o755)
            status, body = await c.post("api/check", req)
            check_example("error-launcher-unusable", http("The launcher was replaced by one this GUI cannot use.",
                                                          "POST", "api/check", req, status, body), gui, paths)
        run_async(talk, gui)
    with gui_server(tmp_path, args=("--stop-timeout", "5")) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "ignore-stop.yaml"})
            await sock.until(lambda m: m.get("event", {}).get("event") == "time")
            gui.proc.send_signal(signal.SIGTERM)
            await asyncio.sleep(0.3)
            req = {"file": "simulation.yaml"}
            status, body = await c.post("api/run", req)
            check_example("error-shutting-down", http("A run asked for while the server is ending.", "POST",
                                                      "api/run", req, status, body), gui)
        run_async(talk, gui)


# ---------------------------------------------------------------- WebSocket


SCENARIOS = {
    "pass": ("simulation.yaml", "A run that passes: hello with no session, then everything of the run."),
    "check-error": ("check-error.yaml", "A run whose check fails: nothing is started."),
    "participant-crash": ("participant-crash.yaml", "A required participant fails while running."),
    "launcher-crash": ("launcher-crash.yaml", "The launcher dies without a result: the launcher's exit in the "
                                              "summary, then no-result."),
    "bad-lines": ("bad-lines.yaml", "The launcher writes lines that are not events, an unknown event and a gap "
                                    "in seq: errors from the server, the unknown event passed on."),
    "bad-types": ("bad-types.yaml", "The launcher writes known events whose fields have the wrong types: each is "
                                    "reported (bad-event) and not passed on; the run goes on."),
}


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_ws_scenario_examples(tmp_path: Path, scenario: str) -> None:
    file, description = SCENARIOS[scenario]
    with gui_server(tmp_path) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": file})
            await sock.until(is_ended)
            check_example(f"ws-{scenario}", ws(description, sock.sent, sock.received), gui)
            if scenario == "pass":
                again = await c.ws()
                await again.until(is_synced)
                check_example("ws-replay", ws("A screen that connects after the run: the summary in hello, then "
                                              "the kept log lines, then synced.", again.sent, again.received), gui)
                logs = [m["event"]["seq"] for m in sock.received if m.get("event", {}).get("event") == "log"]
                resumed = await c.ws({"session": 1, "from_seq": logs[-2]})
                await resumed.until(is_synced)
                check_example("ws-resume", ws("A screen that reconnects and says what it already has: the summary, "
                                              "then only the log lines from from_seq on.", resumed.sent,
                                              resumed.received), gui)
                status, body = await c.get("api/session")
                check_example("session-ended", http("The summary after the run ended.", "GET", "api/session",
                                                    None, status, body), gui)
        run_async(talk, gui)


def test_ws_stop_examples(tmp_path: Path) -> None:
    # Slow steps, so that a stop sent right after "running" always lands in the first step.
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_TICK_MS": "600"}) as gui:
        async def talk(c: Client) -> None:
            for scenario, file, stops, text in (
                    ("stop", "forever.yaml", [{}], "Stopping a run in order."),
                    ("force-stop", "ignore-stop.yaml", [{}, {"force": True}],
                     "A launcher that waits after the stop; force stops it without waiting."),
            ):
                sock = await c.ws()
                await sock.until(is_synced)
                await c.post("api/run", {"file": file})
                await sock.until(lambda m: m.get("event", {}).get("phase") == "running")
                for n, body in enumerate(stops):
                    await c.post("api/stop", body)
                    if n + 1 < len(stops):
                        await sock.until(lambda m: m.get("event", {}).get("event") == "time")
                await sock.until(is_ended)
                await sock.close()
                check_example(f"ws-{scenario}", ws(text, sock.sent, sock.received), gui)
        run_async(talk, gui)


@pytest.mark.skipif(not POSIX, reason="kills the process left behind by pid")
def test_ws_orphan_example(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_ORPHAN_S": "30"}) as gui:
        async def talk(c: Client) -> int:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "orphan.yaml"})
            await sock.until(is_ended)
            check_example("ws-orphan", ws("The launcher has exited, but a process it started holds its output: the "
                                          "server stops reading after a short time (output-left-open).",
                                          sock.sent, sock.received), gui)
            line = next(m["event"]["line"] for m in sock.received
                        if "left a process behind" in m.get("event", {}).get("line", ""))
            return int(line.rsplit(" ", 1)[1])
        orphan = run_async(talk, gui)
        try:
            os.kill(orphan, signal.SIGKILL)
        except ProcessLookupError:
            pass


def test_ws_dropped_example(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_FLOOD": "2100"}) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            await c.post("api/run", {"file": "flood.yaml"})
            await sock.until(is_ended)
            await sock.close()
            resumed = await c.ws({"session": 1, "from_seq": 20})
            got = await resumed.until(is_synced)
            hello, dropped = got[0], got[1]
            first = got[2]
            check_example("ws-dropped", ws("A screen that reconnects after more log lines than the server keeps: "
                                           "dropped (lines from from_seq on that are not sent), then the kept "
                                           "lines (here only the first of them, and the end).",
                                           resumed.sent, [hello, dropped, first, got[-1]]), gui)
        run_async(talk, gui)


@pytest.mark.parametrize("kind", ["slow-client", "resync"])
def test_ws_slow_client_examples(tmp_path: Path, kind: str) -> None:
    # A screen that reads nothing while the launcher floods: log lines are dropped for it (dropped); findings
    # cannot be, so its queue is emptied and it is sent everything again (resync).
    file = "flood.yaml" if kind == "slow-client" else "finding-flood.yaml"
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_FLOOD": "100000"}) as gui:
        async def talk(c: Client) -> None:
            slow = await c.ws()
            fast = await c.ws()
            await fast.until(is_synced)
            await c.post("api/run", {"file": file})
            await fast.until(is_ended, timeout=120)
            want = "dropped" if kind == "slow-client" else "resync"
            got = await slow.until(lambda m: m["type"] == want, timeout=120)
            message = got[-1]
            if want == "dropped":
                assert message["reason"] == "slow-client" and message["log"].get("Engine", 0) > 0
                message = {**message, "log": {"Engine": 1000}, "events": 0}
            check_example(f"ws-{kind}", ws("What a screen that reads too slowly gets (one message; the numbers "
                                           "are examples).", [], [message]), gui)
        run_async(talk, gui)


def test_ws_unauthorized_example(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
        async def talk(c: Client) -> None:
            sock = await c.ws({"token": "wrong"})
            sock.sent[0]["token"] = "wrong"
            got = await sock.until(lambda m: True)
            check_example("ws-unauthorized", {"description": "A wrong token: an error, then the server closes "
                                                             "the connection with code 4401.",
                                              "client": sock.sent, "server": got}, gui)
        run_async(talk, gui)
