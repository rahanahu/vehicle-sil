"""The dashboard file next to a configuration (<name>.dashboard.yaml): read it safely, check its shape, and give
the screen plain JSON; and the panel modules it names (panels/*.js, *.mjs), which the server serves.

Reading: at most MAX_FILE_BYTES; YAML with the safe loader; aliases are refused (a few lines of them can expand to
an enormous document), nesting is limited (MAX_DEPTH), so is the number of nodes (MAX_NODES); keys must not repeat.
Whether a variable exists and whether a widget fits it is checked by the screen against the run's variables
(it knows them; this server does not when no run is going).

Format version 1 (gui/protocol/dashboard.schema.json, docs/gui.md "ダッシュボードのファイル"):

    vehicle_sil_dashboard: 1
    title: Engine bench                      # optional
    panels: [panels/rpm-bar.js]              # optional: modules that define the panels' custom elements
    sections:
      - title: Engine
        collapsed: false                     # optional
        items:
          - variable: Engine.engine.throttle # <participant>.<variable>: up to the first '.' is the participant
            widget: slider                   # optional (the default widget of the variable)
            label: Throttle                  # optional
            options: {step: 5}               # optional, per widget
          - panel: rpm-bar                   # a custom element defined by one of the panels
            variables: [Engine.engine.speed] # optional
            options: {max: 4000}             # optional
          - auto: Engine                     # the participant's variables not placed elsewhere in the file
"""

from __future__ import annotations

import math
import re
from urllib.parse import quote
from dataclasses import dataclass, field
from pathlib import Path

import yaml

from .paths import ConfigFile, ConfigRoot, PathRejected

FORMAT = 1
SUFFIX = ".dashboard.yaml"
MAX_FILE_BYTES = 256 * 1024
MAX_DEPTH = 16  # nested mappings and sequences
MAX_NODES = 50_000  # scalars, mappings and sequences in all
MAX_SECTIONS = 200
MAX_ITEMS = 2000  # in all sections
MAX_PANELS = 16
MAX_PANEL_VARIABLES = 64
MAX_TEXT = 1024  # characters of a title, a label, a name
MAX_ERRORS = 50
MODULE_SUFFIXES = (".js", ".mjs")
MAX_MODULE_BYTES = 1024 * 1024
PANELS_DIR = "panels"
# A custom element name (ASCII only): a lowercase letter first, and a hyphen.
ELEMENT_NAME = re.compile(r"^[a-z][a-z0-9._]*-[a-z0-9._-]*$")
# The shape of a widget kind's name (which kinds there are, the screen knows: it says so in place).
WIDGET_NAME = re.compile(r"^[a-z][a-z0-9-]*$")

Loader = getattr(yaml, "CSafeLoader", yaml.SafeLoader)


class DashboardError(Exception):
    """One mistake in the file: where (line and column from 1, the place in the data) and what to do."""

    def __init__(self, line: int, column: int, at: str, message: str):
        super().__init__(message)
        self.line, self.column, self.at, self.message = line, column, at, message

    def to_json(self) -> dict:
        return {"line": self.line, "column": self.column, "at": self.at, "message": self.message}


@dataclass
class Result:
    """What GET api/dashboard answers: the file's place, whether it exists, its errors, or its contents."""
    path: str
    found: bool = False
    dashboard: dict | None = None
    errors: list[DashboardError] = field(default_factory=list)

    def to_json(self, config: str) -> dict:
        return {"file": config, "path": self.path, "found": self.found,
                "errors": [e.to_json() for e in self.errors], "dashboard": self.dashboard}


def dashboard_path(config: ConfigFile) -> str:
    """The root-relative name of a configuration's dashboard file: simulation.yaml -> simulation.dashboard.yaml."""
    directory, _, name = config.relative.rpartition("/")
    stem = name.rsplit(".", 1)[0]
    return f"{directory}/{stem}{SUFFIX}" if directory else f"{stem}{SUFFIX}"


def module_path(root: ConfigRoot, relative: object) -> Path:
    """A panel module that may be served: a .js or .mjs file in a directory named panels under the root (also
    after following links), at most MAX_MODULE_BYTES. Raises PathRejected otherwise."""
    parts = root.parts(relative)
    if any(p.startswith(".") for p in parts):
        raise PathRejected("invalid-path", f"{relative!r} has a part that starts with '.': panel modules and their "
                                           "directories are not hidden files")
    if not parts[-1].lower().endswith(MODULE_SUFFIXES):
        raise PathRejected("not-a-module", f"{relative!r} is not a .js or .mjs file")
    places = [i for i, p in enumerate(parts[:-1]) if p == PANELS_DIR]
    if not places:
        raise PathRejected("not-a-module", f"{relative!r} is not in a directory named {PANELS_DIR}: put panel "
                                           f"modules in {PANELS_DIR}/ next to the dashboard file")
    named = root.root / Path(*parts[:places[-1] + 1])
    try:
        directory = named.resolve(strict=True)
        absolute = (root.root / Path(*parts)).resolve(strict=True)
    except (OSError, RuntimeError, ValueError):
        raise PathRejected("file-not-found", f"{relative!r} does not exist under the root ({root.root})") from None
    if directory != named:
        raise PathRejected("outside-root", f"{relative!r}: its {PANELS_DIR} directory, or a directory above it, is "
                                           "a symbolic link: make it a directory of its own")
    if not absolute.is_relative_to(directory):
        raise PathRejected("outside-root", f"{relative!r} leads out of its {PANELS_DIR} directory through a "
                                           "symbolic link: put the module itself there")
    if not absolute.name.lower().endswith(MODULE_SUFFIXES):
        raise PathRejected("not-a-module", f"{relative!r} is a link to {absolute.name}, which is not a .js or "
                                           ".mjs file")
    if not absolute.is_file():
        raise PathRejected("not-a-module", f"{relative!r} is not a regular file")
    if absolute.stat().st_size > MAX_MODULE_BYTES:
        raise PathRejected("too-large", f"{relative!r} is larger than {MAX_MODULE_BYTES // 1024} KiB: a panel "
                                        "module must be smaller")
    return absolute


def read_dashboard(root: ConfigRoot, config: ConfigFile, panel_base: str) -> Result:
    """Reads the dashboard file of a configuration. panel_base: the URL path the panel modules are served under
    (`panels/<key>/`), followed by their root-relative names."""
    relative = dashboard_path(config)
    result = Result(relative)
    try:
        absolute = root.resolve(relative)
    except PathRejected as e:
        if e.code == "file-not-found":
            return result
        result.found = True
        result.errors.append(DashboardError(1, 1, "", f"{relative}: {e}"))
        return result
    result.found = True
    try:
        with open(absolute.absolute, "rb") as f:
            data = f.read(MAX_FILE_BYTES + 1)
    except OSError as e:
        result.errors.append(DashboardError(1, 1, "", f"{relative} cannot be read ({e.strerror or e})"))
        return result
    if len(data) > MAX_FILE_BYTES:
        result.errors.append(DashboardError(1, 1, "", f"{relative} is larger than {MAX_FILE_BYTES // 1024} KiB: "
                                                      "a dashboard file must be smaller (use auto: for the rest "
                                                      "of a participant's variables)"))
        return result
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as e:
        result.errors.append(DashboardError(1, 1, "", f"{relative} is not UTF-8 (byte {e.start}): save it as "
                                                      "UTF-8"))
        return result
    try:
        node = _compose(text)
    except DashboardError as e:
        result.errors.append(e)
        return result
    checker = _Checker(root, relative.rpartition("/")[0], panel_base)
    dashboard = checker.top(node)
    result.errors = checker.errors
    if not checker.errors:
        result.dashboard = dashboard
    return result


# ------------------------------------------------------------ YAML


def _mark(error: yaml.MarkedYAMLError) -> tuple[int, int]:
    mark = error.problem_mark or error.context_mark
    return (mark.line + 1, mark.column + 1) if mark else (1, 1)


def _compose(text: str) -> yaml.Node | None:
    """The document as a node tree, after looking at its events: no aliases, not too deep, not too many nodes,
    one document."""
    depth = nodes = documents = 0
    try:
        for event in yaml.parse(text, Loader=Loader):
            if isinstance(event, yaml.AliasEvent):
                m = event.start_mark
                raise DashboardError(m.line + 1, m.column + 1, "",
                                     f"*{event.anchor}: aliases are not used in a dashboard file: write the value "
                                     "out where it is needed")
            if isinstance(event, yaml.DocumentStartEvent):
                documents += 1
                if documents > 1:
                    m = event.start_mark
                    raise DashboardError(m.line + 1, m.column + 1, "", "a second document (---): a dashboard "
                                                                       "file has one")
            if isinstance(event, (yaml.MappingStartEvent, yaml.SequenceStartEvent)):
                depth += 1
                if depth > MAX_DEPTH:
                    m = event.start_mark
                    raise DashboardError(m.line + 1, m.column + 1, "", f"nested more than {MAX_DEPTH} levels "
                                                                       "deep: a dashboard is not")
            elif isinstance(event, (yaml.MappingEndEvent, yaml.SequenceEndEvent)):
                depth -= 1
            if isinstance(event, yaml.ScalarEvent) and len(event.value) > MAX_TEXT:
                m = event.start_mark
                raise DashboardError(m.line + 1, m.column + 1, "", f"a value longer than {MAX_TEXT} characters: "
                                                                   "shorten it")
            if isinstance(event, (yaml.ScalarEvent, yaml.MappingStartEvent, yaml.SequenceStartEvent)):
                nodes += 1
                if nodes > MAX_NODES:
                    m = event.start_mark
                    raise DashboardError(m.line + 1, m.column + 1, "", f"more than {MAX_NODES} values: a "
                                                                       "dashboard file must be smaller")
        return yaml.compose(text, Loader=Loader)
    except yaml.MarkedYAMLError as e:
        line, column = _mark(e)
        raise DashboardError(line, column, "", f"not YAML: {e.problem or e}") from None
    except yaml.YAMLError as e:
        raise DashboardError(1, 1, "", f"not YAML: {e}") from None


def _scalar(node: yaml.ScalarNode, constructor: yaml.SafeLoader) -> object:
    """The value of a scalar node (str, int, float, bool, None; a timestamp is a datetime)."""
    try:
        return constructor.construct_object(node)
    except (yaml.YAMLError, ValueError) as e:
        line, column = node.start_mark.line + 1, node.start_mark.column + 1
        raise DashboardError(line, column, "", f"{node.tag}: {getattr(e, 'problem', None) or e}: write a plain "
                                               "value") from None


# ------------------------------------------------------------ the shape


class _Stop(Exception):
    """Too many errors: stop looking."""


class _Checker:
    def __init__(self, root: ConfigRoot, directory: str, panel_base: str):
        self.root = root
        self.directory = directory  # of the dashboard file, root-relative ('' at the root)
        self.panel_base = panel_base
        self.errors: list[DashboardError] = []
        self.items = 0
        self.constructor = yaml.SafeLoader("")  # one per file: it keeps what it has made

    def scalar(self, node: yaml.Node) -> object:
        """The value of a scalar node; None for a mapping or a sequence."""
        return _scalar(node, self.constructor) if isinstance(node, yaml.ScalarNode) else None

    def error(self, node: yaml.Node, at: str, message: str) -> None:
        m = node.start_mark
        self.errors.append(DashboardError(m.line + 1, m.column + 1, at, message))
        if len(self.errors) >= MAX_ERRORS:
            raise _Stop

    # -- pieces

    def mapping(self, node: yaml.Node, at: str, keys: dict[str, bool], what: str) -> dict[str, yaml.Node] | None:
        """The keys of a mapping (keys: name -> required). Reports unknown, repeated and missing keys."""
        if not isinstance(node, yaml.MappingNode):
            self.error(node, at, f"{what} must be a mapping (key: value lines); its keys: {', '.join(keys)}")
            return None
        found: dict[str, yaml.Node] = {}
        for key_node, value_node in node.value:
            key = self.scalar(key_node)
            if not isinstance(key, str):
                self.error(key_node, at, f"{what}: a key must be a name; the keys here: {', '.join(keys)}")
                continue
            here = f"{at}.{key}" if at else key
            if key in found:
                self.error(key_node, here, f"{key} is given twice: keep one")
                continue
            if key not in keys:
                self.error(key_node, here, f"unknown key {key!r} in {what}: the keys here are {', '.join(keys)}")
                continue
            found[key] = value_node
        for key, required in keys.items():
            if required and key not in found:
                self.error(node, at, f"{what} has no {key}: add it")
        return found

    def text(self, node: yaml.Node, at: str, what: str) -> str | None:
        value = self.scalar(node)
        if not isinstance(value, str) or not value.strip():
            self.error(node, at, f"{what} must be a non-empty text (put it in quotes if it looks like a number)")
            return None
        if len(value) > MAX_TEXT or "\x00" in value:
            self.error(node, at, f"{what} is longer than {MAX_TEXT} characters or has a NUL: shorten it")
            return None
        return value

    def sequence(self, node: yaml.Node, at: str, what: str, limit: int) -> list[yaml.Node]:
        if not isinstance(node, yaml.SequenceNode):
            self.error(node, at, f"{what} must be a list (lines that start with '- ')")
            return []
        if len(node.value) > limit:
            self.error(node, at, f"{what} has {len(node.value)} entries; at most {limit}")
            return []
        return node.value

    def variable(self, node: yaml.Node, at: str) -> dict | None:
        value = self.text(node, at, "a variable")
        if value is None:
            return None
        peer, dot, name = value.partition(".")
        if not dot or not peer or not name:
            self.error(node, at, f"{value!r} is not <participant>.<variable>: write the participant's name, a '.', "
                                 "and the variable's name (Engine.engine.speed)")
            return None
        return {"peer": peer, "name": name}

    def plain(self, node: yaml.Node, at: str) -> object:
        """Options: JSON values (finite numbers, text, true/false, null, lists, mappings with text keys)."""
        if isinstance(node, yaml.ScalarNode):
            value = self.scalar(node)
            if isinstance(value, float) and not math.isfinite(value):
                self.error(node, at, "not a finite number: write a number")
                return None
            if value is not None and not isinstance(value, (str, int, float, bool)):
                self.error(node, at, f"{type(value).__name__} is not a JSON value: put it in quotes")
                return None
            return value
        if isinstance(node, yaml.SequenceNode):
            return [self.plain(v, f"{at}[{i}]") for i, v in enumerate(node.value)]
        out: dict[str, object] = {}
        for key_node, value_node in node.value:
            key = self.scalar(key_node)
            if not isinstance(key, str):
                self.error(key_node, at, "a key of options must be a name")
                continue
            if key in out:
                self.error(key_node, f"{at}.{key}", f"{key} is given twice: keep one")
                continue
            out[key] = self.plain(value_node, f"{at}.{key}")
        return out

    def options(self, node: yaml.Node, at: str) -> dict:
        if not isinstance(node, yaml.MappingNode):
            self.error(node, at, "options must be a mapping (name: value)")
            return {}
        value = self.plain(node, at)
        return value if isinstance(value, dict) else {}

    # -- the file

    def top(self, node: yaml.Node | None) -> dict | None:
        try:
            return self._top(node)
        except _Stop:
            return None
        except DashboardError as e:  # a scalar that cannot be read
            self.errors.append(e)
            return None

    def _top(self, node: yaml.Node | None) -> dict | None:
        if node is None:
            self.errors.append(DashboardError(1, 1, "", f"the file is empty: start it with vehicle_sil_dashboard: "
                                                        f"{FORMAT} and sections:"))
            return None
        keys = self.mapping(node, "", {"vehicle_sil_dashboard": True, "title": False, "panels": False,
                                       "sections": True}, "the dashboard file")
        if keys is None:
            return None
        if "vehicle_sil_dashboard" in keys:
            version_node = keys["vehicle_sil_dashboard"]
            version = self.scalar(version_node)
            if version != FORMAT or isinstance(version, bool):
                self.error(version_node, "vehicle_sil_dashboard",
                           f"vehicle_sil_dashboard is {version!r}: this GUI server reads format {FORMAT} (write "
                           f"vehicle_sil_dashboard: {FORMAT})")
        title = self.text(keys["title"], "title", "title") if "title" in keys else None
        panels = self.panels(keys["panels"]) if "panels" in keys else []
        sections = []
        if "sections" in keys:
            for i, s in enumerate(self.sequence(keys["sections"], "sections", "sections", MAX_SECTIONS)):
                section = self.section(s, f"sections[{i}]", bool(panels))
                if section is not None:
                    sections.append(section)
        return {"title": title, "panels": panels, "sections": sections}

    def panels(self, node: yaml.Node) -> list[dict]:
        panels = []
        for i, entry in enumerate(self.sequence(node, "panels", "panels", MAX_PANELS)):
            at = f"panels[{i}]"
            module = self.text(entry, at, "a panel module")
            if module is None:
                continue
            parts = module.split("/")
            if (len(parts) < 2 or parts[0] != PANELS_DIR or any(p in ("", ".", "..") for p in parts)
                    or any(c in module for c in "\\:")):
                self.error(entry, at, f"{module!r} is not a path in {PANELS_DIR}/: put the module in the "
                                      f"{PANELS_DIR} directory next to the dashboard file and write "
                                      f"{PANELS_DIR}/<name>.js (with /, no '..', no ':')")
                continue
            relative = f"{self.directory}/{module}" if self.directory else module
            try:
                module_path(self.root, relative)
            except PathRejected as e:
                self.error(entry, at, str(e))
                continue
            # Each part percent-encoded: a '#', '?' or '%' in a directory's name stays part of the path
            panels.append({"module": module,
                           "url": self.panel_base + "/".join(quote(p, safe="") for p in relative.split("/"))})
        return panels

    def section(self, node: yaml.Node, at: str, has_panels: bool) -> dict | None:
        keys = self.mapping(node, at, {"title": True, "collapsed": False, "items": True}, "a section")
        if keys is None:
            return None
        title = self.text(keys["title"], f"{at}.title", "title") if "title" in keys else None
        collapsed = False
        if "collapsed" in keys:
            value = self.scalar(keys["collapsed"])
            if isinstance(value, bool):
                collapsed = value
            else:
                self.error(keys["collapsed"], f"{at}.collapsed", "collapsed must be true or false")
        items = []
        if "items" in keys:
            for i, item_node in enumerate(self.sequence(keys["items"], f"{at}.items", "items", MAX_ITEMS)):
                self.items += 1
                if self.items > MAX_ITEMS:
                    self.error(item_node, f"{at}.items[{i}]", f"more than {MAX_ITEMS} items in all: use auto: for "
                                                              "the rest of a participant's variables")
                    break
                item = self.item(item_node, f"{at}.items[{i}]", has_panels)
                if item is not None:
                    items.append(item)
        return {"title": title, "collapsed": collapsed, "items": items}

    def item(self, node: yaml.Node, at: str, has_panels: bool) -> dict | None:
        kinds = ("variable", "panel", "auto")
        if not isinstance(node, yaml.MappingNode):
            self.error(node, at, "an item must be a mapping with one of variable:, panel: or auto:")
            return None
        given = [k for k in (self.scalar(key) for key, _ in node.value) if isinstance(k, str) and k in kinds]
        if len(given) != 1:
            self.error(node, at, "an item has exactly one of variable: (a widget), panel: (a panel of your own) "
                                 f"or auto: (the rest of a participant's variables); this one has "
                                 f"{', '.join(given) or 'none'}")
            return None
        kind = given[0]
        if kind == "variable":
            keys = self.mapping(node, at, {"variable": True, "widget": False, "label": False, "options": False},
                                "a variable item")
            if keys is None:
                return None
            ref = self.variable(keys["variable"], f"{at}.variable")
            widget = self.text(keys["widget"], f"{at}.widget", "widget") if "widget" in keys else None
            if widget is not None and not WIDGET_NAME.match(widget):
                self.error(keys["widget"], f"{at}.widget", f"{widget!r} is not the name of a widget (value, number, "
                                                           "slider, switch, lamp, select, graph)")
                widget = None
            label = self.text(keys["label"], f"{at}.label", "label") if "label" in keys else None
            options = self.options(keys["options"], f"{at}.options") if "options" in keys else {}
            return None if ref is None else {"variable": ref, "widget": widget, "label": label, "options": options}
        if kind == "panel":
            keys = self.mapping(node, at, {"panel": True, "label": False, "options": False, "variables": False},
                                "a panel item")
            if keys is None:
                return None
            name = self.text(keys["panel"], f"{at}.panel", "panel")
            if name is not None and not ELEMENT_NAME.match(name):
                self.error(keys["panel"], f"{at}.panel", f"{name!r} is not a custom element name: lowercase "
                                                         "letters, digits, '.', '_' and a '-' (rpm-bar)")
                name = None
            if name is not None and not has_panels:
                self.error(keys["panel"], f"{at}.panel", f"panel {name!r}: list the module that defines it under "
                                                         f"panels: at the top ({PANELS_DIR}/{name}.js)")
            label = self.text(keys["label"], f"{at}.label", "label") if "label" in keys else None
            options = self.options(keys["options"], f"{at}.options") if "options" in keys else {}
            variables = []
            if "variables" in keys:
                for i, v in enumerate(self.sequence(keys["variables"], f"{at}.variables", "variables",
                                                    MAX_PANEL_VARIABLES)):
                    ref = self.variable(v, f"{at}.variables[{i}]")
                    if ref is not None:
                        variables.append(ref)
            return None if name is None else {"panel": name, "label": label, "options": options,
                                              "variables": variables}
        keys = self.mapping(node, at, {"auto": True}, "an auto item")
        if keys is None:
            return None
        peer = self.text(keys["auto"], f"{at}.auto", "auto (a participant's name)")
        return None if peer is None else {"auto": peer}
