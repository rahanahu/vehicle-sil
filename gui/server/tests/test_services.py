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
