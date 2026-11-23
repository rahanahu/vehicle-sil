# SPDX-License-Identifier: MIT
"""Acceptance tests for vehicle_sil_launch serve with the gRPC Control Server (#5, #7b):
- Fixed address gRPC rejects stale clients bound to Run N when Run N+1 starts (run-changed).
- Unbound clients can Ping (getting PingReply.run == Run N+1) but are refused on all other calls (run-required).
- Control.for_current_run() binds cleanly to the active Run.
- Per-run credentials file reload rejects the previous Run's token (UNAUTHENTICATED).
- gRPC Stop ends the current Run only; the launcher returns to idle.
- Audit log records Run ID prefix and refusal lines."""
from __future__ import annotations

import unittest

import grpc

import serve_fixtures as fx
from serve_digest import stream_problems
from serve_support import (
    EXIT_IDLE_EOF,
    REASON_RUN_CHANGED,
    REASON_RUN_REQUIRED,
    Scratch,
    Serve,
    env_path,
    free_port,
)
from vehicle_sil_control import Control, ControlError


class ServeGrpcAcceptanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.launch = env_path("VEHICLE_SIL_LAUNCH_EXE")

    def test_05_fixed_address_stale_and_unbound_clients_and_grpc_stop(self) -> None:
        scratch = Scratch("grpc_binding")
        cfg = fx.bench(scratch, ["T1"])
        req_paused = scratch.request("paused", start_paused=True, stop_at_ms=200)
        port = free_port()
        address = f"127.0.0.1:{port}"
        audit_path = scratch.path("audit.log")

        with Serve(self.launch, cfg, arguments=["--control", address, "--control-audit-log", str(audit_path)]) as srv:
            # --- Run 1 ---
            before1 = srv.ready()
            cid1 = srv.run(req_paused)
            run1 = srv.run_started(cid1, after=before1)["run"]
            cs1 = srv.wait_for(lambda e: e.get("event") == "control_server" and e.get("run") == run1, "control_server 1")
            self.assertEqual(cs1["address"], address)
            srv.wait_for(lambda e: e.get("event") == "state" and e.get("run") == run1 and e.get("holding") is True,
                         "holding run 1")

            old_explicit = Control(address, run=run1)
            old_bound = Control.for_current_run(address)
            unbound = Control(address)
            try:
                self.assertEqual(old_bound.run, run1)
                self.assertEqual(unbound.current_run(), run1)
                # Write in Run 1 succeeds and records an operation sequence number.
                op1 = old_explicit.write({"T1": {"level": 5}})
                self.assertGreater(op1, 0)
                # #7(b): gRPC Stop ends Run 1 only; launcher stays alive.
                old_bound.sim.stop("test stop run 1")
                res1 = srv.result_of(run1)
                self.assertFalse(res1["launcher_ends"])
                self.assertEqual(res1["result"]["result"], "stopped")
                after1 = srv.wait_idle(after=res1)

                # --- Run 2 on the exact same fixed address ---
                cid2 = srv.run(req_paused)
                run2 = srv.run_started(cid2, after=after1)["run"]
                self.assertNotEqual(run1, run2)
                cs2 = srv.wait_for(lambda e: e.get("event") == "control_server" and e.get("run") == run2, "control_server 2")
                self.assertEqual(cs2["address"], address)
                srv.wait_for(lambda e: e.get("event") == "state" and e.get("run") == run2 and e.get("holding") is True,
                             "holding run 2")

                # Unbound client: Ping / current_run works and reports run2; every other call -> run-required.
                self.assertEqual(unbound.current_run(), run2)
                for call in (
                    lambda: unbound.peers(),
                    lambda: unbound.write({"T1": {"level": 9}}),
                    lambda: unbound.sim.stop(),
                    lambda: unbound.sim.pause(),
                    lambda: unbound.sim.step(10),
                    lambda: next(unbound.watch(time=True)),
                ):
                    with self.assertRaises(ControlError) as ctx:
                        call()
                    self.assertEqual(ctx.exception.code, grpc.StatusCode.FAILED_PRECONDITION)
                    self.assertEqual(ctx.exception.reason, REASON_RUN_REQUIRED)

                # Stale clients (reconnecting to same fixed address): every call -> run-changed.
                for stale in (old_explicit, old_bound):
                    self.assertEqual(stale.current_run(), run2)
                    for call in (
                        lambda s=stale: s.write({"T1": {"level": 9}}),
                        lambda s=stale: s.sim.stop(),
                        lambda s=stale: s.sim.pause(),
                        lambda s=stale: s.sim.step(10),
                        lambda s=stale: s.operation(op1),
                        lambda s=stale: next(s.watch(time=True)),
                    ):
                        with self.assertRaises(ControlError) as ctx:
                            call()
                        self.assertEqual(ctx.exception.code, grpc.StatusCode.FAILED_PRECONDITION)
                        self.assertEqual(ctx.exception.reason, REASON_RUN_CHANGED)

                # Current client works normally and resumes Run 2 to completion (passed, not stopped!).
                with Control.for_current_run(address) as cur:
                    self.assertEqual(cur.run, run2)
                    cur.sim.resume()
                res2 = srv.result_of(run2)
                self.assertFalse(res2["launcher_ends"])
                self.assertEqual(res2["result"]["result"], "passed")
                srv.wait_idle(after=res2)
            finally:
                old_explicit.close()
                old_bound.close()
                unbound.close()

            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])

        audit_text = audit_path.read_text(encoding="utf-8")
        self.assertIn(f"[run {run1}]", audit_text)
        self.assertIn(f"[run {run2}]", audit_text)
        self.assertIn("run-changed", audit_text)
        self.assertIn("run-required", audit_text)

    def test_05_per_run_credentials_reloaded(self) -> None:
        scratch = Scratch("grpc_creds")
        cfg = fx.bench(scratch, ["T1"])
        req_paused = scratch.request("paused", start_paused=True, stop_at_ms=100)
        port = free_port()
        address = f"127.0.0.1:{port}"
        tok1 = "token-run-1-0123456789abcdef"
        tok2 = "token-run-2-fedcba9876543210"
        creds = scratch.write("creds.txt", f"admin alice {tok1}\n")

        with Serve(self.launch, cfg, arguments=["--control", address, "--control-credentials", str(creds)]) as srv:
            before1 = srv.ready()
            cid1 = srv.run(req_paused)
            run1 = srv.run_started(cid1, after=before1)["run"]
            srv.wait_for(lambda e: e.get("event") == "state" and e.get("run") == run1 and e.get("holding") is True,
                         "holding run 1")
            with Control(address, token=tok1, run=run1) as c1:
                c1.sim.resume()
            res1 = srv.result_of(run1)
            self.assertEqual(res1["result"]["result"], "passed")
            after1 = srv.wait_idle(after=res1)

            # Rotate credentials before Run 2.
            creds.write_text(f"admin bob {tok2}\n", encoding="utf-8")
            cid2 = srv.run(req_paused)
            run2 = srv.run_started(cid2, after=after1)["run"]
            srv.wait_for(lambda e: e.get("event") == "state" and e.get("run") == run2 and e.get("holding") is True,
                         "holding run 2")

            # Old token is rejected with UNAUTHENTICATED.
            with Control(address, token=tok1, run=run2) as old_tok:
                with self.assertRaises(ControlError) as ctx:
                    old_tok.peers()
                self.assertEqual(ctx.exception.code, grpc.StatusCode.UNAUTHENTICATED)

            # New token succeeds.
            with Control(address, token=tok2, run=run2) as c2:
                c2.sim.resume()
            res2 = srv.result_of(run2)
            self.assertEqual(res2["result"]["result"], "passed")
            srv.wait_idle(after=res2)

            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])


if __name__ == "__main__":
    unittest.main()
