# SPDX-License-Identifier: MIT
"""Integration tests for python/vehicle_sil_control/serve.py against live vehicle_sil_launch serve + gRPC.

Environment: VEHICLE_SIL_LAUNCH_EXE, VEHICLE_SIL_EXAMPLE_DIR (examples/ecu_main in the build tree),
VEHICLE_SIL_LAUNCH_SCRATCH (writable scratch directory), PYTHONPATH with python/, VEHICLE_SIL_CONTROL_STUBS.
"""
from __future__ import annotations

import gc
import glob
import hashlib
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import weakref
from unittest import mock

import grpc

from vehicle_sil_control import Control, ControlError, Serve, ServeError, ms

_COMMON_DIR = str(pathlib.Path(__file__).resolve().parents[1] / "common")
if _COMMON_DIR not in sys.path:
    sys.path.insert(0, _COMMON_DIR)
from timeout_scale import scaled, timeout_scale  # noqa: E402

EXE = ".exe" if os.name == "nt" else ""
LAUNCH = EXAMPLE = SCRATCH = None


def setUpModule():
    # Read here, not on import (scripts/verify-python.py imports the module without CMake env vars).
    global LAUNCH, EXAMPLE, SCRATCH
    try:
        timeout_scale()
    except ValueError as e:
        raise SystemExit(str(e)) from None
    LAUNCH = os.environ["VEHICLE_SIL_LAUNCH_EXE"]
    EXAMPLE = pathlib.Path(os.environ["VEHICLE_SIL_EXAMPLE_DIR"])
    SCRATCH = pathlib.Path(os.environ["VEHICLE_SIL_LAUNCH_SCRATCH"])


def _config(name: str) -> pathlib.Path:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    engine = (EXAMPLE / "engine" / f"vehicle_sil_engine_ecu{EXE}").as_posix()
    dashboard = (EXAMPLE / "dashboard" / f"vehicle_sil_dashboard_ecu{EXE}").as_posix()
    path = SCRATCH / f"{name}.yaml"
    path.write_text(
        f"vehicle_sil_simulation: 2\n"
        f"networks:\n"
        f"  - name: J1939_CAN\n"
        f"    kind: can\n"
        f"participants:\n"
        f"  - name: Engine\n"
        f"    command: ['{engine}']\n"
        f"    step_ms: 10\n"
        f"  - name: Dashboard\n"
        f"    command: ['{dashboard}']\n"
        f"    step_ms: 5\n",
        encoding="utf-8",
    )
    return path


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


class ServeClientTest(unittest.TestCase):
    def test_prepare_status_and_clean_exit(self) -> None:
        cfg = _config("serve_prepare_status")
        expected_sha = hashlib.sha256(cfg.read_bytes()).hexdigest()

        with Serve(LAUNCH, cfg, control=True, startup_timeout=scaled(30.0)) as runner:
            self.assertEqual(len(runner.instance), 16)
            st0 = runner.status(timeout=scaled(30.0))
            self.assertEqual(st0["launcher_state"], "unprepared")

            prep = runner.prepare(timeout=scaled(30.0))
            self.assertTrue(prep["accepted"])
            self.assertEqual(prep["status"]["launcher_state"], "ready")
            self.assertTrue(prep["status"]["eligibility"]["eligible"])
            self.assertEqual(prep["metadata"]["file_sha256"], expected_sha)
            self.assertEqual(set(prep["metadata"]["participants"]), {"Engine", "Dashboard"})

            st1 = runner.status(timeout=scaled(30.0))
            self.assertEqual(st1["launcher_state"], "ready")

        self.assertEqual(runner.returncode, 0)

    def test_inline_intent_and_oversized_fallback(self) -> None:
        cfg = _config("serve_intent_fallback")
        scenario = SCRATCH / "serve_smoke.yaml"
        scenario.write_text(
            "vehicle_sil_scenario: 1\n"
            "name: serve smoke\n"
            "steps:\n"
            "  - at_ms: 0\n"
            "    set: { Engine.engine.throttle: 50 }\n"
            "  - wait_until: { Engine.engine.speed: { ge: 1700 } }\n"
            "    timeout_ms: 500\n",
            encoding="utf-8",
        )

        before_tmp = set(glob.glob(os.path.join(tempfile.gettempdir(), "vehicle-sil-serve-request-*.json")))
        with Serve(LAUNCH, cfg, control=True, startup_timeout=scaled(30.0)) as runner:
            with runner.start(scenario=scenario.as_posix(), timeout=scaled(60.0)) as run1:
                res1 = run1.wait(timeout=scaled(120.0))
                self.assertEqual(res1["result"], "passed", res1)
                self.assertFalse(run1.launcher_ends)

            with runner.start(params={"pad": "x" * 70000}, stop_at_ms=200, timeout=scaled(60.0)) as run2:
                res2 = run2.wait(timeout=scaled(120.0))
                self.assertEqual(res2["result"], "passed", res2)
                self.assertFalse(run2.launcher_ends)

        after_tmp = set(glob.glob(os.path.join(tempfile.gettempdir(), "vehicle-sil-serve-request-*.json")))
        self.assertEqual(after_tmp - before_tmp, set())

    def test_run_control_auto_binding_and_stale_run_changed(self) -> None:
        cfg = _config("serve_run_binding")
        address = f"127.0.0.1:{_free_port()}"

        with Serve(LAUNCH, cfg, control=True, address=address, startup_timeout=scaled(30.0)) as runner:
            # Ephemeral default address=0 check is covered in other tests; here fixed address verifies run-changed.
            with runner.start(start_paused=True, stop_at_ms=5000, timeout=scaled(60.0)) as run1:
                c1 = run1.control(timeout=scaled(10.0))
                self.assertEqual(c1.run, run1.id)
                c1.sim.wait_paused(timeout=scaled(30.0))
                seq = c1.write({"Engine": {"engine.throttle": 50.0}})
                c1.wait(seq, until="queued", timeout=scaled(30.0))
                c1.sim.step(ms(100))
                c1.sim.wait_paused(timeout=scaled(30.0))
                run1.stop()
                res1 = run1.wait(timeout=scaled(120.0))
                self.assertEqual(res1["result"], "stopped")
                self.assertFalse(run1.launcher_ends)

            try:
                with runner.start(start_paused=True, stop_at_ms=200, timeout=scaled(60.0)) as run2:
                    self.assertNotEqual(run2.id, run1.id)
                    with run2.control(timeout=scaled(10.0)) as c2:
                        self.assertEqual(c2.run, run2.id)
                        c2.sim.wait_paused(timeout=scaled(30.0))
                        self.assertEqual(c1.current_run(), run2.id)
                        for call in (
                            lambda: c1.peers(),
                            lambda: c1.write({"Engine": {"engine.throttle": 10.0}}),
                            lambda: c1.sim.pause(),
                            lambda: c1.sim.stop(),
                        ):
                            with self.assertRaises(ControlError) as ctx:
                                call()
                            self.assertEqual(ctx.exception.code, grpc.StatusCode.FAILED_PRECONDITION)
                            self.assertEqual(ctx.exception.reason, "run-changed")

                        c2.sim.resume()
                    res2 = run2.wait(timeout=scaled(120.0))
                    self.assertEqual(res2["result"], "passed")
                    self.assertFalse(run2.launcher_ends)
            finally:
                c1.close()

    def test_stop_modes_force_false_true_and_none(self) -> None:
        cfg = _config("serve_stop_modes")
        with Serve(LAUNCH, cfg, control=True, startup_timeout=scaled(30.0)) as runner:
            for force_arg in (False, True, None):
                with runner.start(start_paused=True, timeout=scaled(60.0)) as run:
                    with run.control(timeout=scaled(10.0)) as ctrl:
                        ctrl.sim.wait_paused(timeout=scaled(30.0))
                    run.stop(force=force_arg)
                    if force_arg is False:
                        run.stop(force=False)
                    res = run.wait(timeout=scaled(120.0))
                    self.assertEqual(res["result"], "stopped")
                    self.assertFalse(run.launcher_ends)

    def test_vehicle_sil_trace_fast_fail(self) -> None:
        cfg = _config("serve_trace_fail")
        trace_path = (SCRATCH / "trace_fail.vsiltrace").as_posix()
        t0 = time.monotonic()
        with mock.patch.dict(os.environ, {"VEHICLE_SIL_TRACE": trace_path}):
            with self.assertRaises(ServeError) as ctx:
                Serve(LAUNCH, cfg, stderr=subprocess.PIPE, startup_timeout=scaled(30.0))
        self.assertLess(time.monotonic() - t0, scaled(10.0))
        self.assertIn("VEHICLE_SIL_TRACE", str(ctx.exception))

    def test_early_with_exit_keeps_serve_alive(self) -> None:
        cfg = _config("serve_early_exit")
        with Serve(LAUNCH, cfg, control=True, startup_timeout=scaled(30.0)) as runner:
            with runner.start(start_paused=True, timeout=scaled(60.0)) as run1:
                with run1.control(timeout=scaled(10.0)) as c1:
                    c1.sim.wait_paused(timeout=scaled(30.0))

            self.assertIsNotNone(run1.result)
            self.assertEqual(run1.result["result"], "stopped")
            self.assertFalse(run1.launcher_ends)
            self.assertIsNone(runner.returncode)
            self.assertIn(runner.status(timeout=scaled(30.0))["launcher_state"], ("unprepared", "ready"))

            with runner.start(stop_at_ms=200, timeout=scaled(60.0)) as run2:
                self.assertTrue(run2.id.startswith(f"{runner.instance}-"))
                res2 = run2.wait(timeout=scaled(120.0))
                self.assertEqual(res2["result"], "passed")

    def test_sequential_runs_release_handles_and_on_event_streams(self) -> None:
        cfg = _config("serve_bounded_history")
        seen: list[dict] = []

        with Serve(
            LAUNCH,
            cfg,
            control=True,
            keep_logs=False,
            on_event=seen.append,
            startup_timeout=scaled(30.0),
        ) as runner:
            self.assertFalse(hasattr(runner, "events"))
            refs = []
            for idx in range(3):
                run = runner.start(stop_at_ms=50, timeout=scaled(60.0))
                refs.append(weakref.ref(run))
                res = run.wait(timeout=scaled(120.0))
                self.assertEqual(res["result"], "passed", res)
                self.assertFalse(any(e.get("event") == "log" for e in run.events))
                self.assertTrue(
                    any(e.get("event") == "result" and e.get("run") == run.id for e in seen),
                    f"run {idx} result should have reached on_event before wait() returned",
                )
                del run
                gc.collect()
                self.assertIsNone(refs[-1](), f"completed RunHandle {idx} should be collected")

        seqs = [e["seq"] for e in seen if "seq" in e]
        self.assertEqual(seqs, sorted(seqs))
        self.assertGreaterEqual(sum(1 for e in seen if e.get("event") == "run_start"), 3)
        self.assertGreaterEqual(sum(1 for e in seen if e.get("event") == "result"), 3)
        self.assertEqual(seen[0].get("event"), "hello")


if __name__ == "__main__":
    unittest.main()
