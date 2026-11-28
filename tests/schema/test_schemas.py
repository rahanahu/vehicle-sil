# SPDX-License-Identifier: MIT
"""Tests of the JSON Schemas of the simulation configuration and of a scenario (schema/): that they say what the
launcher's readers say (libs/config/src/config.cpp, libs/scenario/src/scenario.cpp), no more.

    python tests/schema/test_schemas.py          (exit code 0 when every test passes)
    python tests/schema/test_schemas.py --sync   (sync shared scenario definitions and template/schema copies)

Without the launcher (the files only):
  - the schemas are JSON Schemas, and every $ref in them leads somewhere;
  - every configuration and scenario of examples/, template/ and the GUI server's tests, and every one a document
    shows (docs/, the READMEs; the English and the Japanese pages), is valid; the samples below are valid or not as
    they say;
  - every key of the schemas is in the document of its file (docs/simulation-config.md, docs/scenario.md, and their
    Japanese pages in docs/ja/);
  - the copies in template/schema/ are the files they are copies of, and the first line of each YAML file of examples/
    and template/ names the schema of its kind (# yaml-language-server: $schema=<relative path>);
  - the timeline files of tests/launch/timeline/ (what the scenario's engine writes for `run --timeline`) are valid
    for schema/timeline.schema.json, every key of that schema is described, and files with a mistake are not valid;
  - the timeline files that the tests of the timeline page use (gui/timeline/tests/fixtures/) are valid too, so that
    the page is not tested on a shape the launcher cannot write.

With the built examples (VEHICLE_SIL_EXAMPLE_DIR):
  - the first line of each copy of an example's YAML file in the build directory leads to a copy of its schema.

With the launcher (VEHICLE_SIL_LAUNCH_EXE, and VEHICLE_SIL_EXAMPLE_DIR with the built examples/ecu_main; CTest sets
both, as schema.files_and_launcher):
  - the keys of every mapping are the ones the launcher knows: an unknown key is given to `check` at each place, and
    its message lists the keys there;
  - the kinds of network and the kinds of step are the launcher's;
  - `check` passes every sample the schema passes (unless the sample says the schema cannot tell), and fails every
    sample the schema fails: the schema never refuses a file that the launcher reads;
  - the timeline file of a run of the example (`run --scenario --timeline`) is valid for schema/timeline.schema.json.

The request file (schema/run-request.schema.json; run --request, check --request) the same way: the schema's keys
are described, in docs/simulation-run.md and the ones the launcher knows; the request files of examples/ and the
JSON blocks of the documents are valid; the launcher reads the samples the schema accepts and refuses the others.

Needs PyYAML and jsonschema (the `schema` dependency group of pyproject.toml).
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import copy
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

import jsonschema
import yaml

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_DIR = ROOT / "schema"
SIMULATION = json.loads((SCHEMA_DIR / "simulation.schema.json").read_text(encoding="utf-8"))
SCENARIO = json.loads((SCHEMA_DIR / "scenario.schema.json").read_text(encoding="utf-8"))
SCENARIO_TEMPLATE = json.loads((SCHEMA_DIR / "scenario-template.schema.json").read_text(encoding="utf-8"))
TIMELINE = json.loads((SCHEMA_DIR / "timeline.schema.json").read_text(encoding="utf-8"))
RUN_REQUEST = json.loads((SCHEMA_DIR / "run-request.schema.json").read_text(encoding="utf-8"))
CONTROL_METRICS = json.loads((SCHEMA_DIR / "control-metrics.schema.json").read_text(encoding="utf-8"))
BENCHMARK_REPORT = json.loads((SCHEMA_DIR / "benchmark-report.schema.json").read_text(encoding="utf-8"))
TIMELINE_REFERENCES = ROOT / "tests" / "launch" / "timeline"
TIMELINE_PAGE_FIXTURES = ROOT / "gui" / "timeline" / "tests" / "fixtures"
MODELINE = re.compile(r"^# yaml-language-server: \$schema=(\S+)$")
LAUNCH = os.environ.get("VEHICLE_SIL_LAUNCH_EXE", "")
EXAMPLE_DIR = os.environ.get("VEHICLE_SIL_EXAMPLE_DIR", "")
needs_launcher = unittest.skipUnless(
    LAUNCH, "VEHICLE_SIL_LAUNCH_EXE is not set: the comparison with the launcher is left out (CTest runs it)")

# The copies a project made from the template has next to its YAML files.
TEMPLATE_COPIES = {
    "template/schema/simulation.schema.json": "schema/simulation.schema.json",
    "template/schema/scenario.schema.json": "schema/scenario.schema.json",
    "template/schema/scenario-template.schema.json": "schema/scenario-template.schema.json",
    "template/schema/run-request.schema.json": "schema/run-request.schema.json",
    "template/schema/dashboard.schema.json": "gui/protocol/dashboard.schema.json",
}
# Definitions in scenario-template.schema.json that intentionally keep template-specific wording/patterns
# (templates allow relative '..' segments that do not escape the root scenario directory).
TEMPLATE_SPECIFIC_OVERRIDES = (
    ("template_path", "pattern"),
    ("template_path", "description"),
    ("step_template", "properties", "template", "description"),
    ("points_file", "properties", "file", "pattern"),
    ("points_file", "properties", "file", "description"),
)


def _get_nested(mapping: dict, path: tuple[str, ...]):
    cur = mapping
    for part in path:
        cur = cur[part]
    return cur


def _set_nested(mapping: dict, path: tuple[str, ...], value) -> None:
    cur = mapping
    for part in path[:-1]:
        cur = cur[part]
    cur[path[-1]] = value


def sync_scenario_template_schema(scenario_schema: dict, template_schema: dict) -> dict:
    """Return a copy of `template_schema` with every shared definition synchronized from `scenario_schema`,
    preserving only `parameter_def` and the intentional template-specific path overrides."""
    out = copy.deepcopy(template_schema)
    old_defs = template_schema["definitions"]
    new_defs: dict = {}
    for key, value in scenario_schema["definitions"].items():
        new_defs[key] = copy.deepcopy(value)
    for path in TEMPLATE_SPECIFIC_OVERRIDES:
        _set_nested(new_defs, path, copy.deepcopy(_get_nested(old_defs, path)))
    if "parameter_def" in old_defs:
        new_defs["parameter_def"] = copy.deepcopy(old_defs["parameter_def"])
    out["definitions"] = new_defs
    return out


def sync_schemas(root: Path = ROOT) -> list[str]:
    """Synchronize `schema/scenario-template.schema.json` from `schema/scenario.schema.json` and update every
    copy listed in `TEMPLATE_COPIES`. Returns the relative POSIX paths of modified files."""
    updated: list[str] = []
    scenario_path = root / "schema" / "scenario.schema.json"
    template_path = root / "schema" / "scenario-template.schema.json"
    scenario_data = json.loads(scenario_path.read_text(encoding="utf-8"))
    template_data = json.loads(template_path.read_text(encoding="utf-8"))
    synced_template = sync_scenario_template_schema(scenario_data, template_data)
    rendered = json.dumps(synced_template, indent=2, ensure_ascii=False) + "\n"
    if template_path.read_text(encoding="utf-8") != rendered:
        template_path.write_text(rendered, encoding="utf-8", newline="\n")
        updated.append("schema/scenario-template.schema.json")
    for copy_rel, source_rel in TEMPLATE_COPIES.items():
        source_path = root / source_rel
        copy_path = root / copy_rel
        source_bytes = source_path.read_bytes()
        if not copy_path.is_file() or copy_path.read_bytes() != source_bytes:
            copy_path.parent.mkdir(parents=True, exist_ok=True)
            copy_path.write_bytes(source_bytes)
            updated.append(copy_rel)
    return updated
# The schema of each kind of file, by the key that says the format version.
KINDS = {
    "vehicle_sil_simulation": "simulation.schema.json",
    "vehicle_sil_scenario": "scenario.schema.json",
    "vehicle_sil_scenario_template": "scenario-template.schema.json",
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
        "j1939 node": definitions["j1939_nodes"]["items"],
        "can external": definitions["external_can"],
        "ethernet external": definitions["external_ethernet"],
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
        "case": SCENARIO["properties"]["cases"]["additionalProperties"],
        "step": step,
        "comparison": comparison,
        "variable reference": definitions["variable_reference"],
        "ramp": definitions["step_ramp"]["properties"]["ramp"]["additionalProperties"],
        "wave": definitions["wave"],
        **{f"wave {shape}": definitions["wave"]["properties"][shape] for shape in WAVE_SHAPES_WITH_KEYS},
        "wave points file": definitions["points_file"],
        "input": SCENARIO["properties"]["inputs"]["additionalProperties"],
        "dither": definitions["overlay"],
    }


def template_mappings() -> dict[str, dict]:
    definitions = SCENARIO_TEMPLATE["definitions"]
    return {
        "template top": SCENARIO_TEMPLATE,
        "template parameter": definitions["parameter_def"],
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
    """Every page of the manual, English and Japanese (docs/ja/, README.ja.md)."""
    return sorted([*(ROOT / "docs").rglob("*.md"), *ROOT.glob("README*.md"), *(ROOT / "template").glob("README*.md"),
                   *(ROOT / "examples").rglob("README*.md")])


def english_name(path: Path) -> str:
    """The path of a page relative to ROOT, the Japanese page named as its English page."""
    name = path.relative_to(ROOT).as_posix()
    if name.startswith("docs/ja/"):
        return "docs/" + name[len("docs/ja/"):]
    return name[:-len("README.ja.md")] + "README.md" if name.endswith("README.ja.md") else name


def pages(document: str) -> list[Path]:
    """The English page docs/<document> and its Japanese page, those that exist."""
    return [p for p in (ROOT / "docs" / document, ROOT / "docs" / "ja" / document) if p.is_file()]


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
# The key is "<document>:<the line before the block>", the document named as its English page.
MISTAKES_IN_DOCUMENTS = {
    "docs/simulation-check.md:<!-- two-engines.yaml -->": ["participants/2: "],  # a key written wrongly
    "docs/scenario.md:<!-- throttle_mistakes.yaml -->": ["steps/4: "],  # near without tolerance
}


# --- samples --------------------------------------------------------------------------------------------------

# @EXE@ is an executable that exists (the launcher itself; such a participant is not asked what it is).
CONFIG_HEAD = "vehicle_sil_simulation: 2\nnetworks:\n  - { name: Body, kind: can }\n"
P = "participants:\n  - name: Tool\n    command: ['@EXE@']\n    describe: false\n"

# (what it shows, the text, "valid" | "invalid" | "check only": the launcher refuses it, the schema cannot tell)
CONFIG_SAMPLES = [
    ("the least", CONFIG_HEAD + P, "valid"),
    ("JSON", '{"vehicle_sil_simulation": 2, "networks": [], "participants": [{"name": "Tool", "command": ["@EXE@"], '
             '"describe": false}]}', "valid"),
    ("every key at the top",
     CONFIG_HEAD + P + "registry_uri: silkit://127.0.0.1:8500\ntime_step_ms: 10\n"
     "params: { idle_rpm: 800 }\nvariants: { demo: {} }\n", "valid"),
    ("capital booleans",
     CONFIG_HEAD + "participants:\n  - name: Tool\n    command: '@EXE@'\n"
     "    describe: False\n    required: TRUE\n    enabled: True\n    manual: false\n", "valid"),
    ("more milliseconds than there are", CONFIG_HEAD + P + "time_step_ms: 1000000000001\n", "invalid"),
    ("names that YAML reads as numbers and booleans",
     "vehicle_sil_simulation: 2\nnetworks:\n  - { name: 7, kind: lin }\n  - { name: true, kind: ethernet }\n"
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
    ("an unknown key of a network", "vehicle_sil_simulation: 2\nnetworks:\n  - { name: Body, kind: can, baud: 1 }\n" + P,
     "invalid"),
    ("an unknown key of a participant", CONFIG_HEAD + P + "    step: 5\n", "invalid"),
    ("an unknown key of a variable mapping", CONFIG_HEAD + P + "    variables: { a.b: { from: x, factor: 2 } }\n", "invalid"),
    ("an unknown key of a variant", CONFIG_HEAD + P + "variants: { small: { networks: [] } }\n", "invalid"),
    ("an unknown key of a variant's participant",
     CONFIG_HEAD + P + "variants: { small: { participants: { Tool: { step_ms: 5 } } } }\n", "invalid"),
    ("no format version", "networks: []\n" + P, "invalid"),
    ("another format version", CONFIG_HEAD.replace(": 2", ": 1") + P, "invalid"),
    ("no networks", "vehicle_sil_simulation: 2\n" + P, "invalid"),
    ("no participants", CONFIG_HEAD, "invalid"),
    ("an empty participants", CONFIG_HEAD + "participants: []\n", "invalid"),
    ("a participant without a name", CONFIG_HEAD + "participants:\n  - command: ['@EXE@']\n    describe: false\n", "invalid"),
    ("a participant without a command", CONFIG_HEAD + "participants:\n  - name: Tool\n    describe: false\n", "invalid"),
    ("a manual describe: false participant without a command",
     CONFIG_HEAD + "participants:\n  - name: RemotePeer\n    describe: false\n    manual: true\n", "valid"),
    ("a manual describe: false participant with a non-existent remote command",
     CONFIG_HEAD + "participants:\n  - name: RemotePeer\n    command: [remote_only_peer, --uri, \"{registry_uri}\"]\n"
     "    describe: false\n    manual: true\n", "valid"),
    ("a manual describe: false participant with an empty command list",
     CONFIG_HEAD + "participants:\n  - name: RemotePeer\n    command: []\n    describe: false\n    manual: true\n", "invalid"),
    ("a manual describe: false participant with an empty command string",
     CONFIG_HEAD + "participants:\n  - name: RemotePeer\n    command: \"\"\n    describe: false\n    manual: true\n", "invalid"),
    ("a manual describe: true participant without a command",
     CONFIG_HEAD + "participants:\n  - name: RemotePeer\n    describe: true\n    manual: true\n", "invalid"),
    ("a manual participant (default describe) without a command",
     CONFIG_HEAD + "participants:\n  - name: RemotePeer\n    manual: true\n", "invalid"),
    ("a manual: false describe: false participant without a command",
     CONFIG_HEAD + "participants:\n  - name: RemotePeer\n    describe: false\n    manual: false\n", "invalid"),
    ("a manual participant with invalid manual and describe: false without a command",
     CONFIG_HEAD + "participants:\n  - name: RemotePeer\n    describe: false\n    manual: \"true\"\n", "invalid"),
    ("a manual: true participant with invalid describe without a command",
     CONFIG_HEAD + "participants:\n  - name: RemotePeer\n    describe: \"false\"\n    manual: true\n", "invalid"),
    ("a network without a kind", "vehicle_sil_simulation: 2\nnetworks:\n  - name: Body\n" + P, "invalid"),
    ("a kind there is not", CONFIG_HEAD.replace("can", "flexray") + P, "invalid"),
    ("a kind in capitals", CONFIG_HEAD.replace("can", "CAN") + P, "invalid"),
    ("a participant for testing", CONFIG_HEAD + P + "    role: test\n", "valid"),
    ("a bridge", CONFIG_HEAD + P + "    role: bridge\n", "valid"),
    ("a role there is not", CONFIG_HEAD + P + "    role: spy\n", "invalid"),
    ("a role in capitals", CONFIG_HEAD + P + "    role: Test\n", "invalid"),
    ("a role in a variant", CONFIG_HEAD + P + "variants: { small: { participants: { Tool: { role: test } } } }\n",
     "invalid"),
    ("a number in quotes", CONFIG_HEAD + P + 'time_step_ms: "1000"\n', "invalid"),
    ("a number with a fraction", CONFIG_HEAD + P + "time_step_ms: 1.5\n", "invalid"),
    ("zero milliseconds", CONFIG_HEAD + P + "time_step_ms: 0\n", "invalid"),
    ("a boolean in quotes", CONFIG_HEAD + P + '    manual: "true"\n', "invalid"),
    ("yes for a boolean", CONFIG_HEAD + P + "    manual: yes\n", "invalid"),
    ("a speed of 0", CONFIG_HEAD + P + "speed: 0\n", "invalid"),
    ("a speed that is a word", CONFIG_HEAD + P + "speed: fast\n", "invalid"),
    ("a registry without a port", CONFIG_HEAD + P + "registry_uri: silkit://localhost\n", "invalid"),
    ("networks as a mapping", "vehicle_sil_simulation: 2\nnetworks: { Body: can }\n" + P, "invalid"),
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
    ("a key twice", CONFIG_HEAD + P + "time_step_ms: 10\ntime_step_ms: 20\n", "check only"),
    ("a name twice", CONFIG_HEAD + P + P.split("\n", 1)[1], "check only"),
    ("an executable that is not there", CONFIG_HEAD + P.replace("@EXE@", "no/such/executable"), "check only"),
    ("required: false with describe: true", CONFIG_HEAD + P.replace("describe: false", "required: false"), "check only"),
    ("a port out of range", CONFIG_HEAD + P + "registry_uri: silkit://127.0.0.1:70000\n", "check only"),
    ("a name that starts with -", CONFIG_HEAD + P.replace("Tool", "-Tool"), "check only"),
    ("a placeholder there is not", CONFIG_HEAD + P.replace("['@EXE@']", "['@EXE@', '{port}']"), "check only"),
    ("a variant of a participant there is not", CONFIG_HEAD + P + "variants: { small: { participants: { Door: {} } } }\n",
     "check only"),
    ("two names for one value", CONFIG_HEAD + P + "    variables: { a.b: { from: x, map: { P: 0, Q: 0 } } }\n",
     "check only"),
    ("network names, one that renames nothing and one that YAML reads as a number",
     CONFIG_HEAD + "  - { name: Other, kind: can }\n" + P + "    network_names: { Body: CAN_1, Other: 5, Third: Third }\n", "valid"),
    ("network names of a variant",
     CONFIG_HEAD + P + "variants: { small: { participants: { Tool: { network_names: { Body: CAN_1 } } } } }\n", "valid"),
    ("a key of network_names with =", CONFIG_HEAD + P + "    network_names: { \"a=b\": CAN_1 }\n", "invalid"),
    ("a key of network_names that starts with -", CONFIG_HEAD + P + "    network_names: { \"-v\": CAN_1 }\n", "invalid"),
    ("an empty name in network_names", CONFIG_HEAD + P + "    network_names: { Body: \"\" }\n", "invalid"),
    ("a list as the executable's name of a network", CONFIG_HEAD + P + "    network_names: { Body: [CAN_1] }\n", "invalid"),
    ("network_names that is a list", CONFIG_HEAD + P + "    network_names: [Body]\n", "invalid"),
    ("a key of network_names that is not in networks", CONFIG_HEAD + P + "    network_names: { Bodi: CAN_1 }\n", "check only"),
    ("one name of the executable under two keys of network_names",
     CONFIG_HEAD + "  - { name: Other, kind: can }\n" + P + "    network_names: { Body: CAN_1, Other: CAN_1 }\n", "check only"),
    ("j1939 nodes in hexadecimal and in decimal, the ends of the ranges, an entry that changes nothing",
     CONFIG_HEAD + P + "    j1939_nodes:\n      - { network: Body, from: 0x00, address: 0xFE, identity_number: 2097151 }\n"
     "      - { network: Body, from: 254, address: 0, identity_number: 0, function_instance: 31, ecu_instance: 7 }\n"
     "      - { network: Body, from: 0x1f, function_instance: 0, ecu_instance: 0 }\n"
     "      - { network: Body, from: 0x21 }\n", "valid"),
    ("external of each kind that can be connected, EtherTypes in each form, the ends of the ranges",
     "vehicle_sil_simulation: 2\nnetworks:\n  - { name: C, kind: can, external: { bitrate: 1000000 } }\n"
     "  - { name: D, kind: can, external: { bitrate: 1 } }\n  - { name: E, external: {}, kind: can }\n"
     "  - name: F\n    kind: ethernet\n    external: { ethertypes: [0x88b5, 2048, \"0x86DD\", 0, 0xFFFF, \"0x1\"] }\n"
     "  - { name: G, kind: ethernet, external: { all_frames: true } }\n"
     "  - { name: H, kind: ethernet, external: { all_frames: false, ethertypes: [0x0800] } }\n" + P, "valid"),
    ("external in JSON, with an EtherType in hexadecimal as a text",
     '{"vehicle_sil_simulation": 2, "networks": [{"name": "E", "kind": "ethernet", "external": {"ethertypes": '
     '["0x88b5", 2048]}}], "participants": [{"name": "Tool", "command": "@EXE@", "describe": false}]}', "valid"),
    ("external of a lin network", "vehicle_sil_simulation: 2\nnetworks:\n  - { name: L, kind: lin, external: {} }\n" + P,
     "invalid"),
    ("an EtherType of a can network", CONFIG_HEAD.replace("can }", "can, external: { ethertypes: [0x88b5] } }") + P,
     "invalid"),
    ("a bit rate of an ethernet network", CONFIG_HEAD.replace("can }", "ethernet, external: { bitrate: 500000 } }") + P,
     "invalid"),
    ("where a network is connected, in the file",
     CONFIG_HEAD.replace("can }", "ethernet, external: { connect: eth0 } }") + P, "invalid"),
    ("a bit rate of 0", CONFIG_HEAD.replace("can }", "can, external: { bitrate: 0 } }") + P, "invalid"),
    ("a bit rate above the fastest CAN", CONFIG_HEAD.replace("can }", "can, external: { bitrate: 1000001 } }") + P,
     "invalid"),
    ("a bit rate in quotes", CONFIG_HEAD.replace("can }", "can, external: { bitrate: \"500000\" } }") + P, "invalid"),
    ("no EtherTypes", CONFIG_HEAD.replace("can }", "ethernet, external: { ethertypes: [] } }") + P, "invalid"),
    ("an EtherType above 0xFFFF", CONFIG_HEAD.replace("can }", "ethernet, external: { ethertypes: [0x10000] } }") + P,
     "invalid"),
    ("an EtherType in quotes above 0xFFFF",
     CONFIG_HEAD.replace("can }", "ethernet, external: { ethertypes: [\"0x10000\"] } }") + P, "invalid"),
    ("a decimal EtherType in quotes", CONFIG_HEAD.replace("can }", "ethernet, external: { ethertypes: [\"2048\"] } }") + P,
     "invalid"),
    ("an EtherType twice", CONFIG_HEAD.replace("can }", "ethernet, external: { ethertypes: [0x0800, 2048] } }") + P,
     "invalid"),
    ("all frames and EtherTypes",
     CONFIG_HEAD.replace("can }", "ethernet, external: { all_frames: true, ethertypes: [0x0800] } }") + P, "invalid"),
    ("external that is not a mapping", CONFIG_HEAD.replace("can }", "can, external: 500000 }") + P, "invalid"),
    ("j1939 nodes of a variant",
     CONFIG_HEAD + P + "variants: { small: { participants: { Tool: { j1939_nodes: [{ network: Body, address: 1 }] } } } }\n",
     "valid"),
    ("j1939 nodes that is a mapping", CONFIG_HEAD + P + "    j1939_nodes: { network: Body, address: 1 }\n", "invalid"),
    ("an entry of j1939_nodes without network", CONFIG_HEAD + P + "    j1939_nodes: [{ address: 1 }]\n", "invalid"),
    ("an address of 255", CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, address: 255 }]\n", "invalid"),
    ("an identity number above its range",
     CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, identity_number: 0x200000 }]\n", "invalid"),
    ("an ECU instance above its range", CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, ecu_instance: 8 }]\n", "invalid"),
    ("an address in quotes", CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, address: \"0x01\" }]\n", "invalid"),
    ("an address in hexadecimal with a capital X, which YAML reads as a text",
     CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, address: 0X01 }]\n", "invalid"),
    ("j1939 nodes with the other integers of YAML 1.2: a leading zero (ten), a sign, octal",
     CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, from: 010, address: +1, identity_number: 0o10 }]\n", "valid"),
    ("an address in binary, which YAML reads as a text",
     CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, address: 0b11 }]\n", "invalid"),
    ("an address with an underscore, which YAML reads as a text",
     CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, address: 1_0 }]\n", "invalid"),
    ("an address in hexadecimal with a sign, which YAML reads as a text",
     CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, address: +0x10 }]\n", "invalid"),
    ("a network of j1939_nodes with =", CONFIG_HEAD + P + "    j1939_nodes: [{ network: \"a=b\", address: 1 }]\n",
     "invalid"),
    ("a network of j1939_nodes that is not in networks",
     CONFIG_HEAD + P + "    j1939_nodes: [{ network: Bodi, address: 1 }]\n", "check only"),
    ("two entries of j1939_nodes for one node",
     CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, from: 0, address: 1 }, { network: Body, from: 0x00, address: 2 }]\n",
     "check only"),
]

# For the two ECUs of examples/ecu_main: Engine.engine.throttle (real input, 0 to 100), Engine.engine.speed (integer
# output), Engine.ecu.connected (bool input), Dashboard.dashboard.engine_lost (bool output). The launcher checks them
# with this configuration, as its variant small (@DIR@: the directory of the built example).
SCENARIO_CONFIG = (
    "vehicle_sil_simulation: 2\nnetworks:\n  - { name: J1939_CAN, kind: can }\nparticipants:\n"
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
    ("a dither in the inputs and in a step",
     "vehicle_sil_scenario: 1\ninputs:\n"
     "  Engine.engine.throttle: { dither: { triangle: { amplitude: 2, period_ms: 40, phase_deg: 90 } }, undersampled: ok }\n"
     "steps:\n" + SET +
     "  - after_ms: 10\n    dither: { Engine.engine.throttle: none }\n"
     "  - dither: { Engine.engine.throttle: { sine: { amplitude: 1, frequency_hz: 5 } } }\n"
     "  - dither: { Engine.engine.throttle: { sawtooth: { amplitude: 1, period_ms: 100, falling: true } } }\n"
     "  - dither: { Engine.engine.throttle: { square: { amplitude: 1, period_ms: 100, duty: 30 } } }\n"
     "  - at_ms: 500\n    dither: { Engine.engine.throttle: { noise: { amplitude: \"${a}\", hold_ms: 2, seed: 7, distribution: gaussian } } }\n"
     "    undersampled: ok\n"
     "params: { a: 1 }\n", "valid"),
    ("JSON with a dither",
     '{"vehicle_sil_scenario": 1, "inputs": {"Engine.engine.throttle": {"dither": {"triangle": {"amplitude": 2, "period_ms": 40}}}}, '
     '"steps": [{"set": {"Engine.engine.throttle": 50}}, {"dither": {"Engine.engine.throttle": "none"}}]}', "valid"),
    ("end_ms 0 and positive", "vehicle_sil_scenario: 1\nend_ms: 500\nsteps:\n" + SET, "valid"),
    ("end_ms negative", "vehicle_sil_scenario: 1\nend_ms: -1\nsteps: []\n", "invalid"),
    ("end_ms as a float", "vehicle_sil_scenario: 1\nend_ms: 1.5\nsteps: []\n", "invalid"),
    ("a dither with a length", S + "  - dither: { Engine.engine.throttle: { triangle: { amplitude: 1, period_ms: 100 }, for_ms: 500 } }\n",
     "invalid"),
    ("a dither with a middle", S + "  - dither: { Engine.engine.throttle: { sine: { amplitude: 1, period_ms: 100 }, around: 50 } }\n",
     "invalid"),
    ("a dither of a shape that has a length", S + "  - dither: { Engine.engine.throttle: { pulse: { value: 1, width_ms: 50 } } }\n",
     "invalid"),
    ("a dither that is a square with low and high",
     S + "  - dither: { Engine.engine.throttle: { square: { low: 0, high: 1, period_ms: 100 } } }\n", "invalid"),
    ("a dither with two shapes",
     S + "  - dither: { Engine.engine.throttle: { sine: { amplitude: 1, period_ms: 100 }, noise: { amplitude: 1, hold_ms: 10, seed: 1 } } }\n",
     "invalid"),
    ("a dither that is off", S + "  - dither: { Engine.engine.throttle: off }\n", "invalid"),
    ("a dither step with a hold", S + "  - dither: { Engine.engine.throttle: none }\n    hold_for_ms: 10\n", "invalid"),
    ("an input with a clip", "vehicle_sil_scenario: 1\ninputs: { Engine.engine.throttle: { dither: { triangle: { amplitude: 1, "
                             "period_ms: 100 } }, clip: true } }\nsteps:\n" + SET, "invalid"),
    ("an input without a dither", "vehicle_sil_scenario: 1\ninputs: { Engine.engine.throttle: { undersampled: ok } }\nsteps:\n" + SET,
     "invalid"),
    ("an input whose dither is none", "vehicle_sil_scenario: 1\ninputs: { Engine.engine.throttle: { dither: none } }\nsteps:\n" + SET,
     "invalid"),
    ("a dither of a bool", S + "  - dither: { Engine.ecu.connected: { square: { amplitude: 1, period_ms: 100 } } }\n", "check only"),
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
    ("samples as the ECUs send them", S + SET + "samples: as_sent\n", "valid"),
    ("samples of every step", S + SET + "samples: every_step\n", "valid"),
    ("samples with another word", S + SET + "samples: often\n", "invalid"),
    ("only that is a text", S + SET + "only: small\n", "invalid"),
    ("a parameter that uses a parameter", S + SET + "params: { a: \"${b}\" }\n", "invalid"),
    ("a parameter's name with a digit first", S + SET + "params: { 1st: 5 }\n", "invalid"),
    ("step name on every kind of step with interpolation",
     "vehicle_sil_scenario: 1\nparams: { p: 50, phase: 'init' }\nsteps:\n"
     "  - name: \"apply ${p}%\"\n    set: { Engine.engine.throttle: 50 }\n"
     "  - name: 1\n    ramp: { Engine.engine.throttle: { to: 80, over_ms: 100 } }\n"
     "  - name: \"${phase} wave\"\n    wave: { Engine.engine.throttle: { sine: { amplitude: 5, period_ms: 100 }, for_ms: 200 } }\n"
     "  - name: \"dither step\"\n    dither: { Engine.engine.throttle: { triangle: { amplitude: 1, period_ms: 100 } } }\n"
     "  - name: \"wait speed\"\n    wait_until: { Engine.engine.speed: { ge: 0 } }\n    timeout_ms: 100\n"
     "  - name: \"check speed\"\n    expect: { Engine.engine.speed: { ge: 0 } }\n", "valid"),
    ("step name that is empty", S + "  - name: \"\"\n" + SET.replace("  - ", "    "), "invalid"),
    ("step name that is spaces only", S + "  - name: \"   \"\n" + SET.replace("  - ", "    "), "invalid"),
    ("step name with a newline", S + "  - name: \"a\\nb\"\n" + SET.replace("  - ", "    "), "invalid"),
    ("step name with a trailing newline", S + "  - name: \"abc\\n\"\n" + SET.replace("  - ", "    "), "invalid"),
    ("step name as a block scalar with trailing newline", S + "  - name: |\n      hello\n" + SET.replace("  - ", "    "), "invalid"),
    ("step name with ideographic space only", S + "  - name: \"\u3000\"\n" + SET.replace("  - ", "    "), "invalid"),
    ("step name with non-breaking space only", S + "  - name: \"\u00a0\"\n" + SET.replace("  - ", "    "), "invalid"),
    ("step name with non-whitespace around ideographic space", S + "  - name: \"a\u3000b\"\n" + SET.replace("  - ", "    "), "valid"),
    ("step name that is null", S + "  - name: \n" + SET.replace("  - ", "    "), "invalid"),
    # What the schema leaves to check.
    ("step name interpolated to empty", S + "  - name: \"${empty}\"\n" + SET.replace("  - ", "    ") + "params: { empty: \"\" }\n", "check only"),
    ("step name with undefined parameter", S + "  - name: \"${missing}\"\n" + SET.replace("  - ", "    "), "check only"),
    ("step name with invalid parameter syntax", S + "  - name: \"bad ${1x}\"\n" + SET.replace("  - ", "    "), "check only"),
    ("step name with unclosed parameter", S + "  - name: \"unclosed ${x\"\n" + SET.replace("  - ", "    "), "check only"),
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
    ("a template call with params and name",
     S + "  - name: call noop\n    template: templates/noop.yaml\n    params: { ms: 20, label: \"go\" }\n", "valid"),
    ("a template call with ${...} in template path",
     S + "  - template: \"templates/${name}.yaml\"\n", "invalid"),
    ("a template call with .. in root scenario",
     S + "  - template: ../outside.yaml\n", "invalid"),
    ("a template call with params on a set step",
     S + SET + "    params: { ms: 20 }\n", "invalid"),
    ("a template file passed as scenario",
     "vehicle_sil_scenario_template: 1\nsteps:\n" + SET, "invalid"),
    ("named cases with params and only",
     "vehicle_sil_scenario: 1\nparams: { val: 10 }\ncases:\n"
     "  無負荷: { params: { val: 20 } }\n"
     "  定格: { only: [small], params: { val: 50 } }\n"
     "  \"-10C\": { params: { val: 30 } }\n"
     "steps:\n  - set: { Engine.engine.throttle: \"${val}\" }\n",
     "valid"),
    ("empty cases", S + SET + "cases: {}\n", "invalid"),
    ("case starting with --", S + SET + "cases: { \"--bad\": {} }\n", "invalid"),
    ("empty case name", S + SET + "cases: { \"\": {} }\n", "invalid"),
    ("case name with trailing newline", S + SET + "cases: { \"a\\n\": {} }\n", "invalid"),
    ("case name with control char", S + SET + "cases: { \"a\\nb\": {} }\n", "invalid"),
    ("case value null", S + SET + "cases: { a: }\n", "invalid"),
    ("unknown key in case", S + SET + "cases: { a: { steps: [] } }\n", "invalid"),
    ("case param referencing another param", S + SET + "cases: { a: { params: { x: \"${y}\" } } }\n", "invalid"),
    ("case only with variant not in config", S + SET + "cases: { a: { only: [large] } }\n", "check only"),
    ("empty intersection of scenario only and case only",
     S + SET + "only: [small]\ncases: { a: { only: [7] } }\n", "check only"),
    ("points file with double slash",
     S + "  - wave: { Engine.engine.throttle: { points: { file: traces//points.csv, value: throttle } } }\n", "invalid"),
    ("points file with trailing slash",
     S + "  - wave: { Engine.engine.throttle: { points: { file: traces/, value: throttle } } }\n", "invalid"),
    ("YAML 1.2 leading dot float", S + "  - set: { Engine.engine.throttle: .5 }\n", "check only"),
    ("YAML 1.2 plus float", S + "  - set: { Engine.engine.throttle: +1.5 }\n", "check only"),
    ("YAML 1.2 plus int", S + "  - set: { Engine.engine.throttle: +5 }\n", "check only"),
    ("YAML 1.2 octal int", S + "  - set: { Engine.engine.throttle: 0o7 }\n", "check only"),
    ("comparing variables with all operators and shorthand",
     "vehicle_sil_scenario: 1\nparams: { tol: 25 }\nsteps:\n" + SET +
     "  - expect: { Dashboard.dashboard.engine_speed: { variable: Engine.engine.speed } }\n"
     "  - expect: { Dashboard.dashboard.engine_speed: { eq: { variable: Engine.engine.speed }, ne: -1 } }\n"
     "  - expect: { Dashboard.dashboard.engine_speed: { ge: { variable: Engine.engine.throttle }, le: { variable: Engine.engine.speed } } }\n"
     "    hold_for_ms: 20\n"
     "  - expect: { Dashboard.dashboard.engine_speed: { gt: { variable: Engine.engine.throttle }, lt: { variable: Engine.engine.speed } } }\n"
     "    within_ms: 50\n"
     "  - expect: { Dashboard.dashboard.engine_speed: { near: { variable: Engine.engine.speed }, tolerance: \"${tol}\" } }\n"
     "  - wait_until: { Dashboard.dashboard.engine_lost: { ne: { variable: Engine.ecu.connected } } }\n"
     "    timeout_ms: 100\n"
     "  - ramp: { Engine.engine.throttle: { to: 40, over_ms: 100 } }\n"
     "    expect: { Dashboard.dashboard.engine_speed: { le: { variable: Engine.engine.speed } } }\n"
     "  - wave: { Engine.engine.throttle: { sine: { amplitude: 5, period_ms: 100 }, around: 40, for_ms: 100 } }\n"
     "    expect: { Dashboard.dashboard.engine_speed: { near: { variable: Engine.engine.speed }, tolerance: 500 } }\n",
     "valid"),
    ("variable reference without participant dot",
     S + "  - expect: { Dashboard.dashboard.engine_speed: { variable: speed } }\n", "invalid"),
    ("variable reference with parameter",
     S + "  - expect: { Dashboard.dashboard.engine_speed: { variable: \"${rhs}\" } }\nparams: { rhs: Engine.engine.speed }\n",
     "invalid"),
    ("variable reference with interpolated parameter",
     S + "  - expect: { Dashboard.dashboard.engine_speed: { variable: \"Engine.${rhs}\" } }\nparams: { rhs: engine.speed }\n",
     "invalid"),
    ("variable reference as unquoted number",
     S + "  - expect: { Dashboard.dashboard.engine_speed: { variable: 1.5 } }\n", "invalid"),
    ("variable and tolerance mixed at top",
     S + "  - expect: { Dashboard.dashboard.engine_speed: { variable: Engine.engine.speed, tolerance: 5 } }\n", "invalid"),
    ("variable and operator mixed",
     S + "  - expect: { Dashboard.dashboard.engine_speed: { eq: 5, variable: Engine.engine.speed } }\n", "invalid"),
    ("near with tolerance inside variable mapping",
     S + "  - expect: { Dashboard.dashboard.engine_speed: { near: { variable: Engine.engine.speed, tolerance: 5 }, tolerance: 5 } }\n",
     "invalid"),
    ("empty variable reference mapping",
     S + "  - expect: { Dashboard.dashboard.engine_speed: { eq: {} } }\n", "invalid"),
    ("bare dotted variable name in condition",
     S + "  - expect: { Dashboard.dashboard.engine_speed: Engine.engine.speed }\n", "check only"),
    ("comparing a variable with itself",
     S + "  - expect: { Engine.engine.speed: { variable: Engine.engine.speed } }\n", "check only"),
    ("comparing incompatible variable types",
     S + "  - expect: { Engine.ecu.connected: { eq: { variable: Engine.engine.speed } } }\n", "check only"),
    ("ordered comparison of bool variables",
     S + "  - expect: { Dashboard.dashboard.engine_lost: { ge: { variable: Engine.ecu.connected } } }\n", "check only"),
]

T = "vehicle_sil_scenario_template: 1\n"
TEMPLATE_SAMPLES = [
    ("the least", T + "steps:\n" + SET, "valid"),
    ("JSON", '{"vehicle_sil_scenario_template": 1, "steps": [{"set": {"Engine.engine.throttle": 5}}]}', "valid"),
    ("parameters and relative .. inside templates",
     T + "name: reusable\nparameters:\n  ms:\n    type: integer\n    default: 10\n    values: [10, 20]\n"
     "    description: delay\n  label:\n    type: string\n    default: ok\nsteps:\n"
     "  - name: \"${label}\"\n    after_ms: ${ms}\n    template: ../templates/noop.yaml\n"
     "  - wave: { Engine.engine.throttle: { points: { file: ../points.csv, value: throttle } } }\n", "valid"),
    ("no format version", "steps:\n" + SET, "invalid"),
    ("wrong format version", "vehicle_sil_scenario_template: 2\nsteps:\n" + SET, "invalid"),
    ("unknown top key", T + "only: [small]\nsteps:\n" + SET, "invalid"),
    ("empty steps", T + "steps: []\n", "invalid"),
    ("empty parameter definition", T + "parameters:\n  x: {}\nsteps:\n" + SET, "invalid"),
    ("null parameter definition", T + "parameters:\n  x:\nsteps:\n" + SET, "invalid"),
    ("parameter without type", T + "parameters:\n  x:\n    default: 1\nsteps:\n" + SET, "invalid"),
    ("parameter default type mismatch", T + "parameters:\n  x:\n    type: integer\n    default: 1.5\nsteps:\n" + SET, "invalid"),
    ("parameter string with unquoted int default", T + "parameters:\n  x:\n    type: string\n    default: 1\nsteps:\n" + SET, "invalid"),
    ("parameter values empty", T + "parameters:\n  x:\n    type: integer\n    values: []\nsteps:\n" + SET, "invalid"),
    ("parameter description with quoted number",
     T + 'parameters:\n  x:\n    type: integer\n    description: "123"\nsteps:\n' + SET, "valid"),
    ("parameter description with unquoted text",
     T + "parameters:\n  x:\n    type: integer\n    description: hello world\nsteps:\n" + SET, "valid"),
    ("parameter description with unquoted number",
     T + "parameters:\n  x:\n    type: integer\n    description: 123\nsteps:\n" + SET, "invalid"),
    ("parameter description with unquoted boolean",
     T + "parameters:\n  x:\n    type: integer\n    description: true\nsteps:\n" + SET, "invalid"),
    ("root-escaping .. in template path",
     T + "steps:\n  - template: ../../escape.yaml\n", "check only"),
    ("root-escaping .. in points file inside template",
     T + "steps:\n  - wave: { Engine.engine.throttle: { points: { file: ../../points.csv, value: throttle } } }\n",
     "check only"),
    ("points file with double slash inside template",
     T + "steps:\n  - wave: { Engine.engine.throttle: { points: { file: traces//points.csv, value: throttle } } }\n",
     "invalid"),
    ("points file with trailing slash inside template",
     T + "steps:\n  - wave: { Engine.engine.throttle: { points: { file: traces/, value: throttle } } }\n",
     "invalid"),
    ("YAML 1.2 leading dot float in template",
     T + "parameters:\n  x: { type: number, default: .5 }\nsteps:\n  - set: { Engine.engine.throttle: \"${x}\" }\n",
     "check only"),
    ("YAML 1.2 plus float in template",
     T + "parameters:\n  x: { type: number, default: +1.5 }\nsteps:\n  - set: { Engine.engine.throttle: \"${x}\" }\n",
     "check only"),
    ("YAML 1.2 plus int in template",
     T + "parameters:\n  x: { type: integer, default: +5 }\nsteps:\n  - set: { Engine.engine.throttle: \"${x}\" }\n",
     "check only"),
    ("YAML 1.2 octal int in template",
     T + "parameters:\n  x: { type: integer, default: 0o7 }\nsteps:\n  - set: { Engine.engine.throttle: \"${x}\" }\n",
     "check only"),
    ("variable comparison in template",
     T + "parameters:\n  tol: { type: number, default: 10 }\nsteps:\n"
     "  - expect: { Dashboard.dashboard.engine_speed: { near: { variable: Engine.engine.speed }, tolerance: \"${tol}\" } }\n",
     "valid"),
    ("parameter in variable reference inside template",
     T + "parameters:\n  rhs: { type: string, default: Engine.engine.speed }\nsteps:\n"
     "  - expect: { Dashboard.dashboard.engine_speed: { variable: \"${rhs}\" } }\n",
     "invalid"),
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
        self._lock = threading.RLock()
        self.scenario_configuration: Path | None = None
        # The points file the samples name (points: { file: points.csv }), beside the scenarios.
        (directory / "points.csv").write_text("time,throttle,connected\n0,0,false\n100,50,true\n", encoding="utf-8", newline="\n")
        (directory / "templates").mkdir(exist_ok=True)
        (directory / "templates" / "noop.yaml").write_text(
            "vehicle_sil_scenario_template: 1\n"
            "parameters:\n"
            "  ms: { type: integer, default: 10 }\n"
            "  label: { type: string, default: ok }\n"
            "steps:\n"
            "  - name: \"${label}\"\n"
            "    after_ms: ${ms}\n"
            "    set: { Engine.engine.throttle: 5 }\n",
            encoding="utf-8",
            newline="\n",
        )

    def write(self, text: str) -> Path:
        with self._lock:
            self.count += 1
            count = self.count
        path = self.directory / f"file{count}.yaml"
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
        with self._lock:
            if self.scenario_configuration is None:
                self.scenario_configuration = self.write(SCENARIO_CONFIG.replace("@DIR@", str(Path(EXAMPLE_DIR).resolve())))
                report = check(str(self.scenario_configuration), "--variant", "small")
                if errors_of(report):
                    raise AssertionError(f"the configuration the scenarios are checked with has errors: {errors_of(report)}")
            scenario_configuration = self.scenario_configuration
        try:
            data = load(text)
        except yaml.YAMLError:
            data = None
        case_names = []
        if isinstance(data, dict) and isinstance(data.get("cases"), dict):
            case_names = [
                k for k in data["cases"]
                if isinstance(k, str) and k and not k.startswith("--") and not any(ord(c) < 0x20 or ord(c) == 0x7F for c in k)
            ]
        report = check(str(scenario_configuration), "--variant", "small", "--scenario", str(self.write(text)),
                       *(["--case", case_names[0]] if case_names else []))
        return errors_of(report)

    def template(self, text: str) -> list[dict]:
        if not EXAMPLE_DIR or not (Path(EXAMPLE_DIR) / "simulation.yaml").is_file():
            raise AssertionError("VEHICLE_SIL_EXAMPLE_DIR must be the directory of the built examples/ecu_main "
                                 f"(simulation.yaml next to its ECUs), not {EXAMPLE_DIR!r}")
        with self._lock:
            if self.scenario_configuration is None:
                self.scenario_configuration = self.write(SCENARIO_CONFIG.replace("@DIR@", str(Path(EXAMPLE_DIR).resolve())))
                report = check(str(self.scenario_configuration), "--variant", "small")
                if errors_of(report):
                    raise AssertionError(f"the configuration the scenarios are checked with has errors: {errors_of(report)}")
            scenario_configuration = self.scenario_configuration
            self.count += 1
            count = self.count
        tpl_path = self.directory / "templates" / f"tpl{count}.yaml"
        tpl_path.write_text(text.replace("@EXE@", LAUNCH), encoding="utf-8", newline="\n")
        args: dict[str, object] = {}
        try:
            data = load(text)
        except yaml.YAMLError:
            data = None
        if isinstance(data, dict) and isinstance(data.get("parameters"), dict):
            for pname, pdef in data["parameters"].items():
                if not isinstance(pdef, dict) or "default" in pdef:
                    continue
                vals = pdef.get("values")
                if isinstance(vals, list) and vals:
                    args[pname] = vals[0]
                else:
                    ptype = pdef.get("type")
                    args[pname] = False if ptype == "boolean" else 0 if ptype in ("integer", "number") else "ok"
        call = {"template": f"templates/tpl{count}.yaml"}
        if args:
            call["params"] = args
        wrapper = {"vehicle_sil_scenario": 1, "steps": [call]}
        wrapper_path = self.directory / f"wrap{count}.yaml"
        wrapper_path.write_text(json.dumps(wrapper), encoding="utf-8", newline="\n")
        report = check(str(scenario_configuration), "--variant", "small", "--scenario", str(wrapper_path))
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
        for schema in (SIMULATION, SCENARIO, SCENARIO_TEMPLATE, TIMELINE, RUN_REQUEST, CONTROL_METRICS, BENCHMARK_REPORT):
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
        for name, schema, mappings in (
            ("simulation", SIMULATION, simulation_mappings()),
            ("scenario", SCENARIO, scenario_mappings()),
            ("scenario-template", SCENARIO_TEMPLATE, template_mappings()),
        ):
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

    def test_the_template_schema_matches_the_scenario_schema(self):
        self.assertIn("parameter_def", SCENARIO_TEMPLATE["definitions"])
        synced = sync_scenario_template_schema(SCENARIO, SCENARIO_TEMPLATE)
        self.assertEqual(
            SCENARIO_TEMPLATE,
            synced,
            "schema/scenario-template.schema.json is out of sync with schema/scenario.schema.json: "
            "run `python3 tests/schema/test_schemas.py --sync`",
        )
        self.assertEqual(
            list(SCENARIO_TEMPLATE["definitions"].keys()),
            list(SCENARIO["definitions"].keys()) + ["parameter_def"],
            "schema/scenario-template.schema.json definition order drifted: "
            "run `python3 tests/schema/test_schemas.py --sync`",
        )

    def test_sync_schemas_repairs_template_definitions_and_copies(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = Path(tmp)
            for rel in {
                "schema/scenario.schema.json",
                "schema/scenario-template.schema.json",
                *TEMPLATE_COPIES.values(),
                *TEMPLATE_COPIES.keys(),
            }:
                dst = tmp_root / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes((ROOT / rel).read_bytes())

            # Mutate a shared definition in scenario.schema.json and verify sync propagates it to
            # scenario-template.schema.json and both template/schema/ copies.
            scen_path = tmp_root / "schema" / "scenario.schema.json"
            tpl_path = tmp_root / "schema" / "scenario-template.schema.json"
            updated_scen = json.loads(scen_path.read_text(encoding="utf-8"))
            updated_scen["definitions"]["at_ms"]["description"] = "updated at_ms description"
            scen_path.write_text(json.dumps(updated_scen, indent=2) + "\n", encoding="utf-8", newline="\n")

            updated = sync_schemas(tmp_root)
            self.assertIn("schema/scenario-template.schema.json", updated)
            self.assertIn("template/schema/scenario-template.schema.json", updated)
            self.assertIn("template/schema/scenario.schema.json", updated)
            synced_tpl = json.loads(tpl_path.read_text(encoding="utf-8"))
            self.assertEqual(synced_tpl["definitions"]["at_ms"]["description"], "updated at_ms description")
            self.assertEqual(
                (tmp_root / "template" / "schema" / "scenario-template.schema.json").read_bytes(),
                tpl_path.read_bytes(),
            )
            self.assertEqual(
                (tmp_root / "template" / "schema" / "scenario.schema.json").read_bytes(),
                scen_path.read_bytes(),
            )
            # Second sync is a no-op.
            self.assertEqual(sync_schemas(tmp_root), [])

    def test_the_examples_are_valid(self):
        schemas = {
            "simulation.schema.json": SIMULATION,
            "scenario.schema.json": SCENARIO,
            "scenario-template.schema.json": SCENARIO_TEMPLATE,
        }
        seen = {schema: 0 for schema in KINDS.values()}
        for path in yaml_files():
            data = load(path.read_text(encoding="utf-8"))
            kind = kind_of(data)
            if kind is None or kind == "dashboard.schema.json":  # the dashboard's schema is the screen's to test
                continue
            seen[kind] += 1
            schema = schemas[kind]
            self.assertEqual(problems(schema, data), [],
                             f"{path.relative_to(ROOT)} is not valid for schema/{kind}: if the launcher reads the "
                             "file, the schema is too strict; correct the schema")
        self.assertGreater(seen["simulation.schema.json"], 3)
        self.assertGreater(seen["scenario.schema.json"], 3)
        self.assertGreater(seen["scenario-template.schema.json"], 0)

    def test_the_yaml_of_the_documents_is_valid(self):
        seen = {"simulation": 0, "scenario": 0, "scenario-template": 0}
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
                elif whole == "scenario-template.schema.json" or (
                    whole is None and set(data) <= keys_of(SCENARIO_TEMPLATE) and "parameters" in data and "steps" in data
                ):
                    kind, schema = "scenario-template", SCENARIO_TEMPLATE
                elif whole == "scenario.schema.json" or (whole is None and set(data) <= keys_of(SCENARIO)
                                                         and "steps" in data):
                    kind, schema = "scenario", SCENARIO
                else:
                    continue
                seen[kind] += 1
                said = problems(schema if whole else without_required(schema), data)
                key = f"{english_name(path)}:{before}"
                expected = MISTAKES_IN_DOCUMENTS.get(key)
                if expected is None:
                    self.assertEqual(said, [], f"{name}:{line}: the YAML block is not valid for schema/{kind}.schema."
                                               "json: correct the block, or the schema if the launcher reads it")
                else:
                    found.add(key)
                    for start in expected:
                        self.assertTrue(any(s.startswith(start) for s in said),
                                        f"{name}:{line}: the schema should find the mistake at {start!r}; it says {said}")
        self.assertGreater(seen["simulation"], 3)
        self.assertGreater(seen["scenario"], 3)
        self.assertGreater(seen["scenario-template"], 0)
        self.assertEqual(found, set(MISTAKES_IN_DOCUMENTS), "a block of MISTAKES_IN_DOCUMENTS is not in the documents "
                                                             "any more: correct the list")

    def test_the_samples_are_valid_or_not_as_they_say(self):
        for schema, samples in ((SIMULATION, CONFIG_SAMPLES), (SCENARIO, SCENARIO_SAMPLES),
                                (SCENARIO_TEMPLATE, TEMPLATE_SAMPLES)):
            for what, text, verdict in samples:
                said = problems(schema, load(text))
                if verdict == "invalid":
                    self.assertNotEqual(said, [], f"{what}: the schema should refuse\n{text}")
                else:
                    self.assertEqual(said, [], f"{what}: the schema should accept\n{text}")

    def test_every_key_is_in_its_document(self):
        def code_spans(path: Path) -> set[str]:
            return set(re.findall(r"`([^`\n]+)`", path.read_text(encoding="utf-8")))

        config_pages, scenario_pages = pages("simulation-config.md"), pages("scenario.md")
        self.assertTrue(config_pages and scenario_pages, "docs/simulation-config.md and docs/scenario.md are missing")
        mappings = simulation_mappings()
        for path in config_pages:
            doc = path.relative_to(ROOT).as_posix()
            spans = code_spans(path)
            words = {word for span in spans for word in re.findall(r"[A-Za-z_]+", span)}
            for key in keys_of(mappings["top"]):
                self.assertIn(key, spans, f"{doc} has no `{key}` (the table of the keys)")
            for key in keys_of(mappings["network"]):
                self.assertIn(f"networks[].{key}", spans, f"{doc}: the table of the keys")
            for key in keys_of(mappings["participant"]):
                self.assertIn(f"participants[].{key}", spans, f"{doc}: the table of the keys")
            for key in keys_of(mappings["variant participant"]):
                self.assertIn(f"…<participant>.{key}", spans, f"{doc}: the table of the variants")
            for key in keys_of(mappings["variant"]):
                self.assertTrue(any(s.startswith("variants.<") and s.endswith(f".{key}") or
                                    s.startswith("variants.<") and f".{key}." in s for s in spans),
                                f"{doc} has no `variants.<variant>.{key}` (the table of the variants)")
            for key in keys_of(mappings["variable"]):
                self.assertIn(key, words, f"{doc} does not name the key {key} of a variable mapping")
            for key in keys_of(mappings["j1939 node"]):
                self.assertIn(key, words, f"{doc} does not name the key {key} of an entry of j1939_nodes")
            for key in keys_of(mappings["can external"]) | keys_of(mappings["ethernet external"]):
                self.assertIn(key, words, f"{doc} does not name the key {key} of a network's external")

        for path in scenario_pages:
            doc = path.relative_to(ROOT).as_posix()
            spans = code_spans(path)
            words = {word for span in spans for word in re.findall(r"[A-Za-z_]+", span)}
            for where, mapping in scenario_mappings().items():
                for key in keys_of(mapping):
                    if where in ("top", "step"):
                        self.assertTrue(any(s == key or s.startswith(key + ":") for s in spans),
                                        f"{doc} has no `{key}` (the tables of the file format)")
                    else:
                        self.assertIn(key, words, f"{doc} does not name the key {key} ({where})")
            for where, mapping in template_mappings().items():
                for key in keys_of(mapping):
                    if where == "template top":
                        self.assertTrue(any(s == key or s.startswith(key + ":") for s in spans),
                                        f"{doc} has no `{key}` (the template format)")
                    else:
                        self.assertIn(key, words, f"{doc} does not name the key {key} ({where})")

    def test_the_copies_of_the_template_are_the_schemas(self):
        for copy_path, source in TEMPLATE_COPIES.items():
            self.assertTrue((ROOT / copy_path).is_file(),
                            f"{copy_path} is missing: run `python3 tests/schema/test_schemas.py --sync`")
            self.assertEqual((ROOT / copy_path).read_bytes(), (ROOT / source).read_bytes(),
                             f"{copy_path} is not {source} any more: run `python3 tests/schema/test_schemas.py --sync`")
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


# (what it shows, the JSON text, "valid" | "invalid")
R = '"vehicle_sil_run_request": 1'
REQUEST_SAMPLES = [
    ("the least", "{" + R + "}", "valid"),
    ("every key", "{" + R + ', "$schema": "../schema/run-request.schema.json", "variant": "left_hand", '
     '"params": {"target_speed": "80", "gear": 3, "lamp": true}, "scenario": "scenarios/brake.yaml", '
     '"case": "nominal", "disable": ["B"], '
     '"enable": ["C"], "manual": ["A"], "connect": {"ETH_EXT": "enp3s0", "CAN1": "physical:1", "LIN1": "none"}, '
     '"time_policy": "free", "speed": 2.5, "start_paused": false, "strict": true}', "valid"),
    ("a paused start (not with a scenario: that is a rule of the request, not of the file)",
     "{" + R + ', "start_paused": true}', "valid"),
    ("speed max", "{" + R + ', "speed": "max"}', "valid"),
    ("params without a scenario (they wait for a run with one)", "{" + R + ', "params": {"rpm": 650}}', "valid"),
    ("variant null", "{" + R + ', "variant": null}', "invalid"),
    ("scenario null", "{" + R + ', "scenario": null}', "invalid"),
    ("params null", "{" + R + ', "params": null}', "invalid"),
    ("disable null", "{" + R + ', "disable": null}', "invalid"),
    ("connect null", "{" + R + ', "connect": null}', "invalid"),
    ("time_policy null", "{" + R + ', "time_policy": null}', "invalid"),
    ("strict null", "{" + R + ', "strict": null}', "invalid"),
    ("empty lists and mappings", "{" + R + ', "params": {}, "disable": [], "enable": [], "manual": [], "connect": {}}', "valid"),
    ("no version", '{"strict": true}', "invalid"),
    ("another version", '{"vehicle_sil_run_request": 2}', "invalid"),
    ("the version as a text", '{"vehicle_sil_run_request": "1"}', "invalid"),
    ("an unknown key", "{" + R + ', "conect": {}}', "invalid"),
    ("what the caller decides: the configuration file", "{" + R + ', "file": "simulation.yaml"}', "invalid"),
    ("what the caller decides: the timeline", "{" + R + ', "timeline": "t.json"}', "invalid"),
    ("variant as a number", "{" + R + ', "variant": 5}', "invalid"),
    ("an empty variant", "{" + R + ', "variant": ""}', "invalid"),
    ("scenario as a list", "{" + R + ', "scenario": ["a.yaml"]}', "invalid"),
    ("an empty scenario", "{" + R + ', "scenario": ""}', "invalid"),
    ("params as a list", "{" + R + ', "params": [1]}', "invalid"),
    ("a parameter with a list", "{" + R + ', "params": {"x": [1]}}', "invalid"),
    ("a parameter's name with a dash", "{" + R + ', "params": {"idle-rpm": 1}}', "invalid"),
    ("a parameter that uses another", "{" + R + ', "params": {"x": "${y}"}}', "invalid"),
    ("disable as a text", "{" + R + ', "disable": "B"}', "invalid"),
    ("enable with a number", "{" + R + ', "enable": [1]}', "invalid"),
    ("manual with an empty name", "{" + R + ', "manual": [""]}', "invalid"),
    ("connect as a list", "{" + R + ', "connect": ["CAN1=physical:1"]}', "invalid"),
    ("a target as a number", "{" + R + ', "connect": {"CAN1": 1}}', "invalid"),
    ("a target that is null", "{" + R + ', "connect": {"CAN1": null}}', "invalid"),
    ("a network without a name", "{" + R + ', "connect": {"": "eth0"}}', "invalid"),
    ("another time policy", "{" + R + ', "time_policy": "warn"}', "invalid"),
    ("a speed as a text", "{" + R + ', "speed": "2"}', "invalid"),
    ("speed 0", "{" + R + ', "speed": 0}', "invalid"),
    ("a speed above the range", "{" + R + ', "speed": 2000000}', "invalid"),
    ("speed null", "{" + R + ', "speed": null}', "invalid"),
    ("start_paused as a text", "{" + R + ', "start_paused": "true"}', "invalid"),
    ("strict as a number", "{" + R + ', "strict": 1}', "invalid"),
    ("after_scenario stop", "{" + R + ', "after_scenario": "stop"}', "valid"),
    ("after_scenario pause", "{" + R + ', "scenario": "scenarios/brake.yaml", "after_scenario": "pause"}', "valid"),
    ("after_scenario invalid", "{" + R + ', "after_scenario": "continue"}', "invalid"),
    ("after_scenario null", "{" + R + ', "after_scenario": null}', "invalid"),
    ("case valid", "{" + R + ', "scenario": "scenarios/brake.yaml", "case": "-10C"}', "valid"),
    ("case empty", "{" + R + ', "case": ""}', "invalid"),
    ("case null", "{" + R + ', "case": null}', "invalid"),
    ("case number", "{" + R + ', "case": 1}', "invalid"),
    ("cli_params is serve intent only", "{" + R + ', "cli_params": {"x": "1"}}', "invalid"),
    ("record valid", "{" + R + ', "record": ["Engine.engine.speed", "Dashboard.ecu.connected"]}', "valid"),
    ("record empty", "{" + R + ', "record": []}', "valid"),
    ("record string", "{" + R + ', "record": "Engine.engine.speed"}', "invalid"),
    ("record empty item", "{" + R + ', "record": [""]}', "invalid"),
    ("record non-string item", "{" + R + ', "record": [1]}', "invalid"),
    ("record null", "{" + R + ', "record": null}', "invalid"),
    ("record_after_fail_ms valid", "{" + R + ', "scenario": "scenarios/brake.yaml", "record_after_fail_ms": 300}', "valid"),
    ("record_after_fail_ms min", "{" + R + ', "scenario": "scenarios/brake.yaml", "record_after_fail_ms": 1}', "valid"),
    ("record_after_fail_ms max", "{" + R + ', "scenario": "scenarios/brake.yaml", "record_after_fail_ms": 600000}', "valid"),
    ("record_after_fail_ms zero", "{" + R + ', "scenario": "scenarios/brake.yaml", "record_after_fail_ms": 0}', "invalid"),
    ("record_after_fail_ms negative", "{" + R + ', "scenario": "scenarios/brake.yaml", "record_after_fail_ms": -5}', "invalid"),
    ("record_after_fail_ms above max", "{" + R + ', "scenario": "scenarios/brake.yaml", "record_after_fail_ms": 600001}', "invalid"),
    ("record_after_fail_ms float", "{" + R + ', "scenario": "scenarios/brake.yaml", "record_after_fail_ms": 1.5}', "invalid"),
    ("record_after_fail_ms string", "{" + R + ', "scenario": "scenarios/brake.yaml", "record_after_fail_ms": "300"}', "invalid"),
    ("record_after_fail_ms null", "{" + R + ', "scenario": "scenarios/brake.yaml", "record_after_fail_ms": null}', "invalid"),
    ("record_events true", "{" + R + ', "record_events": true}', "valid"),
    ("record_events false", "{" + R + ', "record_events": false}', "valid"),
    ("record_events int", "{" + R + ', "record_events": 1}', "invalid"),
    ("record_events string", "{" + R + ', "record_events": "true"}', "invalid"),
    ("record_events null", "{" + R + ', "record_events": null}', "invalid"),
    ("record_can true", "{" + R + ', "record_can": true}', "valid"),
    ("record_can false", "{" + R + ', "record_can": false}', "valid"),
    ("record_can int", "{" + R + ', "record_can": 1}', "invalid"),
    ("record_can string", "{" + R + ', "record_can": "true"}', "invalid"),
    ("record_can null", "{" + R + ', "record_can": null}', "invalid"),
]


def request_files() -> list[Path]:
    """The request files that people read as examples: the JSON files of examples/ and template/ with the key."""
    out = []
    for top in ("examples", "template"):
        for path in sorted((ROOT / top).rglob("*.json")):
            if {"build", "tools", "node_modules"} & set(path.relative_to(ROOT).parts):
                continue
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue
            if isinstance(data, dict) and "vehicle_sil_run_request" in data:
                out.append(path)
    return out


def json_blocks(path: Path) -> list[tuple[int, str]]:
    """The ```json blocks of a document: (line of the fence, the text)."""
    lines = path.read_text(encoding="utf-8").split("\n")
    out = []
    i = 0
    while i < len(lines):
        if lines[i].strip() == "```json":
            end = next(j for j in range(i + 1, len(lines)) if lines[j].strip() == "```")
            out.append((i + 1, "\n".join(lines[i + 1:end])))
            i = end
        i += 1
    return out


class RunRequestSchema(unittest.TestCase):
    maxDiff = None

    def test_it_refuses_unknown_keys_and_describes_its_keys(self):
        self.assertIs(RUN_REQUEST.get("additionalProperties"), False)
        self.assertEqual(RUN_REQUEST["required"], ["vehicle_sil_run_request"])
        for key, value in RUN_REQUEST["properties"].items():
            self.assertTrue(value.get("description"), f"run-request.schema.json: {key} has no description (the text an "
                                                      "editor shows for the key)")

    def test_the_samples_are_valid_or_not_as_they_say(self):
        for what, text, verdict in REQUEST_SAMPLES:
            said = problems(RUN_REQUEST, json.loads(text))
            if verdict == "invalid":
                self.assertNotEqual(said, [], f"{what}: the schema should refuse\n{text}")
            else:
                self.assertEqual(said, [], f"{what}: the schema should accept\n{text}")

    def test_the_examples_are_valid_and_name_the_schema(self):
        files = request_files()
        self.assertGreater(len(files), 0, "examples/ has no request file any more (a JSON file with "
                                          "vehicle_sil_run_request)")
        for path in files:
            data = json.loads(path.read_text(encoding="utf-8"))
            where = path.relative_to(ROOT).as_posix()
            self.assertEqual(problems(RUN_REQUEST, data), [],
                             f"{where} is not valid for schema/run-request.schema.json: if the launcher reads the "
                             "file, the schema is too strict; correct the schema")
            self.assertIn("$schema", data, f"{where}: name the schema in \"$schema\", so that an editor checks the file")
            target = (path.parent / data["$schema"]).resolve()
            in_template = where.startswith("template/")
            expected = (ROOT / "template" / "schema" if in_template else SCHEMA_DIR) / "run-request.schema.json"
            self.assertEqual(target, expected.resolve(), f"{where}: \"$schema\" must lead to {expected.relative_to(ROOT)}")

    def test_the_json_of_the_documents_is_valid(self):
        seen = 0
        for path in documents():
            for line, text in json_blocks(path):
                try:
                    data = json.loads(text)
                except json.JSONDecodeError:
                    continue  # a sketch, or the output of a command
                if not isinstance(data, dict) or "vehicle_sil_run_request" not in data:
                    continue
                seen += 1
                self.assertEqual(problems(RUN_REQUEST, data), [],
                                 f"{path.relative_to(ROOT).as_posix()}:{line}: the JSON block is not valid for "
                                 "schema/run-request.schema.json: correct the block, or the schema if the launcher "
                                 "reads it")
        self.assertGreater(seen, 1, "the documents show no request file any more (docs/simulation-run.md and its "
                                    "Japanese page)")

    def test_every_key_is_in_its_document(self):
        run_pages = pages("simulation-run.md")
        self.assertEqual(len(run_pages), 2, "docs/simulation-run.md or its Japanese page is missing")
        for path in run_pages:
            spans = set(re.findall(r"`([^`\n]+)`", path.read_text(encoding="utf-8")))
            for key in RUN_REQUEST["properties"]:
                self.assertIn(key, spans, f"{path.relative_to(ROOT).as_posix()} has no `{key}` (the table of the keys of "
                                          "the request file)")


@needs_launcher
class RunRequestSchemaAndLauncher(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        if not Path(LAUNCH).is_file():
            raise AssertionError(f"VEHICLE_SIL_LAUNCH_EXE is {LAUNCH!r}, which is not a file: build vehicle_sil_launch")
        cls.temporary = tempfile.TemporaryDirectory(prefix="vehicle-sil-request-")
        cls.directory = Path(cls.temporary.name)
        cls.count = 0
        cls._lock = threading.Lock()

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def refusal(self, text: str) -> list[str]:
        """What `check --request` says is wrong with the request file of `text` (the configuration is not reached)."""
        with type(self)._lock:
            type(self).count += 1
            count = self.count
        path = self.directory / f"request{count}.json"
        path.write_text(text, encoding="utf-8", newline="\n")
        report = check("--request", str(path), str(self.directory / "no-configuration.yaml"))
        said = [f["message"] for f in report["findings"] if f["message"].startswith(str(path))]
        self.assertEqual(bool(said), report["result"] == "usage",
                         f"a request file with a mistake is a usage error that names the file, and nothing else is: {report}")
        return said

    def test_the_keys_are_the_launchers(self):
        said = self.refusal("{" + R + ', "zz_probe": 1}')
        match = re.search(r"the keys here \((.*)\)$|\(the keys here: (.*)\)$", said[0]) if len(said) == 1 else None
        self.assertTrue(match, f"expected one error about zz_probe that lists the keys, got {said}")
        launcher_keys = listed(match.group(1) or match.group(2)) | {"$schema"}  # an editor's key, which the launcher skips
        schema_keys = set(RUN_REQUEST["properties"])
        self.assertEqual(schema_keys, launcher_keys,
                         "schema/run-request.schema.json and the launcher know different keys (only in the schema: "
                         f"{sorted(schema_keys - launcher_keys)}; only in the launcher: "
                         f"{sorted(launcher_keys - schema_keys)}): add or remove them in the schema, with a description")

    def test_the_launcher_reads_what_the_schema_accepts_and_refuses_what_it_refuses(self):
        workers = min(os.cpu_count() or 4, 4)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(lambda sample: (sample, self.refusal(sample[1])), REQUEST_SAMPLES))
        for (what, text, verdict), said in results:
            if verdict == "valid":
                self.assertEqual(said, [], f"request sample '{what}': the schema accepts it, the launcher does not; "
                                           f"correct the sample or the schema\n{text}")
            else:
                self.assertNotEqual(said, [], f"request sample '{what}': the launcher reads it without an error; the "
                                              f"schema refuses a file that is right: loosen the schema\n{text}")

    @unittest.skipUnless(EXAMPLE_DIR, "VEHICLE_SIL_EXAMPLE_DIR is not set")
    def test_the_request_files_of_the_example_pass_the_check(self):
        example = ROOT / "examples" / "ecu_main"
        files = [p for p in request_files() if example in p.parents]
        self.assertGreater(len(files), 0, "examples/ecu_main has no request file any more")
        for path in files:
            report = check("--request", str(path), str(Path(EXAMPLE_DIR) / "simulation.yaml"))
            self.assertEqual(report["result"], "passed",
                             f"vehicle_sil_launch check --request {path.relative_to(ROOT).as_posix()} with the built "
                             f"example: {report['findings']}")


class TimelineSchema(unittest.TestCase):
    maxDiff = None

    def test_every_key_is_described(self):
        def walk(node, where):
            for key, value in node.get("properties", {}).items():
                self.assertTrue(value.get("description"), f"timeline.schema.json: {where}{key} has no description")
                walk(value, f"{where}{key}.")
                if isinstance(value.get("items"), dict):
                    walk(value["items"], f"{where}{key}[].")
        walk(TIMELINE, "")

    def test_every_key_is_required(self):
        # Every key of the file is always there (null when it does not apply), except path-scoped optional
        # provenance/conditional fields so existing timeline records stay valid unchanged.
        may_be_left_out = {
            ("", "recorded_to_ms"),
            ("", "events"),
            ("", "can_frames"),
            ("scenario.", "case"),
            ("variables.", "recorded"),
            ("steps.", "file"),
            ("steps.", "name"),
            ("steps.", "expect_skipped"),
            ("steps.", "call_stack"),
            ("steps.watch.conditions.compare.", "variable"),
            ("steps.failed.conditions.", "compared"),
            ("lines.", "file"),
            ("can_frames.", "rtr"),
            ("can_frames.", "fd"),
            ("limits.", "events_per_participant"),
            ("limits.", "events_total"),
            ("limits.", "events_dropped"),
            ("limits.", "can_frames_per_network"),
            ("limits.", "can_frames_total"),
            ("limits.", "can_dropped"),
        }

        def walk(node, where):
            if isinstance(node, dict):
                if "properties" in node:
                    optional = {
                        (where, key)
                        for key in set(node["properties"]) - set(node.get("required", []))
                    } - may_be_left_out
                    self.assertEqual(optional, set(), f"timeline.schema.json: {where or 'the file'} does not require "
                                     f"{sorted(k for _, k in optional)}: the launcher writes every key, so add them to required "
                                     "(and null to their type, if they may be null)")
                for key, value in node.items():
                    walk(value, f"{where}{key}." if key not in ("properties", "items") else where)
            elif isinstance(node, list):
                for value in node:
                    walk(value, where)
        walk(TIMELINE, "")

    def test_the_references_are_valid(self):
        files = sorted(TIMELINE_REFERENCES.glob("*.timeline.json"))
        self.assertGreater(len(files), 3, f"{TIMELINE_REFERENCES} has no timeline files any more")
        for path in files:
            self.assertEqual(problems(TIMELINE, json.loads(path.read_text(encoding="utf-8"))), [],
                             f"{path.relative_to(ROOT)} is not valid for schema/timeline.schema.json: the launcher "
                             "wrote it, so correct the schema (and docs/simulation-run.md)")

    def test_the_fixtures_of_the_timeline_page_are_valid(self):
        files = sorted(TIMELINE_PAGE_FIXTURES.glob("*.timeline.json"))
        self.assertGreater(len(files), 0, f"{TIMELINE_PAGE_FIXTURES} has no timeline files any more")
        for path in files:
            self.assertEqual(problems(TIMELINE, json.loads(path.read_text(encoding="utf-8"))), [],
                             f"{path.relative_to(ROOT)} is not valid for schema/timeline.schema.json: correct the "
                             "fixture, so that the page is tested on what the launcher can write")

    def test_a_file_with_a_mistake_is_not_valid(self):
        good = json.loads((TIMELINE_REFERENCES / "fails_beside.timeline.json").read_text(encoding="utf-8"))

        def changed(change) -> list[str]:
            data = json.loads(json.dumps(good))
            change(data)
            return problems(TIMELINE, data)

        self.assertEqual(changed(lambda d: None), [])

        def failed_step(data) -> dict:
            return next(step for step in data["steps"] if step["failed"] is not None)

        mistakes = {
            "another format version": lambda d: d.update(vehicle_sil_timeline=2),
            "no series": lambda d: d.pop("series"),
            "a point that is no pair": lambda d: d["series"][0]["points"].append([10]),
            "a time that is a text": lambda d: d["series"][0]["points"].append(["10", 1]),
            "a step without its watch": lambda d: d["steps"][0].pop("watch"),
            "a variable that is an index": lambda d: d["series"][0].update(variable=0),
            "no recorded_to_ms": lambda d: d["scenario"].pop("recorded_to_ms"),
            "a line without its variable": lambda d: d["lines"][0].pop("variable"),
            "a line whose step is a text": lambda d: d["lines"][0].update(step="1"),
            "a shape without its type": lambda d: d["lines"][-1].update(shape={"to": 1}),
            "step lines that are no positions": lambda d: d["steps"][0].update(lines=["0"]),
            "columns that are no names": lambda d: d["series"][0].update(columns=[0, 1, 2]),
            "writes of a failed step without their variable": lambda d: failed_step(d)["failed"].update(
                writes=[{"at_ms": 20, "value": 13, "point": 2, "applied": False}]),
            "writes of a failed step that do not say whether they were applied": lambda d: failed_step(d)["failed"].update(
                writes=[{"variable": "Body.level", "at_ms": 20, "value": 13, "point": 2}]),
            "a failed step without its writes": lambda d: failed_step(d)["failed"].pop("writes"),
            "a failed write that is not an index": lambda d: failed_step(d)["failed"].update(write={"variable": "Body.level"}),
            "a line whose end is a text": lambda d: d["lines"][0].update(to_ms="never"),
            # Every key is always there, null when it does not apply.
            "no lines, in the file and in the steps": lambda d: (d.pop("lines"), [step.pop("lines") for step in d["steps"]]),
            "no time_step_ms": lambda d: d.pop("time_step_ms"),
            "a series without its columns": lambda d: d["series"][0].pop("columns"),
            "a series without not_applied_from": lambda d: d["series"][0].pop("not_applied_from"),
            "a series without samples": lambda d: d["series"][0].pop("samples"),
            "a variable without its unit": lambda d: d["variables"][0].pop("unit"),
            "a step without its message": lambda d: d["steps"][0].pop("message"),
            "a call_stack frame without params": lambda d: d["steps"][0].update(
                call_stack=[{"file": "a.yaml", "line": 3, "name": None}]),
            "a call_stack frame without file": lambda d: d["steps"][0].update(
                call_stack=[{"line": 3, "name": None, "params": {}}]),
            "a step name that is a number": lambda d: d["steps"][0].update(name=123),
            "a step file that is a number": lambda d: d["steps"][0].update(file=123),
            "a line file that is a number": lambda d: d["lines"][0].update(file=123),
            "a watch compare variable that is a number": lambda d: failed_step(d)["watch"]["conditions"][0]["compare"][0].update(
                value=None, variable=123),
            "a failed compared entry without sample_ms": lambda d: failed_step(d)["failed"]["conditions"][0].update(
                compared=[{"variable": "Body.other", "value": 1}]),
            "a failed compared entry whose variable is a number": lambda d: failed_step(d)["failed"]["conditions"][0].update(
                compared=[{"variable": 0, "value": 1, "sample_ms": 10}]),
            "a variable recorded flag that is a text": lambda d: d["variables"][0].update(recorded="yes"),
            "scenario that is a string": lambda d: d.update(scenario="manual"),
            "top-level recorded_to_ms that is a string": lambda d: d.update(recorded_to_ms="200"),
        }
        for what, change in mistakes.items():
            self.assertNotEqual(changed(change), [], f"the schema passes a timeline with {what}")
        # Manual-run timeline (scenario: null + top-level recorded_to_ms, empty steps & lines) is valid,
        # while malformed manual shapes are rejected.
        manual_good = {
            "vehicle_sil_timeline": 1,
            "vehicle_sil": "0.4.0",
            "stage": "run",
            "file": "simulation.yaml",
            "variant": None,
            "params": [],
            "time_step_ms": 5,
            "scenario": None,
            "recorded_to_ms": 200,
            "variables": [
                {
                    "name": "Engine.engine.speed",
                    "participant": "Engine",
                    "type": "real",
                    "causality": "output",
                    "unit": "rpm",
                    "min": 0,
                    "max": 8000,
                    "items": [],
                    "ecu_step_ms": 10,
                    "written": False,
                    "watched": False,
                    "recorded": True,
                }
            ],
            "steps": [],
            "series": [
                {
                    "variable": "Engine.engine.speed",
                    "source": "sampled",
                    "encoding": "changes",
                    "columns": ["at_ms", "value"],
                    "samples": 20,
                    "first_ms": 0,
                    "last_ms": 190,
                    "not_applied_from": None,
                    "points": [[0, 800]],
                }
            ],
            "lines": [],
            "findings": [],
            "limits": {"points_per_series": 100000, "points_total": 1000000, "dropped": []},
        }
        self.assertEqual(problems(TIMELINE, manual_good), [])
        manual_null = copy.deepcopy(manual_good)
        manual_null["recorded_to_ms"] = None
        manual_null["series"][0].update(samples=0, first_ms=None, last_ms=None, points=[])
        self.assertEqual(problems(TIMELINE, manual_null), [])
        manual_bad_rec = copy.deepcopy(manual_good)
        manual_bad_rec["recorded_to_ms"] = "200"
        self.assertNotEqual(problems(TIMELINE, manual_bad_rec), [])
        manual_bad_sc = copy.deepcopy(manual_good)
        manual_bad_sc["scenario"] = {}
        self.assertNotEqual(problems(TIMELINE, manual_bad_sc), [])
        # What a later version may add is no mistake: keys, and values of the open lists.
        additions = {
            "a key it does not know": lambda d: d.update(notes=[]),
            "an extra recorded flag on a variable": lambda d: d["variables"][0].update(recorded=True),
            "scenario case and step/line provenance": lambda d: (
                d["scenario"].update(case="nominal"),
                d["steps"][0].update(
                    file="templates/init.yaml",
                    name="init",
                    call_stack=[{"file": "scenario.yaml", "line": 10, "name": "call init", "params": {"ms": 50, "ok": True}}],
                ),
                d["lines"][0].update(file="templates/init.yaml"),
            ),
            "a variable-to-variable watch and failed compared entries": lambda d: (
                failed_step(d)["watch"]["conditions"][0]["compare"][0].update(value=None, variable="Body.other"),
                failed_step(d)["failed"]["conditions"][0].update(
                    compared=[
                        {"variable": "Body.other", "value": 1800, "sample_ms": 20},
                        {"variable": "Body.unseen", "value": None, "sample_ms": None},
                    ]
                ),
            ),
            "a kind of line and a base_source it does not know": lambda d: d["lines"][0].update(kind="hold", base_source="given"),
            "a key of a shape it does not know": lambda d: d["lines"][-1]["shape"].update(steepness=2),
            "a source it does not know": lambda d: d["series"][0].update(source="computed", encoding="minmax"),
            "a third column of a point": lambda d: d["series"][0]["points"].append([10, 1, 2]),
            # A dither: a line of no step and without an end, and the columns of its input's points.
            "a line of no step that has no end": lambda d: d["lines"][0].update(kind="dither", step=None, to_ms=None, base=None,
                                                                              base_source=None),
            "the columns of a series": lambda d: d["series"][0].update(columns=["at_ms", "value", "base"]),
            # Writes that failed without one of them known as the reason, and whether one was applied not known.
            "the writes of a failed step": lambda d: failed_step(d)["failed"].update(
                write=None, writes=[{"variable": "Body.ratio", "at_ms": 20, "value": 0.6, "point": 2, "applied": None},
                                    {"variable": "Body.level", "at_ms": 20, "value": 13, "point": 2, "applied": False}]),
        }
        for what, change in additions.items():
            self.assertEqual(changed(change), [], f"the schema refuses a timeline with {what}")


@needs_launcher
class TimelineOfARun(unittest.TestCase):
    maxDiff = None

    def test_the_timeline_of_the_example_is_valid(self):
        example = Path(EXAMPLE_DIR)
        with tempfile.TemporaryDirectory(prefix="vehicle-sil-timeline-") as directory:
            out = Path(directory) / "throttle_step.timeline.json"
            proc = subprocess.run([LAUNCH, "run", "simulation.yaml", "--scenario", "scenarios/throttle_step.yaml",
                                   "--timeline", str(out)], cwd=example, capture_output=True, text=True,
                                  encoding="utf-8", stdin=subprocess.DEVNULL, timeout=120)
            self.assertEqual(proc.returncode, 0, f"the example's scenario did not pass:\n{proc.stdout[-2000:]}\n"
                                                 f"{proc.stderr[-2000:]}")
            data = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(problems(TIMELINE, data), [],
                         "the timeline file of a run is not valid for schema/timeline.schema.json: correct the schema, "
                         "or the launcher if it writes what docs/simulation-run.md does not say")
        self.assertEqual(data["scenario"]["status"], "passed")
        self.assertEqual({(s["variable"], s["source"]) for s in data["series"]},
                         {("Engine.engine.throttle", "written"), ("Engine.engine.speed", "sampled"),
                          ("Dashboard.dashboard.engine_speed", "sampled")})
        self.assertTrue(all(s["points"] for s in data["series"]))

    def test_the_timeline_of_the_example_with_a_dither_is_valid(self):
        example = Path(EXAMPLE_DIR)
        with tempfile.TemporaryDirectory(prefix="vehicle-sil-timeline-") as directory:
            out = Path(directory) / "throttle_dither.timeline.json"
            proc = subprocess.run([LAUNCH, "run", "simulation.yaml", "--scenario", "scenarios/throttle_dither.yaml",
                                   "--timeline", str(out)], cwd=example, capture_output=True, text=True,
                                  encoding="utf-8", stdin=subprocess.DEVNULL, timeout=120)
            self.assertEqual(proc.returncode, 0, f"the example's scenario did not pass:\n{proc.stdout[-2000:]}\n"
                                                 f"{proc.stderr[-2000:]}")
            data = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(problems(TIMELINE, data), [],
                         "the timeline file of a run with a dither is not valid for schema/timeline.schema.json: correct the "
                         "schema, or the launcher if it writes what docs/simulation-run.md does not say")
        # The dither is a line of no step and without an end, and its input's points have their base.
        dithers = [line for line in data["lines"] if line["kind"] == "dither"]
        self.assertEqual([(line["step"], line["to_ms"], line["shape"] is None) for line in dithers],
                         [(None, None, False), (6, None, True)])
        throttle = next(s for s in data["series"] if s["source"] == "written")
        self.assertEqual(throttle["columns"], ["at_ms", "value", "base"])
        self.assertTrue(all(len(point) == 3 for point in throttle["points"]))

    def test_the_timeline_of_a_template_and_case_run_is_valid(self):
        example = Path(EXAMPLE_DIR).resolve()
        launcher_exe = str(Path(LAUNCH).resolve())
        with tempfile.TemporaryDirectory(prefix="vehicle-sil-timeline-tpl-") as directory:
            work = Path(directory)
            (work / "sim.yaml").write_text(SCENARIO_CONFIG.replace("@DIR@", str(example)), encoding="utf-8", newline="\n")
            templates = work / "scenarios" / "templates"
            templates.mkdir(parents=True)
            (templates / "inner.yaml").write_text(
                "vehicle_sil_scenario_template: 1\n"
                "parameters:\n"
                "  lvl: { type: integer }\n"
                "steps:\n"
                "  - name: skipped absent step\n"
                "    set: { Engine.engine.turbo: 1 }\n"
                "  - name: wait ready\n"
                "    wait_until: { Engine.engine.speed: { ge: 0 } }\n"
                "    timeout_ms: 100\n"
                "  - name: dither throttle\n"
                "    dither: { Engine.engine.throttle: { square: { amplitude: 5, period_ms: 40 } } }\n"
                "  - name: set near edge\n"
                "    set: { Engine.engine.throttle: \"${lvl}\" }\n"
                "  - after_ms: 20\n"
                "    expect: { Engine.engine.speed: { ge: 0 } }\n",
                encoding="utf-8",
                newline="\n",
            )
            (templates / "outer.yaml").write_text(
                "vehicle_sil_scenario_template: 1\n"
                "parameters:\n"
                "  target: { type: integer }\n"
                "steps:\n"
                "  - name: call inner\n"
                "    template: inner.yaml\n"
                "    params: { lvl: \"${target}\" }\n",
                encoding="utf-8",
                newline="\n",
            )
            (work / "scenarios" / "nested_grid.yaml").write_text(
                "vehicle_sil_scenario: 1\n"
                "params: { target: 50 }\n"
                "cases:\n"
                "  c_grid: { params: { target: 99 } }\n"
                "steps:\n"
                "  - name: call outer\n"
                "    template: templates/outer.yaml\n"
                "    params: { target: \"${target}\" }\n",
                encoding="utf-8",
                newline="\n",
            )
            chk = subprocess.run(
                [launcher_exe, "check", "sim.yaml", "--variant", "small", "--scenario", "scenarios/nested_grid.yaml",
                 "--case", "c_grid", "--json"],
                cwd=work, capture_output=True, text=True, encoding="utf-8", stdin=subprocess.DEVNULL, timeout=120,
            )
            self.assertEqual(chk.returncode, 0, f"check failed:\n{chk.stdout[-2000:]}\n{chk.stderr[-2000:]}")
            chk_data = json.loads(chk.stdout)
            after_lines = [
                ln for ln in chk_data["scenario"]["lines"]
                if ln.get("after_file") == "scenarios/templates/inner.yaml" and isinstance(ln.get("after_line"), int)
            ]
            self.assertTrue(after_lines, f"expected after_file/after_line on post-wait template lines: {chk_data['scenario']['lines']}")

            out = work / "nested_grid.timeline.json"
            proc = subprocess.run(
                [launcher_exe, "run", "sim.yaml", "--variant", "small", "--speed", "max",
                 "--scenario", "scenarios/nested_grid.yaml", "--case", "c_grid", "--timeline", str(out)],
                cwd=work, capture_output=True, text=True, encoding="utf-8", stdin=subprocess.DEVNULL, timeout=120,
            )
            self.assertEqual(proc.returncode, 1, f"expected out-of-range dither failure:\n{proc.stdout[-2000:]}\n{proc.stderr[-2000:]}")
            data = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(problems(TIMELINE, data), [],
                         "the timeline file of a template+case run is not valid for schema/timeline.schema.json")
        self.assertEqual(data["scenario"]["case"], "c_grid")
        self.assertTrue(any(st["status"] == "skipped" for st in data["steps"]))
        self.assertTrue(any(
            st.get("file") == "scenarios/templates/inner.yaml" and len(st.get("call_stack", [])) == 2
            for st in data["steps"]
        ))
        self.assertTrue(any(ln.get("file") == "scenarios/templates/inner.yaml" for ln in data["lines"]))
        self.assertTrue(any(s.get("not_applied_from") is not None for s in data["series"]))

    def test_the_timeline_of_variable_comparisons_is_valid(self):
        example = Path(EXAMPLE_DIR)
        with tempfile.TemporaryDirectory(prefix="vehicle-sil-timeline-") as directory:
            out = Path(directory) / "speed_tracking.timeline.json"
            proc = subprocess.run([LAUNCH, "run", "simulation.yaml", "--scenario", "scenarios/speed_tracking.yaml",
                                   "--timeline", str(out)], cwd=example, capture_output=True, text=True,
                                  encoding="utf-8", stdin=subprocess.DEVNULL, timeout=120)
            self.assertEqual(proc.returncode, 0, f"speed_tracking.yaml did not pass:\n{proc.stdout[-2000:]}\n"
                                                 f"{proc.stderr[-2000:]}")
            data = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(problems(TIMELINE, data), [],
                         "the timeline file of speed_tracking.yaml is not valid for schema/timeline.schema.json")
        self.assertEqual(data["scenario"]["status"], "passed")
        var_compares = [
            comp
            for step in data["steps"]
            if step["watch"] is not None
            for cond in step["watch"]["conditions"]
            for comp in cond["compare"]
            if "variable" in comp
        ]
        self.assertGreaterEqual(len(var_compares), 4)
        self.assertTrue(all(comp["value"] is None for comp in var_compares))

    def test_the_timeline_of_a_manual_run_is_valid(self):
        example = Path(EXAMPLE_DIR)
        with tempfile.TemporaryDirectory(prefix="vehicle-sil-timeline-manual-") as directory:
            out = Path(directory) / "manual.timeline.json"
            proc = subprocess.run(
                [LAUNCH, "run", "simulation.yaml", "--stop-at-ms", "200",
                 "--record", "Engine.engine.speed", "--record", "Dashboard.dashboard.engine_speed",
                 "--timeline", str(out)],
                cwd=example, capture_output=True, text=True, encoding="utf-8",
                stdin=subprocess.DEVNULL, timeout=120,
            )
            self.assertEqual(proc.returncode, 0, f"manual timeline run failed:\n{proc.stdout[-2000:]}\n{proc.stderr[-2000:]}")
            data = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(problems(TIMELINE, data), [],
                         "the timeline file of a manual run is not valid for schema/timeline.schema.json")
        self.assertIsNone(data["scenario"])
        self.assertEqual(data["recorded_to_ms"], 200)
        self.assertEqual(data["steps"], [])
        self.assertEqual(data["lines"], [])
        self.assertEqual([v["name"] for v in data["variables"]],
                         ["Engine.engine.speed", "Dashboard.dashboard.engine_speed"])
        self.assertTrue(all(not v["written"] and not v["watched"] and v.get("recorded") is True for v in data["variables"]))
        self.assertTrue(all(s["source"] == "sampled" and s["points"] for s in data["series"]))

    def test_the_timeline_with_record_events_is_valid(self):
        example = Path(EXAMPLE_DIR)
        with tempfile.TemporaryDirectory(prefix="vehicle-sil-timeline-events-") as directory:
            scen_out = Path(directory) / "disconnect.timeline.json"
            proc = subprocess.run(
                [LAUNCH, "run", "simulation.yaml", "--scenario", "scenarios/engine_disconnect.yaml",
                 "--record-events", "--timeline", str(scen_out)],
                cwd=example, capture_output=True, text=True, encoding="utf-8",
                stdin=subprocess.DEVNULL, timeout=120,
            )
            self.assertEqual(proc.returncode, 0, f"engine_disconnect --record-events failed:\n{proc.stdout[-2000:]}\n{proc.stderr[-2000:]}")
            scen_data = json.loads(scen_out.read_text(encoding="utf-8"))

            man_out = Path(directory) / "manual_events.timeline.json"
            proc2 = subprocess.run(
                [LAUNCH, "run", "simulation.yaml", "--stop-at-ms", "300",
                 "--record-events", "--timeline", str(man_out)],
                cwd=example, capture_output=True, text=True, encoding="utf-8",
                stdin=subprocess.DEVNULL, timeout=120,
            )
            self.assertEqual(proc2.returncode, 0, f"manual --record-events failed:\n{proc2.stdout[-2000:]}\n{proc2.stderr[-2000:]}")
            man_data = json.loads(man_out.read_text(encoding="utf-8"))

        self.assertEqual(problems(TIMELINE, scen_data), [],
                         "scenario --record-events timeline is not valid for schema/timeline.schema.json")
        self.assertIsInstance(scen_data.get("events"), list)
        self.assertGreater(len(scen_data["events"]), 0)
        self.assertEqual(scen_data["limits"]["events_per_participant"], 2000)
        self.assertEqual(scen_data["limits"]["events_total"], 5000)
        self.assertEqual(scen_data["limits"]["events_dropped"], [])

        self.assertEqual(problems(TIMELINE, man_data), [],
                         "manual --record-events timeline is not valid for schema/timeline.schema.json")
        self.assertIsNone(man_data["scenario"])
        self.assertEqual(man_data["recorded_to_ms"], 300)
        self.assertEqual(man_data["variables"], [])
        self.assertEqual(man_data["series"], [])
        self.assertGreater(len(man_data["events"]), 0)

        # Synthetic dropped-events entry passes schema, malformed event entry fails schema.
        with_drop = copy.deepcopy(man_data)
        with_drop["limits"]["events_dropped"] = [{
            "participant": "Engine",
            "dropped": 3,
            "dropped_from_ms": 0,
            "dropped_before_ms": 250,
            "agent_dropped": 1,
        }]
        self.assertEqual(problems(TIMELINE, with_drop), [])
        bad_event = copy.deepcopy(man_data)
        bad_event["events"] = [{"at_ms": "not-a-number", "participant": "Engine", "level": "info", "text": "x"}]
        self.assertTrue(problems(TIMELINE, bad_event))

    def test_the_timeline_with_record_can_is_valid(self):
        example = Path(EXAMPLE_DIR)
        with tempfile.TemporaryDirectory(prefix="vehicle-sil-timeline-can-") as directory:
            scen_out = Path(directory) / "disconnect_can.timeline.json"
            proc = subprocess.run(
                [LAUNCH, "run", "simulation.yaml", "--scenario", "scenarios/engine_disconnect.yaml",
                 "--record-can", "--timeline", str(scen_out)],
                cwd=example, capture_output=True, text=True, encoding="utf-8",
                stdin=subprocess.DEVNULL, timeout=120,
            )
            self.assertEqual(proc.returncode, 0, f"engine_disconnect --record-can failed:\n{proc.stdout[-2000:]}\n{proc.stderr[-2000:]}")
            scen_data = json.loads(scen_out.read_text(encoding="utf-8"))

            man_out = Path(directory) / "manual_can.timeline.json"
            proc2 = subprocess.run(
                [LAUNCH, "run", "simulation.yaml", "--stop-at-ms", "350",
                 "--record-can", "--timeline", str(man_out)],
                cwd=example, capture_output=True, text=True, encoding="utf-8",
                stdin=subprocess.DEVNULL, timeout=120,
            )
            self.assertEqual(proc2.returncode, 0, f"manual --record-can failed:\n{proc2.stdout[-2000:]}\n{proc2.stderr[-2000:]}")
            man_data = json.loads(man_out.read_text(encoding="utf-8"))

        self.assertEqual(problems(TIMELINE, scen_data), [],
                         "scenario --record-can timeline is not valid for schema/timeline.schema.json")
        self.assertIsInstance(scen_data.get("can_frames"), list)
        self.assertGreater(len(scen_data["can_frames"]), 0)
        self.assertEqual(scen_data["limits"]["can_frames_per_network"], 10000)
        self.assertEqual(scen_data["limits"]["can_frames_total"], 20000)
        self.assertEqual(scen_data["limits"]["can_dropped"], [])

        self.assertEqual(problems(TIMELINE, man_data), [],
                         "manual --record-can timeline is not valid for schema/timeline.schema.json")
        self.assertIsNone(man_data["scenario"])
        self.assertEqual(man_data["recorded_to_ms"], 350)
        self.assertEqual(man_data["variables"], [])
        self.assertEqual(man_data["series"], [])
        self.assertGreater(len(man_data["can_frames"]), 0)

        # Synthetic can_dropped + optional rtr/fd flags pass schema; malformed can_frames fail schema.
        with_drop = copy.deepcopy(man_data)
        with_drop["can_frames"].append({
            "at_ms": 100,
            "network": "J1939_CAN",
            "id": 0x123,
            "extended": False,
            "dlc": 4,
            "data": "",
            "rtr": True,
        })
        with_drop["can_frames"].append({
            "at_ms": 101,
            "network": "J1939_CAN",
            "id": 0x124,
            "extended": False,
            "dlc": 12,
            "data": "0102030405060708090A0B0C",
            "fd": True,
        })
        with_drop["limits"]["can_dropped"] = [{
            "network": "J1939_CAN",
            "dropped": 5,
            "dropped_from_ms": 0,
            "dropped_before_ms": 250,
            "untimed_dropped": 2,
        }]
        self.assertEqual(problems(TIMELINE, with_drop), [])
        bad_can = copy.deepcopy(man_data)
        bad_can["can_frames"] = [{
            "at_ms": 0,
            "network": "J1939_CAN",
            "id": 0x123,
            "extended": False,
            "dlc": 2,
            "data": "abcd",  # lowercase hex rejected by schema pattern
        }]
        self.assertTrue(problems(TIMELINE, bad_can))


class ControlMetricsSchema(unittest.TestCase):
    maxDiff = None

    @staticmethod
    def _sample_available() -> dict:
        return {
            "vehicle_sil_control_metrics": 1,
            "vehicle_sil": "0.4.0",
            "file": "simulation.yaml",
            "variant": None,
            "status": "available",
            "unavailable_reason": None,
            "source": {
                "transport": "silkit",
                "collector": "HubTransport::stats",
                "scope": "cumulative_hub_transport_lifetime",
                "collection_point": "before_transport_close",
            },
            "run": {
                "result": "passed",
                "end": "completed",
                "simulation": "reached_stop_at",
                "ok": True,
                "sim_ms": 1000,
                "wall_ms": 42,
                "time_step_ms": 1,
                "interrupted_by": None,
            },
            "counters": {
                "messages": 5000,
                "bytes": 412340,
                "handler_calls": 5015,
                "handler_time_ns": 18423900,
                "max_handler_time_ns": 148200,
                "hellos": 5,
                "last_words": 5,
                "late_messages": 0,
                "undecodable": 0,
                "unknown": 0,
                "refused_hellos": 0,
            },
        }

    @staticmethod
    def _sample_unavailable(reason: str = "setup_failed", *, result: str | None = "setup_failed",
                            end: str | None = "setup_failed", simulation: str | None = "not_started",
                            ok: bool = False, time_step_ms: int | None = 10,
                            interrupted_by: str | None = None) -> dict:
        return {
            "vehicle_sil_control_metrics": 1,
            "vehicle_sil": "0.4.0",
            "file": "simulation.yaml",
            "variant": None,
            "status": "unavailable",
            "unavailable_reason": reason,
            "source": {
                "transport": "silkit",
                "collector": "HubTransport::stats",
                "scope": "cumulative_hub_transport_lifetime",
                "collection_point": None,
            },
            "run": {
                "result": result,
                "end": end,
                "simulation": simulation,
                "ok": ok,
                "sim_ms": None,
                "wall_ms": None,
                "time_step_ms": time_step_ms,
                "interrupted_by": interrupted_by,
            },
            "counters": None,
        }

    def test_every_key_is_described_and_required(self):
        for where, node in (
            ("", CONTROL_METRICS),
            ("source.", CONTROL_METRICS["properties"]["source"]),
            ("run.", CONTROL_METRICS["properties"]["run"]),
            ("counters.", CONTROL_METRICS["definitions"]["counters"]),
        ):
            props = node["properties"]
            for key, value in props.items():
                self.assertTrue(value.get("description"), f"control-metrics.schema.json: {where}{key} has no description")
            optional = set(props) - set(node.get("required", []))
            self.assertEqual(optional, set(), f"control-metrics.schema.json: {where or 'top'} does not require {sorted(optional)}")

    def test_valid_available_and_unavailable_variants(self):
        self.assertEqual(problems(CONTROL_METRICS, self._sample_available()), [])

        not_started = self._sample_available()
        not_started["run"].update(result="failed", end="failed", simulation="not_started", ok=False, sim_ms=None, wall_ms=None)
        self.assertEqual(problems(CONTROL_METRICS, not_started), [])

        unavailable_cases = [
            self._sample_unavailable("unreadable", result="unreadable", end=None, simulation=None, time_step_ms=None),
            self._sample_unavailable("check_failed", result="check_failed", end=None, simulation=None, time_step_ms=None),
            self._sample_unavailable("stopped_before_setup", result="stopped", end="stopped", ok=True, interrupted_by="stop command"),
            self._sample_unavailable("interrupted_before_setup", result="interrupted", end="interrupted", interrupted_by="SIGINT"),
            self._sample_unavailable("setup_failed"),
            self._sample_unavailable("internal_error", result=None, end=None, simulation=None, time_step_ms=None),
        ]
        for sample in unavailable_cases:
            self.assertEqual(problems(CONTROL_METRICS, sample), [],
                             f"valid unavailable variant rejected: {sample['unavailable_reason']}")

        close_timeout_unavail = self._sample_unavailable(
            "internal_error", result="passed", end="completed", simulation="reached_stop_at", ok=True, time_step_ms=10)
        close_timeout_unavail["run"].update(sim_ms=1000, wall_ms=42)
        self.assertEqual(problems(CONTROL_METRICS, close_timeout_unavail), [],
                         "valid post-run close-timeout unavailable report rejected")

    def test_invalid_samples_are_rejected(self):
        def check_invalid(sample: dict, what: str):
            self.assertNotEqual(problems(CONTROL_METRICS, sample), [], f"schema accepted invalid sample: {what}")

        bad_ver = self._sample_available()
        bad_ver["vehicle_sil_control_metrics"] = 2
        check_invalid(bad_ver, "wrong format version")

        neg_counter = self._sample_available()
        neg_counter["counters"]["handler_time_ns"] = -1
        check_invalid(neg_counter, "negative handler_time_ns")

        neg_messages = self._sample_available()
        neg_messages["counters"]["messages"] = -1
        check_invalid(neg_messages, "negative messages")

        avail_null_counters = self._sample_available()
        avail_null_counters["counters"] = None
        check_invalid(avail_null_counters, "available with null counters")

        avail_with_reason = self._sample_available()
        avail_with_reason["unavailable_reason"] = "setup_failed"
        check_invalid(avail_with_reason, "available with non-null unavailable_reason")

        avail_null_point = self._sample_available()
        avail_null_point["source"]["collection_point"] = None
        check_invalid(avail_null_point, "available with null collection_point")

        avail_null_result = self._sample_available()
        avail_null_result["run"]["result"] = None
        check_invalid(avail_null_result, "available with null run.result")

        avail_null_end = self._sample_available()
        avail_null_end["run"]["end"] = None
        check_invalid(avail_null_end, "available with null run.end")

        avail_null_sim = self._sample_available()
        avail_null_sim["run"]["simulation"] = None
        check_invalid(avail_null_sim, "available with null run.simulation")

        avail_pre_result = self._sample_available()
        avail_pre_result["run"]["result"] = "setup_failed"
        check_invalid(avail_pre_result, "available with pre-transport run.result")

        avail_pre_end = self._sample_available()
        avail_pre_end["run"]["end"] = "setup_failed"
        check_invalid(avail_pre_end, "available with pre-transport run.end")

        avail_not_started_sim_ms = self._sample_available()
        avail_not_started_sim_ms["run"].update(result="failed", end="failed", simulation="not_started", ok=False, sim_ms=0, wall_ms=None)
        check_invalid(avail_not_started_sim_ms, "available + not_started with non-null sim_ms")

        avail_not_started_wall_ms = self._sample_available()
        avail_not_started_wall_ms["run"].update(result="failed", end="failed", simulation="not_started", ok=False, sim_ms=None, wall_ms=0)
        check_invalid(avail_not_started_wall_ms, "available + not_started with non-null wall_ms")

        unavail_zero_counters = self._sample_unavailable("setup_failed")
        unavail_zero_counters["counters"] = {k: 0 for k in self._sample_available()["counters"]}
        check_invalid(unavail_zero_counters, "unavailable with zero-filled counters")

        unavail_null_reason = self._sample_unavailable("setup_failed")
        unavail_null_reason["unavailable_reason"] = None
        check_invalid(unavail_null_reason, "unavailable with null unavailable_reason")

        unavail_with_point = self._sample_unavailable("setup_failed")
        unavail_with_point["source"]["collection_point"] = "before_transport_close"
        check_invalid(unavail_with_point, "unavailable with before_transport_close collection_point")

        unavail_with_sim_ms = self._sample_unavailable("setup_failed")
        unavail_with_sim_ms["run"]["sim_ms"] = 0
        check_invalid(unavail_with_sim_ms, "unavailable with non-null sim_ms")

        unavail_with_wall_ms = self._sample_unavailable("setup_failed")
        unavail_with_wall_ms["run"]["wall_ms"] = 0
        check_invalid(unavail_with_wall_ms, "unavailable with non-null wall_ms")

        unavail_null_sim_with_sim_ms = self._sample_unavailable("unreadable")
        unavail_null_sim_with_sim_ms["run"]["sim_ms"] = 0
        check_invalid(unavail_null_sim_with_sim_ms, "unavailable unreadable (simulation: null) with non-null sim_ms")

        unavail_null_sim_with_wall_ms = self._sample_unavailable("unreadable")
        unavail_null_sim_with_wall_ms["run"]["wall_ms"] = 0
        check_invalid(unavail_null_sim_with_wall_ms, "unavailable unreadable (simulation: null) with non-null wall_ms")

        zero_step = self._sample_available()
        zero_step["run"]["time_step_ms"] = 0
        check_invalid(zero_step, "time_step_ms == 0")

        missing_counter = self._sample_available()
        missing_counter["counters"].pop("max_handler_time_ns")
        check_invalid(missing_counter, "missing counter field")

        # Forward compatibility: unknown keys are allowed.
        extra = self._sample_available()
        extra["extra_future_key"] = True
        extra["counters"]["future_counter"] = 1
        self.assertEqual(problems(CONTROL_METRICS, extra), [])

    def test_doc_examples_are_valid(self):
        block_re = re.compile(r"```json\n(.*?)\n```", re.DOTALL)
        for rel in ("docs/simulation-run.md", "docs/ja/simulation-run.md"):
            text = (ROOT / rel).read_text(encoding="utf-8")
            blocks = [m.group(1) for m in block_re.finditer(text) if '"vehicle_sil_control_metrics"' in m.group(1)]
            self.assertGreaterEqual(len(blocks), 1, f"{rel} has no vehicle_sil_control_metrics JSON example")
            for block in blocks:
                data = json.loads(block)
                self.assertEqual(problems(CONTROL_METRICS, data), [],
                                 f"documented control-metrics JSON example in {rel} failed schema validation")


@needs_launcher
class ControlMetricsOfARun(unittest.TestCase):
    maxDiff = None

    def test_live_available_and_unavailable_reports_are_valid(self):
        example = Path(EXAMPLE_DIR)
        with tempfile.TemporaryDirectory(prefix="vehicle-sil-control-metrics-") as directory:
            ok_out = Path(directory) / "ok.metrics.json"
            proc = subprocess.run([LAUNCH, "run", "simulation.yaml", "--stop-at-ms", "100",
                                   "--control-metrics", str(ok_out)], cwd=example, capture_output=True, text=True,
                                  encoding="utf-8", stdin=subprocess.DEVNULL, timeout=120)
            self.assertEqual(proc.returncode, 0, f"run --control-metrics failed:\n{proc.stdout[-2000:]}\n{proc.stderr[-2000:]}")
            data = json.loads(ok_out.read_text(encoding="utf-8"))
            self.assertEqual(problems(CONTROL_METRICS, data), [],
                             "live available control-metrics output failed schema/control-metrics.schema.json")
            self.assertEqual(data["status"], "available")
            self.assertIsNone(data["unavailable_reason"])
            c = data["counters"]
            self.assertGreater(c["messages"], 0)
            self.assertGreater(c["bytes"], 0)
            self.assertGreaterEqual(c["handler_calls"], c["messages"])
            self.assertGreaterEqual(c["handler_time_ns"], c["max_handler_time_ns"])
            self.assertGreaterEqual(c["max_handler_time_ns"], 0)

            bad_out = Path(directory) / "unreadable.metrics.json"
            proc_bad = subprocess.run([LAUNCH, "run", "does_not_exist.yaml", "--control-metrics", str(bad_out)],
                                      cwd=example, capture_output=True, text=True, encoding="utf-8",
                                      stdin=subprocess.DEVNULL, timeout=60)
            self.assertEqual(proc_bad.returncode, 3)
            bad_data = json.loads(bad_out.read_text(encoding="utf-8"))
            self.assertEqual(problems(CONTROL_METRICS, bad_data), [],
                             "live unavailable control-metrics output failed schema/control-metrics.schema.json")
            self.assertEqual(bad_data["status"], "unavailable")
            self.assertEqual(bad_data["unavailable_reason"], "unreadable")
            self.assertIsNone(bad_data["counters"])


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
            "network": "vehicle_sil_simulation: 2\nnetworks:\n  - { name: Body, kind: can, zz_probe: 1 }\n" + P,
            "participant": CONFIG_HEAD + P + "    zz_probe: 1\n",
            "variable": CONFIG_HEAD + P + "    variables: { a.b: { from: x, zz_probe: 1 } }\n",
            "variant": CONFIG_HEAD + P + "variants: { small: { zz_probe: 1 } }\n",
            "variant participant": CONFIG_HEAD + P + "variants: { small: { participants: { Tool: { zz_probe: 1 } } } }\n",
            "j1939 node": CONFIG_HEAD + P + "    j1939_nodes: [{ network: Body, zz_probe: 1 }]\n",
            "can external": CONFIG_HEAD.replace("can }", "can, external: { zz_probe: 1 } }") + P,
            "ethernet external": CONFIG_HEAD.replace("can }", "ethernet, external: { zz_probe: 1 } }") + P,
        }
        mappings = simulation_mappings()
        self.assertEqual(set(probes), set(mappings))
        workers = min(os.cpu_count() or 4, 4)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            checked = list(pool.map(lambda item: (item[0], self.launcher.configuration(item[1])), probes.items()))
        for where, errors in checked:
            self.same_keys(where, keys_of(mappings[where]), self.launcher.keys_at(errors),
                           "simulation.schema.json")

    def test_the_keys_of_a_scenario_are_the_launchers(self):
        probes = {
            "top": S + SET + "zz_probe: 1\n",
            "case": S + SET + "cases: { a: { zz_probe: 1 } }\n",
            "step": S + SET + "    zz_probe: 1\n",
            "comparison": S + "  - expect: { Engine.engine.speed: { ge: 0, zz_probe: 1 } }\n",
            "variable reference": S + "  - expect: { Dashboard.dashboard.engine_speed: { variable: Engine.engine.speed, zz_probe: 1 } }\n",
            "ramp": S + "  - ramp: { Engine.engine.throttle: { to: 5, over_ms: 10, zz_probe: 1 } }\n",
            "wave": S + "  - wave: { Engine.engine.throttle: { sine: { amplitude: 1, period_ms: 100 }, for_ms: 100, zz_probe: 1 } }\n",
            **{f"wave {shape}": S + "  - wave: { Engine.engine.throttle: { " + shape + ": { " + keys + ", zz_probe: 1 }, for_ms: 100 } }\n"
               for shape, keys in WAVE_SHAPES_WITH_KEYS.items()},
            "wave points file": S + "  - wave: { Engine.engine.throttle: { points: { file: points.csv, value: throttle, zz_probe: 1 } } }\n",
            "input": "vehicle_sil_scenario: 1\ninputs: { Engine.engine.throttle: { dither: { triangle: { amplitude: 1, period_ms: 100 } }, "
                     "zz_probe: 1 } }\nsteps:\n" + SET,
            "dither": S + "  - dither: { Engine.engine.throttle: { triangle: { amplitude: 1, period_ms: 100 }, zz_probe: 1 } }\n",
        }
        mappings = scenario_mappings()
        self.assertEqual(set(probes), set(mappings))
        workers = min(os.cpu_count() or 4, 4)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            checked = list(pool.map(lambda item: (item[0], self.launcher.scenario(item[1])), probes.items()))
        for where, errors in checked:
            self.same_keys(where, keys_of(mappings[where]), self.launcher.keys_at(errors),
                           "scenario.schema.json")

    def test_the_keys_of_a_template_are_the_launchers(self):
        probes = {
            "template top": "vehicle_sil_scenario_template: 1\nzz_probe: 1\nsteps:\n" + SET,
            "template parameter": "vehicle_sil_scenario_template: 1\nparameters:\n  p:\n    type: integer\n    zz_probe: 1\n"
                                  "steps:\n  - set: { Engine.engine.throttle: \"${p}\" }\n",
        }
        mappings = template_mappings()
        self.assertEqual(set(probes), set(mappings))
        for where, text in probes.items():
            self.same_keys(where, keys_of(mappings[where]), self.launcher.keys_at(self.launcher.template(text)),
                           "scenario-template.schema.json")

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
        workers = min(os.cpu_count() or 4, 4)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            for name, samples, ask in (("configuration", CONFIG_SAMPLES, self.launcher.configuration),
                                       ("scenario", SCENARIO_SAMPLES, self.launcher.scenario),
                                       ("template", TEMPLATE_SAMPLES, self.launcher.template)):
                results = list(pool.map(lambda sample: (sample, ask(sample[1])), samples))
                for (what, text, verdict), raw_errors in results:
                    errors = [f"{e['code']}: {e['message']}" for e in raw_errors]
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
                   "scenario-template.schema.json": SCHEMA_DIR,
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


class BenchmarkReportSchema(unittest.TestCase):
    maxDiff = None

    @staticmethod
    def _sample_report() -> dict:
        target_flags = {
            "compiler_id": "GNU",
            "compiler_version": "14.2.0",
            "compiler_banner": "g++ (GCC) 14.2.0",
            "optimization_flags": ["-O3"],
            "effective_opt_level": "-O3",
            "codegen_and_defines": ["-DNDEBUG"],
            "sanitizers": [],
            "ndebug": True,
        }
        return {
            "$schema": "../schema/benchmark-report.schema.json",
            "vehicle_sil_benchmark": 1,
            "generated_at_utc": "2026-11-27T01:00:00Z",
            "execution_backend": "silkit_coordinated_multiprocess",
            "interpretation_contract": {
                "local_bench_substitutable": False,
                "process_count_equals_participant_count_assumed": False,
                "wall_cpu_handler_summable": False,
                "notes": [
                    "Coordinated SIL Kit multi-process results cannot be substituted by LocalBench.",
                    "ecu_count, silkit_participant_count, and os_process_count are distinct quantities.",
                    "steady_state.wall_ms, process_cpu.*, and handler_time_ns must never be summed.",
                    "Canonical cases use silkit_trivial CAN with network_simulator: null.",
                ],
            },
            "provenance": {
                "repo": {
                    "git_commit": "0" * 40,
                    "git_dirty": False,
                    "vehicle_sil_version": "0.4.0",
                    "launcher_features": ["control-metrics"],
                },
                "build": {
                    "build_dir": "build/linux-gcc-release",
                    "cmake_build_type": "Release",
                    "flags_source": "compile_commands.json",
                    "flags_warning": None,
                    "targets": {
                        "vehicle_sil_launch": copy.deepcopy(target_flags),
                        "vehicle_sil_core": copy.deepcopy(target_flags),
                        "vehicle_sil_bench_ecu": copy.deepcopy(target_flags),
                    },
                    "bench_ecu_build_info": {
                        "compiler_id": "GNU",
                        "compiler_version": "14.2.0",
                        "ndebug": True,
                        "optimize_macro": 1,
                        "sanitizers": [],
                    },
                },
                "dependencies": {
                    "sil_kit": {
                        "submodule_commit": "1" * 40,
                        "tag": "v5.0.8",
                        "patches": [{"name": "0001-example.patch", "sha256": "a" * 64}],
                        "static_linked": False,
                    },
                    "control_server_built": False,
                },
                "host": {
                    "os": "Linux",
                    "os_release": "6.12.0",
                    "arch": "x86_64",
                    "cpu_model": "Test CPU",
                    "cpu_logical_count": 8,
                    "python_version": "3.13.0",
                },
            },
            "run_options": {
                "profile": "smoke",
                "warmup_iterations": 0,
                "measured_iterations": 1,
                "control_metrics_mode": "auto",
                "busy_machine_wait": False,
            },
            "cases": [
                {
                    "id": "case_c_5ecu_mixed",
                    "description": "5 ECUs mixed rates (10 ms x 1 + 1 ms x 4)",
                    "topology": {
                        "ecu_count": 5,
                        "silkit_participant_count": 6,
                        "os_process_count": 6,
                        "silkit_registry_hosted_in": "vehicle_sil_launch",
                        "controller_hosted_in": "vehicle_sil_launch",
                        "ecus": [
                            {
                                "name": "ecu0",
                                "step_ms": 10,
                                "configured_sample_period_ms": 10,
                                "effective_sample_period_ms": 10,
                                "sample_every_step_active": False,
                                "can_period_ms": 10,
                                "can_offset_ms": 0,
                            }
                        ],
                    },
                    "workload": {
                        "speed": "max",
                        "time_step_ms_configured": None,
                        "time_step_ms_resolved": 1,
                        "warmup_sim_ms": 50,
                        "measure_sim_ms": 200,
                        "tail_sim_ms": 10,
                        "stop_at_ms": 260,
                        "declared_vars_per_ecu": 250,
                        "wire_vars_per_ecu": 254,
                        "step_work": 0,
                        "scenario": None,
                        "communication_load": {
                            "can_network": "BENCH_CAN",
                            "can_simulation_mode": "silkit_trivial",
                            "network_simulator": None,
                            "can_tx_frames_per_sim_s": 500,
                            "can_rx_frames_per_sim_s": 2000,
                            "control_samples_per_sim_s": 500,
                        },
                    },
                    "measurement_boundaries": {
                        "steady_state_window_sim_ms": [50, 250],
                        "excluded_from_steady_state": ["setup", "warmup_iterations", "in_run_warmup_0_to_50ms", "tail_250_to_260ms", "hold_paused", "shutdown"],
                    },
                    "warmup_iterations": [],
                    "iterations": [
                        {
                            "index": 0,
                            "phase": "measured",
                            "steady_state": {
                                "wall_ns": 12_500_000,
                                "wall_ms": 12.5,
                                "window_skew_ns": 1000,
                                "virtual_to_realtime_ratio": 16.0,
                                "completed_steps": {
                                    "controller_window_steps": 200,
                                    "ecu_window_steps_total": 820,
                                    "ecu_window_steps_by_participant": {"ecu0": 20},
                                },
                                "app_messages": {
                                    "tx_in_window": 100,
                                    "rx_in_window": 400,
                                    "tx_total": 125,
                                    "rx_total": 500,
                                    "tx_dropped": 0,
                                    "rx_dropped": 0,
                                },
                            },
                            "non_steady_state": {
                                "total_process_wall_ms": 80.0,
                                "launcher_run_wall_ms": 18.0,
                                "setup_and_shutdown_wall_ms": 62.0,
                                "paused_ms": 0,
                            },
                            "process_cpu": {
                                "ecu_steady_state": {
                                    "status": "available",
                                    "reason": None,
                                    "interval": "steady_state_window",
                                    "window_sim_ms": [50, 250],
                                    "target_processes": ["ecu0"],
                                    "excludes": ["vehicle_sil_launch"],
                                    "user_ms": 4.0,
                                    "sys_ms": 1.0,
                                    "total_ms": 5.0,
                                    "per_process": [
                                        {"name": "ecu0", "status": "available", "reason": None, "user_ms": 4.0, "sys_ms": 1.0, "total_ms": 5.0}
                                    ],
                                },
                                "launcher_tree_total": {
                                    "status": "unavailable",
                                    "reason": "os.wait4 not available on this platform",
                                    "interval": "full_process_lifetime",
                                    "includes_non_steady_state": True,
                                    "target_processes": ["vehicle_sil_launch", "<reaped_children>"],
                                    "user_ms": None,
                                    "sys_ms": None,
                                    "total_ms": None,
                                    "max_rss_kb": None,
                                },
                            },
                            "control_transport_metrics": {
                                "status": "unavailable",
                                "unavailable_reason": "stopped_before_setup",
                                "source": None,
                                "counters": None,
                            },
                            "determinism": {
                                "verified": True,
                                "output_sha256": "b" * 64,
                                "comm_trace_sha256": "c" * 64,
                                "comm_events_verified": 625,
                            },
                            "environment_noise": {
                                "waited_s": 0.0,
                                "others_before": [],
                                "others_after": [],
                                "load1_before": None,
                                "load1_after": None,
                            },
                        }
                    ],
                    "summary": {
                        "iteration_count": 1,
                        "steady_state_wall_ms": {"median": 12.5, "min": 12.5, "max": 12.5, "iqr": 0.0},
                        "virtual_to_realtime_ratio": {"median": 16.0, "min": 16.0, "max": 16.0},
                        "ecu_steady_state_cpu_ms": {"status": "available", "median": 5.0, "min": 5.0, "max": 5.0},
                        "launcher_tree_cpu_ms": {"status": "unavailable", "median": None, "min": None, "max": None},
                        "control_transport_metrics": {
                            "status": "unavailable",
                            "scope": None,
                            "handler_time_ns": None,
                            "max_handler_time_ns": None,
                            "messages": None,
                            "bytes": None,
                            "handler_calls": None,
                        },
                        "determinism": {
                            "all_iterations_identical": True,
                            "output_sha256": "b" * 64,
                            "comm_trace_sha256": "c" * 64,
                        },
                    },
                }
            ],
        }

    def test_valid_sample_and_invalid_mutations(self):
        good = self._sample_report()
        self.assertEqual(problems(BENCHMARK_REPORT, good), [])

        def mutate(fn) -> list[str]:
            data = copy.deepcopy(good)
            fn(data)
            return problems(BENCHMARK_REPORT, data)

        # Anti-misinterpretation contracts must be strictly enforced.
        self.assertNotEqual(mutate(lambda d: d["interpretation_contract"].update(local_bench_substitutable=True)), [])
        self.assertNotEqual(mutate(lambda d: d["interpretation_contract"].update(wall_cpu_handler_summable=True)), [])
        self.assertNotEqual(mutate(lambda d: d.update(execution_backend="local_bench")), [])
        # Trivial CAN / NetworkSimulator provenance must be enforced.
        self.assertNotEqual(
            mutate(lambda d: d["cases"][0]["workload"]["communication_load"].update(can_simulation_mode="netsim")), []
        )
        self.assertNotEqual(
            mutate(lambda d: d["cases"][0]["workload"]["communication_load"].update(network_simulator="NetworkSimulator")),
            [],
        )
        # Conditional unavailable -> null rules must reject false 0.
        self.assertNotEqual(
            mutate(lambda d: d["cases"][0]["iterations"][0]["process_cpu"]["launcher_tree_total"].update(total_ms=0.0)),
            [],
        )
        self.assertNotEqual(
            mutate(lambda d: d["cases"][0]["summary"]["launcher_tree_cpu_ms"].update(median=0.0)),
            [],
        )
        # flags_source == "unavailable" must force null target opt/flags.
        self.assertNotEqual(
            mutate(lambda d: d["provenance"]["build"].update(flags_source="unavailable")),
            [],
        )
        # ecu_steady_state == "available" must reject unavailable per_process entries.
        self.assertNotEqual(
            mutate(
                lambda d: d["cases"][0]["iterations"][0]["process_cpu"]["ecu_steady_state"]["per_process"][0].update(
                    status="unavailable", reason="err", user_ms=None, sys_ms=None, total_ms=None
                )
            ),
            [],
        )
        # Phase const enforcement on warmup_iterations vs iterations.
        self.assertNotEqual(
            mutate(lambda d: d["cases"][0]["iterations"][0].update(phase="warmup")),
            [],
        )
        self.assertNotEqual(
            mutate(
                lambda d: d["cases"][0]["warmup_iterations"].append(
                    copy.deepcopy(d["cases"][0]["iterations"][0])
                )
            ),
            [],
        )
        # Dropped CAN counters must always be 0 on verified benchmark reports.
        self.assertNotEqual(
            mutate(lambda d: d["cases"][0]["iterations"][0]["steady_state"]["app_messages"].update(tx_dropped=1)),
            [],
        )
        self.assertNotEqual(
            mutate(lambda d: d["cases"][0]["iterations"][0]["steady_state"]["app_messages"].update(rx_dropped=1)),
            [],
        )
        # Unknown properties must be refused.
        self.assertNotEqual(mutate(lambda d: d.update(extra_field=123)), [])

    def test_generated_report_from_benchmark_vtime_is_valid(self):
        import importlib.util

        spec = importlib.util.spec_from_file_location("benchmark_vtime_schema_check", ROOT / "scripts" / "benchmark-vtime.py")
        assert spec is not None and spec.loader is not None
        bv = importlib.util.module_from_spec(spec)
        sys.modules["benchmark_vtime_schema_check"] = bv
        spec.loader.exec_module(bv)

        case = bv.ALL_CASES["case_a_1ecu_1ms"]
        ecu_summary = {
            "name": "ecu0",
            "configured_sample_period_ms": 10,
            "effective_sample_period_ms": 10,
            "sample_every_step_active": False,
        }

        def make_iter(phase: str, idx: int) -> dict:
            return {
                "index": idx,
                "phase": phase,
                "steady_state": {
                    "wall_ns": 10_000_000,
                    "wall_ms": 10.0,
                    "window_skew_ns": 0,
                    "virtual_to_realtime_ratio": 20.0,
                    "completed_steps": {
                        "controller_window_steps": 200,
                        "ecu_window_steps_total": 200,
                        "ecu_window_steps_by_participant": {"ecu0": 200},
                    },
                    "app_messages": {
                        "tx_in_window": 20,
                        "rx_in_window": 0,
                        "tx_total": 25,
                        "rx_total": 0,
                        "tx_dropped": 0,
                        "rx_dropped": 0,
                    },
                },
                "non_steady_state": {
                    "total_process_wall_ms": 40.0,
                    "launcher_run_wall_ms": 12.0,
                    "setup_and_shutdown_wall_ms": 28.0,
                    "paused_ms": 0,
                },
                "process_cpu": {
                    "ecu_steady_state": {
                        "status": "available",
                        "reason": None,
                        "interval": "steady_state_window",
                        "window_sim_ms": [50, 250],
                        "target_processes": ["ecu0"],
                        "excludes": ["vehicle_sil_launch"],
                        "user_ms": 3.0,
                        "sys_ms": 1.0,
                        "total_ms": 4.0,
                        "per_process": [
                            {"name": "ecu0", "status": "available", "reason": None, "user_ms": 3.0, "sys_ms": 1.0, "total_ms": 4.0}
                        ],
                    },
                    "launcher_tree_total": {
                        "status": "unavailable",
                        "reason": "os.wait4 not available on this platform",
                        "interval": "full_process_lifetime",
                        "includes_non_steady_state": True,
                        "target_processes": ["vehicle_sil_launch", "<reaped_children>"],
                        "user_ms": None,
                        "sys_ms": None,
                        "total_ms": None,
                        "max_rss_kb": None,
                    },
                },
                "control_transport_metrics": {
                    "status": "unavailable",
                    "unavailable_reason": "stopped_before_setup",
                    "source": None,
                    "counters": None,
                },
                "determinism": {
                    "verified": True,
                    "output_sha256": "a" * 64,
                    "comm_trace_sha256": "b" * 64,
                    "comm_events_verified": 25,
                },
                "environment_noise": {
                    "waited_s": 0.0,
                    "others_before": [],
                    "others_after": [],
                    "load1_before": None,
                    "load1_after": None,
                },
            }

        case_rep = bv.build_case_report(
            case,
            50,
            200,
            10,
            warmup_records=[make_iter("warmup", 0)],
            measured_records=[make_iter("measured", 0), make_iter("measured", 1)],
            last_ecu_summaries={"ecu0": ecu_summary},
        )
        build_prov, _ = bv.extract_build_provenance(None, None, is_windows=False)
        generated = {
            "$schema": "../schema/benchmark-report.schema.json",
            "vehicle_sil_benchmark": bv.BENCHMARK_FORMAT_VERSION,
            "generated_at_utc": "2026-11-27T01:00:00Z",
            "execution_backend": "silkit_coordinated_multiprocess",
            "interpretation_contract": {
                "local_bench_substitutable": False,
                "process_count_equals_participant_count_assumed": False,
                "wall_cpu_handler_summable": False,
                "notes": list(bv.INTERPRETATION_NOTES),
            },
            "provenance": {
                "repo": {"git_commit": None, "git_dirty": None, "vehicle_sil_version": "0.4.0", "launcher_features": []},
                "build": build_prov,
                "dependencies": {
                    "sil_kit": {"submodule_commit": None, "tag": None, "patches": [], "static_linked": None},
                    "control_server_built": False,
                },
                "host": bv._collect_host_provenance(),
            },
            "run_options": {
                "profile": "smoke",
                "warmup_iterations": 1,
                "measured_iterations": 2,
                "control_metrics_mode": "auto",
                "busy_machine_wait": False,
            },
            "cases": [case_rep],
        }
        self.assertEqual(problems(BENCHMARK_REPORT, generated), [])

        # Ambiguous compile_commands.json (both partial and all-targets -> unavailable with non-null warning)
        # must also conform to schema/benchmark-report.schema.json.
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            cc_partial = [
                {"directory": tmp, "file": f"{tmp}/tools/launch/main.cpp", "command": "/usr/bin/g++ -O3 -DNDEBUG -c main.cpp"},
                {"directory": tmp, "file": f"{tmp}/tools/launch/main.cpp", "command": "/usr/bin/g++ -O0 -c main.cpp"},
                {"directory": tmp, "file": f"{tmp}/libs/core/src/participant_runtime.cpp", "command": "/usr/bin/g++ -O3 -DNDEBUG -c pr.cpp"},
                {"directory": tmp, "file": f"{tmp}/tools/bench_ecu/main.cpp", "command": "/usr/bin/g++ -O3 -DNDEBUG -c main.cpp"},
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc_partial), encoding="utf-8")
            (bdir / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
            partial_prov, _ = bv.extract_build_provenance(bdir, None, is_windows=False)
            self.assertEqual(partial_prov["flags_source"], "compile_commands.json")
            self.assertIsNotNone(partial_prov["flags_warning"])
            partial_report = copy.deepcopy(generated)
            partial_report["provenance"]["build"] = partial_prov
            self.assertEqual(problems(BENCHMARK_REPORT, partial_report), [])

            cc_all_ambiguous = [
                {"directory": tmp, "file": f"{tmp}/tools/launch/main.cpp", "command": "/usr/bin/g++ -O3 -DNDEBUG -c main.cpp"},
                {"directory": tmp, "file": f"{tmp}/tools/launch/main.cpp", "command": "/usr/bin/g++ -O0 -c main.cpp"},
                {"directory": tmp, "file": f"{tmp}/libs/core/src/participant_runtime.cpp", "command": "/usr/bin/g++ -O3 -DNDEBUG -c pr.cpp"},
                {"directory": tmp, "file": f"{tmp}/libs/core/src/participant_runtime.cpp", "command": "/usr/bin/g++ -O0 -c pr.cpp"},
                {"directory": tmp, "file": f"{tmp}/tools/bench_ecu/main.cpp", "command": "/usr/bin/g++ -O3 -DNDEBUG -c main.cpp"},
                {"directory": tmp, "file": f"{tmp}/tools/bench_ecu/main.cpp", "command": "/usr/bin/g++ -O0 -c main.cpp"},
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc_all_ambiguous), encoding="utf-8")
            all_amb_prov, _ = bv.extract_build_provenance(bdir, None, is_windows=False)
            self.assertEqual(all_amb_prov["flags_source"], "unavailable")
            self.assertIsNotNone(all_amb_prov["flags_warning"])
            all_amb_report = copy.deepcopy(generated)
            all_amb_report["provenance"]["build"] = all_amb_prov
            self.assertEqual(problems(BENCHMARK_REPORT, all_amb_report), [])

            # Missing @response_file entries (both all-unreadable -> unavailable and partial -> compile_commands.json)
            # must also conform to schema/benchmark-report.schema.json.
            cc_all_unreadable_rsp = [
                {"directory": tmp, "file": f"{tmp}/tools/launch/main.cpp", "command": "/usr/bin/g++ @debug.rsp -c main.cpp"},
                {"directory": tmp, "file": f"{tmp}/tools/launch/main.cpp", "command": "/usr/bin/g++ @release.rsp -c main.cpp"},
                {"directory": tmp, "file": f"{tmp}/libs/core/src/participant_runtime.cpp", "command": "/usr/bin/g++ @debug.rsp -c pr.cpp"},
                {"directory": tmp, "file": f"{tmp}/libs/core/src/participant_runtime.cpp", "command": "/usr/bin/g++ @release.rsp -c pr.cpp"},
                {"directory": tmp, "file": f"{tmp}/tools/bench_ecu/main.cpp", "command": "/usr/bin/g++ @debug.rsp -c main.cpp"},
                {"directory": tmp, "file": f"{tmp}/tools/bench_ecu/main.cpp", "command": "/usr/bin/g++ @release.rsp -c main.cpp"},
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc_all_unreadable_rsp), encoding="utf-8")
            all_rsp_prov, _ = bv.extract_build_provenance(bdir, None, is_windows=False)
            self.assertEqual(all_rsp_prov["flags_source"], "unavailable")
            self.assertIsNotNone(all_rsp_prov["flags_warning"])
            all_rsp_report = copy.deepcopy(generated)
            all_rsp_report["provenance"]["build"] = all_rsp_prov
            self.assertEqual(problems(BENCHMARK_REPORT, all_rsp_report), [])

            cc_partial_unreadable_rsp = [
                {"directory": tmp, "file": f"{tmp}/tools/launch/main.cpp", "command": "/usr/bin/g++ @missing.rsp -c main.cpp"},
                {"directory": tmp, "file": f"{tmp}/libs/core/src/participant_runtime.cpp", "command": "/usr/bin/g++ -O3 -DNDEBUG -c pr.cpp"},
                {"directory": tmp, "file": f"{tmp}/tools/bench_ecu/main.cpp", "command": "/usr/bin/g++ -O3 -DNDEBUG -c main.cpp"},
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc_partial_unreadable_rsp), encoding="utf-8")
            partial_rsp_prov, _ = bv.extract_build_provenance(bdir, None, is_windows=False)
            self.assertEqual(partial_rsp_prov["flags_source"], "compile_commands.json")
            self.assertIsNotNone(partial_rsp_prov["flags_warning"])
            partial_rsp_report = copy.deepcopy(generated)
            partial_rsp_report["provenance"]["build"] = partial_rsp_prov
            self.assertEqual(problems(BENCHMARK_REPORT, partial_rsp_report), [])


if __name__ == "__main__":
    if "--sync" in sys.argv[1:]:
        changed = sync_schemas(ROOT)
        if changed:
            print("synced:", ", ".join(changed))
        else:
            print("schemas are already in sync")
        sys.exit(0)
    unittest.main(verbosity=2)
