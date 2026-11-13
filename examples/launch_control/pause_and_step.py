# SPDX-License-Identifier: MIT
"""Runs the two ECUs of examples/ecu_main with vehicle_sil_launch and its control server, pauses the
running simulation, writes a variable at a set time, steps on, reads the time and the values together
and checks them, then lets the simulation run to its end as fast as possible.

    python examples/launch_control/pause_and_step.py \
        --launch build/<preset>/tools/launch/vehicle_sil_launch \
        build/<preset>/examples/ecu_main/simulation.yaml

Needs a vehicle_sil_launch built with -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON, the Python package
python/vehicle_sil_control on PYTHONPATH with its gRPC stubs (docs/control-api.md, "Python").
Prints what it did; exit code 0 if everything was as expected, 1 if not.

The engine's speed moves 50 rpm per 10 ms step towards 800 rpm + 20 rpm per percent of throttle,
from 0 rpm; its throttle starts at 20 % (1200 rpm). The simulation runs at a quarter of real time
(1000 ms take 4 s), so there is time to pause it once the engine passes 1000 rpm.
"""
import argparse
import sys

from vehicle_sil_control import Control, Launch, LaunchError, Values, ms

THROTTLE = 50.0
ENGINE_STEP = ms(10)


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

    with Launch(args.launch, args.config, start_paused=True, arguments=["--speed", "0.25"]) as launch:
        with Control(launch.address) as c:
            c.sim.wait_paused()
            c.sim.resume()

            # Watch the engine's values while the simulation runs; pause once it passes 1000 rpm.
            for update in c.watch(peers=["Engine"], events=False, operations=False):
                if isinstance(update, Values) and update.values.get("engine.speed", 0) >= 1000:
                    print(f"engine.speed {update.values['engine.speed']} rpm at {update.time / 1e6:g} ms")
                    break
            else:
                print("FAIL the simulation ended before the engine passed 1000 rpm")
                return 1
            c.sim.pause()
            stands = c.sim.wait_paused()
            paused_at = stands.now
            expect(stands.paused and update.time <= paused_at <= ms(800), f"it stands still at {paused_at / 1e6:g} ms")

            # Write for a time after the one it stands still at (here 1 ms after); it takes effect in
            # the engine's first step from then on. Wait until the engine has queued it before stepping on.
            at = paused_at + ms(1)
            seq = c.write({"Engine": {"engine.throttle": THROTTLE}}, at=at)
            c.wait(seq, until="queued")
            c.sim.step(ms(100))
            stands = c.sim.wait_paused()
            expect(stands.now == paused_at + ms(100), f"it stands still at {stands.now / 1e6:g} ms after the step")

            done = c.wait(seq).target("Engine")
            first_step = -(-at // ENGINE_STEP) * ENGINE_STEP
            expect(done.status == "applied" and done.applied_at == first_step,
                   f"the throttle took effect in the engine's step at {first_step / 1e6:g} ms "
                   f"({done.status} at {done.applied_at})")

            # The time and the values, read in one go: consistent says the values are those of
            # the time it stands still at.
            snap = c.snapshot(["Engine"])
            engine = snap.values["Engine"]
            expect(snap.consistent and snap.time.now == stands.now,
                   f"the snapshot is of {snap.time.now / 1e6:g} ms (consistent: {snap.consistent})")
            expect(engine.values["engine.throttle"] == THROTTLE and engine.time <= snap.time.now,
                   f"engine.throttle reads {engine.values['engine.throttle']} in the sample of "
                   f"{engine.time / 1e6:g} ms; engine.speed {engine.values['engine.speed']} rpm")

            # The rest as fast as possible; runs on to stop_at_ms of simulation.yaml.
            speed = c.sim.set_speed(None)
            expect(speed.speed is None and speed.paused, "the speed is max, and it still stands still")
            c.sim.resume()
        result = launch.wait()
        commands = [e["command"] for e in launch.events if e["event"] == "command" and e["source"] == "control_server"]
        expect(commands == ["resume", "pause", "step", "speed", "resume"],
               f"the launcher wrote the commands of the control server as events ({', '.join(commands)})")
    expect(result["result"] == "passed" and result["simulation"] == "reached_stop_at",
           f"the run passed ({result['result']}, {result['simulation']})")

    print("all as expected" if not problems else f"{len(problems)} not as expected")
    return 0 if not problems else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except LaunchError as e:
        print(f"FAIL {e}")
        sys.exit(1)
