"""Real vehicle_sil_launch integration tests against the GUI server and Events v4 contract.

Runs only when VEHICLE_SIL_GUI_LAUNCHER names a real vehicle_sil_launch (scripts/verify-gui.py --launcher
<path>) and the ecu_main example ECUs are built next to it (or VEHICLE_SIL_GUI_EXAMPLES names their
directory); otherwise every test here is skipped, with the reason. Linux only.
"""

from __future__ import annotations

import json
import os
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest
from conftest import REPLAY, Client, alive, gui_server, is_ended, is_synced, run_async, scaled, wait_until
from fake_runtime import validate_events_v4_event, validate_version_json
from real_configs import NETWORKS, examples_dir, executables, usable, write
from vehicle_sil_gui import __version__
from vehicle_sil_gui.recording import Recording, scenario_yaml

TESTS = Path(__file__).resolve().parent
LAUNCHER = os.environ.get("VEHICLE_SIL_GUI_LAUNCHER")
EXAMPLES = examples_dir(LAUNCHER)
REASON = ("no real launcher: set VEHICLE_SIL_GUI_LAUNCHER to build/<preset>/tools/launch/vehicle_sil_launch "
          "(scripts/verify-gui.py --launcher <path>)" if not LAUNCHER or not Path(LAUNCHER).is_file()
          else f"no ecu_main example executables in {EXAMPLES}: build them, or set VEHICLE_SIL_GUI_EXAMPLES"
          if not usable(EXAMPLES) else "Linux only" if sys.platform != "linux" else None)
pytestmark = pytest.mark.skipif(REASON is not None, reason=REASON or "")

RUN_TIMEOUT_S = scaled(60)


class _LineReader:
    """Timeout-bounded line reader over an unbuffered binary pipe (avoids TextIOWrapper buffering with select)."""

    def __init__(self, stream) -> None:
        self._fd = stream.fileno()
        self._buf = bytearray()

    def readline(self, timeout_s: float) -> str:
        deadline = time.monotonic() + timeout_s
        while True:
            nl = self._buf.find(b"\n")
            if nl != -1:
                line = bytes(self._buf[:nl + 1])
                del self._buf[:nl + 1]
                return line.decode("utf-8")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"launcher stdout line timed out after {timeout_s:g} s")
            ready, _, _ = select.select([self._fd], [], [], remaining)
            if not ready:
                raise TimeoutError(f"launcher stdout line timed out after {timeout_s:g} s")
            chunk = os.read(self._fd, 4096)
            if not chunk:
                if self._buf:
                    line = bytes(self._buf)
                    self._buf.clear()
                    return line.decode("utf-8")
                return ""
            self._buf.extend(chunk)


def _read_event(reader: _LineReader, proc: subprocess.Popen, timeout_s: float = RUN_TIMEOUT_S) -> dict:
    line = reader.readline(timeout_s)
    assert line, proc.stderr.read().decode("utf-8", "replace") if proc.stderr else ""
    ev = json.loads(line)
    validate_events_v4_event(ev)
    return ev


def _send_cmd(proc: subprocess.Popen, payload: dict) -> None:
    assert proc.stdin is not None
    proc.stdin.write((json.dumps(payload) + "\n").encode("utf-8"))
    proc.stdin.flush()


def _run_persistent_serve_sequence(cmd_prefix: list[str], config: Path, req_path: Path, *,
                                   timeline_req_path: Path, timeline_path: Path,
                                   env: dict[str, str] | None = None) -> list[dict]:
    proc = subprocess.Popen(
        [*cmd_prefix, "serve", "--", str(config)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
        env=env,
    )
    events: list[dict] = []
    try:
        assert proc.stdout is not None and proc.stdin is not None
        reader = _LineReader(proc.stdout)
        for _ in range(2):
            events.append(_read_event(reader, proc))
        assert [e["event"] for e in events] == ["hello", "status"]
        _send_cmd(proc, {"command": "prepare", "id": 1})
        while True:
            ev = _read_event(reader, proc)
            events.append(ev)
            if ev["event"] == "prepare":
                assert ev["accepted"] is True, ev
                break
        _send_cmd(proc, {"command": "run", "request": str(req_path), "id": 2})
        while True:
            ev = _read_event(reader, proc)
            events.append(ev)
            if ev["event"] == "result":
                first_result = ev
                break
        # Immediately query status: proves no unsolicited duplicate post-result status event was emitted.
        _send_cmd(proc, {"command": "status", "id": 3})
        while True:
            ev = _read_event(reader, proc)
            events.append(ev)
            if ev["event"] == "heartbeat":
                continue
            assert ev["event"] == "status" and ev["id"] == 3, ev
            assert ev["status_rev"] == first_result["status"]["status_rev"], ev
            assert ev["launcher_state"] == first_result["status"]["launcher_state"], ev
            break
        # Second run with timeline: proves run_start does not emit an extraneous "timeline" key.
        _send_cmd(proc, {"command": "run", "request": str(timeline_req_path),
                         "timeline": str(timeline_path), "id": 4})
        while True:
            ev = _read_event(reader, proc)
            events.append(ev)
            if ev["event"] == "result":
                break
        proc.stdin.close()
        assert proc.wait(timeout=RUN_TIMEOUT_S) == 0
        return events
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


def _run_once_stop_sequence(cmd_prefix: list[str], config: Path, req_path: Path, *,
                            env: dict[str, str] | None = None) -> list[dict]:
    proc = subprocess.Popen(
        [*cmd_prefix, "serve", "--once", "--", str(config)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
        env=env,
    )
    events: list[dict] = []
    try:
        assert proc.stdout is not None and proc.stdin is not None
        reader = _LineReader(proc.stdout)
        for _ in range(2):
            events.append(_read_event(reader, proc))
        assert [e["event"] for e in events] == ["hello", "status"]
        _send_cmd(proc, {"command": "run", "request": str(req_path), "id": 10})
        run_id: str | None = None
        while True:
            ev = _read_event(reader, proc)
            events.append(ev)
            if ev["event"] == "run_start":
                run_id = ev["run"]
            elif ev["event"] == "state" and ev.get("phase") == "running" and ev.get("paused") is True:
                break
        assert run_id is not None
        _send_cmd(proc, {"command": "step", "run": run_id, "ms": 20, "id": 11})
        while True:
            ev = _read_event(reader, proc)
            events.append(ev)
            if ev["event"] == "command" and ev.get("id") == 11:
                assert ev["accepted"] is True, ev
                break
        # Two stop commands without force: must stay graceful (idempotent, stop.requests <= 1).
        # Deterministic double-stop on a hung participant is also verified in test_units.py.
        proc.stdin.write(
            (json.dumps({"command": "stop", "run": run_id, "id": 12}) + "\n"
             + json.dumps({"command": "stop", "run": run_id, "id": 13}) + "\n").encode("utf-8")
        )
        proc.stdin.flush()
        while True:
            ev = _read_event(reader, proc)
            events.append(ev)
            if ev["event"] == "result":
                break
        proc.stdin.close()
        assert proc.wait(timeout=RUN_TIMEOUT_S) == 0
        return events
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


def test_real_targeted_stop_does_not_stop_the_next_run(tmp_path: Path) -> None:
    """A delayed stop for one real launcher must not stop its successor."""
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)

    async def talk(c: Client) -> None:
        _, app = await c.get("api/app")
        assert app["version"] == __version__
        assert app["launcher"]["version"]["vehicle_sil"] == __version__
        sock = await c.ws()
        await sock.until(is_synced)
        status, first = await c.post("api/run", {"file": "forever.yaml"})
        assert status == 200, first
        first_id, instance = first["run_id"], first["instance"]
        await sock.until(lambda m: m.get("event", {}).get("event") == "time")
        status, _ = await c.post("api/stop-session", {"run_id": first_id, "instance": instance})
        assert status == 200
        await sock.until(is_ended, timeout=60)

        status, second = await c.post("api/run", {"file": "forever.yaml"})
        assert status == 200, second
        second_id = second["run_id"]
        assert second_id != first_id
        await sock.until(lambda m: m.get("event", {}).get("event") == "time")
        status, stale = await c.post("api/stop-session", {"run_id": first_id, "instance": instance})
        assert status == 409 and stale["error"]["code"] == "session-changed", stale
        status, current = await c.get("api/session")
        assert status == 200 and current["session"]["id"] == second_id
        assert current["session"]["stop"] == "none"
        assert alive(current["session"]["launcher"]["pid"])
        status, stopped = await c.post("api/stop-session", {"run_id": second_id, "instance": instance})
        assert status == 200 and stopped["run_id"] == second_id, stopped
        await sock.until(is_ended, timeout=60)
        await sock.close()

    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        run_async(talk, gui)


def test_real_launcher_events_v4_contract_and_stub_parity(tmp_path: Path) -> None:
    """Real vehicle_sil_launch and stub_launcher.py satisfy the Events v4 / version --json wire contract in parity."""
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    stub_env = {**os.environ, "REPLAY_HEARTBEAT_MS": "0", "REPLAY_NO_SLEEPERS": "1"}
    real_prefix = [str(LAUNCHER)]
    stub_prefix = [sys.executable, str(REPLAY)]

    # 1. version --json parity
    real_ver_proc = subprocess.run([*real_prefix, "version", "--json"], capture_output=True, text=True,
                                   timeout=RUN_TIMEOUT_S, check=True)
    stub_ver_proc = subprocess.run([*stub_prefix, "version", "--json"], capture_output=True, text=True,
                                   timeout=RUN_TIMEOUT_S, check=True, env=stub_env)
    real_ver = json.loads(real_ver_proc.stdout)
    stub_ver = json.loads(stub_ver_proc.stdout)
    validate_version_json(real_ver)
    validate_version_json(stub_ver)
    assert set(real_ver.keys()) == set(stub_ver.keys())
    assert real_ver["formats"] == stub_ver["formats"]

    # 2. Persistent serve parity (startup -> prepare -> run -> status -> timeline run -> result)
    req_path = tmp_path / "req.json"
    req_path.write_text(
        json.dumps({"vehicle_sil_run_request": 1, "speed": "max", "stop_at_ms": 100}),
        encoding="utf-8",
    )
    timeline_req_path = tmp_path / "timeline_req.json"
    timeline_req_path.write_text(
        json.dumps({"vehicle_sil_run_request": 1, "speed": "max", "scenario": "scenarios/throttle_step.yaml"}),
        encoding="utf-8",
    )
    real_events = _run_persistent_serve_sequence(
        real_prefix, configs / "simulation.yaml", req_path,
        timeline_req_path=timeline_req_path, timeline_path=tmp_path / "real.timeline.json",
    )
    stub_events = _run_persistent_serve_sequence(
        stub_prefix, configs / "simulation.yaml", req_path,
        timeline_req_path=timeline_req_path, timeline_path=tmp_path / "stub.timeline.json", env=stub_env,
    )

    def first_of(evts: list[dict], kind: str) -> dict:
        return next(e for e in evts if e["event"] == kind)

    # Initial hello + pre-prepare status parity
    assert set(real_events[0].keys()) == set(stub_events[0].keys())
    real_init_st, stub_init_st = real_events[1], stub_events[1]
    assert set(real_init_st.keys()) == set(stub_init_st.keys())
    for st in (real_init_st, stub_init_st):
        assert st["launcher_state"] == "unprepared", st
        assert st["status_rev"] == 0, st
        assert st["eligibility"] == {"eligible": False, "reason": None, "message": None}, st
        assert st["launcher_allowed"]["prepare"] == {"ok": True, "reason": None, "message": None, "caution": None}, st
        assert st["launcher_allowed"]["run"] == {"ok": True, "reason": None, "message": None, "caution": None}, st

    # prepare parity
    real_prep, stub_prep = first_of(real_events, "prepare"), first_of(stub_events, "prepare")
    assert set(real_prep.keys()) == set(stub_prep.keys())
    assert set(real_prep["status"].keys()) == set(stub_prep["status"].keys())
    assert set(real_prep["metadata"].keys()) == set(stub_prep["metadata"].keys())
    for prep in (real_prep, stub_prep):
        assert prep["accepted"] is True and prep["reason"] is None and prep["message"] is None, prep
        assert prep["status"]["launcher_state"] == "ready", prep
        assert prep["status"]["eligibility"] == {"eligible": True, "reason": None, "message": None}, prep
        assert prep["metadata"]["participants"] == ["Engine", "Dashboard"], prep
        assert prep["metadata"]["networks"] == ["J1939_CAN"], prep

    # run_start parity (both plain run and timeline run have no extra timeline key)
    real_rs_all = [e for e in real_events if e["event"] == "run_start"]
    stub_rs_all = [e for e in stub_events if e["event"] == "run_start"]
    assert len(real_rs_all) == 2 and len(stub_rs_all) == 2
    for rs in (*real_rs_all, *stub_rs_all):
        assert set(rs.keys()) == {"event", "seq", "t_ms", "run", "id"}, rs
    real_rs, stub_rs = real_rs_all[0], stub_rs_all[0]

    # running status + state + participant parity
    real_run_st = next(e for e in real_events if e["event"] == "status" and e["launcher_state"] == "running")
    stub_run_st = next(e for e in stub_events if e["event"] == "status" and e["launcher_state"] == "running")
    assert set(real_run_st.keys()) == set(stub_run_st.keys())
    assert set(real_run_st["run_state"].keys()) == set(stub_run_st["run_state"].keys())
    assert set(real_run_st["run_state"]["allowed"].keys()) == set(stub_run_st["run_state"]["allowed"].keys())
    assert real_run_st["launcher_allowed"]["prepare"]["ok"] is False
    assert stub_run_st["launcher_allowed"]["prepare"]["ok"] is False
    assert real_run_st["launcher_allowed"]["run"]["ok"] is False
    assert stub_run_st["launcher_allowed"]["run"]["ok"] is False

    real_state, stub_state = first_of(real_events, "state"), first_of(stub_events, "state")
    assert set(real_state.keys()) == set(stub_state.keys())
    for pstate in ("started", "joined", "ended"):
        rp = next(e for e in real_events if e["event"] == "participant" and e["state"] == pstate)
        sp = next(e for e in stub_events if e["event"] == "participant" and e["state"] == pstate)
        assert set(rp.keys()) == set(sp.keys()), pstate

    # result parity (post-transition settled status)
    real_res, stub_res = first_of(real_events, "result"), first_of(stub_events, "result")
    assert set(real_res.keys()) == set(stub_res.keys())
    assert set(real_res["status"].keys()) == set(stub_res["status"].keys())
    for res, rs in ((real_res, real_rs), (stub_res, stub_rs)):
        assert res["launcher_ends"] is False, res
        assert res["result"]["result"] == "passed", res
        st = res["status"]
        assert st["launcher_state"] == "ready", st
        assert st["active_run_id"] is None and st["last_run_id"] == rs["run"], st
        assert st["run_state"] is None, st
        assert st["launcher_allowed"]["prepare"]["ok"] is True and st["launcher_allowed"]["run"]["ok"] is True, st

    # 3. --once + command + graceful stop idempotency parity
    paused_req = tmp_path / "paused_req.json"
    paused_req.write_text(json.dumps({"vehicle_sil_run_request": 1, "start_paused": True}), encoding="utf-8")
    real_once = _run_once_stop_sequence(real_prefix, configs / "forever.yaml", paused_req)
    stub_once = _run_once_stop_sequence(stub_prefix, configs / "forever.yaml", paused_req, env=stub_env)

    real_step_cmd = next(e for e in real_once if e["event"] == "command" and e.get("id") == 11)
    stub_step_cmd = next(e for e in stub_once if e["event"] == "command" and e.get("id") == 11)
    assert set(real_step_cmd.keys()) == set(stub_step_cmd.keys())
    assert real_step_cmd["message"] is None and real_step_cmd["reason"] is None
    assert stub_step_cmd["message"] is None and stub_step_cmd["reason"] is None

    real_stop_cmd = next(e for e in real_once if e["event"] == "command" and e.get("id") == 12)
    stub_stop_cmd = next(e for e in stub_once if e["event"] == "command" and e.get("id") == 12)
    assert set(real_stop_cmd.keys()) == set(stub_stop_cmd.keys())
    assert real_stop_cmd["accepted"] is True and real_stop_cmd["message"] is None and real_stop_cmd["reason"] is None
    assert stub_stop_cmd["accepted"] is True and stub_stop_cmd["message"] is None and stub_stop_cmd["reason"] is None

    for label, evts in (("real", real_once), ("stub", stub_once)):
        stop_states = [e["stop"] for e in evts if e["event"] == "state" and e["stop"]["requests"] > 0]
        assert stop_states and all(s == {"requests": 1, "by": "stop command"} for s in stop_states), (label, stop_states)
        second_stops = [e for e in evts if e["event"] == "command" and e.get("id") == 13]
        for ev2 in second_stops:
            if ev2["accepted"]:
                assert ev2["message"] is None and ev2["reason"] is None, (label, ev2)
        res = first_of(evts, "result")
        rs = first_of(evts, "run_start")
        assert res["launcher_ends"] is True, (label, res)
        st = res["status"]
        assert st["launcher_state"] == "ending", (label, st)
        assert st["active_run_id"] is None and st["last_run_id"] == rs["run"], (label, st)
        assert st["run_state"] is None, (label, st)
        assert st["launcher_allowed"]["prepare"]["ok"] is False and st["launcher_allowed"]["run"]["ok"] is False, (label, st)
        assert st["launcher_allowed"]["run"]["reason"] == "launcher-ending", (label, st)


def test_a_saved_scenario_with_names_beyond_ascii_passes_the_real_check(tmp_path: Path) -> None:
    """The YAML of GET api/recording, read by the real launcher's YAML reader: names with characters beyond ASCII
    (U+0080 to U+00FF among them, which must not be written as \\xNN) name the participant they are."""
    engine, dashboard = (p.as_posix() for p in executables(EXAMPLES))
    name = "Mot\u00f6r\u00b0\u00b5\u00e9\u4e00"
    (tmp_path / "sim.yaml").write_text(
        f"vehicle_sil_simulation: 2\n{NETWORKS}participants:\n"
        f"  - name: {name}\n    command: ['{engine}']\n    step_ms: 10\n"
        f"  - name: Dashboard\n    command: ['{dashboard}']\n    step_ms: 5\n", encoding="utf-8")
    text, steps, _ = scenario_yaml([(0, 1, name, "engine.throttle", 10.0), (20, 2, name, "ecu.connected", True)],
                                   "recorded from s\u00edm.yaml (run 1)", {}, 0, 500)
    assert steps == 2
    (tmp_path / "rec.yaml").write_text(text, encoding="utf-8")
    p = subprocess.run([LAUNCHER, "check", "--json", "--scenario", "rec.yaml", "--", "sim.yaml"], cwd=tmp_path,
                       capture_output=True, timeout=RUN_TIMEOUT_S)
    check = json.loads(p.stdout)
    assert check["result"] == "passed", check["findings"]
    assert check["scenario"] == {"file": "rec.yaml", "name": "recorded from s\u00edm.yaml (run 1)", "steps": 2,
                                 "inputs": [], "lines": [], "end_ms": 500}


def test_a_variant_chosen_in_the_browser_reaches_the_real_check_and_run(tmp_path: Path) -> None:
    """The intent of the browser is a request file for the real launcher: a configuration with variants, checked and
    run with the variant chosen (and refused with the launcher's own words when none is chosen)."""
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)

    async def talk(c: Client) -> None:
        status, body = await c.post("api/check", {"file": "variants.yaml"})
        assert status == 200 and body["check"]["result"] == "usage", body  # the launcher says to choose a variant
        assert "variant" in json.dumps(body["check"]["findings"])
        status, body = await c.post("api/run", {"file": "variants.yaml"})
        assert (status, body["error"]["code"]) == (422, "check-failed"), body
        for variant in ("small", "big"):
            status, body = await c.post("api/check", {"file": "variants.yaml", "variant": variant})
            assert status == 200 and body["check"]["result"] == "passed", body
        status, body = await c.post("api/check", {"file": "variants.yaml", "variant": "nope"})
        assert status == 200 and body["check"]["result"] == "usage", body
        sock = await c.ws()
        await sock.until(is_synced)
        status, body = await c.post("api/run", {"file": "variants.yaml", "variant": "small", "speed": "max",
                                                 "stop_at_ms": 1000})
        assert status == 200, body
        got = await sock.until(is_ended)
        assert got[-1]["session"]["result"]["result"] == "passed", got[-1]["session"]["result"]
    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        run_async(talk, gui)


@pytest.mark.parametrize("deadline", [None, "0.1"], ids=["default-waits", "explicit-deadline"])
def test_real_launcher_gets_time_to_finish_participant_shutdown(tmp_path: Path, deadline: str | None) -> None:
    """The default waits for cleanup; an explicitly requested deadline still escalates."""
    engine, _ = executables(EXAMPLES)
    ready, got_sigterm, done = tmp_path / "ready", tmp_path / "got-sigterm", tmp_path / "cleanup-complete"
    helper = tmp_path / "slow_exit.py"
    helper.write_text(
        "import os, signal, time\n"
        "from pathlib import Path\n"
        "def stop(*_):\n"
        f"    Path({str(got_sigterm)!r}).write_text('sigterm')\n"
        "    time.sleep(11)\n"
        f"    Path({str(done)!r}).write_text('finished')\n"
        "    raise SystemExit(0)\n"
        "signal.signal(signal.SIGTERM, stop)\n"
        f"Path({str(ready)!r}).write_text(str(os.getpid()))\n"
        "while True: time.sleep(0.1)\n", encoding="utf-8")
    (tmp_path / "sim.yaml").write_text(
        f"vehicle_sil_simulation: 2\n{NETWORKS}participants:\n"
        f"  - name: Engine\n    command: [{json.dumps(str(engine))}]\n"
        f"  - name: Logger\n    command: [{json.dumps(sys.executable)}, {json.dumps(str(helper))}]\n"
        "    describe: false\n    required: false\n", encoding="utf-8")

    async def start(c: Client) -> None:
        sock = await c.ws()
        await sock.until(is_synced)
        status, body = await c.post("api/run", {"file": "sim.yaml"})
        assert status == 200, body
        await sock.until(lambda m: m.get("event", {}).get("event") == "time")
        await sock.close()

    args = ("--no-variables", "--participant-stop-timeout-ms", "15000")
    if deadline is not None:
        args += ("--stop-timeout", deadline)
    with gui_server(tmp_path, root=tmp_path, launcher=Path(LAUNCHER), args=args) as gui:
        run_async(start, gui)
        assert wait_until(lambda: ready.exists() and ready.stat().st_size > 0, 10)
        began = time.monotonic()
        pid = int(ready.read_text(encoding="utf-8"))
        try:
            gui.proc.send_signal(signal.SIGTERM)
            if deadline is None:
                assert wait_until(lambda: got_sigterm.exists(), 10)
                assert gui.proc.poll() is None, "the server ended before the participant could finish"
            assert gui.proc.wait(scaled(30)) == 0
            if deadline is None:
                assert time.monotonic() - began >= 10
                assert done.read_text(encoding="utf-8") == "finished"
            else:
                assert time.monotonic() - began < scaled(10)
                assert not done.exists()
        finally:
            if alive(pid):
                os.kill(pid, signal.SIGKILL)
                assert wait_until(lambda: not alive(pid), 5)


def test_real_single_check_describes_once_per_start_and_recovers_after_failed_check(tmp_path: Path) -> None:
    """Starting a run via GuiServer invokes --describe at most once per ECU per Start (no pre-check subprocess),
    across Cold and Hot A -> B -> A transitions, and recovers cleanly after a 422 check-failed Start."""
    engine, dashboard = executables(EXAMPLES)
    count_file = tmp_path / "describe_calls.log"
    wrap_dir = tmp_path / "wrappers"
    wrap_dir.mkdir()
    wrappers: list[Path] = []
    for name, real_bin in (("engine_wrap.py", engine), ("dashboard_wrap.py", dashboard)):
        w = wrap_dir / name
        w.write_text(
            f"#!{sys.executable}\n"
            "import os, sys\n"
            "if '--describe' in sys.argv[1:]:\n"
            f"    with open({str(count_file)!r}, 'a', encoding='utf-8') as f:\n"
            f"        f.write({name!r} + '\\n')\n"
            f"os.execv({str(real_bin)!r}, [{str(real_bin)!r}, *sys.argv[1:]])\n",
            encoding="utf-8",
        )
        w.chmod(0o755)
        wrappers.append(w)

    cfg = tmp_path / "cfg"
    cfg.mkdir()
    eng_w, dash_w = (p.as_posix() for p in wrappers)
    (cfg / "a.yaml").write_text(
        f"vehicle_sil_simulation: 2\n{NETWORKS}participants:\n"
        f"  - name: Engine\n    command: [{json.dumps(eng_w)}]\n    step_ms: 10\n"
        f"  - name: Dashboard\n    command: [{json.dumps(dash_w)}]\n    step_ms: 5\n",
        encoding="utf-8",
    )
    (cfg / "b.yaml").write_text(
        f"vehicle_sil_simulation: 2\n{NETWORKS}participants:\n"
        f"  - name: Engine\n    command: [{json.dumps(eng_w)}]\n    step_ms: 10\n"
        f"  - name: Dashboard\n    command: [{json.dumps(dash_w)}]\n    step_ms: 5\n",
        encoding="utf-8",
    )
    (cfg / "bad.yaml").write_text(
        f"vehicle_sil_simulation: 2\n{NETWORKS}participants:\n"
        f"  - name: Engine\n    command: [{json.dumps(eng_w)}]\n    step_ms: 20\n"
        f"  - name: Dashboard\n    command: [{json.dumps(dash_w)}]\n    step_ms: 5\n",
        encoding="utf-8",
    )

    def count_calls() -> int:
        if not count_file.exists():
            return 0
        return len([ln for ln in count_file.read_text(encoding="utf-8").splitlines() if ln.strip()])

    async def talk(c: Client) -> None:
        sock = await c.ws()
        await sock.until(is_synced)
        before = count_calls()
        st, body = await c.post("api/run", {"file": "a.yaml", "disable": ["Dashboard"], "stop_at_ms": 100})
        assert st == 200, body
        await sock.until(is_ended, timeout=30)
        # Engine described once (disabled Dashboard is skipped by launcher)
        delta1 = count_calls() - before
        assert delta1 == 1, delta1

        # B with both enabled -> exactly 2 --describe calls (1 per ECU)
        before = count_calls()
        st, body = await c.post("api/run", {"file": "b.yaml", "stop_at_ms": 100})
        assert st == 200, body
        prev_id = body["run_id"]
        await sock.until(is_ended, timeout=30)
        assert count_calls() - before == 2

        # Failed check -> 422 check-failed, leaves session untouched, next A start succeeds cleanly
        st, bad = await c.post("api/run", {"file": "bad.yaml"})
        assert st == 422 and bad["error"]["code"] == "check-failed", bad
        st_sess, cur = await c.get("api/session")
        assert st_sess == 200 and cur["session"]["id"] == prev_id
        before = count_calls()
        st, body = await c.post("api/run", {"file": "a.yaml", "stop_at_ms": 100})
        assert st == 200 and body["run_id"] != prev_id, body
        await sock.until(is_ended, timeout=30)
        assert count_calls() - before == 2
        await sock.close()

    with gui_server(tmp_path, root=cfg, launcher=Path(LAUNCHER)) as gui:
        run_async(talk, gui)


def test_real_template_provenance_multicase_error_and_recording_playback_parity(tmp_path: Path) -> None:
    """Real launcher + GUI server: scenarios/templates/ excluded, multi-case without --case returns clean 422,
    template scenario propagates file/name/call_stack, and GUI recording plays back cleanly."""
    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    (configs / "scenarios" / "templates").mkdir(parents=True)
    (configs / "scenarios" / "templates" / "throttle_check.yaml").write_text(
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  throttle: { type: number }\n"
        "  label: { type: string, default: check }\n"
        "steps:\n"
        "  - name: \"${label}: apply\"\n"
        "    set: { Engine.engine.throttle: \"${throttle}\" }\n"
        "  - name: \"${label}: wait\"\n"
        "    wait_until: { Engine.engine.speed: { ge: 100 } }\n"
        "    timeout_ms: 500\n",
        encoding="utf-8",
    )
    (configs / "scenarios" / "tpl_run.yaml").write_text(
        "vehicle_sil_scenario: 1\n"
        "name: template run\n"
        "steps:\n"
        "  - name: outer call\n"
        "    template: templates/throttle_check.yaml\n"
        "    params: { throttle: 50, label: mid }\n",
        encoding="utf-8",
    )
    (configs / "scenarios" / "cases_run.yaml").write_text(
        "vehicle_sil_scenario: 1\n"
        "cases:\n"
        "  idle: { params: { target: 0 } }\n"
        "  load: { params: { target: 50 } }\n"
        "steps:\n"
        "  - set: { Engine.engine.throttle: \"${target}\" }\n",
        encoding="utf-8",
    )

    async def talk(c: Client) -> None:
        # 1. scenarios/templates/ is excluded from GET api/scenarios
        st, listed = await c.get("api/scenarios?file=simulation.yaml")
        assert st == 200
        paths = [f["path"] for f in listed["scenarios"]]
        assert "scenarios/tpl_run.yaml" in paths and "scenarios/cases_run.yaml" in paths
        assert not any("templates/" in p for p in paths), paths

        # 2. Multi-case without --case returns clean structured error
        st, chk_case = await c.post("api/check", {"file": "simulation.yaml", "scenario": "scenarios/cases_run.yaml"})
        assert st == 200 and chk_case["exit_code"] == 4 and chk_case["check"]["result"] == "usage"
        st, run_case = await c.post("api/run", {"file": "simulation.yaml", "scenario": "scenarios/cases_run.yaml"})
        assert (st, run_case["error"]["code"]) == (422, "check-failed")
        assert run_case["error"]["detail"]["result"] == "usage"

        # 3. Template scenario run preserves step file, name, and call_stack in session summary and /api/timeline/<id>
        ws = await c.ws()
        await ws.until(is_synced)
        st, started = await c.post("api/run", {"file": "simulation.yaml", "scenario": "scenarios/tpl_run.yaml", "speed": "max"})
        assert st == 200, started
        await ws.until(is_ended, timeout=30)
        _, sess = await c.get("api/session")
        sc = sess["session"]["scenario"]
        assert sc["status"] == "passed"
        assert len(sc["steps"]) == 2
        expected_stack = [{
            "file": "scenarios/tpl_run.yaml",
            "line": 4,
            "name": "outer call",
            "params": {"throttle": 50, "label": "mid"},
        }]
        assert sc["steps"][0]["file"] == "scenarios/templates/throttle_check.yaml"
        assert sc["steps"][0]["name"] == "mid: apply"
        assert sc["steps"][0]["call_stack"] == expected_stack

        st_tl, tl = await c.get(f"api/timeline/{started['run_id']}")
        assert st_tl == 200 and isinstance(tl, dict), tl
        assert tl["vehicle_sil_timeline"] == 1
        assert tl["scenario"]["status"] == "passed"
        assert tl["steps"][0]["file"] == "scenarios/templates/throttle_check.yaml"
        assert tl["steps"][0]["name"] == "mid: apply"
        assert tl["steps"][0]["call_stack"] == expected_stack
        assert any(ln.get("file") == "scenarios/templates/throttle_check.yaml" for ln in tl["lines"])

        # 4. Record -> save -> playback parity on the same server
        st, started2 = await c.post("api/run", {"file": "forever.yaml", "start_paused": True})
        assert st == 200, started2
        rid2 = started2["run_id"]
        await ws.until(
            lambda m: m["type"] == "variables"
            and m["variables"]["state"] == "ready"
            and any(p["name"] == "Engine" and p["connected"] for p in m["variables"]["peers"]),
            timeout=30,
        )
        st, w = await c.post("api/write", {"run_id": rid2, "writes": {"Engine": {"engine.throttle": 40}}})
        assert st == 200, w
        await c.post("api/time", {"command": "step", "ms": 20})
        await ws.until(lambda m: m["type"] == "operation" and m["seq"] == w["operation"] and m["done"], timeout=30)
        await c.post("api/stop-session", {"run_id": rid2, "instance": started2["instance"], "force": False})
        await ws.until(is_ended, timeout=30)
        _, rec = await c.get("api/recording")
        assert rec["recording"]["available"] is True and rec["recording"]["yaml"]
        (configs / "scenarios" / "recorded_replay.yaml").write_text(rec["recording"]["yaml"], encoding="utf-8")

        st, chk_rec = await c.post("api/check", {"file": "simulation.yaml", "scenario": "scenarios/recorded_replay.yaml"})
        assert st == 200 and chk_rec["check"]["result"] == "passed", chk_rec
        st, started3 = await c.post("api/run", {"file": "simulation.yaml", "scenario": "scenarios/recorded_replay.yaml", "speed": "max"})
        assert st == 200, started3
        await ws.until(is_ended, timeout=30)
        _, sess3 = await c.get("api/session")
        assert sess3["session"]["scenario"]["status"] == "passed"
        await ws.close()

    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        run_async(talk, gui)


def test_real_recording_roundtrip_with_yaml_reserved_enum_names(tmp_path: Path) -> None:
    """YAML generated by Recording for enum item names that look like YAML scalars ('1', 'true', 'null')
    passes real vehicle_sil_launch check and run without type ambiguity."""
    assert LAUNCHER is not None
    launcher_exe = Path(LAUNCHER).resolve()
    build_root = launcher_exe.parent.parent.parent
    test_ecu = build_root / "tests" / "launch" / ("vehicle_sil_launch_test_ecu" + (".exe" if os.name == "nt" else ""))
    assert test_ecu.is_file(), f"missing test ECU beside launcher: {test_ecu}"

    (tmp_path / "Body.behaviour").write_text("scenario-variables\n", encoding="utf-8")
    (tmp_path / "sim.yaml").write_text(
        "vehicle_sil_simulation: 2\n"
        "networks: []\n"
        "participants:\n"
        "  - name: Body\n"
        f"    command: [{json.dumps(test_ecu.as_posix())}]\n"
        "    variables:\n"
        "      drive.mode:\n"
        "        from: mode\n"
        "        map:\n"
        "          '1': 0\n"
        "          'true': 1\n"
        "          'null': 2\n",
        encoding="utf-8",
    )

    rec = Recording()
    for seq, (t_ms, item_val) in enumerate(((0, 0), (20, 1), (40, 2)), start=1):
        rec.wrote(
            seq,
            [("Body", "drive.mode", item_val)],
            {"seq": seq, "done": True, "peers": {"Body": {"state": "applied", "applied_at_ms": t_ms}}},
        )
    enums = {
        ("Body", "drive.mode"): [
            {"name": "1", "value": 0},
            {"name": "true", "value": 1},
            {"name": "null", "value": 2},
        ],
    }
    rec_json = rec.to_json("reserved-enum-rec", "sim.yaml", "ended", enums, end_ms=60)
    yaml_text = rec_json["yaml"]
    assert yaml_text is not None
    assert '"1"' in yaml_text and '"true"' in yaml_text and '"null"' in yaml_text
    (tmp_path / "scenarios").mkdir()
    rec_path = tmp_path / "scenarios" / "recorded_enums.yaml"
    rec_path.write_text(yaml_text, encoding="utf-8")

    chk = subprocess.run(
        [str(launcher_exe), "check", "--json", "--scenario", "scenarios/recorded_enums.yaml", "--", "sim.yaml"],
        cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", timeout=RUN_TIMEOUT_S,
    )
    assert chk.returncode == 0, chk.stdout + chk.stderr
    chk_data = json.loads(chk.stdout)
    assert [f for f in chk_data["findings"] if f["severity"] == "error"] == [], chk_data["findings"]

    run = subprocess.run(
        [str(launcher_exe), "run", "--speed", "max", "--json", "--scenario", "scenarios/recorded_enums.yaml", "--", "sim.yaml"],
        cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", timeout=RUN_TIMEOUT_S,
    )
    assert run.returncode == 0, run.stdout + run.stderr
    run_data = json.loads(run.stdout)
    assert run_data["scenario"]["status"] == "passed", run_data["scenario"]
    assert run_data["scenario"]["over_at_ms"] == 60, run_data["scenario"]


def test_real_multicase_params_session_intent_rerun_and_hot_standby(tmp_path: Path) -> None:
    """Real launcher + GUI server (subdirectory config):
    - Multi-case check without case preserves participants/hot_eligibility/scenario/param_meta/missing_params
    - GUI API run with case + typed params + template propagates scenario.case, params, provenance, and timeline
    - Session.intent keeps root-relative scenario path and after_scenario, and re-runs cleanly via POST /api/run
    - Hot standby is reused across case and parameter changes without restarting the resident launcher."""
    root = tmp_path / "root"
    sub = root / "sub"
    write(sub, EXAMPLES)
    (sub / "scenarios" / "templates").mkdir(parents=True)
    (sub / "scenarios" / "templates" / "drive.yaml").write_text(
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  throttle:\n"
        "    type: integer\n"
        "    description: engine throttle percent\n"
        "  min_speed:\n"
        "    type: number\n"
        "    default: 10.0\n"
        "    description: minimum speed threshold\n"
        "  verify_only:\n"
        "    type: boolean\n"
        "    default: false\n"
        "  label:\n"
        "    type: string\n"
        "    values: [idle_case, load_case, rated, cool]\n"
        "    default: idle_case\n"
        "steps:\n"
        "  - name: \"${label}: throttle\"\n"
        "    set: { Engine.engine.throttle: \"${throttle}\", Engine.ecu.connected: \"${verify_only}\" }\n"
        "  - name: \"${label}: check\"\n"
        "    after_ms: 40\n"
        "    expect: { Engine.engine.speed: { ge: \"${min_speed}\" } }\n",
        encoding="utf-8",
    )
    (sub / "scenarios" / "cases_tpl.yaml").write_text(
        "vehicle_sil_scenario: 1\n"
        "name: multicase drive\n"
        "params:\n"
        "  min_speed: 10.0\n"
        "  verify_only: true\n"
        "cases:\n"
        "  idle:\n"
        "    params: { throttle: 0, label: idle_case }\n"
        "  load:\n"
        "    params: { throttle: 50, label: load_case }\n"
        "steps:\n"
        "  - name: invoke template\n"
        "    template: templates/drive.yaml\n"
        "    params:\n"
        "      throttle: \"${throttle}\"\n"
        "      min_speed: \"${min_speed}\"\n"
        "      verify_only: \"${verify_only}\"\n"
        "      label: \"${label}\"\n",
        encoding="utf-8",
    )

    async def talk(c: Client) -> None:
        # 1. Multi-case check without case -> usage, keeping participants, hot_eligibility, case_details, param_meta, missing_params
        st, chk_usage = await c.post(
            "api/check",
            {"file": "sub/simulation.yaml", "scenario": "sub/scenarios/cases_tpl.yaml"},
        )
        assert st == 200 and chk_usage["exit_code"] == 4, chk_usage
        chk = chk_usage["check"]
        assert chk["result"] == "usage"
        assert any(p["name"] == "Engine" for p in chk["participants"]), chk
        assert chk["hot_eligibility"]["eligible"] is True, chk
        sc_chk = chk["scenario"]
        assert sc_chk["case"] is None
        assert sc_chk["cases"] == ["idle", "load"]
        assert sc_chk["case_details"] == [{"name": "idle", "only": None}, {"name": "load", "only": None}]
        meta_by_name = {m["name"]: m for m in sc_chk["param_meta"]}
        assert meta_by_name["throttle"]["type"] == "integer"
        assert meta_by_name["throttle"]["description"] == "engine throttle percent"
        assert meta_by_name["min_speed"]["type"] == "number"
        assert meta_by_name["verify_only"]["type"] == "boolean"
        assert meta_by_name["label"]["type"] == "string"
        assert meta_by_name["label"]["values"] == ["idle_case", "load_case", "rated", "cool"]
        assert "throttle" in sc_chk["missing_params"]

        # 2. Check with case + typed params passes
        st, chk_ok = await c.post(
            "api/check",
            {
                "file": "sub/simulation.yaml",
                "scenario": "sub/scenarios/cases_tpl.yaml",
                "case": "load",
                "params": {"min_speed": 12.5, "verify_only": True, "label": "rated"},
            },
        )
        assert st == 200 and chk_ok["exit_code"] == 0 and chk_ok["check"]["result"] == "passed", chk_ok
        assert chk_ok["check"]["scenario"]["case"] == "load"

        # 3. Prepare hot standby for sub/simulation.yaml
        ws = await c.ws()
        await ws.until(is_synced)
        st, prep = await c.post("api/prepare", {"file": "sub/simulation.yaml"})
        assert st == 200 and "instance" in prep, prep
        await ws.until(
            lambda m: m["type"] == "runtime"
            and m["runtime"]["mode"] == "hot"
            and m["runtime"]["state"] == "ready",
            timeout=30,
        )

        # 4. Run case='load' with typed params and after_scenario='pause'
        st, run1 = await c.post(
            "api/run",
            {
                "file": "sub/simulation.yaml",
                "scenario": "sub/scenarios/cases_tpl.yaml",
                "case": "load",
                "after_scenario": "pause",
                "params": {"throttle": 60, "min_speed": 12.5, "verify_only": True, "label": "rated"},
                "speed": "max",
            },
        )
        assert st == 200, run1
        await ws.until(
            lambda m: m["type"] == "event"
            and m["event"]["event"] == "state"
            and m["event"].get("handed_over") is True,
            timeout=30,
        )
        _, sess1_running = await c.get("api/session")
        pid1 = sess1_running["session"]["launcher"]["pid"]
        assert isinstance(pid1, int) and pid1 > 0
        await c.post("api/stop-session", {"run_id": run1["run_id"], "instance": run1["instance"], "force": False})
        await ws.until(is_ended, timeout=30)

        _, sess1 = await c.get("api/session")
        s1 = sess1["session"]
        assert s1["scenario"]["status"] == "passed"
        assert s1["scenario"]["case"] == "load"
        params_by_name = {p["name"]: p for p in s1["scenario"]["params"]}
        assert params_by_name["throttle"]["value"] == 60
        assert params_by_name["throttle"]["type"] == "integer"
        assert params_by_name["min_speed"]["value"] == 12.5
        assert params_by_name["verify_only"]["value"] is True
        assert params_by_name["label"]["value"] == "rated"
        assert s1["scenario"]["steps"][0]["name"] == "rated: throttle"
        assert s1["scenario"]["steps"][0]["file"] == "sub/scenarios/templates/drive.yaml"
        assert s1["scenario"]["steps"][0]["call_stack"][0]["name"] == "invoke template"
        assert s1["intent"] == {
            "scenario": "sub/scenarios/cases_tpl.yaml",
            "case": "load",
            "after_scenario": "pause",
            "params": {"throttle": 60, "min_speed": 12.5, "verify_only": True, "label": "rated"},
            "speed": "max",
        }

        st_tl, tl = await c.get(f"api/timeline/{run1['run_id']}")
        assert st_tl == 200 and tl["vehicle_sil_timeline"] == 1, tl
        assert tl["scenario"]["status"] == "passed"
        assert tl["scenario"]["case"] == "load"
        assert tl["steps"][0]["call_stack"][0]["name"] == "invoke template"

        # 5. Re-run directly from session.intent (root-relative path) and verify hot standby PID is reused
        st, run2 = await c.post("api/run", {"file": s1["file"], **s1["intent"]})
        assert st == 200 and run2["run_id"] != run1["run_id"], run2
        await ws.until(
            lambda m: m["type"] == "event"
            and m["event"]["event"] == "state"
            and m["event"].get("handed_over") is True,
            timeout=30,
        )
        _, sess2_running = await c.get("api/session")
        assert sess2_running["session"]["launcher"]["pid"] == pid1
        await c.post("api/stop-session", {"run_id": run2["run_id"], "instance": run2["instance"], "force": False})
        await ws.until(is_ended, timeout=30)

        # 6. Switch case & params without after_scenario -> still reuses the same hot standby PID
        st, run3 = await c.post(
            "api/run",
            {
                "file": "sub/simulation.yaml",
                "scenario": "sub/scenarios/cases_tpl.yaml",
                "case": "idle",
                "params": {"label": "cool"},
                "speed": "max",
            },
        )
        assert st == 200, run3
        await ws.until(is_ended, timeout=30)
        _, sess3 = await c.get("api/session")
        assert sess3["session"]["launcher"]["pid"] == pid1
        assert sess3["session"]["scenario"]["status"] == "passed"
        assert sess3["session"]["scenario"]["case"] == "idle"
        assert sess3["session"]["scenario"]["steps"][0]["name"] == "cool: throttle"
        await ws.close()

    with gui_server(tmp_path, root=root, launcher=Path(LAUNCHER)) as gui:
        run_async(talk, gui)


def test_real_gui_external_registry_consecutive_runs_and_stop(tmp_path: Path) -> None:
    """GUI server started with `--external-registry` keeps idle prepare free of registry connections and runs
    consecutive Start -> Stop -> Start sessions on distinct `/vsil-<16hex>` simulation paths."""
    assert LAUNCHER is not None and EXAMPLES is not None
    launcher_exe = Path(LAUNCHER).resolve()
    build_root = launcher_exe.parent.parent.parent
    candidates = list(build_root.glob("**/sil-kit-registry"))
    assert candidates, f"missing built sil-kit-registry under {build_root}"
    registry_exe = candidates[0]

    configs = tmp_path / "configs"
    write(configs, EXAMPLES)
    reg_log = tmp_path / "external-registry.log"
    with open(reg_log, "wb") as log_fp:
        reg_proc = subprocess.Popen(
            [str(registry_exe), "--listen-uri", "silkit://127.0.0.1:0"],
            stdin=subprocess.DEVNULL,
            stdout=log_fp,
            stderr=subprocess.STDOUT,
        )
    try:
        ext_uri = ""
        deadline = time.monotonic() + scaled(15)
        while time.monotonic() < deadline:
            text = reg_log.read_text(encoding="utf-8", errors="replace")
            for line in text.splitlines():
                if "SIL Kit Registry listening on " in line:
                    ext_uri = line.split("SIL Kit Registry listening on ", 1)[1].strip()
                    break
            if ext_uri:
                break
            assert reg_proc.poll() is None, text
            time.sleep(0.05)
        assert ext_uri.startswith("silkit://127.0.0.1:"), reg_log.read_text(encoding="utf-8", errors="replace")

        async def talk(c: Client) -> None:
            ws = await c.ws()
            await ws.until(is_synced)

            st_prep, _ = await c.post("api/prepare", {"file": "forever.yaml"})
            assert st_prep == 200
            # Idle prepare must not connect SimulationController to sil-kit-registry before Start.
            assert "SimulationController" not in reg_log.read_text(encoding="utf-8", errors="replace")

            uris: list[str] = []
            for _ in range(2):
                st_run, started = await c.post("api/run", {"file": "forever.yaml"})
                assert st_run == 200, started
                await ws.until(
                    lambda m: m["type"] == "variables"
                    and m["variables"]["state"] == "ready"
                    and any(p["name"] == "Engine" and p["connected"] for p in m["variables"]["peers"]),
                    timeout=30,
                )
                st_stop, _ = await c.post(
                    "api/stop-session",
                    {"run_id": started["run_id"], "instance": started["instance"], "force": False},
                )
                assert st_stop == 200
                await ws.until(is_ended, timeout=30)
                _, sess = await c.get("api/session")
                summary = sess["session"]
                assert summary["result"]["result"] == "stopped" and summary["result"]["ok"] is True, summary
                reg_info = summary["registry"]
                assert reg_info is not None and isinstance(reg_info["uri"], str), summary
                uri = reg_info["uri"]
                assert uri.startswith(f"{ext_uri}/vsil-") and len(uri) == len(ext_uri) + 6 + 16, uri
                uris.append(uri)

            assert uris[0] != uris[1], uris
            await ws.close()

        with gui_server(
            tmp_path,
            root=configs,
            launcher=launcher_exe,
            args=("--external-registry", ext_uri),
        ) as gui:
            run_async(talk, gui)
    finally:
        if reg_proc.poll() is None:
            reg_proc.terminate()
            try:
                reg_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                reg_proc.kill()
                reg_proc.wait()
