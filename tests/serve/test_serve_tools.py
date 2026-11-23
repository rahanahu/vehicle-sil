# SPDX-License-Identifier: MIT
"""The tools of tests/serve on made-up data, without a launcher: the digest leaves out exactly what changes from
process to process, and the stream check finds each kind of broken stream (so that a passing serve test means the
rules were checked, not that the checker saw nothing)."""
import copy
import unittest

from serve_digest import deterministic, differences, digest, first_points_ms, first_time_ms, stream_problems
from serve_support import slope

RESULT = {
    "vehicle_sil_run": 2, "vehicle_sil": "0.0.0", "file": "sim.yaml", "result": "passed", "end": "completed", "ok": True,
    "message": "The run reached its end without errors", "strict": False, "errors": 0, "warnings": 1, "notes": 0,
    "simulation": "reached_stop_at", "interrupted_by": None, "registry_uri": "silkit://127.0.0.1:40001", "stop_at_ms": 500,
    "timing": {"speed": "max", "time_step_ms": 10, "ecu_step_ms": 10, "step_delay_ms": 0, "late_tolerance_ms": 10,
               "sim_ms": 500, "wall_ms": 31, "actual_speed": 16.1, "paced_steps": 0, "late_steps": 0, "max_late_ms": 0,
               "slipped_ms": 0, "late_ms": 0, "paused_ms": 0},
    "participants": [{"name": "T1", "required": True, "enabled": True, "manual": False, "for_network": None,
                      "ended": "exited", "exit_code": 0, "signal": None, "left_processes": False,
                      "log": "/tmp/a/T1.log", "control_link": {"state": "connected", "variables": 5}}],
    "outside": [], "traffic": {"networks": []}, "findings": [],
    "check": {"result": "passed", "findings": [], "participants": [{"name": "T1", "status": "described"}],
              "registry_uri": None},
    "scenario": {"file": "vt0.yaml", "name": "vt0", "status": "passed", "over_at_ms": 510, "steps": [], "inputs": [],
                 "lines": [], "timeline": "/tmp/a/tl-0001.json"},
}
TIMELINE = {"vehicle_sil_timeline": 1, "vehicle_sil": "0.0.0", "stage": "run", "file": "/tmp/a/sim.yaml",
            "variant": None, "params": {}, "time_step_ms": 10, "scenario": {"file": "/tmp/a/vt0.yaml", "name": "vt0"},
            "variables": [], "steps": [], "series": [{"variable": "T1.ticks", "points": [[0, 1], [10, 2]]}],
            "lines": [], "findings": [], "limits": {}}


class DigestTest(unittest.TestCase):
    def test_what_changes_from_process_to_process_is_left_out(self):
        other = copy.deepcopy(RESULT)
        other["registry_uri"] = "silkit://127.0.0.1:40999"
        other["timing"]["wall_ms"] = 900
        other["timing"]["actual_speed"] = 0.5
        other["participants"][0]["log"] = "/tmp/b/T1.log"
        other["scenario"]["timeline"] = "/tmp/b/tl-0002.json"
        other_tl = copy.deepcopy(TIMELINE)
        other_tl["file"] = "/tmp/b/sim.yaml"
        other_tl["scenario"]["file"] = "/tmp/b/vt0.yaml"
        self.assertEqual(digest(RESULT, TIMELINE), digest(other, other_tl))
        self.assertEqual(differences(deterministic(RESULT, TIMELINE), deterministic(other, other_tl)), [])

    def test_what_the_simulation_did_is_kept(self):
        for change in (lambda r, t: r["timing"].__setitem__("sim_ms", 510),
                       lambda r, t: r.__setitem__("result", "failed"),
                       lambda r, t: r["participants"][0].__setitem__("exit_code", 3),
                       lambda r, t: r["findings"].append({"severity": "warning", "code": "x", "message": "m"}),
                       lambda r, t: r["check"]["participants"][0].__setitem__("status", "describe-failed"),
                       lambda r, t: r["scenario"].__setitem__("over_at_ms", 520),
                       lambda r, t: t["series"][0]["points"].__setitem__(0, [10, 1])):
            r, t = copy.deepcopy(RESULT), copy.deepcopy(TIMELINE)
            change(r, t)
            self.assertNotEqual(digest(RESULT, TIMELINE), digest(r, t))
            self.assertNotEqual(differences(deterministic(RESULT, TIMELINE), deterministic(r, t)), [])
        # A run without a timeline is not the same as one with it.
        self.assertNotEqual(digest(RESULT), digest(RESULT, TIMELINE))

    def test_differences_name_the_key(self):
        r = copy.deepcopy(RESULT)
        r["timing"]["sim_ms"] = 510
        self.assertEqual(differences(deterministic(RESULT), deterministic(r)), ["result.timing.sim_ms: 500 != 510"])

    def test_first_times(self):
        self.assertEqual(first_time_ms([{"event": "phase"}, {"event": "time", "sim_ms": 0}, {"event": "time", "sim_ms": 9}]), 0)
        self.assertIsNone(first_time_ms([{"event": "phase"}]))
        self.assertEqual(first_points_ms(TIMELINE), [0])

    def test_slope(self):
        self.assertAlmostEqual(slope([1, 3, 5, 7]), 2.0)
        self.assertEqual(slope([4]), 0.0)


L = "0123456789abcdef"


def stream(*lines):
    """Lines with seq and t_ms filled in."""
    out = []
    for i, line in enumerate(lines):
        e = dict(line)
        e.setdefault("seq", i)
        e.setdefault("t_ms", i)
        out.append(e)
    return out


def good():
    r1, r2 = f"{L}-1", f"{L}-2"
    return stream(
        {"event": "hello", "launcher": L, "vehicle_sil_events": 3},
        {"event": "idle"},
        {"event": "heartbeat"},
        {"event": "run_start", "run": r1, "id": 7},
        {"event": "phase", "run": r1, "phase": "checking"},
        {"event": "state", "run": r1, "rev": 1},
        {"event": "heartbeat"},
        {"event": "command", "accepted": False, "reason": "busy", "id": 8},
        {"event": "state", "run": r1, "rev": 3},
        {"event": "log_dropped", "run": r1, "count": 2},
        {"event": "result", "run": r1, "launcher_ends": False, "result": {"result": "passed"}},
        {"event": "command", "accepted": False, "reason": "run-changed", "asked_run": r1},
        {"event": "idle"},
        {"event": "run_start", "run": r2, "id": 9},
        {"event": "state", "run": r2, "rev": 1},
        {"event": "result", "run": r2, "launcher_ends": True, "result": {"result": "passed"}},
    )


class StreamTest(unittest.TestCase):
    def assertProblem(self, events, part):
        problems = stream_problems(events)
        self.assertTrue(any(part in p for p in problems), f"no problem with {part!r} in {problems}")

    def test_a_good_stream_has_no_problem(self):
        self.assertEqual(stream_problems(good()), [])
        # A launcher that ends at the start: hello and its own result.
        self.assertEqual(stream_problems(stream({"event": "hello", "launcher": L},
                                                {"event": "result", "result": {"result": "usage"}})), [])

    def test_each_broken_rule_is_found(self):
        r1 = f"{L}-1"
        e = good()
        del e[4]
        self.assertProblem(e, "a line is missing")
        e = good()
        e[5]["t_ms"] = 1
        self.assertProblem(e, "t_ms goes back")
        e = good()
        e[0], e[1] = e[1], e[0]
        for i, x in enumerate(e):
            x["seq"] = i
        self.assertProblem(e, "the first line is not hello")
        e = good()
        del e[0]["launcher"]
        self.assertProblem(e, "hello has no")
        e = good()
        e[4] = {"event": "phase", "seq": 4, "t_ms": 4, "phase": "checking"}
        self.assertProblem(e, "without the Run inside the window")
        e = good()
        e[11] = {"event": "log", "seq": 11, "t_ms": 11, "run": r1}
        self.assertProblem(e, f"a line of Run {r1} after its result")
        e = good()
        e[12] = {"event": "heartbeat", "seq": 12, "t_ms": 12}
        self.assertProblem(e, "run_start before the idle")
        e = good()
        e[6] = {"event": "idle", "seq": 6, "t_ms": 6}
        self.assertProblem(e, "idle inside the window")
        e = good()
        e[8]["rev"] = 1
        self.assertProblem(e, "rev 1 does not grow")
        e = good()
        e[13]["run"] = f"{L}-3"
        for x in e[14:]:
            x["run"] = f"{L}-3"
        self.assertProblem(e, "not the next number")
        e = good()
        e[13]["run"] = "fedcba9876543210-2"
        for x in e[14:]:
            x["run"] = "fedcba9876543210-2"
        self.assertProblem(e, "is not <launcher")
        e = good()
        del e[10]["launcher_ends"]
        self.assertProblem(e, "without launcher_ends")
        e = good() + stream(*[{}] * 16)[-1:]
        e[-1] = {"event": "idle", "seq": 16, "t_ms": 16}
        self.assertProblem(e, "after the result that said the launcher ends")
        e = good()[:9]
        self.assertProblem(e, "has no result")
        e = good()
        e[11]["accepted"] = True
        self.assertProblem(e, "not a refusal")
        e = good()
        e.insert(2, {"event": "phase", "phase": "checking"})
        for i, x in enumerate(e):
            x["seq"], x["t_ms"] = i, i
        self.assertProblem(e, "phase outside a Run's window")
        e = good()
        e[7]["accepted"] = True
        self.assertProblem(e, "without the Run inside the window")


if __name__ == "__main__":
    unittest.main()
