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
from vehicle_sil_gui import control
from vehicle_sil_gui.runtime import ServeRuntime, check_hot_eligibility
from vehicle_sil_gui.server import GuiServer, Options
from vehicle_sil_gui.source import ListSample, PeerInfo, SnapshotInfo, SourceError, TimeInfo


FAKE_SERVE_SCRIPT = textwrap.dedent("""\
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
    cred_path = None
    if "--control-credentials" in argv:
        cred_path = Path(argv[argv.index("--control-credentials") + 1])
    cfg = argv[-1]

    def log_event(entry):
        with mode_file.open("a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\\n")

    if mode == "startup-refuse":
        emit({
            "event": "hello", "seq": 0, "t_ms": 0, "vehicle_sil_events": 3,
            "vehicle_sil": "0.3.0", "pid": os.getpid(), "file": cfg,
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
        "vehicle_sil": "0.3.0", "pid": os.getpid(), "file": cfg,
        "arguments": argv, "launcher": "feed000000000001",
    })
    seq += 1
    emit({"event": "idle", "seq": seq, "t_ms": t_ms})
    seq += 1
    run_counter = 0
    current_run = None

    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        cmd = json.loads(line)
        name = cmd.get("command")
        log_event({"pid": os.getpid(), "config": cfg, "cmd": cmd, "cred_exists": cred_path.exists() if cred_path else None})

        if name == "run":
            run_counter += 1
            run_id = f"feed-{run_counter}"
            current_run = run_id
            t_ms += 100
            emit({"event": "run_start", "seq": seq, "t_ms": t_ms, "run": run_id, "id": cmd.get("id")})
            seq += 1
            t_ms += 5
            emit({"event": "phase", "seq": seq, "t_ms": t_ms, "run": run_id, "phase": "checking"})
            seq += 1
            emit({"event": "check", "seq": seq, "t_ms": t_ms, "run": run_id, "check": {
                "vehicle_sil_check": 1, "result": "passed", "errors": 0, "warnings": 0, "notes": 0,
                "participants": [{"name": "Engine", "enabled": True, "manual": False, "variables": []}],
                "networks": [], "findings": [],
            }})
            seq += 1
            t_ms += 10
            emit({"event": "phase", "seq": seq, "t_ms": t_ms, "run": run_id, "phase": "running"})
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
                current_run = None
                emit({"event": "idle", "seq": seq, "t_ms": t_ms + 35})
                seq += 1
            elif mode == "heartbeat-until-stop":
                import time as _time
                emit({"event": "heartbeat", "seq": seq, "t_ms": t_ms + 50, "run": run_id})
                seq += 1
                _time.sleep(0.15)
                emit({"event": "heartbeat", "seq": seq, "t_ms": t_ms + 200, "run": run_id})
                seq += 1
            elif mode != "wait-stop":
                emit({"event": "result", "seq": seq, "t_ms": t_ms + 30, "run": run_id, "launcher_ends": False,
                      "result": {"result": "passed", "errors": 0, "warnings": 0, "notes": 0, "findings": []}})
                seq += 1
                current_run = None
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
                t_ms += 40
                emit({"event": "command", "seq": seq, "t_ms": t_ms, "run": current_run,
                      "command": "stop", "accepted": True, "ms": 0})
                seq += 1
                emit({"event": "result", "seq": seq, "t_ms": t_ms + 5, "run": current_run,
                      "launcher_ends": False, "result": {"result": "stopped", "errors": 0, "warnings": 0, "notes": 0, "findings": []}})
                seq += 1
                current_run = None
                emit({"event": "idle", "seq": seq, "t_ms": t_ms + 10})
                seq += 1
""")


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
    features: Sequence[str] = (*DEFAULT_FEATURES, "serve"),
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
            assert all(e["cred_exists"] is True for e in entries if e["cmd"]["command"] == "run")
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
        _write_configs(root, script)

        # 1. launcher-unsupported
        no_serve = FakeLauncher(features=DEFAULT_FEATURES, path=script)
        assert check_hot_eligibility(no_serve, root, "a.yaml").reason == "launcher-unsupported"

        # 2. trace-enabled
        with_serve = FakeLauncher(features=(*DEFAULT_FEATURES, "serve"), path=script)
        os.environ["VEHICLE_SIL_TRACE"] = "/tmp/trace"
        try:
            assert check_hot_eligibility(with_serve, root, "a.yaml").reason == "trace-enabled"
        finally:
            os.environ.pop("VEHICLE_SIL_TRACE", None)

        # 3. fixed-registry
        assert check_hot_eligibility(with_serve, root, "fixed.yaml").reason == "fixed-registry"

        # 4. manual-participants (YAML and request override)
        assert check_hot_eligibility(with_serve, root, "manual.yaml").reason == "manual-participants"
        assert check_hot_eligibility(with_serve, root, "a.yaml", manual=("Engine",)).reason == "manual-participants"
        assert check_hot_eligibility(with_serve, root, "manual.yaml", disable=("Engine",)).hot is True

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
            else:
                assert end1["result"]["result"] == expected_result

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
