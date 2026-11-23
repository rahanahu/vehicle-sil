# SPDX-License-Identifier: MIT
"""Acceptance tests for vehicle_sil_launch serve: stale stdin rejection (#3), event window
isolation (#4), idle signals and run/SIGINT race (#6), stop vs launcher end + close_cut (#7),
fault-injected not_closed_ms Cold fallback (#8), trace refusal and bounded resources (#9)."""
from __future__ import annotations

import json
import os
import pathlib
import signal
import subprocess
import time
import unittest

import serve_fixtures as fx
from serve_digest import deterministic, differences, digest, first_time_ms, stream_problems
from serve_support import (
    EXIT_CONTAMINATED,
    EXIT_IDLE_EOF,
    EXIT_INTERRUPTED,
    EXIT_USAGE,
    FAULT_CLOSE_TIMEOUT_ENV,
    FAULT_DELAY_ENV,
    REASON_RUN_CHANGED,
    REASON_RUN_REQUIRED,
    Scratch,
    Serve,
    TRACE_ENV,
    WINDOWS,
    asan_built,
    cold,
    descendants,
    env_path,
    fault_delay_built,
    resources,
    run_command,
    runs,
    say,
    scaled,
    settled_resources,
    slope,
)


class ServeIsolationAcceptanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.launch = env_path("VEHICLE_SIL_LAUNCH_EXE")

    def test_03_stale_stdin_stop_and_time_refused_50_runs(self) -> None:
        n = runs("VEHICLE_SIL_SERVE_RUNS_STALE", 50)
        scratch = Scratch("iso_stale")
        cfg = fx.bench(scratch, ["T1"])
        scen = fx.vt0_scenario(scratch)
        req = scratch.request("vt0", scenario=scen.name)
        c_ref = cold(self.launch, cfg, req, scratch.timeline("cold"))
        self.assertEqual(c_ref.returncode, 0, c_ref.stderr)
        ref_digest = digest(c_ref.result, c_ref.timeline)

        with Serve(self.launch, cfg, keep_logs=False) as srv:
            srv.ready()
            # Untargeted stop/pause refused with run-required.
            srv.send({"command": "stop", "id": 1})
            e1 = srv.wait_for(lambda e: e.get("event") == "command" and e.get("id") == 1, "run-required stop")
            self.assertFalse(e1["accepted"])
            self.assertEqual(e1["reason"], REASON_RUN_REQUIRED)

            # First run cleanly.
            first = srv.run_once(req, scratch.timeline("hot_0"))
            self.assertEqual(digest(first.result, first.timeline), ref_digest)
            prev_run = first.run

            for i in range(1, n):
                # Send run(N+1) + stale stop(N) + stale pause(N) in one single write.
                tl = scratch.timeline(f"hot_{i}")
                run_id_num = 5000 + i
                stale_id_1 = 6000 + i
                stale_id_2 = 7000 + i
                payload = (
                    json.dumps(run_command(req, tl, id=run_id_num)) + "\n"
                    + json.dumps({"command": "stop", "run": prev_run, "id": stale_id_1}) + "\n"
                    + json.dumps({"command": "pause", "run": prev_run, "id": stale_id_2}) + "\n"
                ).encode()
                before = srv.ready()
                srv.send_raw(payload)
                rs = srv.run_started(run_id_num, after=before)
                cur_run = rs["run"]
                # Wait for phase ended, then immediately send a late stop for cur_run (must never leak to next Run).
                srv.wait_for(lambda e: e.get("event") == "phase" and e.get("run") == cur_run and e.get("phase") == "ended",
                             f"phase ended for {cur_run}")
                # Check cur_run's state BEFORE sending the late stop for cur_run itself: prev_run's stops never leaked!
                win_before = srv.window(cur_run)
                for st in (e for e in win_before if e.get("event") == "state"):
                    self.assertEqual(st.get("stop", {}).get("requests", 0), 0, f"stale stop leaked into {cur_run}")
                srv.send({"command": "stop", "run": cur_run, "id": 8000 + i})
                res = srv.result_of(cur_run)
                srv.wait_idle(after=res)
                for cid in (stale_id_1, stale_id_2):
                    ref = srv.wait_for(lambda e, c=cid: e.get("event") == "command" and e.get("id") == c, f"refusal {cid}")
                    self.assertFalse(ref["accepted"])
                    self.assertEqual(ref["reason"], REASON_RUN_CHANGED)
                    self.assertEqual(ref.get("asked_run"), prev_run)
                    self.assertNotIn("run", ref)
                self.assertEqual(res["result"]["result"], "passed", f"run {i}")
                from serve_support import read_json
                self.assertEqual(digest(res["result"], read_json(tl)), ref_digest)
                prev_run = cur_run

            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])

    def test_04_events_isolation_and_per_run_log_dir(self) -> None:
        n = runs("VEHICLE_SIL_SERVE_RUNS_EVENTS", 50)
        scratch = Scratch("iso_events")
        # ECU that emits log lines every step.
        cfg = fx.bench(scratch, ["T1"], behaviour="variables talk BUS log-every-step")
        scen = fx.vt0_scenario(scratch)
        req_scen = scratch.request("scen", scenario=scen.name)
        req_paused = scratch.request("paused", start_paused=True, stop_at_ms=100)
        log_dir = scratch.path("logs")

        run_ids = []
        with Serve(self.launch, cfg, arguments=["--log-dir", str(log_dir)], keep_logs=False) as srv:
            srv.ready()
            for i in range(n):
                if i % 10 == 5:
                    # Run with start_paused + step + resume.
                    before = srv.ready()
                    cid = srv.run(req_paused)
                    rs = srv.run_started(cid, after=before)
                    rid = rs["run"]
                    run_ids.append(rid)
                    srv.wait_for(lambda e: e.get("event") == "state" and e.get("run") == rid and e.get("paused") is True,
                                 f"paused in {rid}")
                    srv.send({"command": "step", "run": rid, "ms": 20, "id": 9000 + i})
                    srv.send({"command": "resume", "run": rid, "id": 9500 + i})
                    res = srv.result_of(rid)
                    self.assertEqual(res["result"]["result"], "passed")
                    srv.wait_idle(after=res)
                else:
                    rec = srv.run_once(req_scen)
                    run_ids.append(rec.run)
                    self.assertEqual(rec.result["result"], "passed")
            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(srv.bad_lines, [])
            self.assertEqual(stream_problems(srv.snapshot()), [])

        # Per-run log directories exist and are separated by Run ID.
        for rid in (run_ids[0], run_ids[-1]):
            self.assertTrue((log_dir / rid / "T1.log").is_file(), f"missing log for {rid}")

    def test_06_idle_signals_eof_stdout_closed_and_run_sigint_race(self) -> None:
        scratch = Scratch("iso_idle_end")
        cfg = fx.bench(scratch, ["T1"])
        long_scen = fx.long_scenario(scratch)
        req_long = scratch.request("long", scenario=long_scen.name)

        # Idle stdin EOF -> exit 0.
        with Serve(self.launch, cfg) as srv:
            srv.ready()
            srv.close_input()
            self.assertEqual(srv.wait(10), EXIT_IDLE_EOF)
            self.assertFalse(any(e.get("event") == "run_start" for e in srv.snapshot()))
            self.assertEqual(stream_problems(srv.snapshot()), [])

        if WINDOWS:
            return

        # Idle SIGINT / SIGTERM / SIGHUP -> exit 130, no run_start, no descendants.
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            with Serve(self.launch, cfg) as srv:
                srv.ready()
                pid = srv.process.pid
                srv.signal(sig)
                self.assertEqual(srv.wait(10), EXIT_INTERRUPTED, f"signal {sig}")
                self.assertFalse(any(e.get("event") == "run_start" for e in srv.snapshot()))
                self.assertEqual(descendants(pid), [])

        # Idle stdout closed -> exit 130 + stderr note.
        proc = subprocess.Popen(
            [str(self.launch), "serve", "--", str(cfg)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            start_new_session=True,
        )
        try:
            # Read hello and idle, then close stdout reader.
            for _ in range(2):
                line = proc.stdout.readline()
                self.assertTrue(line)
            proc.stdout.close()
            # Wait for heartbeat to notice closed stdout (1s + margin).
            code = proc.wait(timeout=scaled(10))
            err = proc.stderr.read().decode("utf-8", "replace")
            self.assertEqual(code, EXIT_INTERRUPTED, err)
            self.assertIn("cannot write the events", err)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(10)
            if proc.stdin:
                proc.stdin.close()
            if proc.stderr:
                proc.stderr.close()

        # A-M1 race: send run command and SIGINT concurrently, repeated.
        n_race = runs("VEHICLE_SIL_SERVE_RUNS_RACE", 25)
        for i in range(n_race):
            with Serve(self.launch, cfg, keep_logs=False) as srv:
                srv.ready()
                pid = srv.process.pid
                srv.run(req_long, id=1)
                if i % 3 == 1:
                    time.sleep(0.002)
                elif i % 3 == 2:
                    time.sleep(0.010)
                srv.signal(signal.SIGINT)
                code = srv.wait(30)
                self.assertEqual(code, EXIT_INTERRUPTED, f"race iteration {i}")
                self.assertEqual(descendants(pid), [], f"descendants left in race {i}")
                ev = srv.snapshot()
                self.assertEqual(stream_problems(ev), [], f"stream broken in race {i}")
                starts = [e for e in ev if e.get("event") == "run_start"]
                if starts:
                    results = [e for e in ev if e.get("event") == "result"]
                    self.assertEqual(len(results), 1)
                    self.assertTrue(results[0]["launcher_ends"])
                    self.assertEqual(results[0]["result"]["result"], "interrupted")

    def test_07_stop_vs_launcher_end_and_close_cut(self) -> None:
        scratch = Scratch("iso_stop")
        cfg = fx.bench(scratch, ["T1"])
        vt0 = fx.vt0_scenario(scratch)
        long_s = fx.long_scenario(scratch)
        req_vt0 = scratch.request("vt0", scenario=vt0.name)
        req_long = scratch.request("long", scenario=long_s.name, speed=1)
        c_vt0 = cold(self.launch, cfg, req_vt0)
        self.assertEqual(c_vt0.returncode, 0, c_vt0.stderr)

        # (a) Targeted stdin stop ends Run only; launcher stays idle and runs next Run.
        # (c) Two targeted stops (Force) also keep launcher alive when close succeeds.
        with Serve(self.launch, cfg) as srv:
            srv.ready()
            for double_stop in (False, True):
                before = srv.ready()
                cid = srv.run(req_long)
                rs = srv.run_started(cid, after=before)
                rid = rs["run"]
                srv.wait_for(lambda e: e.get("event") == "phase" and e.get("run") == rid and e.get("phase") == "running",
                             "running")
                srv.send({"command": "stop", "run": rid, "id": 100})
                if double_stop:
                    srv.send({"command": "stop", "run": rid, "id": 101})
                res = srv.result_of(rid)
                self.assertFalse(res["launcher_ends"])
                self.assertIn(res["result"]["result"], ("stopped", "incomplete"))
                srv.wait_idle(after=res)
                nxt = srv.run_once(req_vt0)
                self.assertEqual(digest(nxt.result), digest(c_vt0.result))
            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])

        # (f) stdin EOF during Run -> interrupted ("end of input"), launcher_ends: true, exit 130.
        with Serve(self.launch, cfg) as srv:
            before = srv.ready()
            cid = srv.run(req_long)
            rid = srv.run_started(cid, after=before)["run"]
            srv.wait_for(lambda e: e.get("event") == "phase" and e.get("run") == rid and e.get("phase") == "running",
                         "running")
            srv.close_input()
            res = srv.result_of(rid)
            self.assertTrue(res["launcher_ends"])
            self.assertEqual(res["result"]["result"], "interrupted")
            self.assertEqual(res["result"]["interrupted_by"], "end of input")
            self.assertEqual(srv.wait(30), EXIT_INTERRUPTED)
            self.assertEqual(stream_problems(srv.snapshot()), [])

        if not WINDOWS:
            # (d) SIGINT during Run -> interrupted, launcher_ends: true, no idle, exit 130.
            with Serve(self.launch, cfg) as srv:
                before = srv.ready()
                cid = srv.run(req_long)
                rid = srv.run_started(cid, after=before)["run"]
                srv.wait_for(lambda e: e.get("event") == "phase" and e.get("run") == rid and e.get("phase") == "running",
                             "running")
                srv.signal(signal.SIGINT)
                res = srv.result_of(rid)
                self.assertTrue(res["launcher_ends"])
                self.assertEqual(res["result"]["result"], "interrupted")
                self.assertEqual(srv.wait(30), EXIT_INTERRUPTED)
                self.assertEqual(stream_problems(srv.snapshot()), [])

            # (e) Double SIGINT during Run -> skips waits, exit 130.
            with Serve(self.launch, cfg) as srv:
                before = srv.ready()
                cid = srv.run(req_long)
                rid = srv.run_started(cid, after=before)["run"]
                srv.wait_for(lambda e: e.get("event") == "phase" and e.get("run") == rid and e.get("phase") == "running",
                             "running")
                srv.signal(signal.SIGINT)
                time.sleep(0.05)
                srv.signal(signal.SIGINT)
                self.assertEqual(srv.wait(30), EXIT_INTERRUPTED)

            # (g) SIGKILL launcher -> ECU children die via PDEATHSIG.
            with Serve(self.launch, cfg) as srv:
                before = srv.ready()
                cid = srv.run(req_long)
                rid = srv.run_started(cid, after=before)["run"]
                ev_p = srv.wait_for(
                    lambda e: e.get("event") == "participant" and e.get("run") == rid and e.get("state") == "started",
                    "participant started",
                )
                child_pid = ev_p["pid"]
                os.kill(srv.process.pid, signal.SIGKILL)
                srv.process.wait(10)
                deadline = time.monotonic() + scaled(5)
                while pathlib.Path(f"/proc/{child_pid}").exists() and time.monotonic() < deadline:
                    time.sleep(0.05)
                self.assertFalse(pathlib.Path(f"/proc/{child_pid}").exists(), f"child {child_pid} survived SIGKILL")

        # (h) A-M2 + #8: H2 fault injection (test build only).
        if fault_delay_built():
            # Single late stop during close does NOT cut close_within -> no contamination, launcher stays alive.
            env_h = dict(os.environ)
            env_h[FAULT_DELAY_ENV] = str(int(scaled(500)))
            with Serve(self.launch, cfg, env=env_h) as srv:
                before = srv.ready()
                cid = srv.run(req_vt0)
                rid = srv.run_started(cid, after=before)["run"]
                srv.wait_for(lambda e: e.get("event") == "phase" and e.get("run") == rid and e.get("phase") == "stopping",
                             "stopping")
                time.sleep(0.15)
                srv.send({"command": "stop", "run": rid, "id": 777})
                res = srv.result_of(rid)
                self.assertFalse(res["launcher_ends"], f"single late stop contaminated launcher: {res}")
                codes = [f.get("code") for f in res["result"].get("findings", [])]
                self.assertNotIn("connection-not-closed", codes)
                srv.wait_idle(after=res)
                # Next run matches Cold.
                nxt = srv.run_once(req_vt0)
                self.assertEqual(digest(nxt.result), digest(c_vt0.result))
                srv.close_input()
                self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
                self.assertEqual(stream_problems(srv.snapshot()), [])

    def test_08_not_closed_ms_contaminates_and_fresh_launcher_matches_cold(self) -> None:
        if not fault_delay_built():
            self.skipTest("VEHICLE_SIL_FAULT_INJECTION is OFF in this build")
        scratch = Scratch("iso_contaminated")
        cfg = fx.bench(scratch, ["T1"])
        vt0 = fx.vt0_scenario(scratch)
        req_vt0 = scratch.request("vt0", scenario=vt0.name)
        c_vt0 = cold(self.launch, cfg, req_vt0)
        self.assertEqual(c_vt0.returncode, 0, c_vt0.stderr)

        # Case A: close_within expires its limit -> connection-not-closed finding, launcher_ends: true, exit 6.
        env_timeout = dict(os.environ)
        env_timeout[FAULT_CLOSE_TIMEOUT_ENV] = str(int(scaled(400)))
        env_timeout[FAULT_DELAY_ENV] = str(int(scaled(1000)))
        with Serve(self.launch, cfg, arguments=["--participant-stop-timeout-ms", "1000"], env=env_timeout) as srv:
            old_instance = srv.instance
            before = srv.ready()
            cid = srv.run(req_vt0)
            rid = srv.run_started(cid, after=before)["run"]
            res = srv.result_of(rid)
            self.assertTrue(res["launcher_ends"])
            codes = [f.get("code") for f in res["result"].get("findings", [])]
            self.assertIn("connection-not-closed", codes, res)
            code = srv.wait(30)
            self.assertEqual(code, EXIT_CONTAMINATED)
            ev = srv.snapshot()
            self.assertEqual(stream_problems(ev), [])
            # No automatic retry: exactly one run_start.
            self.assertEqual(len([e for e in ev if e.get("event") == "run_start"]), 1)

        # Case B: Force (2 stops during close) cuts close_within immediately -> launcher_ends: true, exit 6.
        env_force = dict(os.environ)
        env_force[FAULT_DELAY_ENV] = "15000"
        with Serve(self.launch, cfg, env=env_force) as srv_f:
            before = srv_f.ready()
            cid = srv_f.run(req_vt0)
            rid = srv_f.run_started(cid, after=before)["run"]
            srv_f.wait_for(lambda e: e.get("event") == "phase" and e.get("run") == rid and e.get("phase") == "stopping",
                           "stopping")
            time.sleep(0.15)
            srv_f.send({"command": "stop", "run": rid, "id": 801})
            srv_f.send({"command": "stop", "run": rid, "id": 802})
            res = srv_f.result_of(rid)
            self.assertTrue(res["launcher_ends"])
            self.assertEqual(srv_f.wait(30), EXIT_CONTAMINATED)
            self.assertEqual(stream_problems(srv_f.snapshot()), [])

        # Caller starts a fresh launcher without fault delay: new instance, Cold-equivalent result.
        env_clean = dict(os.environ)
        env_clean.pop(FAULT_DELAY_ENV, None)
        env_clean.pop(FAULT_CLOSE_TIMEOUT_ENV, None)
        with Serve(self.launch, cfg, env=env_clean) as srv2:
            self.assertNotEqual(srv2.instance, old_instance)
            rec = srv2.run_once(req_vt0)
            self.assertEqual(rec.run, f"{srv2.instance}-1")
            self.assertEqual(digest(rec.result), digest(c_vt0.result))
            srv2.close_input()
            self.assertEqual(srv2.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv2.snapshot()), [])

    def test_09_trace_refused_and_bounded_resources(self) -> None:
        scratch = Scratch("iso_trace_res")
        cfg = fx.bench(scratch, ["T1"])
        vt0 = fx.vt0_scenario(scratch)
        req = scratch.request("vt0", scenario=vt0.name)

        # T4: serve refuses VEHICLE_SIL_TRACE at startup with exit 4 and guidance to use `run`.
        env_tr = dict(os.environ)
        env_tr[TRACE_ENV] = str(scratch.path("trace_out"))
        with Serve(self.launch, cfg, env=env_tr) as srv:
            code = srv.wait(15)
            self.assertEqual(code, EXIT_USAGE, srv.stderr_text)
            ev = srv.snapshot()
            self.assertEqual(stream_problems(ev), [])
            self.assertEqual(ev[-1]["event"], "result")
            self.assertTrue(ev[-1].get("launcher_ends"))
            self.assertNotIn("run", ev[-1])
            self.assertEqual(ev[-1]["result"]["result"], "usage")
            msg = ev[-1]["result"]["message"]
            self.assertIn("VEHICLE_SIL_TRACE", msg)
            self.assertIn("run", msg)

        # Bounded resources over N Runs (Linux /proc).
        if not WINDOWS:
            n_res = runs("VEHICLE_SIL_SERVE_RUNS_RES", 60)
            rss_series = []
            with Serve(self.launch, cfg, keep_logs=False) as srv:
                srv.ready()
                # Warm up 5 runs before taking baseline (glibc/allocator/SIL Kit one-time caches).
                for _ in range(5):
                    rec = srv.run_once(req)
                    self.assertEqual(rec.result["result"], "passed")
                baseline = resources(srv.process.pid)
                self.assertIsNotNone(baseline)
                for i in range(n_res):
                    rec = srv.run_once(req)
                    self.assertEqual(rec.result["result"], "passed")
                    check_listen = (i % 10 == 0 or i == n_res - 1)
                    now = settled_resources(srv.process.pid, baseline, check_listen=check_listen)
                    self.assertEqual(now["fds"], baseline["fds"], f"fd leak at run {i + 1}: {now} vs {baseline}")
                    self.assertEqual(now["threads"], baseline["threads"], f"thread leak at run {i + 1}: {now} vs {baseline}")
                    self.assertEqual(now["descendants"], 0, f"descendants left at run {i + 1}")
                    if check_listen:
                        self.assertEqual(now["listen"], 0, f"listening sockets left at run {i + 1}")
                    rss_series.append(now["rss_kb"])
                srv.close_input()
                self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
                self.assertEqual(stream_problems(srv.snapshot()), [])
            tail = rss_series[len(rss_series) // 2:]
            rss_slope = slope(tail)
            if asan_built():
                # AddressSanitizer retains freed memory in its quarantine (256 MiB by default) for UAF
                # detection, so VmRSS grows across 60 Runs by design. Keep full-quarantine UAF checking,
                # LeakSanitizer at exit (EXIT_IDLE_EOF above), and FD/thread/descendant/listen bounds;
                # VmRSS slope is enforced on every non-ASan Linux build.
                say(f"[resources] {n_res} Runs after warmup: fds={baseline['fds']} threads={baseline['threads']} "
                    f"rss_start={rss_series[0]}KiB rss_end={rss_series[-1]}KiB tail_slope={rss_slope:.2f}KiB/Run "
                    "(RSS slope guard skipped under ASan; full quarantine UAF + LSan exit check active)")
            else:
                say(f"[resources] {n_res} Runs after warmup: fds={baseline['fds']} threads={baseline['threads']} "
                    f"rss_start={rss_series[0]}KiB rss_end={rss_series[-1]}KiB tail_slope={rss_slope:.2f}KiB/Run")
                # Loose guard against gross leaks (e.g., 37 MB/Run); exact slope is recorded above.
                self.assertLess(rss_slope, 128.0, f"RSS grew too fast: {rss_slope:.2f} KiB/Run")


if __name__ == "__main__":
    unittest.main()
