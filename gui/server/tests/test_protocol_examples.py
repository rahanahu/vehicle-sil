"""gui/protocol/examples: real conversations with the server and the fake launcher, compared with the files.

The values that change from run to run (process IDs, times, the instance, the paths of this computer) are
replaced with fixed ones before the comparison. UPDATE_PROTOCOL_EXAMPLES=1 writes the files instead.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
from conftest import CONFIGS, FAKE, Client, gui_server, is_ended, is_synced, run_async

EXAMPLES = Path(__file__).resolve().parents[2] / "protocol" / "examples"
UPDATE = os.environ.get("UPDATE_PROTOCOL_EXAMPLES") == "1"


class Normalizer:
    def __init__(self, gui):
        self.pids: dict[int, int] = {}
        self.root = str(CONFIGS)
        self.launcher = str(FAKE)
        self.token = gui.token

    def __call__(self, value, key=None):
        if isinstance(value, dict):
            out = {k: self(v, k) for k, v in value.items()}
            if isinstance(out.get("seq"), int) and "t_ms" in out:
                out["t_ms"] = out["seq"] * 10
            return out
        if isinstance(value, list):
            return [self(v) for v in value]
        if key in ("pid", "launcher_pid") and isinstance(value, int):
            return self.pids.setdefault(value, 10001 + len(self.pids))
        if key == "instance" and isinstance(value, str):
            return "0123456789abcdef"
        if key in ("started_at", "ended_at") and isinstance(value, str):
            return "2026-01-01T00:00:00.000Z" if key == "started_at" else "2026-01-01T00:00:01.000Z"
        if key == "modified" and isinstance(value, str):
            return "2026-01-01T00:00:00Z"
        if isinstance(value, str):
            value = value.replace(self.launcher, "/path/to/vehicle_sil_launch").replace(self.root, "/path/to/root")
            assert self.token not in value, "the token must not appear in any response"
        return value


def check_example(name: str, example: dict, gui) -> None:
    example = Normalizer(gui)(example)
    path = EXAMPLES / f"{name}.json"
    text = json.dumps(example, indent=2, ensure_ascii=False) + "\n"
    if UPDATE:
        EXAMPLES.mkdir(parents=True, exist_ok=True)
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


def test_http_examples(tmp_path: Path) -> None:
    # Slow steps, so that the summary read right after the first step is always the same.
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_TICK_MS": "600"}) as gui:
        async def talk(c: Client) -> None:
            status, body = await c.get("api/app")
            check_example("app", http("The server, the launcher it found, and the limits.", "GET", "api/app",
                                      None, status, body), gui)
            status, body = await c.get("api/configs")
            check_example("configs", http("The configuration files under the root.", "GET", "api/configs", None,
                                          status, body), gui)
            status, body = await c.get("api/session")
            check_example("session-none", http("No run since the server started.", "GET", "api/session", None,
                                               status, body), gui)
            for name, req, text in (
                    ("check-passed", {"file": "simulation.yaml", "disable": []}, "A check that passed."),
                    ("check-disabled", {"file": "simulation.yaml", "disable": ["Dashboard"]},
                     "A check with one participant left out."),
                    ("check-failed", {"file": "check-error.yaml", "disable": []},
                     "A check that found an error (still status 200: the check ran)."),
            ):
                status, body = await c.post("api/check", req)
                check_example(name, http(text, "POST", "api/check", req, status, body), gui)
            for name, path, req, kw, text in (
                    ("error-unauthorized", "api/check", {"file": "simulation.yaml"}, {"token": "wrong"},
                     "No token, or a wrong one."),
                    ("error-outside-root", "api/check", {"file": "../secret.yaml"}, {},
                     "A path that leaves the root."),
                    ("error-unknown-participant", "api/check", {"file": "simulation.yaml", "disable": ["Nobody"]},
                     {}, "A participant name that the check of the file does not have."),
                    ("error-forbidden-origin", "api/run", {"file": "simulation.yaml"},
                     {"origin": "http://evil.example"}, "A request from another page."),
            ):
                status, body = await c.post(path, req, **kw)
                check_example(name, http(text, "POST", path, req, status, body), gui)

            sock = await c.ws()
            await sock.until(is_synced)
            req = {"file": "forever.yaml", "disable": ["Logger"], "speed": None, "start_paused": False}
            status, body = await c.post("api/run", req)
            check_example("run", http("Start a run.", "POST", "api/run", req, status, body), gui)
            status, body = await c.post("api/run", req)
            check_example("error-session-running", http("Start while a run is running.", "POST", "api/run", req,
                                                        status, body), gui)
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            status, body = await c.get("api/session")
            check_example("session-running", http("A run that is running (the summary).", "GET", "api/session",
                                                  None, status, body), gui)
            status, body = await c.post("api/stop")
            check_example("stop", http("Ask the run to stop (a second stop stops without waiting).", "POST",
                                       "api/stop", {}, status, body), gui)
            await sock.until(is_ended)
            await sock.close()
            status, body = await c.post("api/stop")
            check_example("error-not-running", http("Stop when nothing runs.", "POST", "api/stop", {}, status,
                                                    body), gui)
        run_async(talk, gui)


SCENARIOS = {
    "pass": ("simulation.yaml", "A run that passes: hello with no session, then everything of the run."),
    "check-error": ("check-error.yaml", "A run whose check fails: nothing is started."),
    "participant-crash": ("participant-crash.yaml", "A required participant fails while running."),
    "launcher-crash": ("launcher-crash.yaml", "The launcher dies without a result: the server reports it."),
    "bad-lines": ("bad-lines.yaml", "The launcher writes lines that are not events, an unknown event and a gap "
                                    "in seq: errors from the server, the unknown event passed on."),
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
                                              "the kept events (all of them here), then synced.",
                                              again.sent, again.received), gui)
                last = sock.received[-2]["event"]["seq"]
                resumed = await c.ws({"session": 1, "from_seq": last - 1})
                await resumed.until(is_synced)
                check_example("ws-resume", ws("A screen that reconnects and says what it already has: only the "
                                              "events from from_seq on.", resumed.sent, resumed.received), gui)
                status, body = await c.get("api/session")
                check_example("session-ended", http("The summary after the run ended.", "GET", "api/session",
                                                    None, status, body), gui)
        run_async(talk, gui)


def test_ws_stop_examples(tmp_path: Path) -> None:
    # Slow steps, so that a stop sent right after "running" always lands in the first step.
    with gui_server(tmp_path, env={"FAKE_LAUNCHER_TICK_MS": "600"}) as gui:
        async def talk(c: Client) -> None:
            for scenario, file, stops, text in (
                    ("stop", "forever.yaml", 1, "Stopping a run that runs until stopped."),
                    ("ignore-stop", "ignore-stop.yaml", 2,
                     "A launcher that does not stop on the first stop; the second stops it without waiting."),
            ):
                sock = await c.ws()
                await sock.until(is_synced)
                await c.post("api/run", {"file": file})
                await sock.until(lambda m: m.get("event", {}).get("phase") == "running")
                for n in range(stops):
                    await c.post("api/stop")
                    if n + 1 < stops:
                        await sock.until(lambda m: m.get("event", {}).get("event") == "time")
                await sock.until(is_ended)
                await sock.close()
                check_example(f"ws-{scenario}", ws(text, sock.sent, sock.received), gui)
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
