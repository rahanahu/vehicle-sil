"""Deterministic tests for ServeRuntime, format 3 stream multiplexing, Hot/Cold auto-switch, and recovery."""

from __future__ import annotations

import asyncio
from collections.abc import Sequence
from contextlib import asynccontextmanager
import json
import os
from pathlib import Path
import sys
import textwrap

import aiohttp
from aiohttp.test_utils import TestServer
import pytest

from conftest import Client, Gui, is_ended
from fake_runtime import DEFAULT_FEATURES, FakeLauncher, default_check_result
from vehicle_sil_gui import __version__, control
from vehicle_sil_gui.runtime import ServeRuntime
from vehicle_sil_gui.server import GuiServer, Options
from vehicle_sil_gui.source import ListSample, PeerInfo, SnapshotInfo, SourceError, TimeInfo


FAKE_SERVE_SCRIPT = textwrap.dedent("""\
    import hashlib
    import json
    import os
    from pathlib import Path
    import sys

    def emit(obj):
        sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\\n")
        sys.stdout.flush()

    mode_file = Path(os.environ["FAKE_SERVE_LOG"])
    mode = os.environ.get("FAKE_SERVE_BEHAVIOR", "normal")
    argv = sys.argv[1:]

    def log_event(entry):
        with mode_file.open("a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\\n")

    def inspect_cfg(cfg_path, disable=(), enable=(), manual=()):
        p = Path(cfg_path)
        try:
            raw = p.read_bytes()
            text = raw.decode("utf-8")
        except (OSError, UnicodeDecodeError):
            return 3, {
                "vehicle_sil_check": 1, "vehicle_sil": "__VEHICLE_SIL_VERSION__",
                "file": cfg_path, "result": "unreadable", "strict": False,
                "errors": 1, "warnings": 0, "notes": 0,
                "participants": [], "networks": [], "findings": [],
                "file_sha256": None,
                "hot_eligibility": {"eligible": False, "reason": "config-unreadable", "message": "configuration could not be read"},
            }
        sha = hashlib.sha256(raw).hexdigest()
        known = []
        for line in text.splitlines():
            s = line.strip()
            if s.startswith("- name:"):
                known.append(s.split(":", 1)[1].strip())
        if not known and "Engine" in text:
            known = ["Engine"]
        unknown = [n for n in (*disable, *manual) if n not in known]
        conflict = set(disable) & set(manual)
        if unknown or conflict:
            return 4, {
                "vehicle_sil_check": 1, "vehicle_sil": "__VEHICLE_SIL_VERSION__",
                "file": cfg_path, "result": "usage", "strict": False,
                "errors": 1, "warnings": 0, "notes": 0,
                "participants": [], "networks": [],
                "findings": [{"severity": "error", "code": "usage", "message": "invalid overrides"}],
                "file_sha256": sha,
                "hot_eligibility": {"eligible": False, "reason": "usage", "message": "invalid overrides"},
            }
        participants = []
        for name in known:
            enabled = name not in disable or name in enable
            is_manual = ("manual: true" in text) or (name in manual)
            participants.append({"name": name, "enabled": enabled, "manual": is_manual, "variables": []})
        if "registry_uri:" in text:
            elig = {"eligible": False, "reason": "fixed-registry", "message": "configuration uses a fixed registry_uri"}
        elif any(p["enabled"] and p["manual"] for p in participants):
            elig = {"eligible": False, "reason": "manual-participants", "message": "configuration waits for manual participants"}
        elif os.environ.get("VEHICLE_SIL_TRACE"):
            elig = {"eligible": False, "reason": "trace-enabled", "message": "serve does not run with VEHICLE_SIL_TRACE set"}
        else:
            elig = {"eligible": True, "reason": None, "message": None}
        return 0, {
            "vehicle_sil_check": 1, "vehicle_sil": "__VEHICLE_SIL_VERSION__",
            "file": cfg_path, "result": "passed", "strict": False,
            "errors": 0, "warnings": 0, "notes": 0,
            "participants": participants, "networks": [], "findings": [],
            "file_sha256": sha,
            "hot_eligibility": elig,
        }

    if argv[:1] == ["check"] and "--file-only" in argv and "--json" in argv:
        cfg = argv[-1]
        dis, ena, man = [], [], []
        i = 0
        while i < len(argv):
            if argv[i] == "--disable" and i + 1 < len(argv):
                dis.append(argv[i + 1]); i += 2
            elif argv[i] == "--enable" and i + 1 < len(argv):
                ena.append(argv[i + 1]); i += 2
            elif argv[i] == "--manual" and i + 1 < len(argv):
                man.append(argv[i + 1]); i += 2
            else:
                i += 1
        log_event({"cmd": "check-file-only", "config": cfg, "argv": argv})
        code, doc = inspect_cfg(cfg, dis, ena, man)
        emit(doc)
        sys.exit(code)

    cred_path = None
    if "--control-credentials" in argv:
        cred_path = Path(argv[argv.index("--control-credentials") + 1])
    cfg = argv[-1]

    if mode == "startup-refuse":
        emit({
            "event": "hello", "seq": 0, "t_ms": 0, "vehicle_sil_events": 3,
            "vehicle_sil": "__VEHICLE_SIL_VERSION__", "pid": os.getpid(), "file": cfg,
            "arguments": argv, "launcher": "feed000000000001",
        })
        emit({
            "event": "result", "seq": 1, "t_ms": 1, "launcher_ends": True,
            "result": {"result": "failed", "findings": [{"severity": "error", "message": "serve refused startup"}]},
        })
        sys.exit(4)

    seq = 0
    t_ms = 10
    emit({
        "event": "hello", "seq": seq, "t_ms": 0, "vehicle_sil_events": 3,
        "vehicle_sil": "__VEHICLE_SIL_VERSION__", "pid": os.getpid(), "file": cfg,
        "arguments": argv, "launcher": "feed000000000001",
    })
    seq += 1
    emit({"event": "idle", "seq": seq, "t_ms": t_ms})
    seq += 1
    run_counter = 0
    current_run = None
    last_run = None
    status_rev = 0
    launcher_state = "idle"
    elig_state = {"eligible": True, "reason": None, "message": None}
    stop_requests = 0

    def current_status_payload():
        busy = launcher_state in ("preparing", "running")
        prep_allowed = {"ok": not busy, "reason": "busy" if busy else None, "message": "busy" if busy else None}
        if busy:
            run_allowed = {"ok": False, "reason": "busy", "message": "busy"}
        elif not elig_state.get("eligible", True):
            run_allowed = {"ok": False, "reason": elig_state.get("reason"), "message": elig_state.get("message")}
        else:
            run_allowed = {"ok": True, "reason": None, "message": None}
        run_state = None
        if current_run is not None:
            run_state = {
                "rev": 1, "phase": "running", "paused": False, "paused_at_ms": None,
                "holding": False, "speed": 1.0, "pacing": "realtime",
                "stop": {"requests": stop_requests, "by": "stdin" if stop_requests else None},
                "scenario": None, "allowed": {}, "outside": {},
            }
        return {
            "instance": "feed000000000001",
            "status_rev": status_rev,
            "launcher_state": launcher_state,
            "config": cfg,
            "eligibility": elig_state,
            "launcher_allowed": {"prepare": prep_allowed, "run": run_allowed},
            "active_run_id": current_run,
            "last_run_id": last_run,
            "run_state": run_state,
        }

    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        cmd = json.loads(line)
        name = cmd.get("command")
        log_event({"pid": os.getpid(), "config": cfg, "cmd": cmd, "cred_exists": cred_path.exists() if cred_path else None})

        if name == "status":
            emit({"event": "status", "seq": seq, "t_ms": t_ms, "id": cmd.get("id"), **current_status_payload()})
            seq += 1
        elif name == "prepare":
            prior_prepares = 0
            if mode_file.exists():
                prior_prepares = sum(
                    1 for ln in mode_file.read_text(encoding="utf-8").splitlines()
                    if '"command": "prepare"' in ln or '"command":"prepare"' in ln
                )
            if mode in ("crash-first-prepare", "crash-second-prepare") and prior_prepares == (1 if mode == "crash-first-prepare" else 2):
                os._exit(9)
            if mode in ("refuse-first-prepare", "refuse-second-prepare") and prior_prepares == (1 if mode == "refuse-first-prepare" else 2):
                emit({
                    "event": "prepare", "seq": seq, "t_ms": t_ms, "id": cmd.get("id"),
                    "accepted": False, "reason": "busy", "message": "cannot prepare right now",
                    "status": current_status_payload(),
                })
                seq += 1
                continue
            if mode == "hang-second-prepare" and prior_prepares == 2:
                status_rev += 1
                launcher_state = "preparing"
                emit({"event": "status", "seq": seq, "t_ms": t_ms, "id": None, **current_status_payload()})
                seq += 1
                import time as _time
                _time.sleep(5.0)
                continue
            _, doc = inspect_cfg(cfg, cmd.get("disable", ()), cmd.get("enable", ()), cmd.get("manual", ()))
            elig_state = doc["hot_eligibility"]
            status_rev += 1
            launcher_state = "preparing"
            emit({
                "event": "status", "seq": seq, "t_ms": t_ms,
                "instance": "feed000000000001", "status_rev": status_rev,
                "launcher_state": "ready" if elig_state["eligible"] else "idle",
                "config": cfg, "eligibility": elig_state,
                "launcher_allowed": {}, "active_run_id": None, "last_run_id": last_run, "run_state": None,
            })
            seq += 1
            status_rev += 1
            launcher_state = "ready" if elig_state["eligible"] else "idle"
            emit({
                "event": "prepare", "seq": seq, "t_ms": t_ms, "id": cmd.get("id"),
                "accepted": True, "reason": None, "message": None,
                "status": current_status_payload(),
                "metadata": {
                    "participants": [p["name"] for p in doc["participants"]],
                    "networks": [], "variants": [], "file_sha256": doc["file_sha256"],
                },
                "findings": doc["findings"],
            })
            seq += 1
        elif name == "run":
            run_counter += 1
            run_id = f"feed-{run_counter}"
            current_run = run_id
            stop_requests = 0
            status_rev += 1
            launcher_state = "running"
            if mode == "hang-run-start":
                import time as _time
                _time.sleep(5.0)
                continue
            t_ms += 100
            emit({"event": "run_start", "seq": seq, "t_ms": t_ms, "run": run_id, "id": cmd.get("id")})
            seq += 1
            t_ms += 5
            emit({"event": "phase", "seq": seq, "t_ms": t_ms, "run": run_id, "phase": "checking"})
            seq += 1
            if mode == "check-hang" and run_counter == 1:
                import time as _time
                _time.sleep(5.0)
                continue
            if mode in ("check-fail-first", "check-fail-request") and run_counter == 1:
                req_file = cmd.get("request", cfg) if mode == "check-fail-request" else cfg
                emit({"event": "check", "seq": seq, "t_ms": t_ms, "run": run_id, "check": {
                    "vehicle_sil_check": 1, "result": "failed", "errors": 1, "warnings": 0, "notes": 0,
                    "participants": [], "networks": [],
                    "findings": [{"severity": "error", "code": "step-mismatch", "file": req_file, "line": 1,
                                  "participant": "Engine", "network": None,
                                  "message": f"{req_file}, line 1: step mismatch"}],
                }})
                seq += 1
                emit({"event": "phase", "seq": seq, "t_ms": t_ms + 1, "run": run_id, "phase": "ended"})
                seq += 1
                emit({"event": "result", "seq": seq, "t_ms": t_ms + 2, "run": run_id, "launcher_ends": False,
                      "result": {"result": "check_failed", "errors": 1, "warnings": 0, "notes": 0, "findings": []}})
                seq += 1
                last_run = current_run
                current_run = None
                status_rev += 1
                launcher_state = "ready" if elig_state["eligible"] else "idle"
                emit({"event": "idle", "seq": seq, "t_ms": t_ms + 3})
                seq += 1
                continue
            emit({"event": "check", "seq": seq, "t_ms": t_ms, "run": run_id, "check": {
                "vehicle_sil_check": 1, "result": "passed", "errors": 0, "warnings": 0, "notes": 0,
                "participants": [{"name": "Engine", "enabled": True, "manual": False, "variables": []}],
                "networks": [], "findings": [],
            }})
            seq += 1
            t_ms += 10
            emit({"event": "phase", "seq": seq, "t_ms": t_ms, "run": run_id, "phase": "running"})
            seq += 1
            if mode == "finding-with-request":
                req_path = cmd.get("request", "")
                emit({"event": "finding", "seq": seq, "t_ms": t_ms, "run": run_id,
                      "severity": "warning", "code": "note", "file": req_path, "line": 2,
                      "message": f"{req_path}, line 2: live warning"})
                seq += 1
            if cred_path is not None and mode != "no-control":
                if not cred_path.exists():
                    emit({"event": "finding", "seq": seq, "t_ms": t_ms, "run": run_id,
                          "severity": "error", "message": "resident credentials file missing on disk"})
                    seq += 1
                else:
                    emit({"event": "control_server", "seq": seq, "t_ms": t_ms, "run": run_id,
                          "address": f"127.0.0.1:{40000 + run_counter}", "tls": False})
                    seq += 1
            emit({"event": "time", "seq": seq, "t_ms": t_ms + 20, "run": run_id, "sim_ms": 0})
            seq += 1
            if mode == "crash-on-second" and run_counter == 2:
                os._exit(9)
            if mode == "crash-on-first" and run_counter == 1:
                os._exit(9)
            if mode == "contaminated-first" and run_counter == 1:
                emit({"event": "result", "seq": seq, "t_ms": t_ms + 30, "run": run_id, "launcher_ends": True,
                      "result": {"result": "failed", "errors": 1, "warnings": 0, "notes": 0, "findings": []}})
                sys.exit(6)
            if mode == "fail-once" and run_counter == 1:
                emit({"event": "result", "seq": seq, "t_ms": t_ms + 30, "run": run_id, "launcher_ends": False,
                      "result": {"result": "failed", "errors": 1, "warnings": 0, "notes": 0, "findings": []}})
                seq += 1
                last_run = current_run
                current_run = None
                status_rev += 1
                launcher_state = "ready" if elig_state["eligible"] else "idle"
                emit({"event": "idle", "seq": seq, "t_ms": t_ms + 35})
                seq += 1
            elif mode == "heartbeat-until-stop":
                import time as _time
                emit({"event": "heartbeat", "seq": seq, "t_ms": t_ms + 50, "run": run_id})
                seq += 1
                _time.sleep(0.15)
                emit({"event": "heartbeat", "seq": seq, "t_ms": t_ms + 200, "run": run_id})
                seq += 1
            elif mode not in ("wait-stop", "stop-ends", "natural-pass", "natural-ends"):
                emit({"event": "result", "seq": seq, "t_ms": t_ms + 30, "run": run_id, "launcher_ends": False,
                      "result": {"result": "passed", "errors": 0, "warnings": 0, "notes": 0, "findings": []}})
                seq += 1
                last_run = current_run
                current_run = None
                status_rev += 1
                launcher_state = "ready" if elig_state["eligible"] else "idle"
                emit({"event": "idle", "seq": seq, "t_ms": t_ms + 35})
                seq += 1
        elif name in ("stop", "pause", "resume", "step", "run_until", "speed"):
            asked_run = cmd.get("run")
            if current_run is None or asked_run != current_run:
                emit({
                    "event": "command", "seq": seq, "t_ms": t_ms + 1, "command": name,
                    "accepted": False, "reason": "run-changed", "asked_run": asked_run,
                    "message": "not current run",
                })
                seq += 1
            elif name == "stop":
                f = cmd.get("force")
                if f is True:
                    stop_requests = max(stop_requests, 2)
                elif f is False:
                    stop_requests = max(stop_requests, 1)
                else:
                    stop_requests = 1 if stop_requests < 1 else 2
                t_ms += 40
                emit({"event": "command", "seq": seq, "t_ms": t_ms, "run": current_run,
                      "command": "stop", "accepted": True, "ms": 0})
                seq += 1
                ends_now = mode == "stop-ends"
                emit({"event": "result", "seq": seq, "t_ms": t_ms + 5, "run": current_run,
                      "launcher_ends": ends_now, "result": {"result": "stopped", "errors": 0, "warnings": 0, "notes": 0, "findings": []}})
                seq += 1
                last_run = current_run
                current_run = None
                status_rev += 1
                if ends_now:
                    launcher_state = "ending"
                    sys.exit(0)
                launcher_state = "ready" if elig_state["eligible"] else "idle"
                emit({"event": "idle", "seq": seq, "t_ms": t_ms + 10})
                seq += 1
            elif name == "resume" and mode in ("natural-pass", "natural-ends"):
                ends_now = mode == "natural-ends"
                t_ms += 40
                emit({"event": "command", "seq": seq, "t_ms": t_ms, "run": current_run,
                      "command": "resume", "accepted": True, "ms": 0})
                seq += 1
                emit({"event": "result", "seq": seq, "t_ms": t_ms + 5, "run": current_run,
                      "launcher_ends": ends_now,
                      "result": {"result": "interrupted" if ends_now else "passed",
                                 "errors": 0, "warnings": 0, "notes": 0, "findings": []}})
                seq += 1
                last_run = current_run
                current_run = None
                status_rev += 1
                if ends_now:
                    launcher_state = "ending"
                    sys.exit(0)
                launcher_state = "ready" if elig_state["eligible"] else "idle"
                emit({"event": "idle", "seq": seq, "t_ms": t_ms + 10})
                seq += 1
""").replace('"__VEHICLE_SIL_VERSION__"', json.dumps(__version__))


class _FakeControlSource:
    created: list[_FakeControlSource] = []

    def __init__(self, address: str, token: str):
        self.address = address
        self.token = token
        self.closed = False
        self.close_events: int = 0
        self._sink = None
        self.created.append(self)

    def start(self, sink) -> None:
        self._sink = sink
        sink.on_peers(
            [PeerInfo(
                name="Engine",
                incarnation="inc-1",
                connected=True,
                variables=[{"name": "rpm", "type": "real", "role": "input"}],
            )],
            [],
        )
        sink.on_samples({"Engine": ListSample("inc-1", 0, 1, [0.0])})
        sink.on_time(TimeInfo(now_ns=0, paused=False, running=True, ended=False))
        sink.on_synced()

    async def close(self) -> None:
        self.closed = True
        self.close_events += 1

    async def read_peers(self, timeout_s: float) -> list[PeerInfo]:
        return []

    async def read_snapshot(self, peers: list[str], timeout_s: float) -> SnapshotInfo:
        return SnapshotInfo(
            time=TimeInfo(now_ns=0, paused=False, running=True, ended=False),
            samples={},
            consistent=True,
        )

    async def read_operation(self, seq: int, timeout_s: float):
        return None

    async def write(self, writes, at_ns: int | None, late: str, client_name: str, idempotency: str, timeout_s: float) -> int:
        raise SourceError("refused", "not needed")


def _write_configs(root: Path, launcher_script: Path) -> None:
    (root / "a.yaml").write_text(
        "vehicle_sil_simulation: 1\nparticipants:\n  - name: Engine\n    command: [echo, ok]\n",
        encoding="utf-8",
    )
    (root / "b.yaml").write_text(
        "vehicle_sil_simulation: 1\nparticipants:\n  - name: Engine\n    command: [echo, ok]\n",
        encoding="utf-8",
    )
    (root / "fixed.yaml").write_text(
        "vehicle_sil_simulation: 1\nregistry_uri: silkit://127.0.0.1:8500\nparticipants:\n  - name: Engine\n    command: [echo, ok]\n",
        encoding="utf-8",
    )
    (root / "manual.yaml").write_text(
        "vehicle_sil_simulation: 1\nparticipants:\n  - name: Engine\n    command: [echo, ok]\n    manual: true\n",
        encoding="utf-8",
    )
    launcher_script.write_text(FAKE_SERVE_SCRIPT, encoding="utf-8")


@asynccontextmanager
async def _serve_gui(
    tmp_path: Path,
    *,
    features: Sequence[str] = (*DEFAULT_FEATURES, "serve", "check-file-only", "serve-prepare"),
    variables: bool = True,
    silent_after_s: float = 5.0,
    stop_timeout_s: float = 1.0,
    env: dict[str, str] | None = None,
):
    root = tmp_path / "cfg"
    root.mkdir(exist_ok=True)
    script = tmp_path / "fake_serve.py"
    log_path = tmp_path / "serve_calls.jsonl"
    _write_configs(root, script)

    prev = {}
    updates = {"FAKE_SERVE_LOG": str(log_path), **(env or {})}
    for k, v in updates.items():
        prev[k] = os.environ.get(k)
        os.environ[k] = v

    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]

    launcher = FakeLauncher(features=features, path=script)
    _FakeControlSource.created.clear()
    server = GuiServer(Options(
        root=root,
        launcher=launcher,
        port=port,
        token="test-token",
        stop_timeout_s=stop_timeout_s,
        silent_after_s=silent_after_s,
        variables=variables,
        source=(lambda addr, tok: _FakeControlSource(addr, tok)) if variables else None,
    ))

    async def fake_check(argv: list[str], request: Path, descriptions: Path | None = None,
                         text: bytes | None = None) -> tuple[int, dict]:
        res = default_check_result(argv[-1])
        if "--compare-commands" in argv:
            res["commands"] = "same"
        return 0, res

    server._launcher_check = fake_check  # type: ignore[method-assign]
    test_server = TestServer(server.app(), host="127.0.0.1", port=port)
    await test_server.start_server()
    http = aiohttp.ClientSession()
    gui = Gui(proc=None, port=port, token="test-token", url=f"http://127.0.0.1:{port}/#token=test-token",
              stderr=tmp_path / ".err")
    try:
        yield server, Client(gui, http), log_path
    finally:
        await http.close()
        await server.shutdown()
        await test_server.close()
        for k, old in prev.items():
            if old is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = old


def test_serve_hot_scenario_aba_isolation_and_per_run_cleanup(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path) as (server, c, log_path):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            # Run A (#1)
            st, body = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200, body
            s1 = body["session"]
            msgs = await sock.until(is_ended)
            end1 = msgs[-1]["session"]
            assert end1["state"] == "ended"
            assert end1["result"]["result"] == "passed"
            assert end1["launcher"]["mode"] == "hot"
            assert end1["launcher"]["running"] is False
            assert end1["launcher"]["process_alive"] is True
            assert rt.resident_starts == 1
            pid1 = rt.resident_pid
            assert pid1 is not None
            # Credentials remain on disk while resident is alive; control source closed on result
            assert len(_FakeControlSource.created) == 1
            assert _FakeControlSource.created[0].closed is True

            # Run A (#2) reuses the exact same resident PID, seq restarts at 0, t_ms rebased
            st, body = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            msgs2 = await sock.until(is_ended)
            assert rt.resident_starts == 1
            assert rt.resident_pid == pid1
            hellos2 = [m["event"] for m in msgs2 if m["type"] == "event" and m["event"]["event"] == "hello"]
            assert hellos2 and hellos2[0]["seq"] == 0 and hellos2[0]["t_ms"] == 0
            assert len(_FakeControlSource.created) == 2
            assert _FakeControlSource.created[0].token == _FakeControlSource.created[1].token
            assert _FakeControlSource.created[1].closed is True

            # Switch to B -> restarts resident cleanly
            st, body = await c.post("api/run", {"file": "b.yaml"})
            assert st == 200
            await sock.until(is_ended)
            assert rt.resident_starts == 2
            pid_b = rt.resident_pid
            assert pid_b is not None and pid_b != pid1

            # Switch back to A -> starts fresh resident for A
            st, body = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            await sock.until(is_ended)
            assert rt.resident_starts == 3
            assert rt.resident_pid != pid_b
            entries = [json.loads(line) for line in log_path.read_text(encoding="utf-8").splitlines()]
            assert all(e["cred_exists"] is True for e in entries if isinstance(e.get("cmd"), dict) and e["cmd"].get("command") == "run")
            assert s1["id"] != body["session"]["id"]
            await sock.close()

    asyncio.run(run())


def test_serve_stop_stops_run_without_killing_resident_and_drops_stale_refusal(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, env={"FAKE_SERVE_BEHAVIOR": "wait-stop"}) as (server, c, _):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            pid1 = rt.resident_pid
            assert pid1 is not None

            # Inject a stale launcher-level command refusal targeting a previous run id -> must not reach Run 1
            assert rt._resident is not None
            rt._handle_stdout_line(
                rt._resident,
                json.dumps({
                    "event": "command", "seq": 999, "t_ms": 999, "command": "stop",
                    "accepted": False, "reason": "run-changed", "asked_run": "feed-0", "message": "stale",
                }),
            )

            st, stop_body = await c.post("api/stop", {"force": False})
            assert st == 200
            msgs = await sock.until(is_ended)
            end_sess = msgs[-1]["session"]
            assert end_sess["result"]["result"] == "stopped"
            assert end_sess["last_command"] is not None and end_sess["last_command"]["command"] == "stop"
            assert end_sess["last_command"]["accepted"] is True
            # Resident process survived Stop!
            assert rt.resident_pid == pid1
            assert rt.status()["state"] == "ready"

            # Next run starts on the same resident without relaunch
            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            assert rt.resident_starts == 1
            await c.post("api/stop", {"force": False})
            await sock.until(is_ended)
            await sock.close()

    asyncio.run(run())


def test_serve_prepare_ready_and_config_save_releases_resident(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path) as (server, c, _):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            st, body = await c.post("api/prepare", {"file": "a.yaml"})
            assert st == 200 and body["runtime"]["mode"] == "hot" and body["runtime"]["state"] == "ready"
            assert rt.resident_starts == 1
            pid1 = rt.resident_pid
            ready_msgs = await sock.until(
                lambda m: m["type"] == "runtime" and m["runtime"]["state"] == "ready" and m["runtime"]["config"] == "a.yaml"
            )
            assert ready_msgs[-1]["runtime"]["mode"] == "hot"

            # Running prepared config reuses same resident (no second spawn)
            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            await sock.until(is_ended)
            assert rt.resident_starts == 1 and rt.resident_pid == pid1

            # Prepare during active run returns 409 already-running
            os.environ["FAKE_SERVE_BEHAVIOR"] = "wait-stop"
            await c.post("api/prepare", {"file": "b.yaml"})
            st, _ = await c.post("api/run", {"file": "b.yaml"})
            assert st == 200
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            st, err = await c.post("api/prepare", {"file": "a.yaml"})
            assert st == 409 and err["error"]["code"] == "already-running"
            await c.post("api/stop", {"force": False})
            await sock.until(is_ended)

            # Saving edited b.yaml releases idle resident so next run uses fresh resident
            old_pid = rt.resident_pid
            st, session_body = await c.get("api/config?file=b.yaml")
            assert st == 200
            st, save_body = await c.post("api/config", {
                "file": "b.yaml",
                "text": session_body["text"] + "# updated\n",
                "stamp": session_body["stamp"],
            })
            assert st == 200, save_body
            assert rt.resident_pid is None
            os.environ["FAKE_SERVE_BEHAVIOR"] = "normal"
            st, _ = await c.post("api/run", {"file": "b.yaml"})
            assert st == 200
            await sock.until(is_ended)
            assert rt.resident_pid != old_pid
            await sock.close()

    asyncio.run(run())


def test_hot_cold_auto_switch_all_five_reasons(tmp_path: Path) -> None:
    async def run() -> None:
        root = tmp_path / "cfg"
        root.mkdir()
        script = tmp_path / "fake_serve.py"
        log_path = tmp_path / "calls.jsonl"
        _write_configs(root, script)
        prev_log = os.environ.get("FAKE_SERVE_LOG")
        os.environ["FAKE_SERVE_LOG"] = str(log_path)
        try:
            # 1. launcher-unsupported
            no_serve = FakeLauncher(features=DEFAULT_FEATURES, path=script)
            rt_no_serve = ServeRuntime(no_serve, cwd=root)
            assert (await rt_no_serve.inspect_config("a.yaml")).hot.reason == "launcher-unsupported"

            # 2. trace-enabled (pre-launch Cold guard runs before spawning serve or check --file-only)
            with_file_only = FakeLauncher(
                features=(*DEFAULT_FEATURES, "serve", "check-file-only", "serve-prepare"),
                path=script,
            )
            rt_check = ServeRuntime(with_file_only, cwd=root)
            os.environ["VEHICLE_SIL_TRACE"] = "/tmp/trace"
            try:
                assert (await rt_check.inspect_config("a.yaml", need_metadata=False)).hot.reason == "trace-enabled"
                assert not log_path.exists()
            finally:
                os.environ.pop("VEHICLE_SIL_TRACE", None)

            # 3. fixed-registry via check --file-only --json (never spawns serve)
            fixed_prep = await rt_check.prepare("fixed.yaml", want_control=False)
            assert fixed_prep["mode"] == "cold" and fixed_prep["reason"] == "fixed-registry"
            assert rt_check.resident_starts == 0

            # 4. manual-participants (YAML via check --file-only, request override pre-launch, disable override)
            assert (await rt_check.inspect_config("manual.yaml")).hot.reason == "manual-participants"
            assert (await rt_check.inspect_config("a.yaml", manual=("Engine",), need_metadata=False)).hot.reason == "manual-participants"
            assert (await rt_check.inspect_config("manual.yaml", disable=("Engine",))).hot.hot is True
            assert (await rt_check.inspect_config("missing.yaml")).hot.reason == "config-unreadable"
            assert (await rt_check.inspect_config("a.yaml", disable=("Nobody",))).hot.reason == "usage"
        finally:
            if prev_log is None:
                os.environ.pop("FAKE_SERVE_LOG", None)
            else:
                os.environ["FAKE_SERVE_LOG"] = prev_log

        # 5. resident-failed falls back to Cold via OneShotRuntime on prepare
        async with _serve_gui(tmp_path, env={"FAKE_SERVE_BEHAVIOR": "startup-refuse"}) as (server, c, _):
            st, body = await c.post("api/prepare", {"file": "a.yaml"})
            assert st == 200
            assert body["runtime"]["mode"] == "cold"
            assert body["runtime"]["reason"] == "resident-failed"
            st, body = await c.post("api/prepare", {"file": "fixed.yaml"})
            assert st == 200
            assert body["runtime"]["mode"] == "cold" and body["runtime"]["reason"] == "fixed-registry"

    asyncio.run(run())


@pytest.mark.parametrize("behavior,expected_result", [
    ("fail-once", "failed"),
    ("contaminated-first", "failed"),
    ("crash-on-first", None),
])
def test_serve_recovery_after_failure_contamination_and_crash_without_retry(
    tmp_path: Path, behavior: str, expected_result: str | None,
) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": behavior}) as (server, c, _):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            st, body = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            first_id = body["session"]["id"]
            msgs1 = await sock.until(is_ended)
            end1 = msgs1[-1]["session"]
            assert end1["id"] == first_id
            if expected_result is None:
                assert end1["result"] is None
                assert any(p["code"] == "no-result" for p in end1["problems"])
                assert end1["launcher"]["process_alive"] is False
            else:
                assert end1["result"]["result"] == expected_result
                if behavior == "contaminated-first":
                    assert end1["launcher"]["process_alive"] is False
                else:
                    assert end1["launcher"]["process_alive"] is True

            # No automatic retry occurred: only 1 run session exists so far
            st, cur = await c.get("api/session")
            assert st == 200 and cur["session"]["id"] == first_id

            # Next Prepare + Run recovers cleanly (spawning fresh resident if exited)
            os.environ["FAKE_SERVE_BEHAVIOR"] = "normal"
            st, prep = await c.post("api/prepare", {"file": "a.yaml"})
            assert st == 200 and prep["runtime"]["state"] == "ready"
            st, body2 = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200 and body2["session"]["id"] == first_id + 1
            msgs2 = await sock.until(is_ended)
            assert msgs2[-1]["session"]["result"]["result"] == "passed"
            await sock.close()

    asyncio.run(run())


def test_serve_heartbeat_keeps_silence_clear(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(
            tmp_path, variables=False, silent_after_s=0.35, env={"FAKE_SERVE_BEHAVIOR": "heartbeat-until-stop"},
        ) as (server, c, _):
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")
            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            await asyncio.sleep(0.25)
            st, s_mid = await c.get("api/session")
            assert st == 200 and s_mid["session"]["launcher"]["silent_s"] is None
            # Wait past silent_after_s without more lines -> launcher-silent triggers
            silent_msg = await sock.until(lambda m: m["type"] == "error" and m.get("code") == "launcher-silent")
            assert silent_msg
            await c.post("api/stop", {"force": False})
            await sock.until(is_ended)
            await sock.close()

    asyncio.run(run())


def test_grpc_source_run_id_metadata_and_fatal_run_changed() -> None:
    class _FakeStatusCode:
        UNAUTHENTICATED = "UNAUTHENTICATED"
        PERMISSION_DENIED = "PERMISSION_DENIED"
        UNIMPLEMENTED = "UNIMPLEMENTED"
        INVALID_ARGUMENT = "INVALID_ARGUMENT"
        FAILED_PRECONDITION = "FAILED_PRECONDITION"
        UNAVAILABLE = "UNAVAILABLE"
        DEADLINE_EXCEEDED = "DEADLINE_EXCEEDED"
        CANCELLED = "CANCELLED"
        UNKNOWN = "UNKNOWN"
        INTERNAL = "INTERNAL"

    class _FakeRpcError(Exception):
        def __init__(self, code: str, details: str, trailing: tuple[tuple[str, str], ...]):
            super().__init__(details)
            self._code = code
            self._details = details
            self._trailing = trailing

        def code(self):
            return self._code

        def details(self):
            return self._details

        def trailing_metadata(self):
            return self._trailing

    class _FakeGrpc:
        StatusCode = _FakeStatusCode
        RpcError = _FakeRpcError

        def insecure_channel(self, address, options=None):
            return object()

    class _FakeDesc:
        fields_by_name: dict = {}

    class _FakePb:
        class WatchRequest:
            DESCRIPTOR = _FakeDesc()

            def __init__(self, **kwargs):
                pass

    class _FakeStub:
        def __init__(self, channel):
            pass

        def Watch(self, req, metadata=None):
            raise _FakeRpcError("FAILED_PRECONDITION", "stale run", ((control.REASON_METADATA, control.REASON_RUN_CHANGED),))

    class _FakeRpc:
        ControlServiceStub = _FakeStub

    class _FakeLib:
        grpc = _FakeGrpc()
        pb = _FakePb()
        rpc = _FakeRpc()

    src = control.GrpcSource(_FakeLib(), "127.0.0.1:5000", "tok", run_id="feed-1")  # type: ignore[arg-type]
    assert src._metadata == (("authorization", "Bearer tok"), (control.RUN_METADATA, "feed-1"))
    woke = asyncio.Event()
    src._loop = type("_L", (), {"call_soon_threadsafe": lambda self, fn: woke.set()})()  # type: ignore[assignment]
    src._read()
    assert src._lost is not None
    reason, fatal = src._lost
    assert fatal is True and "run-changed" in reason


def test_serve_release_config_during_run_recycles_resident_and_prepare_rejects_starting(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": "wait-stop"}) as (server, c, _):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            # Prepare is refused while _starting is True (simulated via _starting flag during await)
            server._starting = True
            try:
                st, err = await c.post("api/prepare", {"file": "a.yaml"})
                assert st == 409 and err["error"]["code"] == "already-running"
            finally:
                server._starting = False

            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            old_pid = rt.resident_pid
            assert old_pid is not None

            # Saving config during active run marks resident as ending so next run replaces it
            st, cfg = await c.get("api/config?file=a.yaml")
            assert st == 200
            st, saved = await c.post("api/config", {
                "file": "a.yaml",
                "text": cfg["text"] + "# edited mid-run\n",
                "stamp": cfg["stamp"],
            })
            assert st == 200, saved
            assert rt._resident is not None and rt._resident.ending is True

            await c.post("api/stop", {"force": False})
            await sock.until(is_ended)

            os.environ["FAKE_SERVE_BEHAVIOR"] = "normal"
            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            await sock.until(is_ended)
            assert rt.resident_pid != old_pid
            assert rt.resident_starts == 2
            await sock.close()

    asyncio.run(run())


def test_serve_shutdown_during_spawn_leaks_nothing(tmp_path: Path) -> None:
    async def run() -> None:
        root = tmp_path / "cfg"
        root.mkdir()
        script = tmp_path / "fake_serve.py"
        log_path = tmp_path / "calls.jsonl"
        _write_configs(root, script)
        os.environ["FAKE_SERVE_LOG"] = str(log_path)
        launcher = FakeLauncher(features=(*DEFAULT_FEATURES, "serve"), path=script)
        rt = ServeRuntime(launcher, cwd=root)

        import threading
        th_gate = threading.Event()
        entered = threading.Event()

        def blocking_creds():
            entered.set()
            th_gate.wait(timeout=2.0)
            return orig_creds()

        orig_creds = control.credentials_file
        control.credentials_file = blocking_creds  # type: ignore[assignment]
        try:
            prep_task = asyncio.create_task(rt.prepare("a.yaml", want_control=True))
            while not entered.is_set():
                await asyncio.sleep(0.005)
            hurry = asyncio.Event()
            shut_task = asyncio.create_task(rt.shutdown(hurry, 1.0))
            await asyncio.sleep(0.02)
            th_gate.set()
            await shut_task
            try:
                await prep_task
                raise AssertionError("expected Refused shutting-down")
            except Exception as e:
                assert getattr(e, "code", None) == "shutting-down"
            assert rt.active_pids() == []
            assert not rt._residents
        finally:
            control.credentials_file = orig_creds  # type: ignore[assignment]
            os.environ.pop("FAKE_SERVE_LOG", None)

    asyncio.run(run())


def test_serve_check_failed_keeps_resident_ready_without_advancing_session_id(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": "check-fail-first"}) as (server, c, _):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            st, body = await c.post("api/run", {"file": "a.yaml"})
            assert st == 422 and body["error"]["code"] == "check-failed", body
            assert body["error"]["detail"]["result"] == "failed"
            assert body["error"]["detail"]["findings"][0]["code"] == "step-mismatch"

            # No session created, resident remains ready on a.yaml
            st_sess, cur = await c.get("api/session")
            assert st_sess == 200 and cur["session"] is None
            assert rt.status()["state"] == "ready"
            pid1 = rt.resident_pid
            assert pid1 is not None

            # Subsequent start succeeds on the same resident with session id == 1
            st_ok, run_body = await c.post("api/run", {"file": "a.yaml"})
            assert st_ok == 200 and run_body["session"]["id"] == 1
            assert rt.resident_pid == pid1
            assert rt.resident_starts == 1
            await sock.until(is_ended)
            await sock.close()

    asyncio.run(run())


def test_serve_check_timeout_aborts_hung_launcher_and_recovers(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, stop_timeout_s=0.1,
                              env={"FAKE_SERVE_BEHAVIOR": "check-hang"}) as (server, c, _):
            server.o.check_timeout_s = 0.2
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            st, err = await c.post("api/run", {"file": "a.yaml"})
            assert st == 504 and err["error"]["code"] == "check-timeout", err
            assert not server._runs
            st_sess, cur = await c.get("api/session")
            assert st_sess == 200 and cur["session"] is None

            os.environ["FAKE_SERVE_BEHAVIOR"] = "normal"
            st_ok, run_body = await c.post("api/run", {"file": "a.yaml"})
            assert st_ok == 200 and run_body["session"]["id"] == 1
            await sock.until(is_ended)
            await sock.close()

    asyncio.run(run())


def test_hot_single_parse_zero_intermediate_json_dumps_and_loads(tmp_path: Path, monkeypatch) -> None:
    import vehicle_sil_gui.runtime as runtime_mod
    import vehicle_sil_gui.session as session_mod

    dumps_in_handle = 0
    loads_in_feed_event = 0
    orig_dumps = runtime_mod.json.dumps
    orig_loads = session_mod.json.loads
    orig_handle = ServeRuntime._handle_stdout_line
    orig_feed_event = session_mod.Session.feed_event
    in_handle = False
    in_feed_event = False

    def spy_dumps(*args, **kwargs):
        nonlocal dumps_in_handle
        if in_handle:
            dumps_in_handle += 1
        return orig_dumps(*args, **kwargs)

    def spy_loads(*args, **kwargs):
        nonlocal loads_in_feed_event
        if in_feed_event:
            loads_in_feed_event += 1
        return orig_loads(*args, **kwargs)

    def wrapped_handle(self, resident, line):
        nonlocal in_handle
        in_handle = True
        try:
            return orig_handle(self, resident, line)
        finally:
            in_handle = False

    def wrapped_feed_event(self, event, size):
        nonlocal in_feed_event
        in_feed_event = True
        try:
            return orig_feed_event(self, event, size)
        finally:
            in_feed_event = False

    monkeypatch.setattr(runtime_mod.json, "dumps", spy_dumps)
    monkeypatch.setattr(session_mod.json, "loads", spy_loads)
    monkeypatch.setattr(ServeRuntime, "_handle_stdout_line", wrapped_handle)
    monkeypatch.setattr(session_mod.Session, "feed_event", wrapped_feed_event)

    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False) as (server, c, _):
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")
            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            msgs = await sock.until(is_ended)
            assert msgs[-1]["session"]["result"]["result"] == "passed"
            assert dumps_in_handle == 0
            assert loads_in_feed_event == 0
            await sock.close()

    asyncio.run(run())


def test_hot_non_finite_and_invalid_stdout_fallback_preserves_seq(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": "wait-stop"}) as (server, c, _):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")
            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            before = await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            assert rt._resident is not None and rt._resident.current_run is not None
            run_id = rt._resident.current_run.run_id

            for bad_line in (
                json.dumps({"event": "time", "seq": 90, "t_ms": 10, "run": run_id, "sim_ms": 5}).replace("5}", "NaN}"),
                '{"event":"time","seq":91,"t_ms":10,"run":"' + str(run_id) + '","sim_ms":5,"x":1e999}',
                "not json at all",
                "[1, 2, 3]",
            ):
                rt._handle_stdout_line(rt._resident, bad_line)

            await c.post("api/stop", {"force": False})
            msgs = await sock.until(is_ended)
            end_sess = msgs[-1]["session"]
            assert end_sess["result"]["result"] == "stopped"
            output_problems = [p for p in end_sess["problems"] if p["code"] == "launcher-output"]
            assert len(output_problems) == 4
            seqs = [m["event"]["seq"] for m in (*before, *msgs) if m["type"] == "event"]
            assert seqs == list(range(len(seqs)))
            await sock.close()

    asyncio.run(run())


def test_hot_prestart_gate_and_live_finding_redact_request_path(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": "check-fail-request"}) as (server, c, _):
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")
            st, bad = await c.post("api/run", {"file": "a.yaml"})
            assert st == 422 and bad["error"]["code"] == "check-failed"
            finding = bad["error"]["detail"]["findings"][0]
            assert finding["file"] == "the request"
            assert finding["line"] is None
            assert "vehicle-sil-gui-request-" not in finding["message"]

            os.environ["FAKE_SERVE_BEHAVIOR"] = "finding-with-request"
            st2, _ = await c.post("api/run", {"file": "b.yaml"})
            assert st2 == 200
            msgs = await sock.until(is_ended)
            live_findings = [m["event"] for m in msgs if m["type"] == "event" and m["event"]["event"] == "finding"]
            assert live_findings
            assert live_findings[0]["file"] == "the request"
            assert live_findings[0]["line"] is None
            assert "vehicle-sil-gui-request-" not in live_findings[0]["message"]
            await sock.close()

    asyncio.run(run())


def test_hot_file_only_single_call_serve_prepare_reuse_and_unknown_participant(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False) as (server, c, log_path):
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            # Run 1: check --file-only --json runs ONCE (never twice) before spawning serve
            st1, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st1 == 200
            await sock.until(is_ended)
            entries1 = [json.loads(line) for line in log_path.read_text(encoding="utf-8").splitlines()]
            assert sum(1 for e in entries1 if e.get("cmd") == "check-file-only") == 1

            # Run 2 on idle resident: uses serve prepare (0 new check --file-only calls!)
            st2, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st2 == 200
            await sock.until(is_ended)
            entries2 = [json.loads(line) for line in log_path.read_text(encoding="utf-8").splitlines()]
            assert sum(1 for e in entries2 if e.get("cmd") == "check-file-only") == 1
            assert sum(1 for e in entries2 if isinstance(e.get("cmd"), dict) and e["cmd"].get("command") == "prepare") == 1

            # Unknown participant triggers fallback --variant-only check --file-only and refuses 400 unknown-participant
            st3, err = await c.post("api/run", {"file": "a.yaml", "disable": ["Nobody"]})
            assert st3 == 400 and err["error"]["code"] == "unknown-participant"
            assert "Engine" in err["error"]["message"]
            await sock.close()

    asyncio.run(run())


def test_hot_deep_nested_event_flows_through_drain_without_recursion_error(tmp_path: Path) -> None:
    def make_deep(leaf: object, depth: int = 1000) -> object:
        cur = leaf
        for i in range(depth):
            cur = [cur] if i % 2 else {"nested": cur}
        return cur

    def unwrap(obj: object, depth: int = 1000) -> dict:
        cur = obj
        for i in range(depth - 1, -1, -1):
            cur = cur[0] if i % 2 else cur["nested"]  # type: ignore[index]
        assert isinstance(cur, dict)
        return cur

    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": "wait-stop"}) as (server, c, _):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")
            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            before = await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            assert rt._resident is not None and rt._resident.current_run is not None
            run_id = rt._resident.current_run.run_id
            active_run = server.current
            assert active_run is not None and active_run.request is not None
            req_path = str(active_run.request)

            next_seq = len([m for m in before if m["type"] == "event"])
            ev_unrelated = {
                "event": "finding", "seq": next_seq, "t_ms": 20, "run": run_id,
                "severity": "note", "message": "deep unrelated",
                "payload": make_deep({"file": "a.yaml", "line": 3}),
            }
            ev_with_req = {
                "event": "finding", "seq": next_seq + 1, "t_ms": 21, "run": run_id,
                "severity": "warning", "message": "deep request",
                "payload": make_deep({"file": req_path, "line": 7, "message": f"{req_path}, line 7: deep"}),
            }
            rt._handle_stdout_line(rt._resident, json.dumps(ev_unrelated, separators=(",", ":")))
            rt._handle_stdout_line(rt._resident, json.dumps(ev_with_req, separators=(",", ":")))

            got_req = await sock.until(
                lambda m: m["type"] == "event" and m["event"].get("message") == "deep request"
            )
            assert any(
                m["type"] == "event" and m["event"].get("message") == "deep unrelated"
                for m in got_req
            )
            deep_msg = got_req[-1]["event"]
            assert unwrap(deep_msg["payload"]) == {
                "file": "the request", "line": None, "message": "the request: deep",
            }

            await c.post("api/stop", {"force": False})
            msgs = await sock.until(is_ended)
            assert msgs[-1]["session"]["result"]["result"] == "stopped"
            assert msgs[-1]["session"]["problems"] == []
            await sock.close()

    asyncio.run(run())


@pytest.mark.parametrize("behavior", ["crash-second-prepare", "refuse-second-prepare"])
def test_prepare_sends_prepare_to_replacement_resident_after_prepare_failure(
    tmp_path: Path, behavior: str,
) -> None:
    async def run() -> None:
        root = tmp_path / "cfg"
        root.mkdir()
        script = tmp_path / "fake_serve.py"
        log_path = tmp_path / "calls.jsonl"
        _write_configs(root, script)
        prev_log = os.environ.get("FAKE_SERVE_LOG")
        prev_beh = os.environ.get("FAKE_SERVE_BEHAVIOR")
        os.environ["FAKE_SERVE_LOG"] = str(log_path)
        os.environ["FAKE_SERVE_BEHAVIOR"] = behavior
        launcher = FakeLauncher(
            features=(*DEFAULT_FEATURES, "serve", "check-file-only", "serve-prepare"),
            path=script,
        )
        rt = ServeRuntime(launcher, cwd=root)
        try:
            st1 = await rt.prepare("a.yaml")
            assert st1["state"] == "ready"
            pid1 = rt.resident_pid
            assert pid1 is not None

            st2 = await rt.prepare("a.yaml", variant="fast", disable=("Engine",))
            assert st2 == {"mode": "hot", "state": "ready", "config": "a.yaml", "reason": None, "message": None}
            pid2 = rt.resident_pid
            assert pid2 is not None and pid2 != pid1

            entries = [json.loads(ln) for ln in log_path.read_text(encoding="utf-8").splitlines()]
            pid2_prepares = [
                e for e in entries
                if e.get("pid") == pid2 and isinstance(e.get("cmd"), dict) and e["cmd"].get("command") == "prepare"
            ]
            assert len(pid2_prepares) == 1
            assert pid2_prepares[0]["cmd"].get("variant") == "fast"
            assert pid2_prepares[0]["cmd"].get("disable") == ["Engine"]
        finally:
            await rt.shutdown(asyncio.Event(), 0.2)
            if prev_log is None:
                os.environ.pop("FAKE_SERVE_LOG", None)
            else:
                os.environ["FAKE_SERVE_LOG"] = prev_log
            if prev_beh is None:
                os.environ.pop("FAKE_SERVE_BEHAVIOR", None)
            else:
                os.environ["FAKE_SERVE_BEHAVIOR"] = prev_beh

    asyncio.run(run())


def test_inspect_config_restores_idle_state_after_closing_failed_resident(tmp_path: Path) -> None:
    async def run() -> None:
        root = tmp_path / "cfg"
        root.mkdir()
        script = tmp_path / "fake_serve.py"
        log_path = tmp_path / "calls.jsonl"
        _write_configs(root, script)
        prev_log = os.environ.get("FAKE_SERVE_LOG")
        prev_beh = os.environ.get("FAKE_SERVE_BEHAVIOR")
        os.environ["FAKE_SERVE_LOG"] = str(log_path)
        os.environ["FAKE_SERVE_BEHAVIOR"] = "crash-second-prepare"
        launcher = FakeLauncher(
            features=(*DEFAULT_FEATURES, "serve", "check-file-only", "serve-prepare"),
            path=script,
        )
        rt = ServeRuntime(launcher, cwd=root)
        try:
            st1 = await rt.prepare("a.yaml")
            assert st1["state"] == "ready"
            inspection = await rt.inspect_config("a.yaml", variant="fast")
            assert inspection.hot.hot is True
            assert rt._resident is None
            assert rt.status() == {"mode": "hot", "state": "idle", "config": None, "reason": None, "message": None}
        finally:
            await rt.shutdown(asyncio.Event(), 0.2)
            if prev_log is None:
                os.environ.pop("FAKE_SERVE_LOG", None)
            else:
                os.environ["FAKE_SERVE_LOG"] = prev_log
            if prev_beh is None:
                os.environ.pop("FAKE_SERVE_BEHAVIOR", None)
            else:
                os.environ["FAKE_SERVE_BEHAVIOR"] = prev_beh

    asyncio.run(run())


def test_cancelling_prepare_mid_send_prepare_clears_pending_prepare(tmp_path: Path) -> None:
    async def run() -> None:
        from vehicle_sil_gui.runtime import HotDecision, RunSpec

        root = tmp_path / "cfg"
        root.mkdir()
        script = tmp_path / "fake_serve.py"
        log_path = tmp_path / "calls.jsonl"
        _write_configs(root, script)
        prev_log = os.environ.get("FAKE_SERVE_LOG")
        prev_beh = os.environ.get("FAKE_SERVE_BEHAVIOR")
        os.environ["FAKE_SERVE_LOG"] = str(log_path)
        os.environ["FAKE_SERVE_BEHAVIOR"] = "hang-second-prepare"
        launcher = FakeLauncher(
            features=(*DEFAULT_FEATURES, "serve", "check-file-only", "serve-prepare"),
            path=script,
        )
        rt = ServeRuntime(launcher, cwd=root, escalate_wait_s=0.1)
        req_file = tmp_path / "req.json"
        req_file.write_text('{"vehicle_sil_run_request":1}\n', encoding="utf-8")
        try:
            st1 = await rt.prepare("a.yaml")
            assert st1["state"] == "ready"
            res = rt._resident
            assert res is not None

            task = asyncio.create_task(rt.prepare("a.yaml", variant="fast"))
            while res.pending_prepare is None:
                await asyncio.sleep(0.01)
            assert rt.status()["state"] == "preparing"
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await task
            assert res.pending_prepare is None
            assert rt.active_pids() == []
            assert rt.status() == {"mode": "hot", "state": "idle", "config": None, "reason": None, "message": None}

            # Subsequent Hot start_run spawns a clean resident and succeeds in Hot mode (never stuck "busy").
            class _NoopSink:
                def __init__(self) -> None:
                    self.events: list[dict] = []
                def on_stdout_line(self, line: str | None) -> None: pass
                def on_stdout_event(self, event: dict, size: int) -> None: self.events.append(event)
                def on_stderr_line(self, line: str) -> None: pass
                def on_problem(self, code: str, message: str, detail: object = None) -> None: pass
                def on_process_exited(self, returncode: int | None) -> None: pass
                async def on_run_finished(self, *, output_cut: bool = False, launcher_alive: bool = False) -> None: pass
                async def on_run_cleanup(self) -> None: pass

            os.environ["FAKE_SERVE_BEHAVIOR"] = "normal"
            handle = await rt.start_run(RunSpec(config="a.yaml", request=req_file, hot_decision=HotDecision(True)))
            assert handle.mode == "hot"
            sink = _NoopSink()
            handle.start(sink)
            await handle._drain_done.wait()
            assert any(e.get("event") == "result" and e["result"]["result"] == "passed" for e in sink.events)
        finally:
            await rt.shutdown(asyncio.Event(), 0.1)
            if prev_log is None:
                os.environ.pop("FAKE_SERVE_LOG", None)
            else:
                os.environ["FAKE_SERVE_LOG"] = prev_log
            if prev_beh is None:
                os.environ.pop("FAKE_SERVE_BEHAVIOR", None)
            else:
                os.environ["FAKE_SERVE_BEHAVIOR"] = prev_beh

    asyncio.run(run())


def test_cancelling_prepare_mid_spawn_closes_resident_and_restores_idle(tmp_path: Path) -> None:
    async def run() -> None:
        root = tmp_path / "cfg"
        root.mkdir()
        script = tmp_path / "fake_serve.py"
        log_path = tmp_path / "calls.jsonl"
        _write_configs(root, script)
        prev_log = os.environ.get("FAKE_SERVE_LOG")
        os.environ["FAKE_SERVE_LOG"] = str(log_path)
        launcher = FakeLauncher(
            features=(*DEFAULT_FEATURES, "serve", "check-file-only", "serve-prepare"),
            path=script,
        )
        rt = ServeRuntime(launcher, cwd=root, escalate_wait_s=0.1)

        import threading
        th_gate = threading.Event()
        entered = threading.Event()
        orig_creds = control.credentials_file
        created_creds: list[Path] = []

        def blocking_creds():
            entered.set()
            th_gate.wait(timeout=2.0)
            path, tok = orig_creds()
            created_creds.append(path)
            return path, tok

        control.credentials_file = blocking_creds  # type: ignore[assignment]
        try:
            prep_task = asyncio.create_task(rt.prepare("a.yaml", want_control=True))
            while not entered.is_set():
                await asyncio.sleep(0.005)
            assert rt.status()["state"] == "preparing"
            prep_task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await prep_task
            th_gate.set()
            await asyncio.sleep(0.05)
            assert created_creds and not created_creds[0].exists()
            assert rt.status() == {"mode": "hot", "state": "idle", "config": None, "reason": None, "message": None}
            assert rt.active_pids() == []
            assert not rt._residents
        finally:
            for p in created_creds:
                control.remove(p)
            control.credentials_file = orig_creds  # type: ignore[assignment]
            await rt.shutdown(asyncio.Event(), 0.1)
            if prev_log is None:
                os.environ.pop("FAKE_SERVE_LOG", None)
            else:
                os.environ["FAKE_SERVE_LOG"] = prev_log

    asyncio.run(run())


def test_cancelling_start_run_mid_wait_clears_pending_and_closes_resident(tmp_path: Path) -> None:
    async def run() -> None:
        from vehicle_sil_gui.runtime import RunSpec

        root = tmp_path / "cfg"
        root.mkdir()
        script = tmp_path / "fake_serve.py"
        log_path = tmp_path / "calls.jsonl"
        _write_configs(root, script)
        prev_log = os.environ.get("FAKE_SERVE_LOG")
        prev_beh = os.environ.get("FAKE_SERVE_BEHAVIOR")
        os.environ["FAKE_SERVE_LOG"] = str(log_path)
        os.environ["FAKE_SERVE_BEHAVIOR"] = "hang-run-start"
        launcher = FakeLauncher(
            features=(*DEFAULT_FEATURES, "serve", "check-file-only", "serve-prepare"),
            path=script,
        )
        rt = ServeRuntime(launcher, cwd=root, escalate_wait_s=0.1)
        req_file = tmp_path / "req.json"
        req_file.write_text('{"vehicle_sil_run_request":1}\n', encoding="utf-8")
        try:
            await rt.prepare("a.yaml", want_control=False)
            res = rt._resident
            assert res is not None

            task = asyncio.create_task(rt.start_run(RunSpec(config="a.yaml", request=req_file)))
            while res.pending_run is None:
                await asyncio.sleep(0.01)
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await task
            assert res.pending_run is None
            assert rt.resident_pid is None
            assert rt.active_pids() == []
            assert rt.status() == {"mode": "hot", "state": "idle", "config": None, "reason": None, "message": None}
        finally:
            await rt.shutdown(asyncio.Event(), 0.1)
            if prev_log is None:
                os.environ.pop("FAKE_SERVE_LOG", None)
            else:
                os.environ["FAKE_SERVE_LOG"] = prev_log
            if prev_beh is None:
                os.environ.pop("FAKE_SERVE_BEHAVIOR", None)
            else:
                os.environ["FAKE_SERVE_BEHAVIOR"] = prev_beh

    asyncio.run(run())


def test_serve_status_command_and_monotonic_execution_status_adoption(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": "wait-stop"}) as (server, c, _):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            st, _ = await c.post("api/prepare", {"file": "a.yaml"})
            assert st == 200
            queried = await rt.query_status()
            assert queried is not None
            assert queried["launcher_state"] == "ready"
            assert queried["launcher_allowed"]["run"]["ok"] is True
            cur_rev = queried["status_rev"]

            res = rt._resident
            assert res is not None
            # Stale status_rev and mismatched instance are ignored.
            rt._handle_stdout_line(res, json.dumps({
                "event": "status", "seq": 900, "t_ms": 900,
                "instance": "feed000000000001", "status_rev": cur_rev - 1,
                "launcher_state": "contaminated", "config": "a.yaml",
            }))
            assert rt.status()["state"] == "ready"
            rt._handle_stdout_line(res, json.dumps({
                "event": "status", "seq": 901, "t_ms": 901,
                "instance": "wrong-instance", "status_rev": cur_rev + 10,
                "launcher_state": "contaminated", "config": "a.yaml",
            }))
            assert rt.status()["state"] == "ready"

            # Newer status_rev on matching instance updates runtime status via WS.
            rt._handle_stdout_line(res, json.dumps({
                "event": "status", "seq": 902, "t_ms": 902,
                "instance": "feed000000000001", "status_rev": cur_rev + 1,
                "launcher_state": "preparing", "config": "a.yaml",
                "eligibility": {"eligible": True, "reason": None, "message": None},
                "launcher_allowed": {"prepare": {"ok": False, "reason": "busy"}, "run": {"ok": False, "reason": "busy"}},
            }))
            prep_msg = await sock.until(lambda m: m["type"] == "runtime" and m["runtime"]["state"] == "preparing")
            assert prep_msg[-1]["runtime"]["config"] == "a.yaml"

            # Restore ready before run.
            rt._handle_stdout_line(res, json.dumps({
                "event": "status", "seq": 903, "t_ms": 903,
                "instance": "feed000000000001", "status_rev": cur_rev + 2,
                "launcher_state": "ready", "config": "a.yaml",
                "eligibility": {"eligible": True, "reason": None, "message": None},
                "launcher_allowed": {"prepare": {"ok": True, "reason": None}, "run": {"ok": True, "reason": None}},
            }))
            await sock.until(lambda m: m["type"] == "runtime" and m["runtime"]["state"] == "ready")
            await sock.close()

    asyncio.run(run())


def test_hot_allowed_projects_cpp_stop_requests_and_post_result_drain(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": "wait-stop"}) as (server, c, _):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            res = rt._resident
            assert res is not None and res.current_run is not None
            run_id = res.current_run.run_id
            assert run_id is not None

            # C++ state event with stop.requests == 1 updates allowed.stop without Session.request_stop().
            rt._handle_stdout_line(res, json.dumps({
                "event": "state", "seq": 95, "t_ms": 200, "run": run_id,
                "rev": 5, "phase": "running", "paused": False,
                "stop": {"requests": 1, "by": "control_server"},
                "allowed": {},
            }))
            stopping_msg = await sock.until(
                lambda m: m["type"] == "allowed" and m["allowed"]["stop"]["reason"] == "already-stopping"
            )
            assert stopping_msg[-1]["allowed"]["force"]["ok"] is True

            # C++ state event with stop.requests == 2 updates allowed.force -> already-forced.
            rt._handle_stdout_line(res, json.dumps({
                "event": "state", "seq": 96, "t_ms": 205, "run": run_id,
                "rev": 6, "phase": "stopping", "paused": False,
                "stop": {"requests": 2, "by": "control_server"},
                "allowed": {},
            }))
            forced_msg = await sock.until(
                lambda m: m["type"] == "allowed" and m["allowed"]["force"]["reason"] == "already-forced"
            )
            assert forced_msg[-1]["allowed"]["start"]["reason"] == "already-running"

            # Check post-result drain window: while session is still running, stoppable drops to False.
            sess = server.session
            assert sess is not None
            # Simulate result arriving on execution_status before session.ended() runs:
            assert res.execution_status is not None
            res.execution_status["launcher_state"] = "ready"
            drain_allowed = sess.allowed()
            assert drain_allowed["start"]["reason"] == "already-running"
            assert drain_allowed["stop"]["reason"] == "not-running"
            assert drain_allowed["force"]["reason"] == "not-running"

            # Restore running so normal stop completes cleanly.
            res.execution_status["launcher_state"] = "running"
            res.execution_status["run_state"]["stop"]["requests"] = 0
            await c.post("api/stop", {"force": True})
            await sock.until(is_ended)
            await sock.close()

    asyncio.run(run())


def test_prepare_early_returns_clear_preparing_config_without_spurious_ws_event(tmp_path: Path) -> None:
    async def run() -> None:
        (tmp_path / "case1").mkdir()
        (tmp_path / "case2").mkdir()
        # 1. parsed is None (_send_prepare refused/crashed on freshly spawned resident) -> returns cold, never preparing.
        async with _serve_gui(tmp_path / "case1", variables=False, env={"FAKE_SERVE_BEHAVIOR": "refuse-first-prepare"}) as (server, c, _):
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")
            st, body = await c.post("api/prepare", {"file": "a.yaml"})
            assert st == 200
            assert body["runtime"]["mode"] == "cold"
            assert body["runtime"]["state"] == "cold"
            assert body["runtime"]["reason"] == "resident-failed"
            assert server.runtime._preparing_config is None  # type: ignore[attr-defined]
            await sock.close()

        # 2. not parsed.hot.hot (without check-file-only, serve prepare returns eligible=False) -> returns cold, no preparing after resetting.
        async with _serve_gui(
            tmp_path / "case2",
            features=(*DEFAULT_FEATURES, "serve", "serve-prepare"),
            variables=False,
        ) as (server, c, _):
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")
            st, body = await c.post("api/prepare", {"file": "fixed.yaml"})
            assert st == 200
            assert body["runtime"] == {
                "mode": "cold",
                "state": "cold",
                "config": "fixed.yaml",
                "reason": "fixed-registry",
                "message": "configuration uses a fixed registry_uri",
            }
            cold_msgs = await sock.until(lambda m: m["type"] == "runtime" and m["runtime"]["state"] == "cold")
            states = [m["runtime"]["state"] for m in cold_msgs if m["type"] == "runtime"]
            reset_idx = states.index("resetting")
            assert "preparing" not in states[reset_idx:]
            await sock.close()

    asyncio.run(run())


def test_execution_status_validation_and_pre_run_equal_rev_protection(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": "wait-stop"}) as (server, c, _):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")
            st, _ = await c.post("api/prepare", {"file": "a.yaml"})
            assert st == 200
            res = rt._resident
            assert res is not None
            cur_rev = res.status_rev

            # Reject missing/non-int/bool status_rev and missing/None/non-str instance.
            for bad in (
                {"launcher_state": "contaminated", "instance": "feed000000000001", "status_rev": None},
                {"launcher_state": "contaminated", "instance": "feed000000000001"},
                {"launcher_state": "contaminated", "instance": "feed000000000001", "status_rev": False},
                {"launcher_state": "contaminated", "instance": "feed000000000001", "status_rev": "99"},
                {"launcher_state": "contaminated", "instance": "feed000000000001", "status_rev": float(cur_rev + 1)},
                {"launcher_state": "contaminated", "instance": None, "status_rev": cur_rev + 10},
                {"launcher_state": "contaminated", "status_rev": cur_rev + 10},
                {"launcher_state": "contaminated", "instance": 12345, "status_rev": cur_rev + 10},
                {"launcher_state": "contaminated", "instance": "", "status_rev": cur_rev + 10},
            ):
                assert rt._apply_execution_status(res, bad) is False

            # Foreign/mismatched status and prepare events with id=None do NOT resolve pending futures.
            loop = asyncio.get_running_loop()
            fut_status: asyncio.Future[dict] = loop.create_future()
            res.pending_status = (999, fut_status)
            rt._handle_stdout_line(res, json.dumps({
                "event": "status", "seq": 910, "t_ms": 910, "id": None,
                "instance": "feed000000000001", "status_rev": cur_rev, "launcher_state": "ready", "config": "a.yaml",
            }))
            rt._handle_stdout_line(res, json.dumps({
                "event": "status", "seq": 911, "t_ms": 911, "id": 999,
                "instance": None, "status_rev": cur_rev + 5, "launcher_state": "contaminated", "config": "a.yaml",
            }))
            assert not fut_status.done()
            res.pending_status = None

            fut_prep: asyncio.Future[dict] = loop.create_future()
            res.pending_prepare = (998, fut_prep)
            rt._handle_stdout_line(res, json.dumps({
                "event": "prepare", "seq": 912, "t_ms": 912, "id": None, "accepted": True,
                "status": {
                    "instance": "feed000000000001", "status_rev": cur_rev, "launcher_state": "ready",
                    "config": "a.yaml", "eligibility": {"eligible": True, "reason": None, "message": None},
                },
                "metadata": {"participants": ["Engine"], "networks": [], "variants": []},
            }))
            rt._handle_stdout_line(res, json.dumps({
                "event": "prepare", "seq": 913, "t_ms": 913, "id": 998, "accepted": True,
                "status": {
                    "instance": "wrong-instance", "status_rev": cur_rev + 5, "launcher_state": "ready",
                    "config": "a.yaml", "eligibility": {"eligible": True, "reason": None, "message": None},
                },
                "metadata": {"participants": ["Engine"], "networks": [], "variants": []},
            }))
            assert not fut_prep.done()
            res.pending_prepare = None

            # Start run and verify equal-status_rev pre-run status cannot regress "running" -> "ready",
            # and bool run_state.rev cannot overwrite integer run_state.rev.
            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            assert res.current_run is not None
            run_id = res.current_run.run_id
            rt._handle_stdout_line(res, json.dumps({
                "event": "status", "seq": 914, "t_ms": 914,
                "instance": "feed000000000001", "status_rev": cur_rev,
                "launcher_state": "ready", "config": "a.yaml",
            }))
            assert res.execution_status is not None and res.execution_status["launcher_state"] == "running"

            rt._handle_stdout_line(res, json.dumps({
                "event": "state", "seq": 915, "t_ms": 915, "run": run_id,
                "rev": 3, "phase": "running", "paused": False,
                "stop": {"requests": 0, "by": None}, "allowed": {},
            }))
            rt._handle_stdout_line(res, json.dumps({
                "event": "state", "seq": 916, "t_ms": 916, "run": run_id,
                "rev": False, "phase": "stopping", "paused": False,
                "stop": {"requests": 2, "by": "bad"}, "allowed": {},
            }))
            assert res.execution_status["run_state"]["rev"] == 3

            await c.post("api/stop", {"force": False})
            await sock.until(is_ended)
            await sock.close()

    asyncio.run(run())


def test_hot_external_stop_does_not_escalate_on_graceful_api_stop_and_release_mid_run(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": "wait-stop"}) as (server, c, log_path):
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")
            res = rt._resident
            assert res is not None and res.current_run is not None
            run_id = res.current_run.run_id

            # release_config() mid-run marks resident ending while keeping rt.status() == "running".
            await rt.release_config("a.yaml")
            assert res.ending is True
            assert rt.status()["state"] == "running"

            # External C++ stop sets stop.requests == 1; POST api/stop {force: false} must NOT escalate.
            rt._handle_stdout_line(res, json.dumps({
                "event": "state", "seq": 95, "t_ms": 200, "run": run_id,
                "rev": 5, "phase": "stopping", "paused": False,
                "stop": {"requests": 1, "by": "control_server"}, "allowed": {},
            }))
            await sock.until(lambda m: m["type"] == "allowed" and m["allowed"]["stop"]["reason"] == "already-stopping")

            st_grace, grace_body = await c.post("api/stop", {"force": False})
            assert st_grace == 200
            assert grace_body["session"]["allowed"]["stop"]["reason"] == "already-stopping"
            assert grace_body["session"]["allowed"]["force"]["ok"] is True

            # Verify no unforced stop line was written while already stopping, and force=True sends {"force": true}.
            st_force, force_body = await c.post("api/stop", {"force": True})
            assert st_force == 200
            await sock.until(is_ended)
            assert rt.status()["state"] == "idle"

            entries = [json.loads(ln) for ln in log_path.read_text(encoding="utf-8").splitlines()]
            stop_cmds = [e["cmd"] for e in entries if isinstance(e.get("cmd"), dict) and e["cmd"].get("command") == "stop"]
            assert len(stop_cmds) == 1
            assert stop_cmds[0].get("force") is True
            await sock.close()

    asyncio.run(run())


def test_hot_launcher_ends_post_result_async_drain_keeps_stoppable_false(tmp_path: Path) -> None:
    async def run() -> None:
        class _SlowCloseSource(_FakeControlSource):
            def __init__(self, address: str, token: str):
                super().__init__(address, token)
                self.close_gate = asyncio.Event()

            async def close(self) -> None:
                await self.close_gate.wait()
                await super().close()

        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": "stop-ends"}) as (server, c, _):
            # Enable slow variables close via source hook so post-result async drain stays observable.
            sources: list[_SlowCloseSource] = []
            server.o.variables = True
            server.o.source = lambda addr, tok: sources.append(_SlowCloseSource(addr, tok)) or sources[-1]
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            st, _ = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")

            st_stop, _ = await c.post("api/stop", {"force": False})
            assert st_stop == 200
            # Wait for result to clear current_run and set resident.ending while _SlowCloseSource.close() holds drain.
            rt = server.runtime
            assert isinstance(rt, ServeRuntime)
            for _ in range(100):
                if rt.execution_status() is None and server.session is not None and server.session.state == "running":
                    break
                await asyncio.sleep(0.01)
            assert server.session is not None and server.session.state == "running"
            assert rt.execution_status() is None

            st_sess, sess_body = await c.get("api/session")
            assert st_sess == 200
            al = sess_body["allowed"]
            assert al["start"]["reason"] == "already-running"
            assert al["stop"]["reason"] == "not-running"
            assert al["force"]["reason"] == "not-running"

            sources[0].close_gate.set()
            await sock.until(is_ended)
            await sock.close()

    asyncio.run(run())


@pytest.mark.parametrize("launcher_ends", [False, True])
def test_hot_post_result_async_drain_stop_is_noop_without_spurious_problem(
    tmp_path: Path, launcher_ends: bool,
) -> None:
    async def run() -> None:
        class _SlowCloseSource(_FakeControlSource):
            def __init__(self, address: str, token: str):
                super().__init__(address, token)
                self.close_entered = asyncio.Event()
                self.close_gate = asyncio.Event()

            async def close(self) -> None:
                self.close_entered.set()
                await self.close_gate.wait()
                await super().close()

        behavior = "natural-ends" if launcher_ends else "natural-pass"
        async with _serve_gui(tmp_path, variables=False, env={"FAKE_SERVE_BEHAVIOR": behavior}) as (server, c, log_path):
            sources: list[_SlowCloseSource] = []
            server.o.variables = True
            server.o.source = lambda addr, tok: sources.append(_SlowCloseSource(addr, tok)) or sources[-1]
            sock = await c.ws()
            await sock.until(lambda m: m["type"] == "synced")

            st, started = await c.post("api/run", {"file": "a.yaml"})
            assert st == 200, started
            sid = started["session"]["id"]
            await sock.until(lambda m: m["type"] == "event" and m["event"]["event"] == "time")

            st_res, _ = await c.post("api/time", {"command": "resume"})
            assert st_res == 200

            await asyncio.wait_for(sources[0].close_entered.wait(), timeout=5.0)
            assert server.session is not None and server.session.state == "running"
            assert server.session.launcher_running is True
            assert server.session.result is not None

            st_sess, sess_body = await c.get("api/session")
            assert st_sess == 200
            assert sess_body["session"]["stop"] == "none"
            assert sess_body["session"]["problems"] == []
            assert sess_body["allowed"]["stop"]["ok"] is False
            assert sess_body["allowed"]["stop"]["reason"] == "not-running"
            assert sess_body["allowed"]["force"]["ok"] is False
            assert sess_body["allowed"]["force"]["reason"] == "not-running"

            for endpoint, payload in (
                ("api/stop", {"force": False}),
                ("api/stop", {"force": True}),
                ("api/stop-session", {"session": sid, "instance": server.instance, "force": False}),
                ("api/stop-session", {"session": sid, "instance": server.instance, "force": True}),
            ):
                st_stop, stop_body = await c.post(endpoint, payload)
                assert st_stop == 200, (endpoint, payload, stop_body)
                assert stop_body["session"]["stop"] == "none"
                assert stop_body["session"]["problems"] == []
                assert stop_body["session"]["allowed"]["stop"]["reason"] == "not-running"
                assert stop_body["session"]["allowed"]["force"]["reason"] == "not-running"

            sources[0].close_gate.set()
            msgs = await sock.until(is_ended)
            end_sess = msgs[-1]["session"]
            expected_result = "interrupted" if launcher_ends else "passed"
            assert end_sess["state"] == "ended"
            assert end_sess["stop"] == "none"
            assert end_sess["problems"] == []
            assert end_sess["result"]["result"] == expected_result
            assert not any(m["type"] == "error" for m in msgs)

            entries = [json.loads(ln) for ln in log_path.read_text(encoding="utf-8").splitlines()]
            assert not [e for e in entries if isinstance(e.get("cmd"), dict) and e["cmd"].get("command") == "stop"]
            await sock.close()

    asyncio.run(run())


def test_shutdown_waits_for_in_flight_post_prepare(tmp_path: Path) -> None:
    async def run() -> None:
        async with _serve_gui(tmp_path, variables=True) as (server, c, _):
            import threading
            th_gate = threading.Event()
            entered = threading.Event()
            orig_creds = control.credentials_file
            created_creds: list[Path] = []

            def blocking_creds():
                entered.set()
                th_gate.wait(timeout=2.0)
                path, tok = orig_creds()
                created_creds.append(path)
                return path, tok

            control.credentials_file = blocking_creds  # type: ignore[assignment]
            try:
                prep_task = asyncio.create_task(c.post("api/prepare", {"file": "a.yaml"}))
                while not entered.is_set():
                    await asyncio.sleep(0.005)
                shut_task = asyncio.create_task(server.shutdown())
                await asyncio.sleep(0.05)
                assert not shut_task.done()
                th_gate.set()
                st, body = await prep_task
                await shut_task
                assert st == 503 and body["error"]["code"] == "shutting-down"
                assert server.runtime.active_pids() == []
                assert created_creds and not created_creds[0].exists()
            finally:
                for p in created_creds:
                    control.remove(p)
                control.credentials_file = orig_creds  # type: ignore[assignment]

    asyncio.run(run())
