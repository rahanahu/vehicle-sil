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


def flow(ph, pid, tid, name, time, fid):
    e = {"pid": pid, "tid": tid, "name": name, "ts": time / 1000, "ph": ph, "cat": "flow", "id": fid}
    return {**e, "bp": "e"} if ph == "f" else e


def command_flow(seq, session):
    """As vehicle_sil::trace::command_flow()."""
    return (((seq << 20) ^ (session & 0xFFFFF)) & ((1 << 52) - 1)) * 2


def round_trip(launcher, ecu, pid, session, seq, t, later=0):
    """One agent's part of write `seq` (its times as in made_up_run() from `t`, its answer `later` ns later)."""
    c = command_flow(seq, session)
    launcher.append(span(100, 3, "hub.send", t + 12_000, t + 13_000, -1, seq))
    launcher.append(flow("s", 100, 3, "command", t + 12_500, c))
    ecu.append(span(pid, 2, "agent.receive_command", t + 18_000 + later, t + 19_000 + later, -1, seq))
    ecu.append(flow("f", pid, 2, "command", t + 18_100 + later, c))
    ecu.append(flow("s", pid, 2, "answer", t + 18_900 + later, c + 1))
    launcher.append(span(100, 1, "hub.receive_answer", t + 24_000 + later, t + 25_000 + later, -1, seq))
    launcher.append(flow("f", 100, 1, "answer", t + 24_100 + later, c + 1))


def instant(pid, tid, name, time, vt=-1, arg=0):
    return {"pid": pid, "tid": tid, "name": name, "ts": time / 1000, "ph": "i", "s": "t", "args": {"vt": vt, "arg": arg}}


def made_up_run(two=()) -> tuple[list[dict], list[dict]]:
    """A launcher (pid 100) that holds every step for its scenario, which writes to the ECU (pid 200) in every even
    step. Times in ns from each step's base T (all made up so that each part has its own length):
      ECU step T..T+10us; launcher step T+1..T+3us, held; scenario woken T+5us, tick T+6..T+40us;
      write T+10..T+14us (send T+12..T+13, ECU handler T+18..T+19, answer T+24..T+25), the wait for it (settle)
      T+14..T+30us; gate T+45..T+51us, step released at T+50us.
    In the steps of `two` a second write follows, T+14..T+16us (all its times 3 us after the first's), and the wait
    is T+16..T+30us."""
    launcher, ecu = [], []
    for k in range(STEPS):
        t, vt = 1_000_000_000 + k * PERIOD, k * STEP
        ecu.append(span(200, 2, "step", t, t + 10_000, vt))
        launcher.append(span(100, 1, "step", t + 1_000, t + 3_000, vt, 1))
        launcher.append(instant(100, 3, "scenario.woken", t + 5_000, vt))
        launcher.append(span(100, 3, "scenario.tick", t + 6_000, t + 40_000, vt))
        if k % 2 == 0:
            launcher.append(span(100, 3, "scenario.write", t + 10_000, t + 14_000, vt, k))
            round_trip(launcher, ecu, 200, 7, k, t)
            settle = t + 14_000
            if k in two:
                launcher.append(span(100, 3, "scenario.write", t + 14_000, t + 16_000, vt, 1000 + k))
                round_trip(launcher, ecu, 200, 7, 1000 + k, t + 3_000)
                settle = t + 16_000
            launcher.append(span(100, 3, "scenario.settle", settle, t + 30_000, -1, 2 if k in two else 1))
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
        self.assertEqual({k: with_writes[k]["p50_us"] for k in ("launcher_step", "scenario_wake", "writes", "settle",
                                                                "rest_of_tick", "gate_wake", "hold")},
                         {"launcher_step": 2.0, "scenario_wake": 2.0, "writes": 4.0, "settle": 16.0, "rest_of_tick": 14.0,
                          "gate_wake": 5.0, "hold": 49.0})
        self.assertEqual(d["hold_without_writes"]["rest_of_tick"]["p50_us"], 34.0)
        w = d["write_round_trip"]
        self.assertEqual(w["n"], 7)  # the even steps among 0..13
        self.assertEqual({k: w[k]["p50_us"] for k in ("total", "hub to send", "send", "to the ECU", "ECU handler", "back",
                                                      "hub handler", "the other writes", "scenario wake")},
                         {"total": 20.0, "hub to send": 2.0, "send": 1.0, "to the ECU": 5.0, "ECU handler": 1.0,
                          "back": 5.0, "hub handler": 1.0, "the other writes": 0.0, "scenario wake": 5.0})
        self.assertEqual(w["share_arriving_during_an_ecu_step"], 0)
        self.assertEqual(d["clock_offset_from_launcher_us"], {"Engine": 0.0})
        self.assertIn(f"details: {self.details}", out)

    def test_writes_of_one_tick_end_with_the_wait_for_all_of_them(self):
        launcher, ecu = made_up_run(two=(4,))
        write_trace(self.dir, 100, "vehicle_sil_launch", launcher)
        write_trace(self.dir, 200, "Engine", ecu)
        code, out, err = run(str(self.dir), "--skip-ms", "0", "--json", str(self.details))
        self.assertEqual(code, 0, err)
        d = json.loads(self.details.read_text(encoding="utf-8"))
        self.assertEqual(d["hold_with_writes"]["writes_per_step"], 1.14)  # 8 writes in 7 steps
        w = d["write_round_trip"]
        self.assertEqual(w["n"], 8)
        # The first write of step 4 waits 3 us for the answer to the second; both end with the wait, 2 us after
        # the last answer.
        self.assertEqual({k: (w[k]["p50_us"], w[k]["p99_us"]) for k in ("total", "the other writes", "scenario wake")},
                         {"total": (20.0, 20.0), "the other writes": (0.0, 3.0), "scenario wake": (5.0, 5.0)})
        self.assertIn("wait for the agents (settle)", out)
        self.assertIn("    the other writes", out)

    def test_a_write_to_two_ecus_is_taken_by_the_last_answer(self):
        launcher, ecu = made_up_run()
        other = [span(300, 2, "step", e["ts"] * 1000, e["ts"] * 1000 + 5_000, e["args"]["vt"])
                 for e in ecu if e["name"] == "step"]
        t = 1_000_000_000 + 4 * PERIOD  # step 4 writes to both: the second ECU answers 2 us later
        round_trip(launcher, other, 300, 9, 4, t, later=2_000)
        write_trace(self.dir, 100, "vehicle_sil_launch", launcher)
        write_trace(self.dir, 200, "Engine", ecu)
        write_trace(self.dir, 300, "Dashboard", other)
        code, _, err = run(str(self.dir), "--skip-ms", "0", "--json", str(self.details))
        self.assertEqual(code, 0, err)
        w = json.loads(self.details.read_text(encoding="utf-8"))["write_round_trip"]
        self.assertEqual((w["n"], w["with_more_targets"]), (7, 1))
        self.assertEqual(w["to the ECU"]["p99_us"], 7.0)   # the later answer of the write of step 4
        self.assertEqual(w["to the ECU"]["p50_us"], 5.0)

    def test_files_of_several_hosts_with_one_pid(self):
        launcher, ecu = made_up_run()
        write_trace(self.dir, 100, "vehicle_sil_launch", launcher)
        write_trace(self.dir, 200, "Engine", ecu)
        ecu_on_another_host = [{**e, "pid": 100} for e in ecu]
        path = self.dir / "trace-Engine-100.json"
        path.write_text(json.dumps({"otherData": {"vehicle_sil_trace": 1, "pid": 100, "process": "Engine"},
                                    "traceEvents": ecu_on_another_host}), encoding="utf-8")
        (self.dir / "trace-Engine-200.json").unlink()
        code, out, err = run(str(self.dir), "--skip-ms", "0", "--json", str(self.details))
        self.assertEqual(code, 0, err)
        self.assertIn("2 processes (launcher: vehicle_sil_launch; ECUs: Engine)", out)

    def test_several_runs_in_one_file(self):
        launcher, ecu = made_up_run()
        again = [{**e, "ts": e["ts"] + 10_000} for e in ecu]  # a second run, 10 ms later, from 0 again
        write_trace(self.dir, 200, "Engine", ecu + again)
        code, out, err = run(str(self.dir), "--json", str(self.details), "--skip-ms", "0")
        self.assertEqual((code, out), (1, ""))
        self.assertIn("the file of Engine holds more than one run (its steps start again at 0 ms after 19 ms)", err)

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
