# SPDX-License-Identifier: MIT
"""Tests of scripts/trace-report.py on made-up traces of a launcher with a scenario and one ECU: the critical
participant of each step, the synchronisation time, the launcher's hold and each write's round trip, and the exit
codes for a directory without traces, a file that is not whole and a usage error.

    python tests/scripts/test_trace_report.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("trace_report", ROOT / "scripts" / "trace-report.py")
trace_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace_report)

STEP = 1_000_000   # 1 ms of virtual time
PERIOD = 100_000   # 100 us of real time per step
STEPS = 20


def span(pid, tid, name, start, end, vt=-1, arg=0):
    return {"pid": pid, "tid": tid, "name": name, "ts": start / 1000, "ph": "X", "dur": (end - start) / 1000,
            "args": {"vt": vt, "arg": arg}}


def instant(pid, tid, name, time, vt=-1, arg=0):
    return {"pid": pid, "tid": tid, "name": name, "ts": time / 1000, "ph": "i", "s": "t", "args": {"vt": vt, "arg": arg}}


def made_up_run() -> tuple[list[dict], list[dict]]:
    """A launcher (pid 100) that holds every step for its scenario, which writes to the ECU (pid 200) in every even
    step. Times in ns from each step's base T (all made up so that each part has its own length):
      ECU step T..T+10us; launcher step T+1..T+3us, held; scenario woken T+5us, tick T+6..T+40us;
      write T+10..T+30us (send T+12..T+13, ECU handler T+18..T+19, answer T+24..T+25); gate T+45..T+51us,
      step released at T+50us."""
    launcher, ecu = [], []
    for k in range(STEPS):
        t, vt = 1_000_000_000 + k * PERIOD, k * STEP
        ecu.append(span(200, 2, "step", t, t + 10_000, vt))
        launcher.append(span(100, 1, "step", t + 1_000, t + 3_000, vt, 1))
        launcher.append(instant(100, 3, "scenario.woken", t + 5_000, vt))
        launcher.append(span(100, 3, "scenario.tick", t + 6_000, t + 40_000, vt))
        if k % 2 == 0:
            launcher.append(span(100, 3, "scenario.write", t + 10_000, t + 30_000, vt, k))
            launcher.append(span(100, 3, "hub.send", t + 12_000, t + 13_000, vt, k))
            ecu.append(span(200, 2, "agent.receive_command", t + 18_000, t + 19_000, -1, k))
            launcher.append(span(100, 1, "hub.receive_answer", t + 24_000, t + 25_000, -1, k))
        launcher.append(span(100, 4, "gate.release", t + 45_000, t + 51_000, vt))
        launcher.append(instant(100, 4, "step.release", t + 50_000, vt))
    return launcher, ecu


def write_trace(directory: Path, pid: int, name: str, events: list[dict], dropped: int = 0) -> Path:
    path = directory / f"trace-{name}-{pid}.json"
    path.write_text(json.dumps({"displayTimeUnit": "ns",
                                "otherData": {"vehicle_sil_trace": 1, "pid": pid, "process": name, "dropped": dropped},
                                "traceEvents": events}), encoding="utf-8")
    return path


def run(*args: str) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = trace_report.main(list(args))
    return code, out.getvalue(), err.getvalue()


class TraceReportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.details = self.dir / "out" / "details.json"

    def tearDown(self):
        self.tmp.cleanup()

    def test_steps_hold_and_round_trip(self):
        launcher, ecu = made_up_run()
        write_trace(self.dir, 100, "vehicle_sil_launch", launcher)
        write_trace(self.dir, 200, "Engine", ecu, dropped=3)
        code, out, err = run(str(self.dir), "--skip-ms", "0", "--json", str(self.details))
        self.assertEqual(code, 0, err)
        self.assertTrue(out.startswith("trace-report: 2 processes (launcher: vehicle_sil_launch; ECUs: Engine), "
                                       "step 1 ms, 14 steps from 0 to 13 ms"), out)
        self.assertIn("records dropped (the rings kept the newest; raise VEHICLE_SIL_TRACE_EVENTS for more): Engine 3", out)
        d = json.loads(self.details.read_text(encoding="utf-8"))
        self.assertEqual(d["last_to_complete"], {"vehicle_sil_launch": 14})
        self.assertEqual(d["period"]["p50_us"], 100.0)
        self.assertEqual(d["critical_step"]["p50_us"], 49.0)   # launcher step start to its release
        self.assertEqual(d["sync"]["p50_us"], 50.0)            # release to the ECU's next start
        self.assertEqual(d["slowest_ecu_step"]["p50_us"], 10.0)
        with_writes = d["hold_with_writes"]
        self.assertEqual(with_writes["writes_per_step"], 1)
        self.assertEqual({k: with_writes[k]["p50_us"] for k in ("launcher_step", "scenario_wake", "writes", "rest_of_tick",
                                                                "gate_wake", "hold")},
                         {"launcher_step": 2.0, "scenario_wake": 2.0, "writes": 20.0, "rest_of_tick": 14.0,
                          "gate_wake": 5.0, "hold": 49.0})
        self.assertEqual(d["hold_without_writes"]["rest_of_tick"]["p50_us"], 34.0)
        w = d["write_round_trip"]
        self.assertEqual(w["n"], 7)  # the even steps among 0..13
        self.assertEqual({k: w[k]["p50_us"] for k in ("total", "hub to send", "send", "to the ECU", "ECU handler", "back",
                                                      "hub handler", "scenario wake")},
                         {"total": 20.0, "hub to send": 2.0, "send": 1.0, "to the ECU": 5.0, "ECU handler": 1.0,
                          "back": 5.0, "hub handler": 1.0, "scenario wake": 5.0})
        self.assertEqual(w["share_arriving_during_an_ecu_step"], 0)
        self.assertEqual(d["clock_offset_from_launcher_us"], {"Engine": 0.0})
        self.assertIn(f"details: {self.details}", out)

    def test_merged_file_holds_every_process(self):
        launcher, ecu = made_up_run()
        write_trace(self.dir, 100, "vehicle_sil_launch", launcher)
        write_trace(self.dir, 200, "Engine", ecu)
        merged = self.dir / "merged.json"
        code, _, err = run(str(self.dir), "--json", str(self.details), "--merged", str(merged), "--skip-ms", "0")
        self.assertEqual(code, 0, err)
        events = json.loads(merged.read_text(encoding="utf-8"))["traceEvents"]
        self.assertEqual({e["pid"] for e in events}, {100, 200})
        self.assertEqual(len(events), len(launcher) + len(ecu))

    def test_a_directory_without_traces(self):
        code, out, err = run(str(self.dir), "--json", str(self.details))
        self.assertEqual((code, out), (1, ""))
        self.assertIn(f"run with the environment variable VEHICLE_SIL_TRACE={self.dir}", err)

    def test_a_file_that_is_not_whole(self):
        (self.dir / "trace-Engine-200.json").write_text('{"otherData":{"pid":200,"process":"Engine"},"traceEvents":[\n{',
                                                        encoding="utf-8")
        code, _, err = run(str(self.dir), "--json", str(self.details))
        self.assertEqual(code, 1)
        self.assertIn("is not a whole trace", err)

    def test_too_few_steps(self):
        launcher, ecu = made_up_run()
        write_trace(self.dir, 200, "Engine", ecu)
        code, _, err = run(str(self.dir), "--json", str(self.details), "--skip-ms", "100")
        self.assertEqual(code, 1)
        self.assertIn("fewer than 3 steps after the first 100 ms", err)

    def test_usage(self):
        self.assertEqual(run()[0], 2)
        self.assertEqual(run(str(self.dir), "--skip-ms", "-1")[0], 2)
        self.assertEqual(run("--help")[0], 0)


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
