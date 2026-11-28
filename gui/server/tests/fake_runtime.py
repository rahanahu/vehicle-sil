"""In-process fake LauncherRuntime and TestServer harness for deterministic GUI server tests."""

from __future__ import annotations

import asyncio
import json
from collections.abc import AsyncIterator, Awaitable, Callable, Sequence
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from pathlib import Path

import aiohttp
from aiohttp.test_utils import TestServer
from conftest import CONFIGS, Client, Gui, REPLAY

from vehicle_sil_gui import __version__
from vehicle_sil_gui.launcher import Launcher, REQUIRED_FEATURES
from vehicle_sil_gui.runtime import ConfigInspection, DEFAULT_ESCALATE_WAIT_S, HotDecision, RunSink, RunSpec
from vehicle_sil_gui.server import GuiServer, Options

DEFAULT_FEATURES = (
    *REQUIRED_FEATURES,
    "timeline",
    "run-stop-at",
    "scenario-end-ms",
    "scenario-handover",
    "scenario-cases",
    "scenario-templates",
    "control-links",
    "control-server",
    "external-registry",
    "record-signals",
)


class FakeLauncher(Launcher):
    """Launcher stand-in whose probe() succeeds in-memory without starting a subprocess."""

    def __init__(self, features: Sequence[str] = DEFAULT_FEATURES, path: Path = REPLAY):
        super().__init__(path=path)
        self.usable = True
        self.problems = []
        self.version = {
            "vehicle_sil_launch": 1,
            "vehicle_sil": __version__,
            "formats": {"check": 1, "run": 2, "events": 4, "describe": 1, "test": 1,
                        "timeline": 1, "connections": 1, "control_metrics": 1},
            "features": list(features),
        }

    async def probe(self) -> None:
        return


def default_check_result(config: str = "simple.yaml",
                         participants: Sequence[str] = ("Engine",)) -> dict:
    """Minimal valid `check --json` result that allows a run to start."""
    return {
        "vehicle_sil_check": 1,
        "vehicle_sil": __version__,
        "file": config,
        "result": "passed",
        "strict": False,
        "errors": 0,
        "warnings": 0,
        "notes": 0,
        "stop_at_ms": 1000,
        "time_step_ms": 10,
        "start_timeout_ms": 30000,
        "stop_timeout_ms": 10000,
        "registry_uri": None,
        "commands": None,
        "participants": [
            {"name": name, "enabled": True, "manual": False, "variables": []}
            for name in participants
        ],
        "networks": [],
        "findings": [],
        "file_sha256": "0" * 64,
        "hot_eligibility": {"eligible": True, "reason": None, "message": None},
    }


_VERSION_FORMATS = {
    "check": 1,
    "run": 2,
    "events": 4,
    "describe": 1,
    "test": 1,
    "timeline": 1,
    "connections": 1,
    "control_metrics": 1,
}

_LAUNCHER_STATES = frozenset({
    "unprepared", "cold_only", "preparing", "ready", "running", "ending", "contaminated",
})
_RUN_PHASES = frozenset({"checking", "starting", "waiting", "running", "stopping", "ended"})
_PACINGS = frozenset({"not-paced", "on-time", "late"})
_PARTICIPANT_STATES = frozenset({
    "disabled", "not_started", "started", "manual", "joined", "failed", "ended", "stop_requested", "killed",
})
_STATUS_KEYS = frozenset({
    "instance", "status_rev", "launcher_state", "config", "eligibility",
    "launcher_allowed", "active_run_id", "last_run_id", "run_state",
})
_RUN_STATE_KEYS = frozenset({
    "rev", "phase", "paused", "paused_at_ms", "holding", "speed", "pacing",
    "stop", "scenario", "handed_over", "scenario_over_at_ms", "allowed", "outside",
})
_RUN_ALLOWED_RANGE_KEYS: dict[str, tuple[tuple[str, str], bool]] = {
    "step": (("min_ms", "max_ms"), True),
    "speed": (("min", "max"), False),
}
_EVENTS_V4_OTHER_KINDS = frozenset({
    "phase", "check", "registry", "control_server", "control_link",
    "scenario_step", "log", "log_dropped", "outside", "finding", "time", "heartbeat",
})


def validate_version_json(doc: dict) -> None:
    """Validates `vehicle_sil_launch version --json` against the C++ wire contract."""
    assert isinstance(doc, dict), doc
    assert set(doc.keys()) == {"vehicle_sil_launch", "vehicle_sil", "formats", "features"}, doc
    assert doc["vehicle_sil_launch"] == 1, doc
    assert isinstance(doc["vehicle_sil"], str) and doc["vehicle_sil"], doc
    formats = doc["formats"]
    assert isinstance(formats, dict), doc
    assert set(formats.keys()) == set(_VERSION_FORMATS.keys()), formats
    for key, expected in _VERSION_FORMATS.items():
        assert type(formats[key]) is int and formats[key] == expected, (key, formats)
    features = doc["features"]
    assert isinstance(features, list) and all(isinstance(f, str) and f for f in features), features


def _validate_allowed_entry(
    entry: object,
    *,
    range_keys: tuple[str, str] | None = None,
    whole: bool = False,
) -> None:
    assert isinstance(entry, dict), entry
    base = {"ok", "reason", "message", "caution"}
    keys = set(entry.keys())
    if range_keys is not None and (range_keys[0] in keys or range_keys[1] in keys):
        assert keys == base | set(range_keys), entry
        for rk in range_keys:
            val = entry[rk]
            if whole:
                assert type(val) is int, (rk, entry)
            else:
                assert type(val) in (int, float), (rk, entry)
    else:
        assert keys == base, entry
    assert isinstance(entry["ok"], bool), entry
    assert entry["reason"] is None or isinstance(entry["reason"], str), entry
    assert entry["message"] is None or isinstance(entry["message"], str), entry
    caution = entry["caution"]
    if caution is not None:
        assert isinstance(caution, dict) and set(caution.keys()) == {"code", "message"}, entry
        assert isinstance(caution["code"], str) and isinstance(caution["message"], str), entry


def _validate_run_state(rs: object, *, envelope: bool = False) -> None:
    assert isinstance(rs, dict), rs
    expected_keys = _RUN_STATE_KEYS | ({"event", "seq", "t_ms", "run"} if envelope else set())
    if envelope and "t_ms" not in rs:
        expected_keys = expected_keys - {"t_ms"}
    assert set(rs.keys()) == expected_keys, rs
    assert type(rs["rev"]) is int and rs["rev"] >= 0, rs
    assert rs["phase"] in _RUN_PHASES, rs
    assert isinstance(rs["paused"], bool), rs
    assert rs["paused_at_ms"] is None or type(rs["paused_at_ms"]) is int, rs
    assert isinstance(rs["holding"], bool), rs
    speed = rs["speed"]
    assert speed is None or speed == "max" or type(speed) in (int, float), rs
    assert rs["pacing"] in _PACINGS, rs
    stop = rs["stop"]
    assert isinstance(stop, dict) and set(stop.keys()) == {"requests", "by"}, rs
    assert type(stop["requests"]) is int and stop["requests"] >= 0, rs
    assert stop["by"] is None or isinstance(stop["by"], str), rs
    assert isinstance(rs["scenario"], bool), rs
    assert isinstance(rs["handed_over"], bool), rs
    assert rs["scenario_over_at_ms"] is None or (
        type(rs["scenario_over_at_ms"]) is int and rs["scenario_over_at_ms"] >= 0
    ), rs
    allowed = rs["allowed"]
    assert isinstance(allowed, dict), rs
    assert set(allowed.keys()) == {"pause", "resume", "step", "speed", "run_until", "stop", "write"}, rs
    for op, entry in allowed.items():
        range_spec = _RUN_ALLOWED_RANGE_KEYS.get(op)
        if range_spec is not None:
            _validate_allowed_entry(entry, range_keys=range_spec[0], whole=range_spec[1])
        else:
            _validate_allowed_entry(entry)
    outside = rs["outside"]
    assert isinstance(outside, list), rs
    for row in outside:
        assert isinstance(row, dict), row
        assert set(row.keys()) == {
            "network", "target", "participant", "link", "bus", "nothing_received", "condition", "reasons",
        }, row
        assert isinstance(row["network"], str) and isinstance(row["target"], str), row
        assert isinstance(row["participant"], str) and isinstance(row["condition"], str), row
        assert row["link"] is None or isinstance(row["link"], str), row
        assert row["bus"] is None or isinstance(row["bus"], str), row
        assert isinstance(row["nothing_received"], bool), row
        assert isinstance(row["reasons"], list) and all(isinstance(r, str) for r in row["reasons"]), row


def _validate_status_members(st: object, *, exact_keys: bool = True) -> None:
    assert isinstance(st, dict), st
    if exact_keys:
        assert set(st.keys()) == _STATUS_KEYS, st
    else:
        assert _STATUS_KEYS <= set(st.keys()), st
    assert isinstance(st["instance"], str) and st["instance"], st
    assert type(st["status_rev"]) is int and st["status_rev"] >= 0, st
    assert st["launcher_state"] in _LAUNCHER_STATES, st
    assert st["config"] is None or isinstance(st["config"], str), st
    elig = st["eligibility"]
    assert isinstance(elig, dict) and set(elig.keys()) == {"eligible", "reason", "message"}, st
    assert isinstance(elig["eligible"], bool), st
    assert elig["reason"] is None or isinstance(elig["reason"], str), st
    assert elig["message"] is None or isinstance(elig["message"], str), st
    la = st["launcher_allowed"]
    assert isinstance(la, dict) and set(la.keys()) == {"prepare", "run"}, st
    _validate_allowed_entry(la["prepare"])
    _validate_allowed_entry(la["run"])
    assert st["active_run_id"] is None or (isinstance(st["active_run_id"], str) and st["active_run_id"]), st
    assert st["last_run_id"] is None or (isinstance(st["last_run_id"], str) and st["last_run_id"]), st
    if st["run_state"] is not None:
        _validate_run_state(st["run_state"], envelope=False)


def validate_events_v4_event(ev: dict) -> None:
    """Validates that a launcher event dictionary conforms to the Events v4 wire contract."""
    assert isinstance(ev, dict) and isinstance(ev.get("event"), str), ev
    assert type(ev.get("seq")) is int and ev["seq"] >= 0, ev
    envelope = {"event", "seq"}
    if "t_ms" in ev:
        assert type(ev["t_ms"]) is int and ev["t_ms"] >= 0, ev
        envelope.add("t_ms")
    kind = ev["event"]
    if kind == "hello":
        assert set(ev.keys()) == envelope | {
            "vehicle_sil_events", "vehicle_sil", "pid", "file", "arguments", "launcher",
        }, ev
        assert ev["vehicle_sil_events"] == 4, ev
        assert isinstance(ev["vehicle_sil"], str) and ev["vehicle_sil"], ev
        assert type(ev["pid"]) is int, ev
        assert ev["file"] is None or isinstance(ev["file"], str), ev
        assert isinstance(ev["arguments"], list) and all(isinstance(a, str) for a in ev["arguments"]), ev
        assert isinstance(ev["launcher"], str) and ev["launcher"], ev
    elif kind == "status":
        assert set(ev.keys()) == envelope | {"id"} | _STATUS_KEYS, ev
        assert ev["id"] is None or type(ev["id"]) is int, ev
        _validate_status_members(ev, exact_keys=False)
    elif kind == "prepare":
        assert set(ev.keys()) == envelope | {
            "id", "accepted", "reason", "message", "status", "metadata", "findings",
        }, ev
        assert ev["id"] is None or type(ev["id"]) is int, ev
        assert isinstance(ev["accepted"], bool), ev
        assert ev["reason"] is None or isinstance(ev["reason"], str), ev
        assert ev["message"] is None or isinstance(ev["message"], str), ev
        if ev["accepted"]:
            assert ev["reason"] is None and ev["message"] is None, ev
            meta = ev["metadata"]
            assert isinstance(meta, dict) and set(meta.keys()) == {
                "participants", "networks", "variants", "file_sha256",
            }, ev
            assert isinstance(meta["participants"], list) and all(isinstance(p, str) for p in meta["participants"]), ev
            assert isinstance(meta["networks"], list) and all(isinstance(n, str) for n in meta["networks"]), ev
            assert isinstance(meta["variants"], list) and all(isinstance(v, str) for v in meta["variants"]), ev
            assert meta["file_sha256"] is None or isinstance(meta["file_sha256"], str), ev
        else:
            assert ev["metadata"] is None, ev
        _validate_status_members(ev["status"], exact_keys=True)
        assert isinstance(ev["findings"], list), ev
    elif kind == "run_start":
        assert set(ev.keys()) == envelope | {"run", "id"}, ev
        assert isinstance(ev["run"], str) and ev["run"], ev
        assert ev["id"] is None or type(ev["id"]) is int, ev
    elif kind == "command":
        required = envelope | {"command", "source", "accepted", "message", "reason", "id"}
        optional = {"run", "ms", "at_ms", "asked_run"}
        assert required <= set(ev.keys()) <= required | optional, ev
        assert ev["command"] is None or isinstance(ev["command"], str), ev
        assert isinstance(ev["source"], str) and ev["source"], ev
        assert isinstance(ev["accepted"], bool), ev
        assert ev["message"] is None or isinstance(ev["message"], str), ev
        assert ev["reason"] is None or isinstance(ev["reason"], str), ev
        if ev["accepted"]:
            assert ev["reason"] is None, ev
        assert ev["id"] is None or type(ev["id"]) is int, ev
        if "run" in ev:
            assert isinstance(ev["run"], str) and ev["run"], ev
        if "ms" in ev:
            assert type(ev["ms"]) is int, ev
        if "at_ms" in ev:
            assert type(ev["at_ms"]) is int, ev
        if "asked_run" in ev:
            assert ev["asked_run"] is None or isinstance(ev["asked_run"], str), ev
    elif kind == "state":
        assert isinstance(ev.get("run"), str) and ev["run"], ev
        _validate_run_state(ev, envelope=True)
    elif kind == "participant":
        base = envelope | {"run", "name", "state"}
        assert base <= set(ev.keys()), ev
        assert isinstance(ev["run"], str) and ev["run"], ev
        assert isinstance(ev["name"], str) and ev["name"], ev
        pstate = ev["state"]
        assert pstate in _PARTICIPANT_STATES, ev
        if pstate == "started":
            assert set(ev.keys()) == base | {"pid", "command", "log"}, ev
            assert type(ev["pid"]) is int, ev
            assert isinstance(ev["command"], list) and all(isinstance(w, str) for w in ev["command"]), ev
            assert ev["log"] is None or isinstance(ev["log"], str), ev
        elif pstate == "ended":
            assert set(ev.keys()) == base | {"exit_code", "signal"}, ev
            assert (type(ev["exit_code"]) is int and ev["signal"] is None) or (
                ev["exit_code"] is None and isinstance(ev["signal"], str)
            ), ev
        elif pstate == "manual":
            req_m = base | {"command", "working_directory", "command_line"}
            assert req_m <= set(ev.keys()) <= req_m | {"environment"}, ev
        elif pstate == "failed":
            assert base | {"reason"} <= set(ev.keys()) <= base | {"reason", "message"}, ev
        elif pstate == "not_started":
            assert base <= set(ev.keys()) <= base | {"message"}, ev
        else:
            assert set(ev.keys()) == base, ev
    elif kind == "result":
        req_r = envelope | {"result", "launcher_ends", "status"}
        assert req_r <= set(ev.keys()) <= req_r | {"run"}, ev
        assert isinstance(ev["result"], dict), ev
        assert isinstance(ev["launcher_ends"], bool), ev
        _validate_status_members(ev["status"], exact_keys=True)
    elif kind in _EVENTS_V4_OTHER_KINDS:
        pass
    else:
        raise AssertionError(f"unknown Events v4 kind {kind!r}: {ev!r}")


class FakeRun:
    """In-memory RunHandle driven directly by tests."""

    waits_for_check = False

    def __init__(self, spec: RunSpec, *, pid: int, on_done: Callable[[FakeRun], None]):
        self.spec = spec
        self._pid = pid
        self._on_done = on_done
        self.sink: RunSink | None = None
        self.started = asyncio.Event()
        self.finished = asyncio.Event()
        self.stops: list[str] = []
        self.stop_event = asyncio.Event()
        self.time_commands: list[dict] = []
        self.input_open = True
        self.drain_ok = True
        self.drain_gate: asyncio.Event | None = None
        self.shutdown_requested = asyncio.Event()
        self.escalated = False
        self.killed = False
        self._process_exited = False
        self._seq = 0
        self._t_ms = 0

    @property
    def pid(self) -> int | None:
        return self._pid

    @property
    def run_id(self) -> str | None:
        return None

    @property
    def mode(self) -> str:
        return "cold"

    @property
    def control_token(self) -> str | None:
        return None

    def start(self, sink: RunSink) -> None:
        self.sink = sink
        self.started.set()

    def can_send(self) -> bool:
        return self.input_open

    def send_stop(self, *, force: bool = False) -> bool:
        if not self.input_open:
            return False
        self.stops.append("force" if force else "graceful")
        self.stop_event.set()
        return True

    def send_time(self, command: dict) -> bool:
        if not self.input_open:
            return False
        self.time_commands.append(dict(command))
        return True

    async def drain(self, timeout_s: float) -> bool:
        if self.drain_gate is not None:
            try:
                async with asyncio.timeout(timeout_s):
                    await self.drain_gate.wait()
            except TimeoutError:
                return True
        return self.drain_ok

    def emit(self, event: str, **fields) -> dict:
        """Emits one launcher event to the server sink."""
        return self.emit_event(event, **fields)

    def emit_event(self, event: str, **fields) -> dict:
        """Emits one already-parsed launcher event dict to the server sink."""
        assert self.sink is not None
        payload = {"event": event, "seq": self._seq, "t_ms": self._t_ms, **fields}
        self._seq += 1
        self._t_ms += 10
        size = len(json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8", "surrogatepass"))
        self.sink.on_stdout_event(payload, size)
        return payload

    def emit_raw(self, line: str | None) -> None:
        assert self.sink is not None
        self.sink.on_stdout_line(line)

    def emit_stderr(self, line: str) -> None:
        assert self.sink is not None
        self.sink.on_stderr_line(line)

    def emit_problem(self, code: str, message: str, detail: object = None) -> None:
        assert self.sink is not None
        self.sink.on_problem(code, message, detail)

    def exit_process(self, returncode: int | None = 0) -> None:
        assert self.sink is not None
        if not self._process_exited:
            self._process_exited = True
            self.input_open = False
            self.sink.on_process_exited(returncode)

    async def finish(self, *, returncode: int | None = 0, output_cut: bool = False,
                     process_exited: bool = True) -> None:
        """Ends the run cleanly or as a crash, optionally keeping the process alive."""
        if self.finished.is_set():
            return
        assert self.sink is not None
        if process_exited:
            self.exit_process(returncode)
        await self.sink.on_run_finished(output_cut=output_cut)
        await self.sink.on_run_cleanup()
        self.finished.set()
        self._on_done(self)


class FakeRuntime:
    """In-memory LauncherRuntime that records starts, stops, shutdown escalation, and kill_all."""

    def __init__(self, *, escalate_wait_s: float = DEFAULT_ESCALATE_WAIT_S):
        self.runs: list[FakeRun] = []
        self._active: set[FakeRun] = set()
        self._next_pid = 10000
        self.start_gate: asyncio.Event | None = None
        self.start_entered = asyncio.Event()
        self.start_error: BaseException | None = None
        self.shutdown_auto_finish = True
        self.escalate_wait_s = escalate_wait_s
        self.ignore_kill = False
        self.kill_all_called = 0

    async def start_run(self, spec: RunSpec) -> FakeRun:
        self.start_entered.set()
        if self.start_gate is not None:
            await self.start_gate.wait()
        if self.start_error is not None:
            raise self.start_error
        self._next_pid += 1
        run = FakeRun(spec, pid=self._next_pid, on_done=self._active.discard)
        self.runs.append(run)
        self._active.add(run)
        return run

    async def shutdown(self, hurry: asyncio.Event, stop_timeout_s: float | None) -> None:
        await asyncio.gather(*(self._stop_run(run, hurry, stop_timeout_s) for run in list(self._active)))

    async def _stop_run(self, run: FakeRun, hurry: asyncio.Event, stop_timeout_s: float | None) -> None:
        run.shutdown_requested.set()
        run.input_open = False
        if self.shutdown_auto_finish:
            await run.finish(returncode=0)
            return
        if not await _wait_run_or_hurry(run, hurry, stop_timeout_s):
            why = ("shutdown was requested again" if hurry.is_set() or stop_timeout_s is None else
                   f"the launcher did not stop in {stop_timeout_s:g} s after its input closed")
            run.escalated = True
            if run.sink is not None:
                run.sink.on_problem("stop-escalated", f"{why}: the launcher was asked to stop "
                                    "(SIGTERM / Ctrl+Break)", None)
            if not await _wait_run_or_hurry(run, hurry, self.escalate_wait_s) and not self.ignore_kill:
                run.killed = True
                await run.finish(returncode=-9)

    def kill_all(self) -> None:
        self.kill_all_called += 1

    def active_pids(self) -> list[int]:
        return [run.pid for run in list(self._active) if run.pid is not None]

    def sync_launcher(self) -> None:
        return

    def status(self) -> dict:
        return {
            "mode": "cold",
            "state": "running" if self._active else "cold",
            "config": None,
            "reason": "fake-runtime",
            "message": None,
        }

    def execution_status(self) -> dict | None:
        return None

    async def prepare(self, config: str, *, variant: str | None = None, disable: tuple[str, ...] = (),
                      enable: tuple[str, ...] = (), manual: tuple[str, ...] = (),
                      want_control: bool = True) -> dict:
        return self.status()

    async def inspect_config(self, config: str, *, variant: str | None = None, disable: tuple[str, ...] = (),
                             enable: tuple[str, ...] = (), manual: tuple[str, ...] = (),
                             want_control: bool = True, need_metadata: bool = True,
                             allow_prepare_on_resident: bool = True) -> ConfigInspection:
        return ConfigInspection(hot=HotDecision(False, "fake-runtime"))

    async def release_config(self, config: str) -> None:
        return


async def _wait_run_or_hurry(run: FakeRun, hurry: asyncio.Event, seconds: float | None) -> bool:
    if run.finished.is_set():
        return True
    hurry_task = asyncio.create_task(hurry.wait())
    done_task = asyncio.create_task(run.finished.wait())
    try:
        await asyncio.wait({hurry_task, done_task}, timeout=seconds, return_when=asyncio.FIRST_COMPLETED)
        return run.finished.is_set()
    finally:
        for task in (hurry_task, done_task):
            if not task.done():
                task.cancel()
        await asyncio.gather(hurry_task, done_task, return_exceptions=True)


@dataclass
class InProcessGui:
    server: GuiServer
    runtime: FakeRuntime
    client: Client
    port: int
    token: str


@asynccontextmanager
async def in_process_gui(
    root: Path = CONFIGS,
    *,
    runtime: FakeRuntime | None = None,
    features: Sequence[str] = DEFAULT_FEATURES,
    check_fn: Callable[[list[str], Path, Path | None, bytes | None], Awaitable[tuple[int, dict]]] | None = None,
    variables: bool | None = None,
    source: Callable[[str, str], object] | None = None,
    variable_limits: object | None = None,
    stop_timeout_s: float | None = 1.0,
    silent_after_s: float = 5.0,
    token: str = "test-token",
) -> AsyncIterator[InProcessGui]:
    """Starts GuiServer over aiohttp TestServer with an in-memory LauncherRuntime."""
    from conftest import free_port
    port = free_port()

    fake_runtime = runtime or FakeRuntime()
    use_variables = (source is not None) if variables is None else variables
    async def fake_check(argv: list[str], request: Path, descriptions: Path | None = None,
                         text: bytes | None = None) -> tuple[int, dict]:
        if check_fn is not None:
            return await check_fn(argv, request, descriptions, text)
        return 0, default_check_result(argv[-1])

    extra = {"variable_limits": variable_limits} if variable_limits is not None else {}
    server = GuiServer(Options(
        root=root,
        launcher=FakeLauncher(features=features),
        port=port,
        token=token,
        stop_timeout_s=stop_timeout_s,
        silent_after_s=silent_after_s,
        variables=use_variables,
        source=source,
        runtime=fake_runtime,
        check_runner=fake_check,
        **extra,
    ))
    test_server = TestServer(server.app(), host="127.0.0.1", port=port)
    await test_server.start_server()
    http = aiohttp.ClientSession()
    gui = Gui(proc=None, port=port, token=token, url=f"http://127.0.0.1:{port}/#token={token}",
              stderr=root / ".unused.err")
    try:
        yield InProcessGui(server=server, runtime=fake_runtime, client=Client(gui, http),
                           port=port, token=token)
    finally:
        await http.close()
        await server.shutdown()
        await test_server.close()
