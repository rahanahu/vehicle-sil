# SPDX-License-Identifier: MIT
"""Runs the two ECUs of examples/ecu_main with vehicle_sil_launch and its control server, writes
a variable before the first step, lets the simulation run, reads the result back and checks it.

    python examples/launch_control/write_before_start.py \
        --launch build/<preset>/tools/launch/vehicle_sil_launch \
        build/<preset>/examples/ecu_main/simulation.yaml

Needs a vehicle_sil_launch built with -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON, the Python package
python/vehicle_sil_control on PYTHONPATH with its gRPC stubs (docs/control-api.md, "Python").
Prints what it did; exit code 0 if everything was as expected, 1 if not.

The engine's speed moves 50 rpm per 10 ms step towards 800 rpm + 20 rpm per percent of throttle,
from 0 rpm. Its throttle starts at 20 % (1200 rpm); written as 50 % before the first step, the
engine runs towards 1800 rpm from its step at 0, and stands there at 500 ms.
"""
import argparse
import sys

from vehicle_sil_control import Control, Launch, LaunchError, ms

THROTTLE = 50.0
EXPECTED_RPM = 800 + 20 * int(THROTTLE)


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

    # --start-paused with the control server: the simulation stands still before its first step.
    with Launch(args.launch, args.config, start_paused=True) as launch:
        print(f"control server at {launch.address}")
        with Control(launch.address) as c:
            stands = c.sim.wait_paused()
            expect(stands.now == 0 and not stands.running, "it stands still at 0 before the first step")

            # Before the first step: the write takes effect in the engine's step at 0. Wait until the
            # engine has queued it before letting the simulation go on.
            seq = c.write({"Engine": {"engine.throttle": THROTTLE}})
            queued = c.wait(seq, until="queued")
            expect(queued.target("Engine").status == "queued", f"Engine queued the throttle ({queued.target('Engine').status})")

            c.sim.step(ms(500))
            stands = c.sim.wait_paused()
            expect(stands.now == ms(500), f"it stands still at 500 ms ({stands.now / 1e6:g} ms)")

            done = c.wait(seq)
            expect(done.target("Engine").status == "applied" and done.target("Engine").applied_at == 0,
                   f"the throttle took effect in the step at 0 ({done.target('Engine').status} at "
                   f"{done.target('Engine').applied_at})")
            values = c.values("Engine").values
            expect(values["engine.throttle"] == THROTTLE, f"engine.throttle reads {values['engine.throttle']}")
            expect(values["engine.speed"] == EXPECTED_RPM, f"engine.speed reads {values['engine.speed']} rpm "
                                                           f"(expected {EXPECTED_RPM})")

            # Runs on to stop_at_ms of simulation.yaml.
            c.sim.resume()
        result = launch.wait()
        commands = [e for e in launch.events if e["event"] == "command" and e["source"] == "control_server"]
        expect([e["command"] for e in commands] == ["step", "resume"] and commands[0].get("ms") == 500,
               "the launcher wrote the step and the resume as command events from the control server")
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
