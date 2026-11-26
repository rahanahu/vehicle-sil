# SPDX-License-Identifier: MIT
"""Runs two sequential simulations in one resident `vehicle_sil_launch serve` process with
`Serve` and `RunHandle`, verifying fresh virtual time 0 state and per-run `Control` binding.

    python examples/launch_control/serve_runs.py \
        --launch build/<preset>/tools/launch/vehicle_sil_launch \
        build/<preset>/examples/ecu_main/simulation.yaml

Needs a vehicle_sil_launch with --control (included in prebuilt vehicle-sil-tools-* archives,
or built from source with -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON) and vehicle_sil_control with
its gRPC stubs on PYTHONPATH (docs/control-api.md, "Python").
Prints what it did; exit code 0 if everything was as expected, 1 if not.
"""
import argparse
import sys

from vehicle_sil_control import ControlError, Serve, ServeError, ms

THROTTLE_RUN1 = 50.0
EXPECTED_RUN1_RPM = 800 + 20 * int(THROTTLE_RUN1)
DEFAULT_THROTTLE = 20.0
EXPECTED_DEFAULT_RPM = 800 + 20 * int(DEFAULT_THROTTLE)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--launch", required=True, help="the vehicle_sil_launch executable")
    parser.add_argument("config", help="examples/ecu_main/simulation.yaml in the build tree")
    args = parser.parse_args()

    problems = []

    def expect(ok: bool, what: str) -> None:
        print(("ok   " if ok else "FAIL ") + what)
        if not ok:
            problems.append(what)

    with Serve(args.launch, args.config, control=True) as runner:
        print(f"launcher instance {runner.instance}")
        prep = runner.prepare()
        expect(
            prep["accepted"] and prep["status"]["launcher_state"] == "ready",
            f"prepare ready ({prep['status']['launcher_state']})",
        )

        # Run 1: write 50 % throttle before the first step, step 500 ms to observe 1800 rpm, then finish.
        with runner.start(start_paused=True, stop_at_ms=1000) as run1:
            with run1.control() as c1:
                expect(c1.run == run1.id, f"run 1 control bound to {run1.id}")
                c1.sim.wait_paused()
                seq = c1.write({"Engine": {"engine.throttle": THROTTLE_RUN1}})
                c1.wait(seq, until="queued")
                c1.sim.step(ms(500))
                stands1 = c1.sim.wait_paused()
                expect(stands1.now == ms(500), f"run 1 stepped to 500 ms ({stands1.now / 1e6:g} ms)")
                values1 = c1.values("Engine").values
                expect(
                    values1["engine.throttle"] == THROTTLE_RUN1
                    and values1["engine.speed"] == EXPECTED_RUN1_RPM,
                    f"run 1 applied throttle={values1['engine.throttle']}, "
                    f"speed={values1['engine.speed']} rpm",
                )
                c1.sim.resume()
            res1 = run1.wait()
            expect(
                res1["result"] == "passed"
                and res1["simulation"] == "reached_stop_at"
                and run1.launcher_ends is False,
                f"run 1 passed ({res1['result']}, {res1['simulation']})",
            )
            expect(
                (run1.status or {}).get("launcher_state") == "ready",
                f"launcher returned to ready after run 1 ({(run1.status or {}).get('launcher_state')})",
            )

        # Run 2 in the same launcher: starts fresh at time 0 with default throttle (20 %).
        with runner.start(start_paused=True, stop_at_ms=1000) as run2:
            expect(run2.id != run1.id, f"run 2 got a distinct run ID ({run2.id})")
            with run2.control() as c2:
                expect(c2.run == run2.id, f"run 2 control bound to {run2.id}")
                c2.sim.wait_paused()
                c2.sim.step(ms(500))
                stands2 = c2.sim.wait_paused()
                expect(stands2.now == ms(500), f"run 2 stepped to 500 ms ({stands2.now / 1e6:g} ms)")
                values2 = c2.values("Engine").values
                expect(
                    values2["engine.throttle"] == DEFAULT_THROTTLE
                    and values2["engine.speed"] == EXPECTED_DEFAULT_RPM,
                    f"run 2 started fresh: throttle={values2['engine.throttle']}, "
                    f"speed={values2['engine.speed']} rpm",
                )
                c2.sim.resume()
            res2 = run2.wait()
            expect(
                res2["result"] == "passed" and run2.launcher_ends is False,
                f"run 2 passed ({res2['result']})",
            )

    expect(runner.returncode == 0, f"resident launcher exited cleanly ({runner.returncode})")
    print("all as expected" if not problems else f"{len(problems)} not as expected")
    return 0 if not problems else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ServeError, ControlError) as e:
        print(f"FAIL {e}")
        sys.exit(1)
