# SPDX-License-Identifier: MIT
from vehicle_sil_control import Control, Launch, ms

# Starts "vehicle_sil_launch serve --once simulation.yaml" with its control server, paused before the first step.
with Launch("vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch:
    with Control(launch.address) as c:
        c.sim.wait_paused()                                   # every ECU stands still at 0 ms
        seq = c.write({"Cooler": {"cooler.target": 22.0}})    # participant name, then variable name
        c.wait(seq, until="queued")                           # the ECU has received the write

        c.sim.step(ms(500))                                   # run 500 ms, then pause again
        now = c.sim.wait_paused().now
        values = c.values("Cooler").values                    # the values of the last step
        print(f"at {now / 1e6:g} ms: cooler.target = {values['cooler.target']}, "
              f"cooler.temperature = {values['cooler.temperature']}")
