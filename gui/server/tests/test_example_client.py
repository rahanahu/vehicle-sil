"""The example clients of the GUI server's interface, run with Node against the GUI server and the fake launcher:
examples/gui_client/run.mjs (plain JavaScript, no library) and run-with-library.mjs (the client library gui/client,
bundled into one ESM file by `npm run build:client` in gui/web). Each checks the file, starts a run, reads what
comes and waits for the end.

scripts/verify-gui.py runs this file as its own step (example), after the web step; the server step leaves it out.
The bundle is built again here (that needs gui/web/node_modules), so the test never tries an old one."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

from conftest import gui_server

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


def run_example(name: str, url: str | None, *args: str) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if k != "VEHICLE_SIL_GUI_URL"}
    if url is not None:
        env["VEHICLE_SIL_GUI_URL"] = url  # not on the command line, where other users could read the token
    return subprocess.run([NODE, str(EXAMPLES / name), *args], capture_output=True, text=True, encoding="utf-8",
                          stdin=subprocess.DEVNULL, env=env, timeout=90)


@pytest.mark.parametrize("example", ["run.mjs", "run-with-library.mjs"])
@pytest.mark.parametrize("file, code, last", [
    ("simulation.yaml", 0, "result: passed"),
    ("participant-crash.yaml", 1, "result: failed"),
    ("launcher-crash.yaml", 1, "result: none"),
    ("check-error.yaml", 1, "check: failed"),
])
def test_runs_a_configuration_to_its_end(tmp_path: Path, bundle: Path, example: str, file: str, code: int,
                                         last: str) -> None:
    with gui_server(tmp_path) as gui:
        out = run_example(example, gui.url, file, "--timeout", "60")
    assert out.returncode == code, out.stdout + out.stderr
    assert out.stdout.strip().splitlines()[-1].startswith(last), out.stdout + out.stderr
    assert gui.token not in out.stdout + out.stderr


@pytest.mark.parametrize("example", ["run.mjs", "run-with-library.mjs"])
def test_says_why_it_cannot(tmp_path: Path, bundle: Path, example: str) -> None:
    with gui_server(tmp_path) as gui:
        wrong = run_example(example, gui.url.split("#", 1)[0] + "#token=wrong", "simulation.yaml")
        missing = run_example(example, gui.url, "no-such-file.yaml")
        # the URL may also come first on the command line
        given = run_example(example, None, gui.url, "check-error.yaml")
    assert wrong.returncode == 1 and "unauthorized" in wrong.stdout, wrong.stdout + wrong.stderr
    assert missing.returncode == 1 and "file-not-found" in missing.stdout, missing.stdout + missing.stderr
    assert given.returncode == 1 and "check: failed" in given.stdout, given.stdout + given.stderr
    usage = run_example(example, None)
    assert usage.returncode == 2 and "VEHICLE_SIL_GUI_URL" in usage.stderr, usage.stdout + usage.stderr
