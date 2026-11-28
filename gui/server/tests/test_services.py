"""Independent unit tests for ConfigService, RunManager, WsGateway, RouteSpec, and the acyclic import DAG."""

from __future__ import annotations

import ast
import asyncio
import json
from pathlib import Path

import pytest
from conftest import CONFIGS, REPLAY
from fake_runtime import FakeLauncher, FakeRuntime, default_check_result

from vehicle_sil_gui.allowed import Facts
from vehicle_sil_gui.config_service import ConfigService
from vehicle_sil_gui.contracts import DescribeRequest, RunRequest, StopSessionRequest
from vehicle_sil_gui.launcher import Launcher
from vehicle_sil_gui.refusal import Refused
from vehicle_sil_gui.run_manager import RunManager
from vehicle_sil_gui.server import GuiServer, Options, route_specs
from vehicle_sil_gui.session import Session
from vehicle_sil_gui.variables import Watch
from vehicle_sil_gui.ws_gateway import WsGateway


def _simple_root(tmp_path: Path) -> Path:
    root = tmp_path / "root"
    root.mkdir()
    (root / "simple.yaml").write_text(
        "vehicle_sil_simulation: 2\nparticipants:\n  - name: Engine\n    command: [echo, ok]\n",
        encoding="utf-8",
    )
    return root


def test_route_specs_cover_all_17_api_endpoints_and_match_aiohttp_router() -> None:
    specs = route_specs()
    assert len(specs) == 17
    assert len({(s.method, s.path) for s in specs}) == 17
    assert len({s.operation_id for s in specs}) == 17
    for spec in specs:
        assert spec.path.startswith("/api/")
        assert spec.tags
        assert hasattr(GuiServer, spec.handler)
        if spec.method == "POST":
            assert spec.request_model is not None
            assert spec.request_body_required is True
        if spec.path != "/api/timeline/{run_id}":
            assert spec.response_model is not None
    write_spec = next(s for s in specs if s.path == "/api/write")
    assert 502 in write_spec.error_statuses

    server = GuiServer(Options(root=CONFIGS, launcher=Launcher(REPLAY), port=1, token="t"))
    app = server.app()
    registered_api: set[tuple[str, str]] = set()
    for route in app.router.routes():
        canonical = route.resource.canonical if route.resource is not None else ""
        if canonical.startswith("/api/") and canonical != "/api/{rest}":
            registered_api.add((route.method, canonical))

    expected = {(s.method, s.path) for s in specs}
    expected_heads = {("HEAD", s.path) for s in specs if s.method == "GET"}
    assert registered_api == expected | expected_heads


def test_package_import_dag_is_strictly_acyclic_and_respects_service_boundaries() -> None:
    pkg_dir = Path(__file__).resolve().parents[1] / "src" / "vehicle_sil_gui"
    modules = {p.stem: p for p in sorted(pkg_dir.glob("*.py"))}
    graph: dict[str, set[str]] = {name: set() for name in modules}
    for name, path in modules.items():
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for node in ast.walk(tree):
            if isinstance(node, ast.ImportFrom) and node.level == 1:
                if node.module:
                    target = node.module.split(".")[0]
                    if target in modules and target != name:
                        graph[name].add(target)
                else:
                    for alias in node.names:
                        if alias.name in modules and alias.name != name:
                            graph[name].add(alias.name)

    # Detect any cycle via 3-color DFS.
    WHITE, GRAY, BLACK = 0, 1, 2
    color = {m: WHITE for m in graph}
    cycles: list[list[str]] = []

    def dfs(u: str, stack: list[str]) -> None:
        color[u] = GRAY
        stack.append(u)
        for v in sorted(graph[u]):
            if color[v] == GRAY:
                cycles.append(stack[stack.index(v):] + [v])
            elif color[v] == WHITE:
                dfs(v, stack)
        stack.pop()
        color[u] = BLACK

    for m in sorted(graph):
        if color[m] == WHITE:
            dfs(m, [])

    assert cycles == [], f"import cycles detected: {cycles}"
    assert not (graph["config_service"] & {"run_manager", "ws_gateway", "server", "session", "hub", "variables", "runtime"})
    assert not (graph["run_manager"] & {"ws_gateway", "server"})
    assert not (graph["ws_gateway"] & {"config_service", "server", "runtime"})
    # Session no longer carries circular back-callbacks to server/runtime.
    s = Session("1", "simple.yaml", [], None, False)
    assert not hasattr(s, "facts")
    assert not hasattr(s, "execution_status")


def test_config_service_operates_independently_and_invokes_save_hook(tmp_path: Path) -> None:
    root = _simple_root(tmp_path)
    saved_events: list[tuple[str, bool]] = []

    async def fake_check(argv: list[str], request: Path, descriptions: Path | None = None,
                         text: bytes | None = None) -> tuple[int, dict]:
        res = default_check_result("simple.yaml")
        res["networks"] = [{"name": "CAN1"}]
        if "--compare-commands" in argv:
            res["commands"] = "same"
        return 0, res

    async def main() -> None:
        async def on_saved(rel: str) -> None:
            # Must run while _check_lock is held (before caches are cleared and next queued op runs).
            saved_events.append((rel, svc._check_lock.locked()))

        svc = ConfigService(root, FakeLauncher(), check_runner=fake_check, on_config_saved=on_saved)
        listed = await svc.list_configs()
        assert [f["path"] for f in listed["files"]] == ["simple.yaml"]

        got = await svc.get_config("simple.yaml")
        assert got["file"] == "simple.yaml" and "vehicle_sil_simulation: 2" in got["text"]

        checked = await svc.check({"file": "simple.yaml"})
        assert checked["exit_code"] == 0 and checked["check"]["result"] == "passed"
        st = svc.stamp("simple.yaml")
        assert svc.cached_names("simple.yaml", None, st) == ["Engine"]
        assert svc.cached_networks("simple.yaml", None, st) == ["CAN1"]

        # Connect target not listed -> 400 target-not-listed.
        with pytest.raises(Refused) as exc:
            svc.validate_connect_targets("simple.yaml", {"CAN1": "vcan0"}, ["CAN1"])
        assert (exc.value.status, exc.value.code) == (400, "target-not-listed")

        # Direct programmatic save_config enforces Facts(save=False) without mutating disk.
        ro_svc = ConfigService(root, FakeLauncher(), check_runner=fake_check, facts=lambda: Facts(save=False))
        before_text = (root / "simple.yaml").read_text(encoding="utf-8")
        with pytest.raises(Refused) as save_exc:
            await ro_svc.save_config({"file": "simple.yaml", "text": before_text + "# blocked\n", "stamp": got["stamp"]})
        assert (save_exc.value.status, save_exc.value.code) == (403, "save-disabled")
        assert (root / "simple.yaml").read_text(encoding="utf-8") == before_text

        # Save replaces file, invokes on_config_saved under _check_lock, and clears caches.
        new_text = got["text"] + "# edited\n"
        saved = await svc.save_config({"file": "simple.yaml", "text": new_text, "stamp": got["stamp"]})
        assert saved["file"] == "simple.yaml" and saved["stamp"] != got["stamp"]
        assert saved_events == [("simple.yaml", True)]
        assert svc.cached_names("simple.yaml", None, svc.stamp("simple.yaml")) is None

        # Describe uses injected check_runner cleanly.
        described = await svc.describe(DescribeRequest(file="simple.yaml"), "simple.yaml")
        assert described["exit_code"] == 0
        await svc.cleanup()

    asyncio.run(main())


def test_run_manager_and_ws_gateway_operate_independently(tmp_path: Path) -> None:
    root = _simple_root(tmp_path)

    async def fake_check(argv: list[str], request: Path, descriptions: Path | None = None,
                         text: bytes | None = None) -> tuple[int, dict]:
        return 0, default_check_result("simple.yaml")

    async def main() -> None:
        launcher = FakeLauncher()
        configs = ConfigService(root, launcher, check_runner=fake_check)
        runtime = FakeRuntime()
        runs = RunManager(instance="inst-1", launcher=launcher, configs=configs, runtime=runtime, variables=False)
        ws = WsGateway(runs, auth=False)
        runs.bind_hub(ws.hub, ws.wake_watches)

        # Idle snapshot & allowed projection.
        snap = ws.snapshot(None, None)
        assert snap[0]["type"] == "hello" and snap[0]["session"] is None
        assert snap[0]["allowed"]["start"]["ok"] is True
        assert snap[0]["allowed"]["stop"]["reason"] == "not-running"
        assert snap[1]["type"] == "synced" and snap[1]["run_id"] is None

        # Start a run directly through RunManager.
        req = RunRequest(file="simple.yaml")
        started = await runs.start_run(req, {"file": "simple.yaml"})
        assert started == {"instance": "inst-1", "run_id": "inst-1-1"}
        assert runs.allowed()["start"]["reason"] == "already-running"
        assert runs.allowed()["stop"]["ok"] is True

        # Session JSON payload matches RunManager projection.
        payload = json.loads(runs.get_session_payload())
        assert payload["session"]["id"] == "inst-1-1"
        assert payload["allowed"] == runs.allowed()

        # Stop through RunManager updates allowed immediately.
        stopped = await runs.stop_session(StopSessionRequest(instance="inst-1", run_id="inst-1-1", force=False))
        assert stopped == {"instance": "inst-1", "run_id": "inst-1-1"}
        assert runs.allowed()["stop"]["reason"] == "already-stopping"
        assert runs.allowed()["force"]["ok"] is True

        # Second begin_shutdown call sets _hurry and returns False.
        await runtime.runs[0].finish(returncode=0)
        assert runs.begin_shutdown() is True
        assert runs.closing is True
        assert not runs._hurry.is_set()
        assert runs.begin_shutdown() is False
        assert runs._hurry.is_set()

        # WsGateway wake_watches marks watches due & sets _tick_now.
        w = Watch(1, (), 10.0, 1e18, lambda _: None)
        ws._watching[w] = ws.hub.client(lambda _: asyncio.sleep(0))
        ws.wake_watches()
        assert ws._tick_now.is_set()
        assert w.due < 1e18
        ws._watching.clear()

        await runs.shutdown_runtime()
        await ws.shutdown()
        await runs.cleanup_timelines()
        await configs.cleanup()

    asyncio.run(main())


def test_config_service_validates_and_forwards_after_scenario(tmp_path: Path) -> None:
    root = _simple_root(tmp_path)
    (root / "scenarios").mkdir()
    (root / "scenarios" / "a.yaml").write_text("vehicle_sil_scenario: 1\nsteps: []\n", encoding="utf-8")

    async def main() -> None:
        cfg_file = ConfigService(root, FakeLauncher()).resolve("simple.yaml")

        # 1. Valid after_scenario="pause" and "stop" with scenario-handover feature
        svc = ConfigService(root, FakeLauncher())
        intent, _, _ = await svc.build_intent(
            cfg_file,
            RunRequest(file="simple.yaml", scenario="scenarios/a.yaml", after_scenario="pause"),
            "scenarios/a.yaml",
        )
        assert intent["after_scenario"] == "pause"

        intent_stop, _, _ = await svc.build_intent(
            cfg_file,
            RunRequest(file="simple.yaml", scenario="scenarios/a.yaml", after_scenario="stop"),
            "scenarios/a.yaml",
        )
        assert intent_stop["after_scenario"] == "stop"

        # Omitted after_scenario stays omitted from intent
        intent_omitted, _, _ = await svc.build_intent(
            cfg_file,
            RunRequest(file="simple.yaml", scenario="scenarios/a.yaml"),
            "scenarios/a.yaml",
        )
        assert "after_scenario" not in intent_omitted

        # 2. Missing scenario -> 400 invalid-field
        with pytest.raises(Refused) as exc_no_sc:
            await svc.build_intent(cfg_file, RunRequest(file="simple.yaml", after_scenario="pause"), None)
        assert (exc_no_sc.value.status, exc_no_sc.value.code) == (400, "invalid-field")

        # 3. Combined with stop_at_ms -> 400 invalid-field
        with pytest.raises(Refused) as exc_stop_at:
            await svc.build_intent(
                cfg_file,
                RunRequest(file="simple.yaml", scenario="scenarios/a.yaml", after_scenario="pause", stop_at_ms=500),
                "scenarios/a.yaml",
            )
        assert (exc_stop_at.value.status, exc_stop_at.value.code) == (400, "invalid-field")

        # 4. Launcher without scenario-handover -> 409 launcher-unsupported for pause, omitted for stop
        no_ho_features = [f for f in FakeLauncher().features if f != "scenario-handover"]
        svc_old = ConfigService(root, FakeLauncher(features=no_ho_features))
        with pytest.raises(Refused) as exc_unsup:
            await svc_old.build_intent(
                cfg_file,
                RunRequest(file="simple.yaml", scenario="scenarios/a.yaml", after_scenario="pause"),
                "scenarios/a.yaml",
            )
        assert (exc_unsup.value.status, exc_unsup.value.code) == (409, "launcher-unsupported")

        old_stop_intent, _, _ = await svc_old.build_intent(
            cfg_file,
            RunRequest(file="simple.yaml", scenario="scenarios/a.yaml", after_scenario="stop"),
            "scenarios/a.yaml",
        )
        assert "after_scenario" not in old_stop_intent

    asyncio.run(main())


def test_config_service_validates_and_forwards_record(tmp_path: Path) -> None:
    from vehicle_sil_control.protocol import request_text

    root = _simple_root(tmp_path)

    async def main() -> None:
        cfg_file = ConfigService(root, FakeLauncher()).resolve("simple.yaml")

        # 1. Valid record list is deduplicated preserving order and serialized by request_text()
        svc = ConfigService(root, FakeLauncher())
        intent, _, _ = await svc.build_intent(
            cfg_file,
            RunRequest(
                file="simple.yaml",
                record=["Dashboard.dashboard.speed", "Engine.engine.mode", "Dashboard.dashboard.speed"],
            ),
            None,
        )
        assert intent["record"] == ["Dashboard.dashboard.speed", "Engine.engine.mode"]
        serialized = json.loads(request_text(intent))
        assert serialized["record"] == ["Dashboard.dashboard.speed", "Engine.engine.mode"]

        # 2. Omitted or empty record stays omitted from intent
        intent_omitted, _, _ = await svc.build_intent(cfg_file, RunRequest(file="simple.yaml"), None)
        assert "record" not in intent_omitted
        intent_empty, _, _ = await svc.build_intent(cfg_file, RunRequest(file="simple.yaml", record=[]), None)
        assert "record" not in intent_empty

        # 3. Launcher without record-signals -> 409 launcher-unsupported for non-empty record, ok for empty
        no_rec_features = [f for f in FakeLauncher().features if f != "record-signals"]
        svc_old = ConfigService(root, FakeLauncher(features=no_rec_features))
        with pytest.raises(Refused) as exc_unsup:
            await svc_old.build_intent(
                cfg_file,
                RunRequest(file="simple.yaml", record=["Engine.engine.mode"]),
                None,
            )
        assert (exc_unsup.value.status, exc_unsup.value.code) == (409, "launcher-unsupported")

        old_empty, _, _ = await svc_old.build_intent(cfg_file, RunRequest(file="simple.yaml", record=[]), None)
        assert "record" not in old_empty

    asyncio.run(main())


def test_session_intent_preserves_record_in_summary(tmp_path: Path) -> None:
    from fake_runtime import in_process_gui

    root = _simple_root(tmp_path)

    async def main() -> None:
        async with in_process_gui(root=root) as gui:
            status, body = await gui.client.post(
                "api/run",
                {"file": "simple.yaml", "disable": [], "record": ["Dashboard.dashboard.speed"]},
            )
            assert status == 200, body
            s_status, s_body = await gui.client.get("api/session")
            assert s_status == 200
            assert s_body["session"]["intent"]["record"] == ["Dashboard.dashboard.speed"]
            await gui.runtime.runs[0].finish(returncode=0)

    asyncio.run(main())


def test_config_service_validates_and_forwards_record_after_fail_ms(tmp_path: Path) -> None:
    from vehicle_sil_control.protocol import request_text

    root = _simple_root(tmp_path)
    sc_dir = root / "scenarios"
    sc_dir.mkdir()
    (sc_dir / "fails.yaml").write_text("vehicle_sil_scenario: 1\nsteps: []\n", encoding="utf-8")

    async def main() -> None:
        cfg_file = ConfigService(root, FakeLauncher()).resolve("simple.yaml")

        # 1. Valid record_after_fail_ms is forwarded and serialized by request_text()
        svc = ConfigService(root, FakeLauncher())
        intent, _, _ = await svc.build_intent(
            cfg_file,
            RunRequest(file="simple.yaml", scenario="scenarios/fails.yaml", record_after_fail_ms=300),
            "scenarios/fails.yaml",
        )
        assert intent["record_after_fail_ms"] == 300
        serialized = json.loads(request_text(intent))
        assert serialized["record_after_fail_ms"] == 300

        # 2. Omitted stays omitted from intent
        intent_omitted, _, _ = await svc.build_intent(cfg_file, RunRequest(file="simple.yaml"), None)
        assert "record_after_fail_ms" not in intent_omitted

        # 3. Launcher without record-after-fail -> 409 launcher-unsupported
        no_tail_features = [f for f in FakeLauncher().features if f != "record-after-fail"]
        svc_old = ConfigService(root, FakeLauncher(features=no_tail_features))
        with pytest.raises(Refused) as exc_unsup:
            await svc_old.build_intent(
                cfg_file,
                RunRequest(file="simple.yaml", scenario="scenarios/fails.yaml", record_after_fail_ms=300),
                "scenarios/fails.yaml",
            )
        assert (exc_unsup.value.status, exc_unsup.value.code) == (409, "launcher-unsupported")

    asyncio.run(main())


def test_session_intent_preserves_record_after_fail_ms_in_summary(tmp_path: Path) -> None:
    from fake_runtime import in_process_gui

    root = _simple_root(tmp_path)
    sc_dir = root / "scenarios"
    sc_dir.mkdir()
    (sc_dir / "fails.yaml").write_text("vehicle_sil_scenario: 1\nsteps: []\n", encoding="utf-8")

    async def main() -> None:
        async with in_process_gui(root=root) as gui:
            status, body = await gui.client.post(
                "api/run",
                {
                    "file": "simple.yaml",
                    "disable": [],
                    "scenario": "scenarios/fails.yaml",
                    "record_after_fail_ms": 300,
                },
            )
            assert status == 200, body
            s_status, s_body = await gui.client.get("api/session")
            assert s_status == 200
            assert s_body["session"]["intent"]["record_after_fail_ms"] == 300
            await gui.runtime.runs[0].finish(returncode=0)

    asyncio.run(main())


def test_config_service_validates_and_forwards_record_events(tmp_path: Path) -> None:
    from vehicle_sil_control.protocol import request_text

    root = _simple_root(tmp_path)

    async def main() -> None:
        cfg_file = ConfigService(root, FakeLauncher()).resolve("simple.yaml")

        # 1. record_events=True is forwarded and serialized by request_text()
        svc = ConfigService(root, FakeLauncher())
        intent, _, _ = await svc.build_intent(
            cfg_file,
            RunRequest(file="simple.yaml", record_events=True),
            None,
        )
        assert intent["record_events"] is True
        serialized = json.loads(request_text(intent))
        assert serialized["record_events"] is True

        # 2. Omitted or False stays omitted from intent (even on launchers without record-events)
        no_ev_features = [f for f in FakeLauncher().features if f != "record-events"]
        svc_old = ConfigService(root, FakeLauncher(features=no_ev_features))
        intent_omitted, _, _ = await svc_old.build_intent(cfg_file, RunRequest(file="simple.yaml"), None)
        assert "record_events" not in intent_omitted
        intent_false, _, _ = await svc_old.build_intent(
            cfg_file, RunRequest(file="simple.yaml", record_events=False), None,
        )
        assert "record_events" not in intent_false

        # 3. Launcher without record-events + record_events=True -> 409 launcher-unsupported
        with pytest.raises(Refused) as exc_unsup:
            await svc_old.build_intent(cfg_file, RunRequest(file="simple.yaml", record_events=True), None)
        assert (exc_unsup.value.status, exc_unsup.value.code) == (409, "launcher-unsupported")

    asyncio.run(main())


def test_session_intent_preserves_record_events_in_summary(tmp_path: Path) -> None:
    from fake_runtime import in_process_gui

    root = _simple_root(tmp_path)

    async def main() -> None:
        async with in_process_gui(root=root) as gui:
            status, body = await gui.client.post(
                "api/run",
                {
                    "file": "simple.yaml",
                    "disable": [],
                    "record_events": True,
                },
            )
            assert status == 200, body
            s_status, s_body = await gui.client.get("api/session")
            assert s_status == 200
            assert s_body["session"]["intent"]["record_events"] is True
            await gui.runtime.runs[0].finish(returncode=0)

    asyncio.run(main())


def test_run_manager_write_variables_honors_handover_and_prestate_refusals(tmp_path: Path) -> None:
    from vehicle_sil_gui.session import Limits as SessionLimits
    from vehicle_sil_gui.source import PeerInfo

    class _StubSource:
        def start(self, sink) -> None:
            sink.on_peers([PeerInfo("Engine", "1", True, [{
                "name": "engine.demand", "type": "real", "role": "input", "writable": True,
                "unit": "%", "min": 0.0, "max": 100.0, "step": None, "description": "", "enum": None,
                "availability": None,
            }])], [])
            sink.on_synced()

        async def write(self, writes, at_ns, late, timeout):
            from vehicle_sil_gui.source import OperationInfo, TargetInfo
            self._last_peers = list(dict.fromkeys(w[0] for w in writes))
            return 1

        async def wait_queued(self, seq, timeout):
            from vehicle_sil_gui.source import OperationInfo, TargetInfo
            peers = getattr(self, "_last_peers", ["Engine"])
            return OperationInfo(seq, False, [TargetInfo(p, "queued") for p in peers])

        async def close(self) -> None:
            return

    root = _simple_root(tmp_path)
    (root / "scenarios").mkdir()
    (root / "scenarios" / "a.yaml").write_text("vehicle_sil_scenario: 1\nsteps: []\n", encoding="utf-8")

    async def fake_check(argv: list[str], request: Path, descriptions: Path | None = None,
                         text: bytes | None = None) -> tuple[int, dict]:
        return 0, default_check_result("simple.yaml")

    async def main() -> None:
        launcher = FakeLauncher()
        configs = ConfigService(root, launcher, check_runner=fake_check)
        runtime = FakeRuntime()
        stub_src = _StubSource()
        runs = RunManager(
            instance="inst-1", launcher=launcher, configs=configs, runtime=runtime,
            variables=True, source=lambda _addr, _tok: stub_src,
        )
        ws = WsGateway(runs, auth=False)
        runs.bind_hub(ws.hub, ws.wake_watches)
        try:
            req = RunRequest(file="simple.yaml", scenario="scenarios/a.yaml", after_scenario="pause")
            started = await runs.start_run(req, {"file": "simple.yaml", "scenario": "scenarios/a.yaml",
                                                 "after_scenario": "pause"})
            run = runs.current
            assert run is not None
            run.session.limits = SessionLimits(state_bytes=120)
            # Connect stub control variables directly
            run.variables.connect(stub_src)

            # 1. Before first state event (launcher_state is None): refused as scenario-running
            with pytest.raises(Refused) as exc_pre:
                await runs.write_variables({"run_id": started["run_id"], "writes": {"Engine": {"engine.demand": 10}}})
            assert (exc_pre.value.status, exc_pre.value.code) == (409, "scenario-running")

            # 2. During replay (launcher_state says scenario-running): refused
            run.session.feed_event({
                "event": "state", "seq": 1, "rev": 1, "handed_over": False, "scenario_over_at_ms": None,
                "allowed": {"write": {"ok": False, "reason": "scenario-running", "message": "scenario running"}},
            }, 80)
            with pytest.raises(Refused) as exc_rep:
                await runs.write_variables({"run_id": started["run_id"], "writes": {"Engine": {"engine.demand": 10}}})
            assert (exc_rep.value.status, exc_rep.value.code) == (409, "scenario-running")

            # 3. Oversized handover state (> state_bytes) leaves launcher_state stale, yet unlocks writes
            run.session.feed_event({
                "event": "state", "seq": 2, "rev": 2, "handed_over": True, "scenario_over_at_ms": 500,
                "allowed": {"write": {"ok": True, "reason": None, "message": None}},
                "outside": ["x" * 400],
            }, 600)
            assert run.session.scenario_handed_over is True
            assert run.session.launcher_state["rev"] == 1
            wrote = await runs.write_variables({"run_id": started["run_id"], "writes": {"Engine": {"engine.demand": 25}}})
            assert wrote == {"run_id": started["run_id"], "operation": 1}

            # 4. After normal state with not-running, still refused as not-running even when handed_over is True
            run.session.feed_event({
                "event": "state", "seq": 3, "rev": 3, "handed_over": True, "scenario_over_at_ms": 500,
                "allowed": {"write": {"ok": False, "reason": "not-running", "message": "not running"}},
            }, 80)
            with pytest.raises(Refused) as exc_nr:
                await runs.write_variables({"run_id": started["run_id"], "writes": {"Engine": {"engine.demand": 10}}})
            assert (exc_nr.value.status, exc_nr.value.code) == (409, "not-running")
        finally:
            await runs.shutdown_runtime()
            await ws.shutdown()
            await runs.cleanup_timelines()
            await configs.cleanup()

    asyncio.run(main())


def test_case_validation_feature_gate_and_root_relative_session_intent(tmp_path: Path) -> None:
    """case requires FEATURE_SCENARIO_CASES, rejects invalid values, writes config-relative scenario to
    launcher request while preserving root-relative Session.intent, and re-run still runs TOCTOU validation."""
    from pydantic import ValidationError
    from fake_runtime import DEFAULT_FEATURES
    from vehicle_sil_gui.launcher import FEATURE_SCENARIO_CASES

    root = tmp_path / "root"
    sub = root / "sub"
    (sub / "scenarios").mkdir(parents=True)
    cfg = sub / "simple.yaml"
    cfg.write_text(
        "vehicle_sil_simulation: 2\n"
        "networks:\n"
        "  - name: CAN1\n"
        "    connect:\n"
        "      lab: { backend: socketcan, channel: vcan0 }\n"
        "participants:\n"
        "  - name: Engine\n"
        "    command: [echo, ok]\n",
        encoding="utf-8",
    )
    (sub / "scenarios" / "cases.yaml").write_text(
        "vehicle_sil_scenario: 1\ncases:\n  load: {}\nsteps: []\n",
        encoding="utf-8",
    )

    # Validation: empty/null/non-string case and case+text rejected
    for bad in ("", None, 42, ["load"]):
        with pytest.raises(ValidationError):
            RunRequest.model_validate({"file": "sub/simple.yaml", "case": bad})

    # Feature gate: launcher without scenario-cases refuses with 409 launcher-unsupported
    async def check_feature_gate() -> None:
        no_cases_launcher = FakeLauncher(features=tuple(f for f in DEFAULT_FEATURES if f != FEATURE_SCENARIO_CASES))
        svc = ConfigService(root, no_cases_launcher)
        try:
            with pytest.raises(Refused) as exc:
                await svc.check({"file": "sub/simple.yaml", "scenario": "sub/scenarios/cases.yaml", "case": "load"})
            assert (exc.value.status, exc.value.code) == (409, "launcher-unsupported")
        finally:
            await svc.cleanup()

    asyncio.run(check_feature_gate())

    # Subdirectory coordinate system + Session.intent round-trip + TOCTOU connect validation on re-run
    seen_intents: list[dict] = []

    async def fake_check(argv: list[str], request: Path, descriptions: Path | None = None,
                         text: bytes | None = None) -> tuple[int, dict]:
        seen_intents.append(json.loads(request.read_text(encoding="utf-8")))
        res = default_check_result("sub/simple.yaml")
        res["networks"] = [{"name": "CAN1"}]
        return 0, res

    async def main() -> None:
        launcher = FakeLauncher()
        configs = ConfigService(root, launcher, check_runner=fake_check)
        runtime = FakeRuntime()
        runs = RunManager(instance="inst-1", launcher=launcher, configs=configs, runtime=runtime)
        ws = WsGateway(runs, auth=False)
        runs.bind_hub(ws.hub, ws.wake_watches)
        try:
            # case with text -> 400 invalid-field
            with pytest.raises(Refused) as bad_text:
                await configs.check({"file": "sub/simple.yaml", "text": cfg.read_text(encoding="utf-8"), "case": "load"})
            assert (bad_text.value.status, bad_text.value.code) == (400, "invalid-field")

            # Seed connect target list via check
            await configs.check({"file": "sub/simple.yaml"})
            # Manually populate connect targets allowlist for CAN1 -> ("lab",)
            st = configs.stamp("sub/simple.yaml")
            configs._targets[("sub/simple.yaml", "CAN1")] = (st, None, ("lab",))

            raw_req = {
                "file": "sub/simple.yaml",
                "scenario": "sub/scenarios/cases.yaml",
                "case": "load",
                "after_scenario": "pause",
                "params": {"rpm": 3000, "dry": False},
                "connect": {"CAN1": "lab"},
            }
            req = RunRequest.model_validate(raw_req)
            await runs.start_run(req, raw_req)
            run = runs.current
            assert run is not None
            # Launcher received config-relative scenario path and case
            sent = json.loads(runtime.runs[-1].spec.request.read_text(encoding="utf-8"))
            assert sent["scenario"] == "scenarios/cases.yaml"
            assert sent["case"] == "load"
            assert sent["after_scenario"] == "pause"

            # Feed check + result events to verify scenario.params metadata merging
            run.session.feed_event({
                "event": "check",
                "seq": 1,
                "check": {
                    "result": "passed",
                    "scenario": {
                        "file": "scenarios/cases.yaml",
                        "name": "cases",
                        "case": "load",
                        "steps": 1,
                        "params": [
                            {"name": "rpm", "value": 3000, "from": "cli", "type": "integer", "description": "target rpm"},
                            {"name": "dry", "value": False, "from": "cli", "type": "bool"},
                        ],
                    },
                },
            }, 200)
            run.session.feed_event({
                "event": "result",
                "seq": 2,
                "result": {
                    "result": "passed",
                    "end": "completed",
                    "ok": True,
                    "scenario": {
                        "file": "scenarios/cases.yaml",
                        "name": "cases",
                        "case": "load",
                        "params": [
                            {"name": "rpm", "value": 3000, "from": "cli"},
                            {"name": "dry", "value": False, "from": "cli"},
                        ],
                        "status": "passed",
                        "over_at_ms": 100,
                        "steps": [],
                    },
                },
            }, 200)

            summary = run.session.summary()
            # Session.intent keeps GUI API root-relative scenario path and exact re-run options
            assert summary["intent"] == {
                "scenario": "sub/scenarios/cases.yaml",
                "case": "load",
                "after_scenario": "pause",
                "params": {"rpm": 3000, "dry": False},
                "connect": {"CAN1": "lab"},
            }
            assert summary["scenario"]["case"] == "load"
            assert summary["scenario"]["params"] == [
                {"name": "rpm", "value": 3000, "from": "cli", "type": "integer", "description": "target rpm"},
                {"name": "dry", "value": False, "from": "cli", "type": "bool"},
            ]

            # End the first run so we can re-run from summary["intent"]
            await runtime.runs[-1].finish(returncode=0)
            rerun_payload = {"file": summary["file"], **summary["intent"]}
            rerun_req = RunRequest.model_validate(rerun_payload)
            await runs.start_run(rerun_req, rerun_payload)
            sent2 = json.loads(runtime.runs[-1].spec.request.read_text(encoding="utf-8"))
            assert sent2["scenario"] == "scenarios/cases.yaml"
            assert sent2["case"] == "load"

            # TOCTOU: if connect targets change on disk/allowlist, re-running via POST /api/run is still rejected
            await runtime.runs[-1].finish(returncode=0)
            configs._targets[("sub/simple.yaml", "CAN1")] = (st, None, ("other",))
            with pytest.raises(Refused) as toctou:
                await runs.start_run(rerun_req, rerun_payload)
            assert (toctou.value.status, toctou.value.code) == (400, "target-not-listed")
        finally:
            await runs.shutdown_runtime()
            await ws.shutdown()
            await runs.cleanup_timelines()
            await configs.cleanup()

    asyncio.run(main())
