# SPDX-License-Identifier: MIT
"""The simulations the tests of tests/serve run: written into the test's scratch directory, with the executables of
the build (the test ECU of tests/launch, the example ECUs, the LIN ECUs of this directory).

The test ECU (tests/launch/run_test_ecu.cpp) reads what it does from "<name>.behaviour" in its working directory,
which is the directory of the configuration file: a test changes it between two Runs of one launcher.
"""
from __future__ import annotations

import json
import os
import pathlib
import sys
from typing import Sequence

from serve_support import EXE, Scratch, env_path


def _q(path: os.PathLike) -> str:
    return json.dumps(pathlib.Path(path).as_posix())


def test_ecu() -> pathlib.Path:
    return env_path("VEHICLE_SIL_TEST_ECU")


def example(*parts: str) -> pathlib.Path:
    return env_path("VEHICLE_SIL_EXAMPLE_DIR").joinpath(*parts)


def bench(scratch: Scratch, names: Sequence[str], behaviour: str = "variables talk BUS", name: str = "sim.yaml",
          extra: str = "", top: str = "") -> pathlib.Path:
    """A simulation of test ECUs `names` on one CAN network BUS, each with `behaviour` (its file is written)."""
    lines = [f"vehicle_sil_simulation: 2\n{top}networks:\n  - name: BUS\n    kind: can\nparticipants:\n"]
    for n in names:
        lines.append(f"  - name: {n}\n    command: [{_q(test_ecu())}]\n    step_ms: 10\n")
        set_behaviour(scratch, n, behaviour)
    return scratch.write(name, "".join(lines) + extra)


def set_behaviour(scratch: Scratch, name: str, behaviour: str) -> None:
    scratch.write(f"{name}.behaviour", behaviour + "\n")


def vt0_scenario(scratch: Scratch, participant: str = "T1", name: str = "vt0") -> pathlib.Path:
    """The test ECU counts its 10 ms steps ("ticks", 1 after its step at 0): its samples at 0, 100 and 500 ms say
    that the Run's virtual time began at 0 and that the ECU's own steps began with it."""
    return scratch.write(f"{name}.scenario.yaml",
                         f"vehicle_sil_scenario: 1\nname: {name}\nsteps:\n"
                         f"  - at_ms: 0\n    expect: {{ {participant}.ticks: {{ ge: 1, le: 1 }} }}\n"
                         f"  - at_ms: 100\n    expect: {{ {participant}.ticks: {{ ge: 11, le: 11 }} }}\n"
                         f"  - at_ms: 500\n    expect: {{ {participant}.ticks: {{ ge: 51, le: 51 }} }}\n")


def probe_scenario(scratch: Scratch, participant: str = "T1", name: str = "probe") -> pathlib.Path:
    """Another scenario on the same ECU (writes its input and reads it back): the B of A -> B -> A."""
    return scratch.write(f"{name}.scenario.yaml",
                         f"vehicle_sil_scenario: 1\nname: {name}\nsteps:\n"
                         f"  - at_ms: 100\n    set: {{ {participant}.level: 7 }}\n"
                         f"  - at_ms: 200\n    expect: {{ {participant}.ticks: {{ ge: 21, le: 21 }} }}\n"
                         f"  - at_ms: 300\n    expect: {{ {participant}.ticks: {{ ge: 31, le: 31 }} }}\n")


def long_scenario(scratch: Scratch, participant: str = "T1", name: str = "long", at_ms: int = 600_000) -> pathlib.Path:
    """A scenario that is not over for a long virtual time (a Run that a stop ends)."""
    return scratch.write(f"{name}.scenario.yaml",
                         f"vehicle_sil_scenario: 1\nname: {name}\nsteps:\n"
                         f"  - at_ms: {at_ms}\n    expect: {{ {participant}.ticks: {{ ge: 0 }} }}\n")


def ten_ecus(scratch: Scratch) -> pathlib.Path:
    """Ten test ECUs on one CAN network: T1 to T10, each sends a frame every step and counts its steps."""
    return bench(scratch, [f"T{i}" for i in range(1, 11)])


def sleeper_command() -> str:
    """A participant that never joins (a process that only waits)."""
    return f"[{_q(sys.executable)}, '-c', 'import time; time.sleep(3600)']"


def quick_command() -> str:
    """A participant that ends at once, without joining."""
    return f"[{_q(sys.executable)}, '-c', 'pass']"


def faults(scratch: Scratch) -> pathlib.Path:
    """T1 and T2 (test ECUs; their behaviour files change between Runs), Late (a required participant that never
    joins) and Quick (one that ends at once). A normal Run disables Late and Quick (fault_request())."""
    extra = (f"  - name: Late\n    command: {sleeper_command()}\n    describe: false\n"
             f"  - name: Quick\n    command: {quick_command()}\n    describe: false\n")
    return bench(scratch, ["T1", "T2"], behaviour="variables talk BUS", extra=extra)


# The behaviours of T1 for each fault, and the parts the Run leaves out (disable) for each.
FAULTS = {
    "exit": ("variables talk BUS exit-at-ms 200 3", ["Late", "Quick"]),
    "throw": ("variables talk BUS throw-at-ms 200", ["Late", "Quick"]),
    "hang": ("variables talk BUS hang-at-ms 200", ["Late", "Quick"]),
    "ignore-stop": ("variables talk BUS hang-after-end ignore-stop", ["Late", "Quick"]),
    "not-joined": ("variables talk BUS", ["Quick"]),
    "ends-at-once": ("variables talk BUS", ["Late"]),
}
NORMAL = "variables talk BUS"


def fixed_registry(scratch: Scratch, port: int) -> pathlib.Path:
    """One test ECU and a registry_uri with a fixed port (a Run that serve refuses)."""
    return bench(scratch, ["T1"], top=f"registry_uri: silkit://127.0.0.1:{port}\n", name="fixed.yaml")


def j1939_example() -> pathlib.Path:
    """examples/ecu_main: Engine and Dashboard on a J1939 CAN network (with its scenarios)."""
    return example("ecu_main", "simulation.yaml")


def ethernet_example() -> pathlib.Path:
    """examples/ethernet: Sensor and Display on an Ethernet network (with its scenario)."""
    return example("ethernet", "simulation.yaml")


def lin_pair(scratch: Scratch) -> pathlib.Path:
    """The LIN ECUs of this directory (lin_ecu.cpp): Master and Slave on the LIN network DOOR."""
    master = env_path("VEHICLE_SIL_SERVE_LIN_MASTER")
    slave = env_path("VEHICLE_SIL_SERVE_LIN_SLAVE")
    return scratch.write("lin.yaml",
                         "vehicle_sil_simulation: 2\nnetworks:\n  - name: DOOR\n    kind: lin\nparticipants:\n"
                         f"  - name: Master\n    command: [{_q(master)}]\n    step_ms: 10\n"
                         f"  - name: Slave\n    command: [{_q(slave)}]\n    step_ms: 5\n")


def lin_scenario(scratch: Scratch) -> pathlib.Path:
    """The master's command goes over LIN to the slave, whose answer comes back: values at fixed times."""
    return scratch.write("lin.scenario.yaml",
                         "vehicle_sil_scenario: 1\nname: lin\nsteps:\n"
                         "  - at_ms: 0\n    set: { Master.door.command: 5 }\n"
                         "  - at_ms: 200\n    expect: { Slave.door.command: 5, Master.door.answer: 6 }\n"
                         "  - at_ms: 500\n    set: { Master.door.command: 9 }\n"
                         "  - at_ms: 700\n    expect: { Master.door.answer: 10 }\n"
                         "  - at_ms: 900\n    expect: { Master.door.answers: { ge: 1 } }\n")


def wrapper(scratch: Scratch, name: str, target: os.PathLike, size: int = 512) -> pathlib.Path:
    """An executable script `name` (POSIX) that runs `target` with its arguments, padded to `size` bytes: two of them
    for two targets have the same size."""
    body = f"#!/bin/sh\nexec {_q(target)} \"$@\"\n"
    body += "#" * (size - len(body.encode()) - 1) + "\n"
    path = scratch.write(name, body)
    path.chmod(0o755)
    return path
