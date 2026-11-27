"""The example clients of the GUI server's interface, run with Node against the GUI server and the stand-in launcher:
examples/gui_client/run.mjs (plain JavaScript, no library) and run-with-library.mjs (the client library gui/client,
bundled into one ESM file by `npm run build:client` in gui/web). Each checks the file, starts a run, reads what
comes and waits for the end. front.mjs does the same through the front door of the library (connect(), status). variables.mjs (the library again) writes a variable of an ECU before the first step,
steps, and checks what it reads while the simulation stands still: against the stand-in launcher with its stand-in for
the control server, and, when there is one (VEHICLE_SIL_GUI_LAUNCHER), against the real launcher and the ecu_main
example ECUs.

scripts/verify-gui.py runs this file as its own step (example), after the web step; the server step leaves it out.
The bundle is built again here (that needs gui/web/node_modules), so the test never tries an old one."""

from __future__ import annotations

import math
import os
import shutil
import subprocess
from collections.abc import Iterator
from pathlib import Path

import pytest

import real_configs
from conftest import Gui, gui_server, scaled

ROOT = Path(__file__).resolve().parents[3]
EXAMPLES = ROOT / "examples" / "gui_client"
BUNDLE = ROOT / "build" / "gui-client" / "vehicle-sil-gui-client.mjs"
NODE = shutil.which("node")

pytestmark = pytest.mark.skipif(NODE is None, reason="node is not on PATH (the examples need Node 22 or later)")


@pytest.fixture(scope="module")
def bundle() -> Path:
    """Builds the bundle each time (it takes well under a second), so an old one is never tried."""
    npm = shutil.which("npm")
    assert npm, "npm is not on PATH: it builds the client library (npm run build:client in gui/web)"
    out = subprocess.run([npm, "--prefix", str(ROOT / "gui" / "web"), "run", "build:client"],
                         capture_output=True, text=True, encoding="utf-8", stdin=subprocess.DEVNULL)
    assert out.returncode == 0, f"npm run build:client failed (it needs gui/web/node_modules):\n{out.stdout}{out.stderr}"
    return BUNDLE


@pytest.fixture(scope="module")
def shared_gui(tmp_path_factory: pytest.TempPathFactory, bundle: Path) -> Iterator[Gui]:
    with gui_server(tmp_path_factory.mktemp("example_gui")) as gui:
        yield gui


def run_example(name: str, url: str | None, *args: str) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if k != "VEHICLE_SIL_GUI_URL"}
    if url is not None:
        env["VEHICLE_SIL_GUI_URL"] = url  # not on the command line, where other users could read the token
    return subprocess.run([NODE, str(EXAMPLES / name), *args], capture_output=True, text=True, encoding="utf-8",
                          stdin=subprocess.DEVNULL, env=env, timeout=scaled(90))


@pytest.mark.parametrize("example", ["run.mjs", "run-with-library.mjs"])
@pytest.mark.parametrize("file, code, last", [
    ("simulation.yaml", 0, "result: passed"),
    ("participant-crash.yaml", 1, "result: failed"),
    ("launcher-crash.yaml", 1, "result: none"),
    ("check-error.yaml", 1, "check: failed"),
])
def test_runs_a_configuration_to_its_end(shared_gui: Gui, example: str, file: str, code: int,
                                         last: str) -> None:
    out = run_example(example, shared_gui.url, file, "--timeout", str(math.ceil(scaled(60))),
                      *(["--stop-at-ms", "1000"] if file in {"simulation.yaml", "launcher-crash.yaml"} else []))
    assert out.returncode == code, out.stdout + out.stderr
    assert out.stdout.strip().splitlines()[-1].startswith(last), out.stdout + out.stderr
    assert shared_gui.token not in out.stdout + out.stderr


@pytest.mark.parametrize("example", ["run.mjs", "run-with-library.mjs"])
def test_check_uses_the_same_stop_time_as_the_run(shared_gui: Gui, example: str) -> None:
    # This file's time step is 10 ms: 25 ms is rejected by check, before any run starts.
    out = run_example(example, shared_gui.url, "simulation-step10.yaml", "--stop-at-ms", "25")
    assert out.returncode == 1 and "check: failed" in out.stdout, out.stdout + out.stderr
    assert "started:" not in out.stdout


@pytest.mark.parametrize("example", ["run.mjs", "run-with-library.mjs", "front.mjs"])
@pytest.mark.parametrize("value", ["0", "1000000000001", "1.5"])
def test_stop_time_rejects_out_of_range_values(example: str, value: str) -> None:
    out = run_example(example, None, "simulation.yaml", "--stop-at-ms", value)
    assert out.returncode == 2 and "--stop-at-ms needs an integer" in out.stderr


@pytest.mark.parametrize("file, code, last", [
    ("simulation.yaml", 0, "result: completed, ok true"),
    ("participant-crash.yaml", 1, "result: failed, ok false"),
    ("launcher-crash.yaml", 1, "result: crashed, ok false"),
    ("check-error.yaml", 1, "error: check-failed (refused)"),
])
def test_front_door_runs_a_configuration_to_its_end(tmp_path: Path, bundle: Path, file: str, code: int,
                                                    last: str) -> None:
    with gui_server(tmp_path) as gui:
        out = run_example("front.mjs", gui.url, file, "--timeout", str(math.ceil(scaled(60))),
                          *(["--stop-at-ms", "1000"] if file in {"simulation.yaml", "launcher-crash.yaml"} else []))
    assert out.returncode == code, out.stdout + out.stderr
    lines = out.stdout.strip().splitlines()
    assert any(line.startswith(last) for line in lines), out.stdout + out.stderr
    assert "status: link online, run none" in lines, out.stdout
    assert gui.token not in out.stdout + out.stderr


@pytest.mark.parametrize("example", ["run.mjs", "run-with-library.mjs"])
def test_says_why_it_cannot(shared_gui: Gui, example: str) -> None:
    wrong = run_example(example, shared_gui.url.split("#", 1)[0] + "#token=wrong", "simulation.yaml")
    missing = run_example(example, shared_gui.url, "no-such-file.yaml")
    # the URL may also come first on the command line
    given = run_example(example, None, shared_gui.url, "check-error.yaml")
    assert wrong.returncode == 1 and "unauthorized" in wrong.stdout, wrong.stdout + wrong.stderr
    assert missing.returncode == 1 and "file-not-found" in missing.stdout, missing.stdout + missing.stderr
    assert given.returncode == 1 and "check: failed" in given.stdout, given.stdout + given.stderr
    usage = run_example(example, None)
    assert usage.returncode == 2 and "VEHICLE_SIL_GUI_URL" in usage.stderr, usage.stdout + usage.stderr


# ---------------------------------------------------------------- variables.mjs


def test_variables_example_with_the_fake_control_server(tmp_path: Path, bundle: Path, control_env: dict) -> None:
    with gui_server(tmp_path, env=control_env) as gui:
        out = run_example("variables.mjs", gui.url, "simulation.yaml", "--write", "Engine", "engine.demand=40",
                          "--expect", "Engine", "engine.speed=600", "--step", "200")
        wrong = run_example("variables.mjs", gui.url, "simulation.yaml", "--write", "Engine", "engine.demand=40",
                            "--expect", "Engine", "engine.speed=1", "--step", "200")
        refused = run_example("variables.mjs", gui.url, "simulation.yaml", "--write", "Engine", "engine.demand=400")
        by_name = run_example("variables.mjs", gui.url, "simulation.yaml", "--write", "Engine", "engine.mode=on",
                              "--expect", "Engine", "engine.mode=1", "--step", "200")
    assert out.returncode == 0, out.stdout + out.stderr
    lines = out.stdout.strip().splitlines()
    assert lines[-1] == "all as expected" and "variables: ended" in lines, out.stdout
    assert "ok   engine.speed is 600 at 200 ms, standing still (expected 600)" in lines, out.stdout
    assert "ok   engine.demand was applied in the step at 0 ms" in lines, out.stdout
    assert wrong.returncode == 1 and "FAIL engine.speed is 600" in wrong.stdout, wrong.stdout + wrong.stderr
    assert refused.returncode == 1 and "FAIL invalid-field: Engine's engine.demand takes a number from 0 to 100" \
        in refused.stdout, refused.stdout + refused.stderr
    assert by_name.returncode == 0, by_name.stdout + by_name.stderr
    for done in (out, wrong, refused, by_name):
        assert gui.token not in done.stdout + done.stderr


def test_variables_example_says_why_there_are_no_variables(shared_gui: Gui) -> None:
    out = run_example("variables.mjs", shared_gui.url, "simulation.yaml")
    assert out.returncode == 1 and out.stdout.startswith("no variables: this launcher was built "
                                                         "without a control server"), out.stdout + out.stderr


REAL = os.environ.get("VEHICLE_SIL_GUI_LAUNCHER")


@pytest.mark.skipif(not REAL or not real_configs.usable(real_configs.examples_dir(REAL)),
                    reason="no real launcher with the ecu_main example next to it (scripts/verify-gui.py --launcher)")
def test_variables_example_with_the_real_launcher(tmp_path: Path, bundle: Path) -> None:
    configs = tmp_path / "configs"
    real_configs.write(configs, real_configs.examples_dir(REAL))
    with gui_server(tmp_path, root=configs, launcher=Path(REAL)) as gui:
        out = run_example("variables.mjs", gui.url, "simulation.yaml")
    assert out.returncode == 0, out.stdout + out.stderr
    lines = out.stdout.strip().splitlines()
    assert lines[-1] == "all as expected", out.stdout
    assert "ok   engine.speed is 1800 at 500 ms, standing still (expected 1800)" in lines, out.stdout
    assert gui.token not in out.stdout + out.stderr
    assert gui.stderr_text() == ""


@pytest.mark.parametrize("example, expected_last", [
    ("run.mjs", "result: passed"),
    ("run-with-library.mjs", "result: passed"),
    ("front.mjs", "result: completed, ok true"),
])
def test_examples_work_with_no_auth_printed_url(tmp_path: Path, bundle: Path, example: str,
                                                expected_last: str) -> None:
    with gui_server(tmp_path, args=("--no-auth",)) as gui:
        assert "#token=" not in gui.url
        out = run_example(example, gui.url, "simulation.yaml", "--timeout", "60", "--stop-at-ms", "1000")
    assert out.returncode == 0, out.stdout + out.stderr
    assert any(line.startswith(expected_last) for line in out.stdout.strip().splitlines()), out.stdout + out.stderr


def test_variables_example_works_with_no_auth(tmp_path: Path, bundle: Path, control_env: dict) -> None:
    with gui_server(tmp_path, args=("--no-auth",), env=control_env) as gui:
        assert "#token=" not in gui.url
        out = run_example("variables.mjs", gui.url, "simulation.yaml", "--write", "Engine", "engine.demand=40",
                          "--expect", "Engine", "engine.speed=600", "--step", "200")
    assert out.returncode == 0, out.stdout + out.stderr
    assert out.stdout.strip().splitlines()[-1] == "all as expected", out.stdout


@pytest.mark.parametrize("example", ["run.mjs", "run-with-library.mjs", "front.mjs"])
def test_tokenless_url_is_rejected_by_default_auth_server(shared_gui: Gui, example: str) -> None:
    tokenless = shared_gui.url.split("#", 1)[0]
    out = run_example(example, tokenless, "simulation.yaml")
    assert out.returncode == 1 and "unauthorized" in out.stdout, out.stdout + out.stderr


# ---------------------------------------------------------------- time-and-stop.mjs


def test_time_and_stop_example_targeted_commands_reconnect_and_stop(tmp_path: Path) -> None:
    """Raw Node example: {instance, run_id} targeting, id correlation (accept + refuse), WS reconnect, stop-session.

    Runs twice sequentially against the same GUI server to verify prior-run replay history cannot poison
    command-id correlation or launcher_state scoping on a subsequent run.
    """
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "200"}) as gui:
        first = run_example("time-and-stop.mjs", gui.url, "forever.yaml", "--step", "100",
                            "--timeout", str(math.ceil(scaled(60))))
        second = run_example("time-and-stop.mjs", gui.url, "forever.yaml", "--step", "100",
                             "--timeout", str(math.ceil(scaled(60))))
    run_ids: list[str] = []
    for out in (first, second):
        assert out.returncode == 0, out.stdout + out.stderr
        lines = out.stdout.strip().splitlines()
        started_line = next((line for line in lines if line.startswith("started: instance=") and "run_id=" in line), "")
        assert started_line, out.stdout
        run_ids.append(started_line.split("run_id=", 1)[1])
        assert any("http ack: step id=1" in line for line in lines), out.stdout
        assert any("command event: id=1 command=step accepted=true" in line for line in lines), out.stdout
        assert any("command event: id=2 command=resume accepted=true" in line for line in lines), out.stdout
        assert any("command event: id=3 command=step accepted=false reason=not-paused" in line for line in lines), out.stdout
        # Under default token auth, WS reconnect sends after_stream_seq and receives delta replay (non-null replay_after).
        reconnect_line = next((line for line in lines if line.startswith("reconnected:")), "")
        assert "replay_after=" in reconnect_line and "replay_after=null" not in reconnect_line, out.stdout
        assert any(line.startswith("stop-session ack: instance=") and "run_id=" in line for line in lines), out.stdout
        assert lines[-1] == "result: stopped", out.stdout
        assert gui.token not in out.stdout + out.stderr
    assert run_ids[0] != run_ids[1], run_ids


def test_time_and_stop_example_with_no_auth_and_usage_errors(tmp_path: Path) -> None:
    """Under --no-auth, reconnect falls back to full replay (replay_after=null); bad options return exit 2."""
    with gui_server(tmp_path, args=("--no-auth",), env={"REPLAY_TICK_MS": "200"}) as gui:
        assert "#token=" not in gui.url
        out = run_example("time-and-stop.mjs", gui.url, "forever.yaml", "--step", "100",
                          "--timeout", str(math.ceil(scaled(60))))
    assert out.returncode == 0, out.stdout + out.stderr
    lines = out.stdout.strip().splitlines()
    assert any("replay_after=null" in line for line in lines), out.stdout
    assert lines[-1] == "result: stopped", out.stdout

    bad_step = run_example("time-and-stop.mjs", None, "forever.yaml", "--step", "0")
    assert bad_step.returncode == 2 and "--step needs an integer" in bad_step.stderr
    missing_url = run_example("time-and-stop.mjs", None, "forever.yaml")
    assert missing_url.returncode == 2 and "VEHICLE_SIL_GUI_URL" in missing_url.stderr


# ---------------------------------------------------------------- out-of-process connect() -> stop() & immediate flush


def _run_node_module(bundle: Path, url: str, code: str) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if k != "VEHICLE_SIL_GUI_URL"}
    env["VEHICLE_SIL_GUI_URL"] = url
    env["VEHICLE_SIL_GUI_CLIENT_URL"] = bundle.as_uri()
    return subprocess.run(
        [NODE, "--input-type=module", "-e", code],
        capture_output=True, text=True, encoding="utf-8", stdin=subprocess.DEVNULL,
        env=env, timeout=scaled(60),
    )


def test_connect_immediate_actions_and_flushing_end_to_end(tmp_path: Path, bundle: Path, control_env: dict) -> None:
    """Sender (WsGateway/Hub) + receiver (Connection/Front) end-to-end:
    - connect() -> stop() when idle rejects with ActionError('not-running', ..., 'refused')
    - separate process connect() -> time.pause() / variable.write() / run.stop() awaits initial WS sync
      and flushes synced, command, operation, and error frames immediately (< BATCH_MS).
    """
    idle_stop_code = """
const { ActionError, connect } = await import(process.env.VEHICLE_SIL_GUI_CLIENT_URL);
const u = new URL(process.env.VEHICLE_SIL_GUI_URL);
const token = new URLSearchParams(u.hash.slice(1)).get('token') ?? '';
u.hash = '';
const front = await connect({ baseUrl: u.href, token });
try {
  await front.run.stop();
  console.log('UNEXPECTED_OK');
  process.exit(1);
} catch (err) {
  if (!(err instanceof ActionError) || err.code !== 'not-running' || err.outcome !== 'refused') {
    console.error('WRONG_ERROR', err);
    process.exit(1);
  }
  console.log(`idle-stop: ${err.code} (${err.outcome})`);
} finally {
  front.close();
}
"""
    start_and_wait_code = """
const { connect, until } = await import(process.env.VEHICLE_SIL_GUI_CLIENT_URL);
const u = new URL(process.env.VEHICLE_SIL_GUI_URL);
const token = new URLSearchParams(u.hash.slice(1)).get('token') ?? '';
u.hash = '';
const front = await connect({ baseUrl: u.href, token });
try {
  await front.run.start({}, { file: 'forever.yaml' });
  await until(
    front.status,
    (s) => s.link === 'online' && (s.run === 'running' || s.run === 'paused'),
    { timeoutMs: 15_000 },
  );
  await until(
    front.low.variables.state,
    (v) => v.status === 'ready',
    { timeoutMs: 15_000 },
  );
  console.log('READY');
} finally {
  front.close();
}
"""
    immediate_control_and_stop_code = """
const { connect, until, webSocketFactory } = await import(process.env.VEHICLE_SIL_GUI_CLIENT_URL);
const u = new URL(process.env.VEHICLE_SIL_GUI_URL);
const token = new URLSearchParams(u.hash.slice(1)).get('token') ?? '';
u.hash = '';

// 1. Fresh client calls variable.write() immediately after connect() while link === 'connecting':
const tWrite = performance.now();
const writer = await connect({ baseUrl: u.href, token });
await writer.variable('Engine.engine.demand').write(40);
const writeMs = performance.now() - tWrite;
writer.close();

// 2. Fresh client calls time.pause() immediately after connect() while link === 'connecting',
//    and also verifies WS error frame flushes immediately end-to-end:
const tPause = performance.now();
let rawSocket = null;
const baseSocket = webSocketFactory({ origin: u.origin });
const front = await connect({
  baseUrl: u.href,
  token,
  socket: (wsUrl, handlers) => {
    rawSocket = baseSocket(wsUrl, handlers);
    return rawSocket;
  },
});
await front.time.pause();
const pauseMs = performance.now() - tPause;

const errorSeen = new Promise((resolve) => {
  const off = front.low.connection.onBatch((batch) => {
    if (batch.some((m) => m && typeof m === 'object' && m.type === 'error' && m.code === 'invalid-watch')) {
      off();
      resolve(performance.now());
    }
  });
});
const tErr = performance.now();
rawSocket.send(JSON.stringify({ type: 'watch', id: -1 }));
const errorMs = (await errorSeen) - tErr;

// 3. Fresh client calls run.stop() immediately after connect() while link === 'connecting':
const stopper = await connect({ baseUrl: u.href, token });
await stopper.run.stop();
const ended = await until(
  front.status,
  (s) => s.run === 'ended' || s.run === 'crashed',
  { timeoutMs: 15_000 },
);
console.log(JSON.stringify({ pauseMs, writeMs, errorMs, end: ended.result?.end, ok: ended.result?.ok }));
stopper.close();
front.close();
"""
    with gui_server(tmp_path, env=control_env) as gui:
        idle = _run_node_module(bundle, gui.url, idle_stop_code)
        assert idle.returncode == 0, idle.stdout + idle.stderr
        assert idle.stdout.strip() == "idle-stop: not-running (refused)", idle.stdout

        started = _run_node_module(bundle, gui.url, start_and_wait_code)
        assert started.returncode == 0 and started.stdout.strip() == "READY", started.stdout + started.stderr

        stopped = _run_node_module(bundle, gui.url, immediate_control_and_stop_code)
        assert stopped.returncode == 0, stopped.stdout + stopped.stderr
        import json
        payload = json.loads(stopped.stdout.strip().splitlines()[-1])
        assert payload["end"] == "stopped" and payload["ok"] is True, payload
        assert isinstance(payload["errorMs"], (int, float)) and payload["errorMs"] >= 0, payload

        idle_after = _run_node_module(bundle, gui.url, idle_stop_code)
        assert idle_after.returncode == 0, idle_after.stdout + idle_after.stderr
        assert idle_after.stdout.strip() == "idle-stop: not-running (refused)", idle_after.stdout


@pytest.mark.skipif(not REAL or not real_configs.usable(real_configs.examples_dir(REAL)),
                    reason="no real launcher with the ecu_main example next to it (scripts/verify-gui.py --launcher)")
def test_connect_then_stop_immediately_with_the_real_launcher(tmp_path: Path, bundle: Path) -> None:
    """Out-of-process verification against the real C++ launcher:
    Process 1 starts simulation.yaml; Process 2 calls connect() -> front.run.stop() immediately."""
    configs = tmp_path / "configs"
    real_configs.write(configs, real_configs.examples_dir(REAL))
    start_code = """
const { connect, until } = await import(process.env.VEHICLE_SIL_GUI_CLIENT_URL);
const u = new URL(process.env.VEHICLE_SIL_GUI_URL);
const token = new URLSearchParams(u.hash.slice(1)).get('token') ?? '';
u.hash = '';
const front = await connect({ baseUrl: u.href, token });
try {
  await front.run.start({ speed: 0.5 }, { file: 'simulation.yaml' });
  await until(front.status, (s) => s.run === 'running', { timeoutMs: 30_000 });
  console.log('RUNNING');
} finally {
  front.close();
}
"""
    stop_code = """
const { connect, until } = await import(process.env.VEHICLE_SIL_GUI_CLIENT_URL);
const u = new URL(process.env.VEHICLE_SIL_GUI_URL);
const token = new URLSearchParams(u.hash.slice(1)).get('token') ?? '';
u.hash = '';
const front = await connect({ baseUrl: u.href, token });
try {
  await front.run.stop();
  const settled = await until(
    front.status,
    (s) => s.run === 'ended' || s.run === 'crashed',
    { timeoutMs: 30_000 },
  );
  console.log(`stopped: end=${settled.result?.end} ok=${settled.result?.ok}`);
} finally {
  front.close();
}
"""
    with gui_server(tmp_path, root=configs, launcher=Path(REAL)) as gui:
        started = _run_node_module(bundle, gui.url, start_code)
        assert started.returncode == 0 and started.stdout.strip() == "RUNNING", started.stdout + started.stderr
        stopped = _run_node_module(bundle, gui.url, stop_code)
        assert stopped.returncode == 0, stopped.stdout + stopped.stderr
        assert stopped.stdout.strip() == "stopped: end=stopped ok=True" or \
            stopped.stdout.strip() == "stopped: end=stopped ok=true", stopped.stdout

