"""The recordings that replay_launcher.py plays, against the real launcher: record_launcher.py --compare records
every case again with the real vehicle_sil_launch and the ecu_main example ECUs and compares them with recordings/
(the events in order without time, log and heartbeat, their keys and JSON types, the commands, the exit code, what
check --json and version --json print). When it differs, record again and read the diff (record_launcher.py says
how). Also the YAML of a saved scenario, read by the real launcher's check.

Runs only when VEHICLE_SIL_GUI_LAUNCHER names a real vehicle_sil_launch (scripts/verify-gui.py --launcher
<path>) and the ecu_main example ECUs are built next to it (or VEHICLE_SIL_GUI_EXAMPLES names their
directory); otherwise every test here is skipped, with the reason. Linux only (record_launcher.py is).
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest
from conftest import Client, alive, gui_server, is_ended, is_synced, run_async, scaled, wait_until
from real_configs import NETWORKS, examples_dir, executables, usable, write
from vehicle_sil_gui import __version__
from vehicle_sil_gui.recording import scenario_yaml

TESTS = Path(__file__).resolve().parent
LAUNCHER = os.environ.get("VEHICLE_SIL_GUI_LAUNCHER")
EXAMPLES = examples_dir(LAUNCHER)
REASON = ("no real launcher: set VEHICLE_SIL_GUI_LAUNCHER to build/<preset>/tools/launch/vehicle_sil_launch "
          "(scripts/verify-gui.py --launcher <path>)" if not LAUNCHER or not Path(LAUNCHER).is_file()
          else f"no ecu_main example executables in {EXAMPLES}: build them, or set VEHICLE_SIL_GUI_EXAMPLES"
          if not usable(EXAMPLES) else "Linux only" if sys.platform != "linux" else None)
pytestmark = pytest.mark.skipif(REASON is not None, reason=REASON or "")

RUN_TIMEOUT_S = 60


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
        first_id, instance = first["session"]["id"], first["instance"]
        await sock.until(lambda m: m.get("event", {}).get("event") == "time")
        status, _ = await c.post("api/stop-session", {"session": first_id, "instance": instance})
        assert status == 200
        await sock.until(is_ended, timeout=60)

        status, second = await c.post("api/run", {"file": "forever.yaml"})
        assert status == 200, second
        second_id = second["session"]["id"]
        assert second_id != first_id
        await sock.until(lambda m: m.get("event", {}).get("event") == "time")
        status, stale = await c.post("api/stop-session", {"session": first_id, "instance": instance})
        assert status == 409 and stale["error"]["code"] == "session-changed", stale
        status, current = await c.get("api/session")
        assert status == 200 and current["session"]["id"] == second_id
        assert current["session"]["stop"] == "none"
        assert alive(current["session"]["launcher"]["pid"])
        status, stopped = await c.post("api/stop-session", {"session": second_id, "instance": instance})
        assert status == 200 and stopped["session"]["stop"] == "requested", stopped
        await sock.until(is_ended, timeout=60)
        await sock.close()

    with gui_server(tmp_path, root=configs, launcher=Path(LAUNCHER)) as gui:
        run_async(talk, gui)


def test_the_recordings_are_what_the_real_launcher_writes() -> None:
    p = subprocess.run([sys.executable, str(TESTS / "record_launcher.py"), "--launcher", LAUNCHER, "--examples",
                        str(EXAMPLES), "--compare"], capture_output=True, text=True, timeout=900)
    assert p.returncode == 0, f"{p.stdout}{p.stderr[-6000:]}"


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
                                 "inputs": [], "lines": []}


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
    ready, done = tmp_path / "ready", tmp_path / "cleanup-complete"
    helper = tmp_path / "slow_exit.py"
    helper.write_text(
        "import os, signal, time\n"
        "from pathlib import Path\n"
        "def stop(*_):\n"
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
            time.sleep(0.3)
            if deadline is None:
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
        prev_id = body["session"]["id"]
        await sock.until(is_ended, timeout=30)
        assert count_calls() - before == 2

        # Failed check -> 422 check-failed, leaves session untouched, next A start succeeds cleanly
        st, bad = await c.post("api/run", {"file": "bad.yaml"})
        assert st == 422 and bad["error"]["code"] == "check-failed", bad
        st_sess, cur = await c.get("api/session")
        assert st_sess == 200 and cur["session"]["id"] == prev_id
        before = count_calls()
        st, body = await c.post("api/run", {"file": "a.yaml", "stop_at_ms": 100})
        assert st == 200 and body["session"]["id"] == prev_id + 1, body
        await sock.until(is_ended, timeout=30)
        assert count_calls() - before == 2
        await sock.close()

    with gui_server(tmp_path, root=cfg, launcher=Path(LAUNCHER)) as gui:
        run_async(talk, gui)
