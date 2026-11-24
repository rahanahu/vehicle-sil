# SPDX-License-Identifier: MIT
"""Acceptance tests for vehicle_sil_launch serve: repetitions (vt=0), Cold equivalence across
scenarios/protocols/faults/binary swap, fixed-registry refusal, and LIN repetitions."""
from __future__ import annotations

import hashlib
import os
import pathlib
import statistics
import time
import unittest

import serve_fixtures as fx
from serve_digest import deterministic, differences, digest, first_points_ms, first_time_ms, stream_problems
from serve_support import (
    EXIT_IDLE_EOF,
    Scratch,
    Serve,
    WINDOWS,
    cold,
    env_path,
    free_port,
    runs,
    say,
    scaled,
)


def percentile(values, pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, max(0, int(round((pct / 100.0) * (len(ordered) - 1)))))
    return ordered[idx]


class ServeBasicAcceptanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.launch = env_path("VEHICLE_SIL_LAUNCH_EXE")

    def assert_same_as_cold(self, label: str, cold_run, serve_run) -> None:
        a = deterministic(cold_run.result, cold_run.timeline)
        b = deterministic(serve_run.result, serve_run.timeline)
        diffs = differences(a, b)
        self.assertEqual(diffs, [], f"{label}: serve differs from cold: {diffs}")
        self.assertEqual(digest(cold_run.result, cold_run.timeline),
                         digest(serve_run.result, serve_run.timeline), label)

    def test_01_many_runs_same_launcher_vt0_1ecu_and_10ecu(self) -> None:
        # 1 ECU: 50 Runs (configurable via VEHICLE_SIL_SERVE_RUNS_1), every Run starts at vt=0.
        n1 = runs("VEHICLE_SIL_SERVE_RUNS_1", 50)
        scratch = Scratch("basic_vt0_1ecu")
        cfg = fx.bench(scratch, ["T1"])
        scen = fx.vt0_scenario(scratch)
        req = scratch.request("vt0", scenario=scen.name)

        cold_times = []
        cold_digests = set()
        for i in range(3):
            t0 = time.monotonic()
            c = cold(self.launch, cfg, req, scratch.timeline(f"cold1_{i}"))
            cold_times.append((time.monotonic() - t0) * 1000.0)
            self.assertEqual(c.returncode, 0, c.stderr)
            self.assertEqual(c.result["result"], "passed")
            cold_digests.add(digest(c.result, c.timeline))
        self.assertEqual(len(cold_digests), 1)
        expected_digest = next(iter(cold_digests))

        hot_times = []
        hot_digests = set()
        with Serve(self.launch, cfg, keep_logs=False) as srv:
            srv.ready()
            for i in range(n1):
                t0 = time.monotonic()
                rec = srv.run_once(req, scratch.timeline(f"hot1_{i}"))
                hot_times.append((time.monotonic() - t0) * 1000.0)
                self.assertEqual(rec.result["result"], "passed", f"1 ECU run {i + 1}")
                self.assertFalse(rec.launcher_ends)
                self.assertEqual(first_time_ms(rec.window), 0, f"1 ECU run {i + 1} first sim_ms")
                pts = first_points_ms(rec.timeline)
                self.assertTrue(pts and all(p == 0 for p in pts), f"1 ECU run {i + 1} timeline starts {pts}")
                hot_digests.add(digest(rec.result, rec.timeline))
            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(srv.bad_lines, [])
            self.assertEqual(stream_problems(srv.snapshot()), [])
        self.assertEqual(hot_digests, {expected_digest})
        say(f"[perf] 1-ECU ({n1} hot vs 3 cold): hot median={statistics.median(hot_times):.1f}ms "
            f"p90={percentile(hot_times, 90):.1f}ms | cold median={statistics.median(cold_times):.1f}ms")

        # 10 ECU: 20 Runs (configurable via VEHICLE_SIL_SERVE_RUNS_10).
        n10 = runs("VEHICLE_SIL_SERVE_RUNS_10", 20)
        scratch10 = Scratch("basic_vt0_10ecu")
        cfg10 = fx.ten_ecus(scratch10)
        scen10 = fx.vt0_scenario(scratch10)
        req10 = scratch10.request("vt0", scenario=scen10.name)
        cold10_times = []
        c10_ref = None
        for i in range(3):
            t0 = time.monotonic()
            c10 = cold(self.launch, cfg10, req10, scratch10.timeline(f"cold10_{i}"))
            cold10_times.append((time.monotonic() - t0) * 1000.0)
            self.assertEqual(c10.returncode, 0, c10.stderr)
            self.assertEqual(c10.result["result"], "passed")
            c10_ref = c10

        hot10_times = []
        with Serve(self.launch, cfg10, keep_logs=False) as srv:
            srv.ready()
            for i in range(n10):
                t0 = time.monotonic()
                rec = srv.run_once(req10, scratch10.timeline(f"hot10_{i}"))
                hot10_times.append((time.monotonic() - t0) * 1000.0)
                self.assertEqual(rec.result["result"], "passed", f"10 ECU run {i + 1}")
                self.assertEqual(first_time_ms(rec.window), 0, f"10 ECU run {i + 1} first sim_ms")
                self.assert_same_as_cold(f"10 ECU run {i + 1}", c10_ref, rec)
            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])
        say(f"[perf] 10-ECU ({n10} hot vs 3 cold): hot median={statistics.median(hot10_times):.1f}ms "
            f"p90={percentile(hot10_times, 90):.1f}ms | cold median={statistics.median(cold10_times):.1f}ms")

    def test_02_cold_equivalence_aba_protocols_faults_and_binary_swap(self) -> None:
        # A -> B -> A on 1 ECU.
        scratch = Scratch("basic_aba")
        cfg = fx.bench(scratch, ["T1"])
        vt0 = fx.vt0_scenario(scratch)
        probe = fx.probe_scenario(scratch)
        req_a = scratch.request("a", scenario=vt0.name)
        req_b = scratch.request("b", scenario=probe.name)
        cold_a = cold(self.launch, cfg, req_a, scratch.timeline("cold_a"))
        cold_a2 = cold(self.launch, cfg, req_a, scratch.timeline("cold_a2"))
        self.assertEqual(digest(cold_a.result, cold_a.timeline), digest(cold_a2.result, cold_a2.timeline))
        cold_b = cold(self.launch, cfg, req_b, scratch.timeline("cold_b"))

        with Serve(self.launch, cfg) as srv:
            srv.ready()
            self.assert_same_as_cold("A1", cold_a, srv.run_once(req_a, scratch.timeline("hot_a1")))
            self.assert_same_as_cold("B", cold_b, srv.run_once(req_b, scratch.timeline("hot_b")))
            self.assert_same_as_cold("A2", cold_a, srv.run_once(req_a, scratch.timeline("hot_a2")))
            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])

        # J1939 CAN: two scenarios on examples/ecu_main/simulation.yaml.
        j1939_cfg = fx.j1939_example()
        if j1939_cfg.exists():
            sj = Scratch("basic_j1939")
            r_step = sj.request("step", scenario="scenarios/throttle_step.yaml")
            r_wave = sj.request("wave", scenario="scenarios/throttle_wave.yaml")
            c_step = cold(self.launch, j1939_cfg, r_step, sj.timeline("c_step"))
            c_wave = cold(self.launch, j1939_cfg, r_wave, sj.timeline("c_wave"))
            self.assertEqual(c_step.result["result"], "passed")
            self.assertEqual(c_wave.result["result"], "passed")
            with Serve(self.launch, j1939_cfg) as srv:
                srv.ready()
                for k in range(3):
                    self.assert_same_as_cold(f"j1939 step {k}", c_step, srv.run_once(r_step, sj.timeline(f"h_step_{k}")))
                    self.assert_same_as_cold(f"j1939 wave {k}", c_wave, srv.run_once(r_wave, sj.timeline(f"h_wave_{k}")))
                srv.close_input()
                self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
                self.assertEqual(stream_problems(srv.snapshot()), [])

        # Ethernet: examples/ethernet/simulation.yaml.
        eth_cfg = fx.ethernet_example()
        if eth_cfg.exists():
            se = Scratch("basic_eth")
            r_eth = se.request("eth", scenario="scenarios/exchange.yaml")
            c_eth = cold(self.launch, eth_cfg, r_eth, se.timeline("c_eth"))
            self.assertEqual(c_eth.result["result"], "passed")
            with Serve(self.launch, eth_cfg) as srv:
                srv.ready()
                for k in range(3):
                    self.assert_same_as_cold(f"eth {k}", c_eth, srv.run_once(r_eth, se.timeline(f"h_eth_{k}")))
                srv.close_input()
                self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
                self.assertEqual(stream_problems(srv.snapshot()), [])

        # ECU faults followed by normal Run recovery.
        sf = Scratch("basic_faults")
        fcfg = fx.faults(sf)
        fscen = fx.vt0_scenario(sf)
        norm_req = sf.request("normal", scenario=fscen.name, disable=["Late", "Quick"])
        fx.set_behaviour(sf, "T1", fx.NORMAL)
        fx.set_behaviour(sf, "T2", fx.NORMAL)
        fault_args = [
            "--timeout-ms", str(int(scaled(800))),
            "--participant-start-timeout-ms", str(int(scaled(500))),
            "--participant-stop-timeout-ms", str(int(scaled(300))),
        ]
        cold_norm = cold(self.launch, fcfg, norm_req, sf.timeline("cold_norm"),
                         arguments=fault_args)
        self.assertEqual(cold_norm.result["result"], "passed")

        with Serve(self.launch, fcfg, arguments=fault_args) as srv:
            srv.ready()
            for fname, (beh, dis) in fx.FAULTS.items():
                fx.set_behaviour(sf, "T1", beh)
                freq = sf.request(f"fault_{fname}", scenario=fscen.name, disable=dis)
                frec = srv.run_once(freq)
                self.assertEqual(frec.result["result"], "failed", f"fault {fname} must fail")
                self.assertFalse(frec.launcher_ends, f"fault {fname} must keep launcher alive")
                # Immediately run a normal Run and check Cold equivalence.
                fx.set_behaviour(sf, "T1", fx.NORMAL)
                nrec = srv.run_once(norm_req, sf.timeline(f"after_{fname}"))
                self.assert_same_as_cold(f"recovery after {fname}", cold_norm, nrec)
            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])

        # A6 re-verification (POSIX): swap ECU binary with identical size & mtime -> re-described.
        if not WINDOWS:
            ss = Scratch("basic_swap")
            exe_path = fx.wrapper(ss, "ecu_bin.sh", fx.test_ecu(), size=512)
            st = exe_path.stat()
            swap_cfg = ss.write(
                "swap.yaml",
                "vehicle_sil_simulation: 2\nnetworks:\n  - name: BUS\n    kind: can\nparticipants:\n"
                "  - name: T1\n    command: [\"./ecu_bin.sh\"]\n    step_ms: 10\n",
            )
            fx.set_behaviour(ss, "T1", fx.NORMAL)
            scen_s = fx.vt0_scenario(ss)
            req_s = ss.request("s", scenario=scen_s.name)
            with Serve(self.launch, swap_cfg) as srv:
                srv.ready()
                r1 = srv.run_once(req_s)
                self.assertEqual(r1.result["result"], "passed")
                # Replace with a failing binary of identical size & mtime.
                bad_script = "#!/bin/sh\nexit 17\n"
                bad_script += "#" * (512 - len(bad_script.encode()) - 1) + "\n"
                exe_path.write_text(bad_script, encoding="utf-8")
                exe_path.chmod(0o755)
                os.utime(exe_path, ns=(st.st_atime_ns, st.st_mtime_ns))
                r2 = srv.run_once(req_s)
                self.assertEqual(r2.result["result"], "check_failed")
                srv.close_input()
                self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
                self.assertEqual(stream_problems(srv.snapshot()), [])

    def test_10_fixed_registry_refused_and_lin_repetitions(self) -> None:
        # 10a: explicit registry_uri is refused per-Run with usage, launcher stays idle; Cold still runs.
        scratch = Scratch("basic_fixed_registry")
        port = free_port()
        fixed_cfg = fx.fixed_registry(scratch, port)
        scen = fx.vt0_scenario(scratch)
        req = scratch.request("vt0", scenario=scen.name)

        c_fixed = cold(self.launch, fixed_cfg, req)
        self.assertEqual(c_fixed.returncode, 0, c_fixed.stderr)
        self.assertEqual(c_fixed.result["result"], "passed")

        with Serve(self.launch, fixed_cfg) as srv:
            srv.ready()
            rec = srv.run_once(req)
            self.assertEqual(rec.result["result"], "usage")
            self.assertFalse(rec.launcher_ends)
            findings = rec.result["check"]["findings"]
            self.assertTrue(any("registry_uri" in f.get("message", "") and "run" in f.get("message", "") for f in findings),
                            findings)
            # No participant or registry event was emitted.
            self.assertFalse(any(e.get("event") in ("registry", "participant") for e in rec.window))
            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])

        # 10b: manual participant in request is refused per-Run, launcher stays idle and runs next Run.
        sm = Scratch("basic_manual")
        mcfg = fx.bench(sm, ["T1"])
        mscen = fx.vt0_scenario(sm)
        mreq_manual = sm.request("manual", scenario=mscen.name, manual=["T1"])
        mreq_ok = sm.request("ok", scenario=mscen.name)
        with Serve(self.launch, mcfg) as srv:
            srv.ready()
            rec_m = srv.run_once(mreq_manual)
            self.assertEqual(rec_m.result["result"], "usage")
            self.assertFalse(rec_m.launcher_ends)
            self.assertTrue(any("manual" in f.get("message", "") for f in rec_m.result["check"]["findings"]))
            rec_ok = srv.run_once(mreq_ok)
            self.assertEqual(rec_ok.result["result"], "passed")
            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])

        # 10c: LIN repetitions (20 Runs) and Cold equivalence.
        if os.environ.get("VEHICLE_SIL_SERVE_LIN_MASTER") and os.environ.get("VEHICLE_SIL_SERVE_LIN_SLAVE"):
            n_lin = runs("VEHICLE_SIL_SERVE_RUNS_LIN", 20)
            sl = Scratch("basic_lin")
            lcfg = fx.lin_pair(sl)
            lscen = fx.lin_scenario(sl)
            lreq = sl.request("lin", scenario=lscen.name)
            c_lin = cold(self.launch, lcfg, lreq, sl.timeline("cold_lin"))
            self.assertEqual(c_lin.returncode, 0, c_lin.stderr)
            self.assertEqual(c_lin.result["result"], "passed")
            with Serve(self.launch, lcfg, keep_logs=False) as srv:
                srv.ready()
                for i in range(n_lin):
                    rec = srv.run_once(lreq, sl.timeline(f"hot_lin_{i}"))
                    self.assertEqual(rec.result["result"], "passed", f"LIN run {i + 1}")
                    self.assertEqual(first_time_ms(rec.window), 0)
                    self.assert_same_as_cold(f"LIN run {i + 1}", c_lin, rec)
                srv.close_input()
                self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
                self.assertEqual(stream_problems(srv.snapshot()), [])


    def test_11_prepare_status_stop_force_and_inline_intent_equivalence(self) -> None:
        scratch = Scratch("basic_serve_extensions")
        cfg = fx.bench(scratch, ["T1"])
        scen = fx.vt0_scenario(scratch)
        req = scratch.request("vt0", scenario=scen.name)
        expected_sha = hashlib.sha256(cfg.read_bytes()).hexdigest()

        # Cold baseline for comparison against serve inline intent.
        c_ref = cold(self.launch, cfg, req, scratch.timeline("cold_intent"))
        self.assertEqual(c_ref.returncode, 0, c_ref.stderr)
        self.assertEqual(c_ref.result["result"], "passed")

        with Serve(self.launch, cfg) as srv:
            srv.ready()
            st0 = srv.status(id=1)
            self.assertEqual(st0["launcher_state"], "idle")
            self.assertIsNone(st0["active_run_id"])

            prep = srv.prepare(id=2)
            self.assertTrue(prep["accepted"])
            self.assertEqual(prep["status"]["launcher_state"], "ready")
            self.assertTrue(prep["status"]["eligibility"]["eligible"])
            self.assertEqual(prep["metadata"]["file_sha256"], expected_sha)
            self.assertEqual(prep["metadata"]["participants"], ["T1"])

            # Inline intent run matches Cold equivalence.
            tl_hot = scratch.timeline("hot_intent")
            rec_intent = srv.run_once(
                timeline=tl_hot,
                intent={"vehicle_sil_run_request": 1, "scenario": scen.name},
            )
            self.assertEqual(rec_intent.result["result"], "passed")
            self.assert_same_as_cold("inline intent", c_ref, rec_intent)

            # Invalid request + intent together is refused without advancing run sequence.
            bad_id = 50
            srv.send({"command": "run", "id": bad_id, "request": req.as_posix(), "intent": {"speed": 2}})
            refused = srv.wait_event("command", id=bad_id)
            self.assertFalse(refused["accepted"])
            self.assertEqual(refused["reason"], "invalid-argument")

            # Next accepted run gets -2 (sequence not advanced by refusal) and stops cleanly with force=false.
            paused_before = srv.ready()
            r2_id = srv.run(intent={"start_paused": True})
            r2_start = srv.run_started(r2_id, after=paused_before)
            r2 = r2_start["run"]
            self.assertTrue(r2.endswith("-2"), r2)
            srv.wait_for(lambda e: e.get("event") == "state" and e.get("run") == r2 and e.get("paused") is True,
                         "paused state")
            st_running = srv.status(id=60)
            self.assertEqual(st_running["launcher_state"], "running")
            self.assertEqual(st_running["active_run_id"], r2)
            self.assertIsNotNone(st_running["run_state"])
            prep_busy = srv.prepare(id=61)
            self.assertFalse(prep_busy["accepted"])
            self.assertEqual(prep_busy["reason"], "busy")

            srv.stop(r2, force=False, id=62)
            srv.stop(r2, force=False, id=63)
            res2 = srv.result_of(r2)
            self.assertEqual(res2["result"]["result"], "stopped")
            srv.ready()

            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(srv.bad_lines, [])
            self.assertEqual(stream_problems(srv.snapshot(), allow_replies=True), [])


if __name__ == "__main__":
    unittest.main()
