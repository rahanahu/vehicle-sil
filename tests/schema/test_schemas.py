# SPDX-License-Identifier: MIT
"""Tests of the JSON Schemas of the simulation configuration and of a scenario (schema/): that they say what the
launcher's readers say (libs/launch/src/config.cpp, scenario.cpp), no more.

    python tests/schema/test_schemas.py      (exit code 0 when every test passes)

Without the launcher (the files only):
  - the schemas are JSON Schemas, and every $ref in them leads somewhere;
  - every configuration and scenario of examples/, template/ and the GUI server's tests, and every one a document
    shows (docs/, the READMEs), is valid; the samples below are valid or not as they say;
  - every key of the schemas is in the document of its file (docs/simulation-config.md, docs/scenario.md);
  - the copies in template/schema/ are the files they are copies of, and the first line of each YAML file of examples/
    and template/ names the schema of its kind (# yaml-language-server: $schema=<relative path>).

With the built examples (VEHICLE_SIL_EXAMPLE_DIR):
  - the first line of each copy of an example's YAML file in the build directory leads to a copy of its schema.

With the launcher (VEHICLE_SIL_LAUNCH_EXE, and VEHICLE_SIL_EXAMPLE_DIR with the built examples/ecu_main; CTest sets
both, as schema.files_and_launcher):
  - the keys of every mapping are the ones the launcher knows: an unknown key is given to `check` at each place, and
    its message lists the keys there;
  - the kinds of network and the kinds of step are the launcher's;
  - `check` passes every sample the schema passes (unless the sample says the schema cannot tell), and fails every
    sample the schema fails: the schema never refuses a file that the launcher reads.

Needs PyYAML and jsonschema (the `schema` dependency group of pyproject.toml).
"""

from __future__ import annotations

import copy
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import jsonschema
import yaml

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_DIR = ROOT / "schema"
SIMULATION = json.loads((SCHEMA_DIR / "simulation.schema.json").read_text(encoding="utf-8"))
SCENARIO = json.loads((SCHEMA_DIR / "scenario.schema.json").read_text(encoding="utf-8"))
MODELINE = re.compile(r"^# yaml-language-server: \$schema=(\S+)$")
LAUNCH = os.environ.get("VEHICLE_SIL_LAUNCH_EXE", "")
EXAMPLE_DIR = os.environ.get("VEHICLE_SIL_EXAMPLE_DIR", "")
needs_launcher = unittest.skipUnless(
    LAUNCH, "VEHICLE_SIL_LAUNCH_EXE is not set: the comparison with the launcher is left out (CTest runs it)")

# The copies a project made from the template has next to its YAML files.
TEMPLATE_COPIES = {
    "template/schema/simulation.schema.json": "schema/simulation.schema.json",
    "template/schema/scenario.schema.json": "schema/scenario.schema.json",
    "template/schema/dashboard.schema.json": "gui/protocol/dashboard.schema.json",
}
# The schema of each kind of file, by the key that says the format version.
KINDS = {
    "vehicle_sil_simulation": "simulation.schema.json",
    "vehicle_sil_scenario": "scenario.schema.json",
    "vehicle_sil_dashboard": "dashboard.schema.json",
}


# --- reading YAML as an editor does ---------------------------------------------------------------------------


class CoreLoader(yaml.SafeLoader):
    """YAML 1.2 (core schema), as the YAML language server and the launcher read a file: PyYAML alone reads YAML 1.1,
    where 1e3 is a text and yes is true. A mapping's keys stay the texts that were written."""

    def construct_mapping(self, node, deep=False):
        out = {}
        for key_node, value_node in node.value:
            key = key_node.value if isinstance(key_node, yaml.ScalarNode) else self.construct_object(key_node, deep=True)
            out[key] = self.construct_object(value_node, deep=True)
        return out


CoreLoader.yaml_implicit_resolvers = {}
for _tag, _pattern, _first in [
    ("tag:yaml.org,2002:null", r"^(?:~|null|Null|NULL|)$", ["~", "n", "N", ""]),
    ("tag:yaml.org,2002:bool", r"^(?:true|True|TRUE|false|False|FALSE)$", "tTfF"),
    ("tag:yaml.org,2002:int", r"^(?:[-+]?[0-9]+|0o[0-7]+|0x[0-9a-fA-F]+)$", "-+0123456789"),
    ("tag:yaml.org,2002:float",
     r"^(?:[-+]?(?:\.[0-9]+|[0-9]+(?:\.[0-9]*)?)(?:[eE][-+]?[0-9]+)?|[-+]?\.(?:inf|Inf|INF)|\.(?:nan|NaN|NAN))$",
     "-+0123456789."),
]:
    CoreLoader.add_implicit_resolver(_tag, re.compile(_pattern), list(_first))
CoreLoader.add_constructor("tag:yaml.org,2002:map", CoreLoader.construct_mapping)
CoreLoader.add_constructor("tag:yaml.org,2002:int", lambda loader, node: _integer(loader.construct_scalar(node)))
CoreLoader.add_constructor("tag:yaml.org,2002:float", lambda loader, node: _real(loader.construct_scalar(node)))


def _integer(text: str) -> int:
    return int(text, 0) if text.lstrip("+-")[:2] in ("0x", "0o") else int(text)


def _real(text: str) -> float:
    return float(text.lower().replace(".inf", "inf").replace(".nan", "nan"))


def load(text: str):
    return yaml.load(text, Loader=CoreLoader)


def problems(schema: dict, data) -> list[str]:
    """What the schema says against `data`, one line a problem ('' at the top)."""
    validator = jsonschema.Draft7Validator(schema)
    return sorted(f"{'/'.join(str(p) for p in e.absolute_path)}: {e.message}" for e in validator.iter_errors(data))


def without_required(schema: dict) -> dict:
    """The schema for a part of a file, as documents show them: the keys at the top may be missing."""
    out = copy.deepcopy(schema)
    out.pop("required", None)
    return out


# --- the mappings of the schemas ------------------------------------------------------------------------------


def resolve(schema: dict, node: dict) -> dict:
    while "$ref" in node:
        target = schema
        for part in node["$ref"].removeprefix("#/").split("/"):
            target = target[part]
        node = target
    return node


def simulation_mappings() -> dict[str, dict]:
    """Each mapping of the configuration whose keys are fixed: its name here, and its schema."""
    definitions = SIMULATION["definitions"]
    variable = next(o for o in definitions["variable"]["oneOf"] if o.get("type") == "object")
    variant = definitions["variant"]
    return {
        "top": SIMULATION,
        "network": SIMULATION["properties"]["networks"]["items"],
        "participant": SIMULATION["properties"]["participants"]["items"],
        "variable": variable,
        "variant": variant,
        "variant participant": variant["properties"]["participants"]["additionalProperties"],
    }


def step_kinds() -> dict[str, dict]:
    """Each kind of step (the key that makes it one) with its schema."""
    kinds = {}
    for entry in SCENARIO["definitions"]["step"]["oneOf"]:
        name = entry["$ref"].rsplit("/", 1)[1]
        kinds[name.removeprefix("step_")] = resolve(SCENARIO, entry)
    return kinds


# The shapes of a wave that are mappings (points is a list), each with what the launcher needs to read its keys.
WAVE_SHAPES_WITH_KEYS = {
    "sine": "amplitude: 1, period_ms: 100",
    "triangle": "amplitude: 1, period_ms: 100",
    "sawtooth": "amplitude: 1, period_ms: 100",
    "square": "low: 0, high: 1, period_ms: 100",
    "pulse": "value: 1, width_ms: 50",
    "noise": "amplitude: 1, hold_ms: 10, seed: 1",
    "chirp": "amplitude: 1, from_hz: 1, to_hz: 2",
    "lag": "to: 1, tau_ms: 100",
    "chatter": "to: 1, interval_ms: 10",
}


def scenario_mappings() -> dict[str, dict]:
    definitions = SCENARIO["definitions"]
    comparison = next(o for o in definitions["condition"]["anyOf"] if o.get("type") == "object")
    step = {"properties": {}}
    for kind in step_kinds().values():
        step["properties"].update(kind["properties"])
    return {
        "top": SCENARIO,
        "step": step,
        "comparison": comparison,
        "ramp": definitions["step_ramp"]["properties"]["ramp"]["additionalProperties"],
        "wave": definitions["wave"],
        **{f"wave {shape}": definitions["wave"]["properties"][shape] for shape in WAVE_SHAPES_WITH_KEYS},
        "wave points file": definitions["points_file"],
    }


def keys_of(mapping: dict) -> set[str]:
    return set(mapping["properties"])


# --- the files and the documents ------------------------------------------------------------------------------


def yaml_files() -> list[Path]:
    """The YAML files that people read as examples, and the ones the GUI server's tests give the launcher."""
    out: list[Path] = []
    for top in ("examples", "template", "gui/server/tests/configs"):
        out += [p for p in sorted((ROOT / top).rglob("*.yaml"))
                if not {"build", "tools", "node_modules"} & set(p.relative_to(ROOT).parts)]
    return out


def kind_of(data) -> str | None:
    if isinstance(data, dict):
        for key, schema in KINDS.items():
            if key in data:
                return schema
    return None


def documents() -> list[Path]:
    return sorted([*(ROOT / "docs").rglob("*.md"), ROOT / "README.md", ROOT / "template" / "README.md",
                   *(ROOT / "examples").rglob("README.md")])


def yaml_blocks(path: Path) -> list[tuple[int, str, str]]:
    """The ```yaml blocks of a document: (line of the fence, the line before the fence, the text)."""
    lines = path.read_text(encoding="utf-8").split("\n")
    out = []
    i = 0
    while i < len(lines):
        if lines[i].strip() == "```yaml":
            end = next(j for j in range(i + 1, len(lines)) if lines[j].strip() == "```")
            indent = len(lines[i]) - len(lines[i].lstrip())
            before = next((ln.strip() for ln in reversed(lines[:i]) if ln.strip()), "")
            out.append((i + 1, before, "\n".join(ln[indent:] for ln in lines[i + 1:end]) + "\n"))
            i = end
        i += 1
    return out


# The blocks of the documents that show a mistake the schema finds (the document says what the launcher says of it).
# The key is "<document>:<the line before the block>".
MISTAKES_IN_DOCUMENTS = {
    "docs/simulation-check.md:<!-- two-engines.yaml -->": ["participants/2: "],  # a key written wrongly
    "docs/scenario.md:<!-- throttle_mistakes.yaml -->": ["steps/4: "],  # near without tolerance
}


# --- samples --------------------------------------------------------------------------------------------------

# @EXE@ is an executable that exists (the launcher itself; such a participant is not asked what it is).
CONFIG_HEAD = "vehicle_sil_simulation: 1\nnetworks:\n  - { name: Body, kind: can }\n"
P = "participants:\n  - name: Tool\n    command: ['@EXE@']\n    describe: false\n"

# (what it shows, the text, "valid" | "invalid" | "check only": the launcher refuses it, the schema cannot tell)
CONFIG_SAMPLES = [
    ("the least", CONFIG_HEAD + P, "valid"),
    ("JSON", '{"vehicle_sil_simulation": 1, "networks": [], "participants": [{"name": "Tool", "command": ["@EXE@"], '
             '"describe": false}]}', "valid"),
    ("every key at the top",
     CONFIG_HEAD + P + "stop_at_ms: 1000\nstart_timeout_ms: 5000\nstop_timeout_ms: 2000\n"
     "registry_uri: silkit://127.0.0.1:8500\nspeed: 0.5\ntime_step_ms: 10\nlate_tolerance_ms: 20\nparams: { idle_rpm: 800 }\n",
     "valid"),
    ("speed max, an exponent, capital booleans",
     CONFIG_HEAD + "speed: max\nlate_tolerance_ms: 010\nparticipants:\n  - name: Tool\n    command: '@EXE@'\n"
     "    describe: False\n    required: TRUE\n    enabled: True\n    manual: false\n", "valid"),
    ("speed 1e3", CONFIG_HEAD + P + "speed: 1e3\n", "valid"),
    ("the ends of the ranges",
     CONFIG_HEAD + P + "speed: 0.000001\nstart_timeout_ms: 1\nstop_timeout_ms: 1000000000000\n"
     "registry_uri: \"silkit://[::1]:65535\"\n", "valid"),
    ("max in quotes, the fastest speed", CONFIG_HEAD + P + "speed: \"max\"\nlate_tolerance_ms: 1\n", "valid"),
    ("more milliseconds than there are", CONFIG_HEAD + P + "stop_at_ms: 1000000000001\n", "invalid"),
    ("names that YAML reads as numbers and booleans",
     "vehicle_sil_simulation: 1\nnetworks:\n  - { name: 7, kind: lin }\n  - { name: true, kind: ethernet }\n"
     "participants:\n  - name: 12\n    command: ['@EXE@', --count, 3, --fast, true, 1.5, '']\n    describe: false\n", "valid"),
    ("every form of a variable mapping",
     CONFIG_HEAD + P + "    variables:\n      a.b: raw\n      a.c: { from: raw_c, scale: 0.1, offset: -40, unit: degC }\n"
     "      a.d: { from: raw_d, map: { P: 0, R: 1 } }\n      a.e: { absent: true }\n      a.f: { from: 5, unit: }\n"
     "      a.g: 5\n", "valid"),
    ("variants",
     CONFIG_HEAD + P + "variants:\n  small:\n  medium: {}\n  7:\n    participants:\n      Tool:\n"
     "        command: ['@EXE@', -v]\n        variables: { a.b: { absent: true } }\n        enabled: true\n"
     "    params: { idle_rpm: 650, name: \"a text\", on: true }\n", "valid"),
    ("an empty params", CONFIG_HEAD + P + "params: {}\n", "valid"),
    ("an unknown key at the top", CONFIG_HEAD + P + "stop_ms: 5\n", "invalid"),
    ("an unknown key of a network", "vehicle_sil_simulation: 1\nnetworks:\n  - { name: Body, kind: can, baud: 1 }\n" + P,
     "invalid"),
    ("an unknown key of a participant", CONFIG_HEAD + P + "    step: 5\n", "invalid"),
    ("an unknown key of a variable mapping", CONFIG_HEAD + P + "    variables: { a.b: { from: x, factor: 2 } }\n", "invalid"),
    ("an unknown key of a variant", CONFIG_HEAD + P + "variants: { small: { networks: [] } }\n", "invalid"),
    ("an unknown key of a variant's participant",
     CONFIG_HEAD + P + "variants: { small: { participants: { Tool: { step_ms: 5 } } } }\n", "invalid"),
    ("no format version", "networks: []\n" + P, "invalid"),
    ("another format version", CONFIG_HEAD.replace(": 1", ": 2") + P, "invalid"),
    ("no networks", "vehicle_sil_simulation: 1\n" + P, "invalid"),
    ("no participants", CONFIG_HEAD, "invalid"),
    ("an empty participants", CONFIG_HEAD + "participants: []\n", "invalid"),
    ("a participant without a name", CONFIG_HEAD + "participants:\n  - command: ['@EXE@']\n    describe: false\n", "invalid"),
    ("a participant without a command", CONFIG_HEAD + "participants:\n  - name: Tool\n    describe: false\n", "invalid"),
    ("a network without a kind", "vehicle_sil_simulation: 1\nnetworks:\n  - name: Body\n" + P, "invalid"),
    ("a kind there is not", CONFIG_HEAD.replace("can", "flexray") + P, "invalid"),
    ("a kind in capitals", CONFIG_HEAD.replace("can", "CAN") + P, "invalid"),
    ("a participant for testing", CONFIG_HEAD + P + "    role: test\n", "valid"),
    ("a bridge", CONFIG_HEAD + P + "    role: bridge\n", "valid"),
    ("a role there is not", CONFIG_HEAD + P + "    role: spy\n", "invalid"),
    ("a role in capitals", CONFIG_HEAD + P + "    role: Test\n", "invalid"),
    ("a role in a variant", CONFIG_HEAD + P + "variants: { small: { participants: { Tool: { role: test } } } }\n",
     "invalid"),
    ("a number in quotes", CONFIG_HEAD + P + 'stop_at_ms: "1000"\n', "invalid"),
    ("a number with a fraction", CONFIG_HEAD + P + "stop_at_ms: 1.5\n", "invalid"),
    ("zero milliseconds", CONFIG_HEAD + P + "stop_timeout_ms: 0\n", "invalid"),
    ("a boolean in quotes", CONFIG_HEAD + P + '    manual: "true"\n', "invalid"),
    ("yes for a boolean", CONFIG_HEAD + P + "    manual: yes\n", "invalid"),
    ("a speed of 0", CONFIG_HEAD + P + "speed: 0\n", "invalid"),
    ("a speed that is a word", CONFIG_HEAD + P + "speed: fast\n", "invalid"),
    ("a registry without a port", CONFIG_HEAD + P + "registry_uri: silkit://localhost\n", "invalid"),
    ("networks as a mapping", "vehicle_sil_simulation: 1\nnetworks: { Body: can }\n" + P, "invalid"),
    ("an empty command", CONFIG_HEAD + "participants:\n  - name: Tool\n    command: []\n    describe: false\n", "invalid"),
    ("a placeholder without quotes",
     CONFIG_HEAD + "participants:\n  - name: Tool\n    command: ['@EXE@', {registry_uri}]\n    describe: false\n", "invalid"),
    ("an empty variants", CONFIG_HEAD + P + "variants: {}\n", "invalid"),
    ("a variant that is a text", CONFIG_HEAD + P + "variants: { small: yes }\n", "invalid"),
    ("a variant's name with -", CONFIG_HEAD + P + "variants: { -small: {} }\n", "invalid"),
    ("params without a value", CONFIG_HEAD + P + "params:\n", "invalid"),
    ("a parameter's name with -", CONFIG_HEAD + P + "params: { idle-rpm: 800 }\n", "invalid"),
    ("a parameter that is a list", CONFIG_HEAD + P + "params: { idle_rpm: [800] }\n", "invalid"),
    ("a parameter that uses a parameter", CONFIG_HEAD + P + "params: { idle_rpm: \"${other}\" }\n", "invalid"),
    ("a mapping without from", CONFIG_HEAD + P + "    variables: { a.b: { scale: 2 } }\n", "invalid"),
    ("absent with another key", CONFIG_HEAD + P + "    variables: { a.b: { absent: true, from: x } }\n", "invalid"),
    ("absent: false", CONFIG_HEAD + P + "    variables: { a.b: { absent: false } }\n", "invalid"),
    ("a scale of 0", CONFIG_HEAD + P + "    variables: { a.b: { from: x, scale: 0 } }\n", "invalid"),
    ("a map with a name for a text", CONFIG_HEAD + P + "    variables: { a.b: { from: x, map: { P: park } } }\n", "invalid"),
    ("scale together with map", CONFIG_HEAD + P + "    variables: { a.b: { from: x, scale: 1, map: { P: 0 } } }\n",
     "invalid"),
    ("offset together with map", CONFIG_HEAD + P + "    variables: { a.b: { from: x, offset: 0, map: { P: 0 } } }\n",
     "invalid"),
    ("an empty map", CONFIG_HEAD + P + "    variables: { a.b: { from: x, map: {} } }\n", "invalid"),
    # What the schema leaves to check.
    ("a key twice", CONFIG_HEAD + P + "stop_at_ms: 10\nstop_at_ms: 20\n", "check only"),
    ("a name twice", CONFIG_HEAD + P + P.split("\n", 1)[1], "check only"),
    ("an executable that is not there", CONFIG_HEAD + P.replace("@EXE@", "no/such/executable"), "check only"),
    ("required: false with describe: true", CONFIG_HEAD + P.replace("describe: false", "required: false"), "check only"),
    ("a port out of range", CONFIG_HEAD + P + "registry_uri: silkit://127.0.0.1:70000\n", "check only"),
    ("stop_at_ms that is not a multiple of time_step_ms", CONFIG_HEAD + P + "stop_at_ms: 15\ntime_step_ms: 10\n",
     "check only"),
    ("a name that starts with -", CONFIG_HEAD + P.replace("Tool", "-Tool"), "check only"),
    ("a placeholder there is not", CONFIG_HEAD + P.replace("['@EXE@']", "['@EXE@', '{port}']"), "check only"),
    ("a variant of a participant there is not", CONFIG_HEAD + P + "variants: { small: { participants: { Door: {} } } }\n",
     "check only"),
    ("two names for one value", CONFIG_HEAD + P + "    variables: { a.b: { from: x, map: { P: 0, Q: 0 } } }\n",
     "check only"),
]

# For the two ECUs of examples/ecu_main: Engine.engine.throttle (real input, 0 to 100), Engine.engine.speed (integer
# output), Engine.ecu.connected (bool input), Dashboard.dashboard.engine_lost (bool output). The launcher checks them
# with this configuration, as its variant small (@DIR@: the directory of the built example).
SCENARIO_CONFIG = (
    "vehicle_sil_simulation: 1\nnetworks:\n  - { name: J1939_CAN, kind: can }\nparticipants:\n"
    "  - name: Engine\n    command: ['@DIR@/engine/vehicle_sil_engine_ecu']\n"
    "    variables: { engine.turbo: { absent: true } }\n"
    "  - name: Dashboard\n    command: ['@DIR@/dashboard/vehicle_sil_dashboard_ecu']\n"
    "variants: { small: {}, 7: {} }\nparams: { from_configuration: 20 }\n")
S = "vehicle_sil_scenario: 1\nsteps:\n"
SET = "  - set: { Engine.engine.throttle: 50 }\n"

SCENARIO_SAMPLES = [
    ("the least", S + SET, "valid"),
    ("JSON", '{"vehicle_sil_scenario": 1, "steps": [{"set": {"Engine.engine.throttle": 50}}]}', "valid"),
    ("every kind of step",
     "vehicle_sil_scenario: 1\nname: every kind\nsteps:\n  - at_ms: 0\n" + SET.replace("  - ", "    ") +
     "  - after_ms: 10\n    ramp: { Engine.engine.throttle: { to: 80, over_ms: 100, from: 20.5 } }\n"
     "  - expect: { Engine.engine.speed: { ge: 0, le: 100000, ne: -1 } }\n"
     "  - expect: { Engine.engine.speed: { gt: -1, lt: 1e6 } }\n    hold_for_ms: 20\n"
     "  - expect: { Engine.engine.speed: { near: 900, tolerance: 5000 }, Engine.engine.throttle: 80 }\n    within_ms: 50\n"
     "  - wait_until: { Dashboard.dashboard.engine_lost: false, Engine.ecu.connected: { eq: true } }\n    timeout_ms: 500\n",
     "valid"),
    ("every shape of a wave",
     S + SET +
     "  - wave: { Engine.engine.throttle: { sine: { amplitude: 5, period_ms: 200, phase_deg: 90 }, around: 50, for_ms: 400 } }\n"
     "  - wave: { Engine.engine.throttle: { triangle: { amplitude: 5, frequency_hz: 2.5 }, for_ms: 400 } }\n"
     "  - wave: { Engine.engine.throttle: { sawtooth: { amplitude: 5, period_ms: 200, falling: true }, for_ms: 400 } }\n"
     "  - wave: { Engine.engine.throttle: { square: { low: 20, high: 80, period_ms: 100, duty: 30, phase_deg: -90 }, for_ms: 400 } }\n"
     "  - wave: { Engine.engine.throttle: { square: { amplitude: 5, frequency_hz: 5 }, around: 50, for_ms: 400 },\n"
     "            Engine.ecu.connected: { square: { period_ms: 100 }, for_ms: 200 } }\n"
     "  - wave: { Engine.engine.throttle: { pulse: { value: 100, width_ms: 50 } }, Engine.ecu.connected: { pulse: { width_ms: 30 } } }\n"
     "  - wave: { Engine.engine.throttle: { noise: { amplitude: 2, hold_ms: 10, seed: 7 }, around: 50, for_ms: 100 } }\n"
     "  - wave: { Engine.engine.throttle: { noise: { amplitude: 2, hold_ms: 10, seed: 7, distribution: gaussian }, for_ms: 100 } }\n"
     "  - wave: { Engine.engine.throttle: { chirp: { amplitude: 5, from_hz: 1, to_hz: 10, sweep: log }, for_ms: 1000 } }\n"
     "  - wave: { Engine.engine.throttle: { chirp: { amplitude: 5, from_hz: 0, to_hz: 10 }, for_ms: 1000 } }\n"
     "  - wave: { Engine.engine.throttle: { lag: { from: 0, to: 80, tau_ms: 200, settle: true }, for_ms: 1000 } }\n"
     "  - wave: { Engine.ecu.connected: { chatter: { to: true, bounces: 3, interval_ms: 10 } },\n"
     "            Engine.engine.throttle: { chatter: { from: 0, to: 80, interval_ms: 20, seed: 4 }, for_ms: 100 } }\n"
     "  - wave: { Engine.engine.throttle: { points: [[0, 0], [100, 50], [400, 20]] }, Engine.ecu.connected: { points: [[0, false], [50, true]], interpolate: hold } }\n"
     "  - wave: { Engine.engine.throttle: { pulse: { value: 100, width_ms: 3 } } }\n    undersampled: ok\n",
     "valid"),
    ("parameters in a wave",
     S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: \"${a}\", frequency_hz: \"${hz}\", phase_deg: \"${a}\" }, around: \"${mid}\", for_ms: \"${ms}\" } }\n"
     "  - wave: { Engine.engine.throttle: { points: [[0, \"${a}\"], [\"${ms}\", \"${mid}\"]], interpolate: \"${how}\" } }\n"
     "  - wave: { Engine.engine.throttle: { sawtooth: { amplitude: \"${a}\", period_ms: \"${ms}\", falling: \"${yes}\" }, around: \"${mid}\", for_ms: \"${ms}\" } }\n"
     "  - wave: { Engine.engine.throttle: { noise: { amplitude: \"${a}\", hold_ms: \"${ms}\", seed: \"${ms}\", distribution: \"${bell}\" }, around: \"${mid}\", for_ms: \"${ms}\" } }\n"
     "params: { a: 5, hz: 2, mid: 50, ms: 400, how: hold, yes: true, bell: gaussian }\n", "valid"),
    ("points from a CSV file",
     S + "  - wave: { Engine.engine.throttle: { points: { file: points.csv, value: throttle } },\n"
     "            Engine.ecu.connected: { points: { file: ./points.csv, value: connected, time: time, time_unit: ms } } }\n"
     "  - wave: { Engine.engine.throttle: { points: { file: \"${trace}\", value: \"${column}\", time_unit: \"${unit}\" } } }\n"
     "params: { trace: points.csv, column: throttle, unit: ms }\n", "valid"),
    ("points from a file by an absolute path",
     S + "  - wave: { Engine.engine.throttle: { points: { file: /tmp/points.csv, value: throttle } } }\n", "invalid"),
    ("points from a file on a drive",
     S + "  - wave: { Engine.engine.throttle: { points: { file: 'C:/points.csv', value: throttle } } }\n", "invalid"),
    ("points from a file above the scenario",
     S + "  - wave: { Engine.engine.throttle: { points: { file: sub/../../points.csv, value: throttle } } }\n", "invalid"),
    ("points from a file with a backslash",
     S + "  - wave: { Engine.engine.throttle: { points: { file: 'sub\\points.csv', value: throttle } } }\n", "invalid"),
    ("points from a file without the column of the values",
     S + "  - wave: { Engine.engine.throttle: { points: { file: points.csv } } }\n", "invalid"),
    ("a time_unit there is not",
     S + "  - wave: { Engine.engine.throttle: { points: { file: points.csv, value: throttle, time_unit: min } } }\n", "invalid"),
    ("a wave without a shape", S + "  - wave: { Engine.engine.throttle: { for_ms: 100 } }\n", "invalid"),
    ("a wave with two shapes",
     S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 1, period_ms: 100 }, pulse: { value: 1, width_ms: 50 }, for_ms: 100 } }\n",
     "invalid"),
    ("an unknown shape", S + "  - wave: { Engine.engine.throttle: { cosine: { amplitude: 1, period_ms: 100 }, for_ms: 100 } }\n", "invalid"),
    ("an unknown key of a shape",
     S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 1, period_ms: 100, offset: 5 }, for_ms: 100 } }\n", "invalid"),
    ("a sine without for_ms", S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 1, period_ms: 100 } } }\n", "invalid"),
    ("a sine without a period", S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 1 }, for_ms: 100 } }\n", "invalid"),
    ("period_ms and frequency_hz",
     S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 1, period_ms: 100, frequency_hz: 10 }, for_ms: 100 } }\n", "invalid"),
    ("a period of 0", S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 1, period_ms: 0 }, for_ms: 100 } }\n", "invalid"),
    ("a frequency of 0", S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 1, frequency_hz: 0 }, for_ms: 100 } }\n", "invalid"),
    ("a negative amplitude", S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: -1, period_ms: 100 }, for_ms: 100 } }\n", "invalid"),
    ("a duty over 100",
     S + "  - wave: { Engine.engine.throttle: { square: { low: 0, high: 1, period_ms: 100, duty: 150 }, for_ms: 100 } }\n", "invalid"),
    ("a noise without a seed", S + "  - wave: { Engine.engine.throttle: { noise: { amplitude: 1, hold_ms: 10 }, for_ms: 100 } }\n", "invalid"),
    ("a distribution there is not",
     S + "  - wave: { Engine.engine.throttle: { noise: { amplitude: 1, hold_ms: 10, seed: 1, distribution: normal }, for_ms: 100 } }\n", "invalid"),
    ("a pulse with for_ms", S + "  - wave: { Engine.engine.throttle: { pulse: { value: 1, width_ms: 50 }, for_ms: 100 } }\n", "invalid"),
    ("points with for_ms", S + "  - wave: { Engine.engine.throttle: { points: [[0, 1], [50, 2]], for_ms: 100 } }\n", "invalid"),
    ("a pulse with around", S + "  - wave: { Engine.engine.throttle: { pulse: { value: 1, width_ms: 50 }, around: 5 } }\n", "invalid"),
    ("a chirp from a negative frequency",
     S + "  - wave: { Engine.engine.throttle: { chirp: { amplitude: 1, from_hz: -1, to_hz: 2 }, for_ms: 100 } }\n", "invalid"),
    ("points with around", S + "  - wave: { Engine.engine.throttle: { points: [[0, 1], [50, 2]], around: 5 } }\n", "invalid"),
    ("a point with three numbers", S + "  - wave: { Engine.engine.throttle: { points: [[0, 1, 2], [50, 2]] } }\n", "invalid"),
    ("points that are a mapping", S + "  - wave: { Engine.engine.throttle: { points: { 0: 1, 50: 2 } } }\n", "invalid"),
    ("interpolate without points",
     S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 1, period_ms: 100 }, for_ms: 100, interpolate: hold } }\n", "invalid"),
    ("a lag with around", S + "  - wave: { Engine.engine.throttle: { lag: { to: 1, tau_ms: 100 }, around: 5, for_ms: 100 } }\n", "invalid"),
    ("a chatter with bounces and for_ms",
     S + "  - wave: { Engine.engine.throttle: { chatter: { to: 1, bounces: 2, interval_ms: 10 }, for_ms: 100 } }\n", "invalid"),
    ("a chatter with neither", S + "  - wave: { Engine.engine.throttle: { chatter: { to: 1, interval_ms: 10 } } }\n", "invalid"),
    ("undersampled that is not ok",
     S + "  - wave: { Engine.engine.throttle: { pulse: { value: 1, width_ms: 50 } } }\n    undersampled: true\n", "invalid"),
    ("undersampled with set", S + SET + "    undersampled: ok\n", "invalid"),
    ("parameters where values stand",
     "vehicle_sil_scenario: 1\nname: 7\nparams: { pedal: 50, ms: 10, rpm: 900, far: 5000.5, on: true, empty: \"\" }\n"
     "steps:\n  - at_ms: ${ms}\n    set: { Engine.engine.throttle: \"${pedal}\", Engine.ecu.connected: \"${on}\" }\n"
     "  - after_ms: ${ms}\n    ramp: { Engine.engine.throttle: { to: \"${pedal}\", over_ms: \"${ms}\", from: \"${pedal}\" } }\n"
     "  - expect: { Engine.engine.speed: { near: \"${rpm}\", tolerance: \"${far}\" } }\n    within_ms: ${ms}\n"
     "  - expect: { Engine.engine.speed: { ge: 0 } }\n    hold_for_ms: ${ms}\n"
     "  - wait_until: { Engine.ecu.connected: \"${on}\" }\n    timeout_ms: ${ms}\n", "valid"),
    ("hexadecimal values of an integer",
     S + "  - expect: { Engine.engine.speed: { ge: 0x0, le: 0X1F40, ne: 0x7fffffffffffffff } }\n"
     "  - wait_until: { Engine.engine.speed: { near: \"${rpm}\", tolerance: 16 } }\n    timeout_ms: 5\n"
     "params: { rpm: 0X384 }\n", "valid"),
    ("only, and a parameter of the configuration",
     S + "  - after_ms: ${from_configuration}\n" + SET.replace("  - ", "    ") + "only: [small, 7]\nparams: {}\n", "valid"),
    ("a step with a variable that is absent", S + SET + "  - expect: { Engine.engine.turbo: { le: 0.1 } }\n", "valid"),
    ("an expect beside a ramp and beside a wave",
     S + SET + "  - ramp: { Engine.engine.throttle: { to: 80, over_ms: 100 } }\n    expect: { Engine.engine.speed: { ge: 0 } }\n"
     "  - wave: { Engine.engine.throttle: { sine: { amplitude: 5, period_ms: 100 }, for_ms: 200 } }\n"
     "    expect: { Engine.engine.speed: { le: 100000 }, Engine.engine.turbo: { le: 0.1 } }\n", "valid"),
    ("an unknown key at the top", S + SET + "description: x\n", "invalid"),
    ("an unknown key of a step", S + SET + "    timeout: 5\n", "invalid"),
    ("an unknown comparison", S + "  - expect: { Engine.engine.speed: { min: 5 } }\n", "invalid"),
    ("an unknown key of a ramp", S + "  - ramp: { Engine.engine.throttle: { to: 5, over_ms: 10, step: 1 } }\n", "invalid"),
    ("no format version", "steps:\n" + SET, "invalid"),
    ("another format version", S.replace(": 1", ": 2") + SET, "invalid"),
    ("no steps", "vehicle_sil_scenario: 1\n", "invalid"),
    ("an empty steps", "vehicle_sil_scenario: 1\nsteps: []\n", "invalid"),
    ("a step that does nothing", S + "  - at_ms: 5\n", "invalid"),
    ("a step that does two things", S + SET + "    expect: { Engine.engine.speed: 0 }\n", "invalid"),
    ("at_ms and after_ms", S + "  - at_ms: 5\n    after_ms: 5\n" + SET.replace("  - ", "    "), "invalid"),
    ("wait_until without timeout_ms", S + "  - wait_until: { Engine.engine.speed: { ge: 0 } }\n", "invalid"),
    ("timeout_ms with expect", S + "  - expect: { Engine.engine.speed: { ge: 0 } }\n    timeout_ms: 5\n", "invalid"),
    ("within_ms with set", S + SET + "    within_ms: 5\n", "invalid"),
    ("within_ms with an expect beside a ramp",
     S + SET + "  - ramp: { Engine.engine.throttle: { to: 80, over_ms: 100 } }\n    expect: { Engine.engine.speed: { ge: 0 } }\n"
     "    within_ms: 5\n", "invalid"),
    ("hold_for_ms with an expect beside a wave",
     S + SET + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 5, period_ms: 100 }, for_ms: 200 } }\n"
     "    expect: { Engine.engine.speed: { ge: 0 } }\n    hold_for_ms: 5\n", "invalid"),
    ("an expect beside a wait_until",
     S + "  - wait_until: { Engine.engine.speed: { ge: 0 } }\n    timeout_ms: 5\n    expect: { Engine.engine.speed: { ge: 0 } }\n",
     "invalid"),
    ("hold_for_ms with wait_until",
     S + "  - wait_until: { Engine.engine.speed: { ge: 0 } }\n    timeout_ms: 5\n    hold_for_ms: 5\n", "invalid"),
    ("within_ms and hold_for_ms",
     S + "  - expect: { Engine.engine.speed: { ge: 0 } }\n    within_ms: 5\n    hold_for_ms: 5\n", "invalid"),
    ("a time in quotes", S + "  - at_ms: \"5\"\n" + SET.replace("  - ", "    "), "invalid"),
    ("a negative time", S + "  - after_ms: -1\n" + SET.replace("  - ", "    "), "invalid"),
    ("a timeout of 0", S + "  - wait_until: { Engine.engine.speed: { ge: 0 } }\n    timeout_ms: 0\n", "invalid"),
    ("an empty set", S + "  - set: {}\n", "invalid"),
    ("a set that is a list", S + "  - set: [Engine.engine.throttle, 50]\n", "invalid"),
    ("a value that is a list", S + "  - set: { Engine.engine.throttle: [50] }\n", "invalid"),
    ("a value that is empty", S + "  - set: { Engine.engine.throttle: }\n", "invalid"),
    ("a variable without its participant", S + "  - set: { throttle: 50 }\n", "invalid"),
    ("a ramp without to", S + "  - ramp: { Engine.engine.throttle: { over_ms: 10 } }\n", "invalid"),
    ("a ramp without over_ms", S + "  - ramp: { Engine.engine.throttle: { to: 10 } }\n", "invalid"),
    ("a ramp to a value", S + "  - ramp: { Engine.engine.throttle: 10 }\n", "invalid"),
    ("a ramp to a text", S + "  - ramp: { Engine.engine.throttle: { to: full, over_ms: 10 } }\n", "invalid"),
    ("near without tolerance", S + "  - expect: { Engine.engine.speed: { near: 5 } }\n", "invalid"),
    ("tolerance without near", S + "  - expect: { Engine.engine.speed: { ge: 5, tolerance: 1 } }\n", "invalid"),
    ("a negative tolerance", S + "  - expect: { Engine.engine.speed: { near: 5, tolerance: -1 } }\n", "invalid"),
    ("nothing to compare with", S + "  - expect: { Engine.engine.speed: {} }\n", "invalid"),
    ("a parameter in a text", S + "  - at_ms: \"${ms} ms\"\n" + SET.replace("  - ", "    ") + "params: { ms: 5 }\n",
     "invalid"),
    ("an empty only", S + SET + "only: []\n", "invalid"),
    ("only that is a text", S + SET + "only: small\n", "invalid"),
    ("a parameter that uses a parameter", S + SET + "params: { a: \"${b}\" }\n", "invalid"),
    ("a parameter's name with a digit first", S + SET + "params: { 1st: 5 }\n", "invalid"),
    # What the schema leaves to check.
    ("a key twice", S + SET + "name: a\nname: b\n", "check only"),
    ("a variable there is not", S + "  - set: { Engine.engine.pedal: 50 }\n", "check only"),
    ("a participant there is not", S + "  - set: { Motor.engine.throttle: 50 }\n", "check only"),
    ("a value out of the range", S + "  - set: { Engine.engine.throttle: 500 }\n", "check only"),
    ("a value of the wrong type", S + "  - set: { Engine.ecu.connected: 1 }\n", "check only"),
    ("a number in quotes for a value", S + "  - set: { Engine.engine.throttle: \"50\" }\n", "check only"),
    ("writing an output", S + "  - set: { Engine.engine.speed: 50 }\n", "check only"),
    ("ge of a bool", S + "  - expect: { Engine.ecu.connected: { ge: true } }\n", "check only"),
    ("a ramp of a bool", S + "  - ramp: { Engine.ecu.connected: { to: 1, over_ms: 10 } }\n", "check only"),
    ("a sine of a bool", S + "  - wave: { Engine.ecu.connected: { sine: { amplitude: 1, period_ms: 100 }, for_ms: 100 } }\n", "check only"),
    ("a wave that leaves the range",
     S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 10, period_ms: 100 }, around: 95, for_ms: 100 } }\n", "check only"),
    ("a square with low and no high",
     S + "  - wave: { Engine.engine.throttle: { square: { low: 1, period_ms: 100 }, for_ms: 100 } }\n", "check only"),
    ("a square of a bool with low and high",
     S + "  - wave: { Engine.ecu.connected: { square: { low: 0, high: 1, period_ms: 100 }, for_ms: 100 } }\n", "check only"),
    ("a log chirp from 0 Hz",
     S + "  - wave: { Engine.engine.throttle: { chirp: { amplitude: 1, from_hz: 0, to_hz: 2, sweep: log }, for_ms: 100 } }\n", "check only"),
    ("points whose times do not go up", S + "  - wave: { Engine.engine.throttle: { points: [[0, 1], [50, 2], [50, 3]] } }\n", "check only"),
    ("points that end at 0", S + "  - wave: { Engine.engine.throttle: { points: [[0, 1]] } }\n", "check only"),
    ("a wave of an output", S + "  - wave: { Engine.engine.speed: { pulse: { value: 1, width_ms: 50 } } }\n", "check only"),
    ("at_ms before the step before", S + "  - at_ms: 50\n" + SET.replace("  - ", "    ") + "  - at_ms: 10\n" +
     SET.replace("  - ", "    "), "check only"),
    ("at_ms after a step that waits",
     S + "  - wait_until: { Engine.engine.speed: { ge: 0 } }\n    timeout_ms: 5\n  - at_ms: 100\n" +
     SET.replace("  - ", "    "), "check only"),
    ("a parameter without a value", S + "  - at_ms: ${ms}\n" + SET.replace("  - ", "    "), "check only"),
    ("a parameter of the wrong type", S + "  - at_ms: ${ms}\n" + SET.replace("  - ", "    ") + "params: { ms: soon }\n",
     "check only"),
    ("points from a file that is not there",
     S + "  - wave: { Engine.engine.throttle: { points: { file: none.csv, value: throttle } } }\n", "check only"),
    ("points from a column that is not there",
     S + "  - wave: { Engine.engine.throttle: { points: { file: points.csv, value: speed } } }\n", "check only"),
    ("hexadecimal of a real", S + "  - set: { Engine.engine.throttle: 0x10 }\n", "check only"),
    ("hexadecimal of a real in a ramp", S + "  - ramp: { Engine.engine.throttle: { to: 0X10, over_ms: 10 } }\n", "check only"),
    ("a negative hexadecimal", S + "  - expect: { Engine.engine.speed: { ge: -0x1 } }\n", "check only"),
    ("hexadecimal in quotes", S + "  - expect: { Engine.engine.speed: \"0x10\" }\n", "check only"),
    ("only with a variant there is not", S + SET + "only: [large]\n", "check only"),
]


# --- the launcher ---------------------------------------------------------------------------------------------


def check(*arguments: str) -> dict:
    """The report of `vehicle_sil_launch check <arguments> --json`."""
    proc = subprocess.run([LAUNCH, "check", *arguments, "--json"], capture_output=True, text=True, encoding="utf-8",
                          stdin=subprocess.DEVNULL, timeout=120)
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        raise AssertionError(f"vehicle_sil_launch check {' '.join(arguments)} --json wrote no JSON (exit code "
                             f"{proc.returncode}):\n{proc.stdout[:500]}\n{proc.stderr[:500]}") from None


def errors_of(report: dict) -> list[dict]:
    return [f for f in report["findings"] if f["severity"] == "error"]


def listed(text: str) -> set[str]:
    """'a, b or c' and 'a, b, c' as a set."""
    return {word for word in re.split(r", | or ", text) if word}


class Launcher:
    """Files in a directory of their own, and what `check` says of them."""

    def __init__(self, directory: Path):
        self.directory = directory
        self.count = 0
        self.scenario_configuration: Path | None = None
        # The points file the samples name (points: { file: points.csv }), beside the scenarios.
        (directory / "points.csv").write_text("time,throttle,connected\n0,0,false\n100,50,true\n", encoding="utf-8", newline="\n")

    def write(self, text: str) -> Path:
        self.count += 1
        path = self.directory / f"file{self.count}.yaml"
        path.write_text(text.replace("@EXE@", LAUNCH), encoding="utf-8", newline="\n")
        return path

    def configuration(self, text: str) -> list[dict]:
        # A file with variants is checked as one of them (without --variant, check says only that).
        variants = load(text).get("variants")
        names = [name for name in variants if not name.startswith("-")] if isinstance(variants, dict) else []
        return errors_of(check(str(self.write(text)), *(["--variant", names[0]] if names else [])))

    def scenario(self, text: str) -> list[dict]:
        if not EXAMPLE_DIR or not (Path(EXAMPLE_DIR) / "simulation.yaml").is_file():
            raise AssertionError("VEHICLE_SIL_EXAMPLE_DIR must be the directory of the built examples/ecu_main "
                                 f"(simulation.yaml next to its ECUs), not {EXAMPLE_DIR!r}")
        if self.scenario_configuration is None:
            self.scenario_configuration = self.write(SCENARIO_CONFIG.replace("@DIR@", str(Path(EXAMPLE_DIR).resolve())))
            report = check(str(self.scenario_configuration), "--variant", "small")
            if errors_of(report):
                raise AssertionError(f"the configuration the scenarios are checked with has errors: {errors_of(report)}")
        report = check(str(self.scenario_configuration), "--variant", "small", "--scenario", str(self.write(text)))
        return errors_of(report)

    def keys_at(self, errors: list[dict]) -> set[str]:
        """The keys the launcher lists in the error of the unknown key zz_probe."""
        messages = [e["message"] for e in errors if e["code"] == "unknown-key" and "'zz_probe'" in e["message"]]
        if len(messages) != 1:
            raise AssertionError(f"expected one unknown-key error about zz_probe, got {[e['message'] for e in errors]}")
        match = re.search(r"the keys here \((.*)\)$|\(the keys here: (.*)\)$", messages[0])
        if not match:
            raise AssertionError(f"the message of unknown-key does not list the keys any more: {messages[0]}")
        return listed(match.group(1) or match.group(2))


# --- tests ----------------------------------------------------------------------------------------------------


class SchemaFiles(unittest.TestCase):
    maxDiff = None

    def test_the_schemas_are_json_schemas(self):
        for schema in (SIMULATION, SCENARIO):
            jsonschema.Draft7Validator.check_schema(schema)
            self.assertEqual(schema["$schema"], "http://json-schema.org/draft-07/schema#")

            def walk(node):
                if isinstance(node, dict):
                    if "$ref" in node:
                        resolve(schema, node)  # KeyError if it leads nowhere
                    for value in node.values():
                        walk(value)
                elif isinstance(node, list):
                    for value in node:
                        walk(value)
            walk(schema)

    def test_every_mapping_refuses_unknown_keys_and_describes_its_keys(self):
        for name, mappings in (("simulation", simulation_mappings()), ("scenario", scenario_mappings())):
            schema = SIMULATION if name == "simulation" else SCENARIO
            for where, mapping in mappings.items():
                if where != "step":
                    self.assertIs(mapping.get("additionalProperties"), False, f"{name}: {where}")
                for key, value in mapping["properties"].items():
                    described = value.get("description") or resolve(schema, value).get("description")
                    self.assertTrue(described, f"{name}.schema.json: {where}: {key} has no description (the text an "
                                               "editor shows for the key)")
        for kind, step in step_kinds().items():
            self.assertIs(step.get("additionalProperties"), False, kind)
            self.assertIn(kind, step["required"], f"step_{kind} must require the key {kind}")
            self.assertLessEqual({"at_ms", "after_ms"}, set(step["properties"]), f"step_{kind} has no time")

    def test_the_examples_are_valid(self):
        seen = {schema: 0 for schema in KINDS.values()}
        for path in yaml_files():
            data = load(path.read_text(encoding="utf-8"))
            kind = kind_of(data)
            if kind is None or kind == "dashboard.schema.json":  # the dashboard's schema is the screen's to test
                continue
            seen[kind] += 1
            schema = SIMULATION if kind == "simulation.schema.json" else SCENARIO
            self.assertEqual(problems(schema, data), [],
                             f"{path.relative_to(ROOT)} is not valid for schema/{kind}: if the launcher reads the "
                             "file, the schema is too strict; correct the schema")
        self.assertGreater(seen["simulation.schema.json"], 3)
        self.assertGreater(seen["scenario.schema.json"], 3)

    def test_the_yaml_of_the_documents_is_valid(self):
        seen = {"simulation": 0, "scenario": 0}
        found = set()
        for path in documents():
            name = path.relative_to(ROOT).as_posix()
            for line, before, text in yaml_blocks(path):
                try:
                    data = load(text)
                except yaml.YAMLError:
                    continue  # a sketch with "..." in it
                if not isinstance(data, dict):
                    continue
                whole = kind_of(data)
                if whole == "simulation.schema.json" or (whole is None and set(data) <= keys_of(SIMULATION)
                                                          and {"participants", "networks", "variants"} & set(data)):
                    kind, schema = "simulation", SIMULATION
                elif whole == "scenario.schema.json" or (whole is None and set(data) <= keys_of(SCENARIO)
                                                         and "steps" in data):
                    kind, schema = "scenario", SCENARIO
                else:
                    continue
                seen[kind] += 1
                said = problems(schema if whole else without_required(schema), data)
                expected = MISTAKES_IN_DOCUMENTS.get(f"{name}:{before}")
                if expected is None:
                    self.assertEqual(said, [], f"{name}:{line}: the YAML block is not valid for schema/{kind}.schema."
                                               "json: correct the block, or the schema if the launcher reads it")
                else:
                    found.add(f"{name}:{before}")
                    for start in expected:
                        self.assertTrue(any(s.startswith(start) for s in said),
                                        f"{name}:{line}: the schema should find the mistake at {start!r}; it says {said}")
        self.assertGreater(seen["simulation"], 3)
        self.assertGreater(seen["scenario"], 3)
        self.assertEqual(found, set(MISTAKES_IN_DOCUMENTS), "a block of MISTAKES_IN_DOCUMENTS is not in the documents "
                                                             "any more: correct the list")

    def test_the_samples_are_valid_or_not_as_they_say(self):
        for schema, samples in ((SIMULATION, CONFIG_SAMPLES), (SCENARIO, SCENARIO_SAMPLES)):
            for what, text, verdict in samples:
                said = problems(schema, load(text))
                if verdict == "invalid":
                    self.assertNotEqual(said, [], f"{what}: the schema should refuse\n{text}")
                else:
                    self.assertEqual(said, [], f"{what}: the schema should accept\n{text}")

    def test_every_key_is_in_its_document(self):
        def code_spans(document: str) -> set[str]:
            text = (ROOT / "docs" / document).read_text(encoding="utf-8")
            return set(re.findall(r"`([^`\n]+)`", text))

        spans = code_spans("simulation-config.md")
        words = {word for span in spans for word in re.findall(r"[A-Za-z_]+", span)}
        mappings = simulation_mappings()
        for key in keys_of(mappings["top"]):
            self.assertIn(key, spans, f"docs/simulation-config.md has no `{key}` (the table of the keys)")
        for key in keys_of(mappings["network"]):
            self.assertIn(f"networks[].{key}", spans, "docs/simulation-config.md: the table of the keys")
        for key in keys_of(mappings["participant"]):
            self.assertIn(f"participants[].{key}", spans, "docs/simulation-config.md: the table of the keys")
        for key in keys_of(mappings["variant participant"]):
            self.assertIn(f"…<participant>.{key}", spans, "docs/simulation-config.md: the table of the variants")
        for key in keys_of(mappings["variant"]):
            self.assertTrue(any(s.startswith("variants.<") and s.endswith(f".{key}") or
                                s.startswith("variants.<") and f".{key}." in s for s in spans),
                            f"docs/simulation-config.md has no `variants.<機種>.{key}`")
        for key in keys_of(mappings["variable"]):
            self.assertIn(key, words, f"docs/simulation-config.md does not name the key {key} of a variable mapping")

        spans = code_spans("scenario.md")
        words = {word for span in spans for word in re.findall(r"[A-Za-z_]+", span)}
        for where, mapping in scenario_mappings().items():
            for key in keys_of(mapping):
                if where in ("top", "step"):
                    self.assertTrue(any(s == key or s.startswith(key + ":") for s in spans),
                                    f"docs/scenario.md has no `{key}` (the tables of ファイルの形式)")
                else:
                    self.assertIn(key, words, f"docs/scenario.md does not name the key {key} ({where})")

    def test_the_copies_of_the_template_are_the_schemas(self):
        for copy_path, source in TEMPLATE_COPIES.items():
            self.assertTrue((ROOT / copy_path).is_file(), f"{copy_path} is missing: copy {source} there")
            self.assertEqual((ROOT / copy_path).read_bytes(), (ROOT / source).read_bytes(),
                             f"{copy_path} is not {source} any more: copy it again (cp {source} {copy_path})")
        extra = {p.name for p in (ROOT / "template" / "schema").iterdir()} - {Path(c).name for c in TEMPLATE_COPIES}
        self.assertEqual(extra, set(), "template/schema has files that are not copies of a schema")

    def test_the_first_line_of_each_example_names_its_schema(self):
        seen = 0
        for path in yaml_files():
            relative = path.relative_to(ROOT).as_posix()
            if relative.startswith("gui/server/tests/"):
                continue  # what a test gives the launcher, not an example to write from
            text = path.read_text(encoding="utf-8")
            kind = kind_of(load(text))
            if kind is None:
                continue
            seen += 1
            in_template = relative.startswith("template/")
            target = (ROOT / "template" / "schema" / kind if in_template
                      else ROOT / "gui" / "protocol" / kind if kind == "dashboard.schema.json" else SCHEMA_DIR / kind)
            wanted = Path(os.path.relpath(target, path.parent)).as_posix()
            first = text.split("\n", 1)[0].rstrip("\r")
            match = MODELINE.match(first)
            self.assertTrue(match and match.group(1) == wanted,
                            f"{relative}: the first line must be '# yaml-language-server: $schema={wanted}' (editors "
                            "complete and check the file with it); it is " + repr(first))
        self.assertGreater(seen, 6)


@needs_launcher
class SchemaAndLauncher(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        if not Path(LAUNCH).is_file():
            raise AssertionError(f"VEHICLE_SIL_LAUNCH_EXE is {LAUNCH!r}, which is not a file: build vehicle_sil_launch")
        cls.temporary = tempfile.TemporaryDirectory(prefix="vehicle-sil-schema-")
        cls.launcher = Launcher(Path(cls.temporary.name))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def same_keys(self, where: str, schema_keys: set[str], launcher_keys: set[str], file: str):
        self.assertEqual(schema_keys, launcher_keys,
                         f"{where}: schema/{file} and the launcher know different keys (only in the schema: "
                         f"{sorted(schema_keys - launcher_keys)}; only in the launcher: "
                         f"{sorted(launcher_keys - schema_keys)}): add or remove them in the schema, with a description")

    def test_the_keys_of_the_configuration_are_the_launchers(self):
        probes = {
            "top": CONFIG_HEAD + P + "zz_probe: 1\n",
            "network": "vehicle_sil_simulation: 1\nnetworks:\n  - { name: Body, kind: can, zz_probe: 1 }\n" + P,
            "participant": CONFIG_HEAD + P + "    zz_probe: 1\n",
            "variable": CONFIG_HEAD + P + "    variables: { a.b: { from: x, zz_probe: 1 } }\n",
            "variant": CONFIG_HEAD + P + "variants: { small: { zz_probe: 1 } }\n",
            "variant participant": CONFIG_HEAD + P + "variants: { small: { participants: { Tool: { zz_probe: 1 } } } }\n",
        }
        mappings = simulation_mappings()
        self.assertEqual(set(probes), set(mappings))
        for where, text in probes.items():
            self.same_keys(where, keys_of(mappings[where]), self.launcher.keys_at(self.launcher.configuration(text)),
                           "simulation.schema.json")

    def test_the_keys_of_a_scenario_are_the_launchers(self):
        probes = {
            "top": S + SET + "zz_probe: 1\n",
            "step": S + SET + "    zz_probe: 1\n",
            "comparison": S + "  - expect: { Engine.engine.speed: { ge: 0, zz_probe: 1 } }\n",
            "ramp": S + "  - ramp: { Engine.engine.throttle: { to: 5, over_ms: 10, zz_probe: 1 } }\n",
            "wave": S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 1, period_ms: 100 }, for_ms: 100, zz_probe: 1 } }\n",
            **{f"wave {shape}": S + "  - wave: { Engine.engine.throttle: { " + shape + ": { " + keys + ", zz_probe: 1 }, for_ms: 100 } }\n"
               for shape, keys in WAVE_SHAPES_WITH_KEYS.items()},
            "wave points file": S + "  - wave: { Engine.engine.throttle: { points: { file: points.csv, value: throttle, zz_probe: 1 } } }\n",
        }
        mappings = scenario_mappings()
        self.assertEqual(set(probes), set(mappings))
        for where, text in probes.items():
            self.same_keys(where, keys_of(mappings[where]), self.launcher.keys_at(self.launcher.scenario(text)),
                           "scenario.schema.json")

    def test_the_kinds_of_step_are_the_launchers(self):
        errors = self.launcher.scenario(S + "  - at_ms: 0\n")
        said = [re.search(r"give it one of (.*)$", e["message"]) for e in errors]
        said = [m.group(1) for m in said if m]
        self.assertEqual(len(said), 1, f"the launcher does not list the kinds of step any more: {errors}")
        self.same_keys("the kinds of step (definitions.step.oneOf)", set(step_kinds()), listed(said[0]),
                       "scenario.schema.json")

    def test_the_kinds_of_network_are_the_launchers(self):
        errors = self.launcher.configuration(CONFIG_HEAD.replace("can", "zz") + P)
        said = [re.search(r"kind is 'zz': write (.*) \(the kinds of network there are\)$", e["message"]) for e in errors]
        said = [m.group(1) for m in said if m]
        self.assertEqual(len(said), 1, f"the launcher does not list the kinds of network any more: {errors}")
        kinds = SIMULATION["properties"]["networks"]["items"]["properties"]["kind"]["enum"]
        self.same_keys("networks[].kind", set(kinds), listed(said[0]), "simulation.schema.json")

    def test_the_roles_of_a_participant_are_the_launchers(self):
        errors = self.launcher.configuration(CONFIG_HEAD + P + "    role: zz\n")
        said = [re.search(r"role is 'zz': write (.*?) \(", e["message"]) for e in errors]
        said = [m.group(1) for m in said if m]
        self.assertEqual(len(said), 1, f"the launcher does not list the roles any more: {errors}")
        roles = SIMULATION["properties"]["participants"]["items"]["properties"]["role"]["enum"]
        self.same_keys("participants[].role", set(roles), listed(said[0]), "simulation.schema.json")

    def test_the_launcher_reads_what_the_schema_accepts_and_refuses_what_it_refuses(self):
        for name, samples, ask in (("configuration", CONFIG_SAMPLES, self.launcher.configuration),
                                   ("scenario", SCENARIO_SAMPLES, self.launcher.scenario)):
            for what, text, verdict in samples:
                errors = [f"{e['code']}: {e['message']}" for e in ask(text)]
                if verdict == "valid":
                    self.assertEqual(errors, [], f"{name} sample '{what}': the schema accepts it, the launcher does "
                                                 f"not; correct the sample or the schema\n{text}")
                else:
                    self.assertNotEqual(errors, [],
                                        f"{name} sample '{what}' ({verdict}): the launcher reads it without an error; "
                                        + ("the schema refuses a file that is right: loosen the schema"
                                           if verdict == "invalid" else "it is not a mistake any more: remove the sample")
                                        + f"\n{text}")



@unittest.skipUnless(EXAMPLE_DIR, "VEHICLE_SIL_EXAMPLE_DIR is not set: the copies of the examples in the build "
                                  "directory are left out (CTest runs it)")
class SchemaOfTheBuiltExamples(unittest.TestCase):
    """CMake copies the YAML files of the examples next to their executables, with the first line as it is."""

    def test_the_first_line_of_each_copy_in_the_build_directory_leads_to_its_schema(self):
        built = Path(EXAMPLE_DIR).resolve().parent  # <build>/examples
        sources = {"simulation.schema.json": SCHEMA_DIR, "scenario.schema.json": SCHEMA_DIR,
                   "dashboard.schema.json": ROOT / "gui" / "protocol"}
        seen = 0
        for path in sorted(built.rglob("*.yaml")):
            match = MODELINE.match(path.read_text(encoding="utf-8").split("\n", 1)[0].rstrip("\r"))
            if not match:
                continue
            seen += 1
            target = path.parent / match.group(1)
            where = f"{path}: the first line names {match.group(1)}"
            self.assertTrue(target.is_file(),
                            f"{where}, and there is no file {target.resolve()}: an editor cannot complete and check "
                            "this copy; copy the schema there at configure time (the foreach of schemas after the "
                            "examples in the top CMakeLists.txt)")
            self.assertIn(target.name, sources, f"{where}, which is not a schema of this repository")
            self.assertEqual(target.read_bytes(), (sources[target.name] / target.name).read_bytes(),
                             f"{where}, and {target.resolve()} is not {sources[target.name] / target.name} any more: "
                             "configure again (cmake --preset <preset>)")
        self.assertGreater(seen, 3, f"{built} has no copies of the examples' YAML files that name a schema: is "
                                    "VEHICLE_SIL_EXAMPLE_DIR the directory of the built examples/ecu_main?")


if __name__ == "__main__":
    unittest.main(verbosity=2)
