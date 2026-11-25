# SPDX-License-Identifier: MIT
import sys

from vehicle_sil_control import Control, Launch, ms

launcher, config = sys.argv[1], sys.argv[2]                   # vehicle_sil_launch and simulation.yaml

# Starts "vehicle_sil_launch serve --once" with its control server, paused before the first step.
with Launch(launcher, config, start_paused=True) as launch:
    with Control(launch.address) as c:
        c.sim.wait_paused()                                   # every ECU stands still at 0 ms
        seq = c.write({"Engine": {"engine.throttle": 50.0}})  # participant name, then variable name
        c.wait(seq, until="queued")                           # the ECU has received the write

        c.sim.step(ms(500))                                   # run 500 ms, then pause again
        now = c.sim.wait_paused().now
        values = c.values("Engine").values                    # the values the ECU sent last
        print(f"at {now / 1e6:g} ms: engine.throttle = {values['engine.throttle']}, "
              f"engine.speed = {values['engine.speed']}")
