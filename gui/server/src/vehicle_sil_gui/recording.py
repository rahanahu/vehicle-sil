"""What the writes of a run became, written as a scenario (docs/scenario.md): the record behind GET api/recording.

The server keeps, for each run, the writes it gave the launcher's control server (POST api/write) and looks at the
results of their operations as every screen sees them: each peer's part of a write that was applied is kept with
the time of the step it was applied at. Nothing else is kept (time commands, writes of other clients, writes that
were late, refused or whose result is unknown). The record is plain data; the YAML is made when it is asked for.

The YAML is made by hand, not by a YAML library, so that what is written is exactly what the launcher reads:
- a variable `<participant>.<name>` is a plain key only when it is made of letters, digits, '_', '-' and '.',
  starts with a letter or '_' and has a '.'; else it is a double-quoted string, with every character that is not printable ASCII
  escaped (so that no line break, tab, BOM or control character reaches the file as itself);
- an enum value is its item's name, always double-quoted (a name such as `on`, `null` or `1` stays a name); when
  the name is not known (or not unique) it is the item's value (a number);
- a real is written so that YAML 1.1 and 1.2 both read a number: with a '.' always (1.0, 1.0e+20); a number that
  is not finite is never written (the server's check of a write refuses it, and it is skipped here if it came);
- an integer is written as it is (the check of a write keeps it in 64 bits).
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field

FORMAT = 1  # vehicle_sil_scenario
MAX_WRITES = 2000  # values kept per run; later ones are counted in `omitted`
MAX_OPEN = 512  # writes waiting for their result; the oldest are given up beyond it

# A plain key: a letter or '_' first, and a '.' (as every `<participant>.<name>` has): never a bool, null or number
PLAIN_KEY = re.compile(r"^[A-Za-z_][A-Za-z0-9_\-]*(\.[A-Za-z0-9_\-]*)+$")


@dataclass
class _Open:
    writes: dict[str, dict[str, object]]  # peer -> name -> value, as the write gave them
    done_peers: set[str] = field(default_factory=set)


@dataclass
class Recording:
    """The applied writes of one run. `available` is False (with `reason`) for a run whose writes are not
    recorded (no variables, or a scenario that the launcher plays)."""
    available: bool = True
    reason: str | None = None
    entries: list[tuple[int, int, str, str, object]] = field(default_factory=list)  # (at_ms, order, peer, name, value)
    omitted: int = 0
    _open: dict[int, _Open] = field(default_factory=dict)
    _order: int = 0

    def wrote(self, seq: int, writes: list[tuple[str, str, object]], operation: dict | None) -> None:
        """A write the control server took (its operation's number), and its operation as known now (its result
        may have come before the write's answer)."""
        if not self.available:
            return
        by_peer: dict[str, dict[str, object]] = {}
        for peer, name, value in writes:
            by_peer.setdefault(peer, {})[name] = value
        self._open[seq] = _Open(by_peer)
        while len(self._open) > MAX_OPEN:
            del self._open[next(iter(self._open))]
        if operation is not None:
            self.on_operation(operation)

    def on_operation(self, operation: dict) -> None:
        """An operation's state (the `operation` message): the peers whose part is applied are kept."""
        entry = self._open.get(operation.get("seq"))  # type: ignore[arg-type]
        if entry is None:
            return
        peers = operation.get("peers") or {}
        for peer, result in peers.items():
            if peer in entry.done_peers or not isinstance(result, dict) or peer not in entry.writes:
                continue
            if result.get("state") == "applied" and isinstance(result.get("applied_at_ms"), int):
                entry.done_peers.add(peer)
                for name, value in entry.writes[peer].items():
                    self._keep(result["applied_at_ms"], peer, name, value)
            elif operation.get("done"):
                entry.done_peers.add(peer)
        if operation.get("done"):
            del self._open[operation["seq"]]

    def _keep(self, at_ms: int, peer: str, name: str, value: object) -> None:
        if len(self.entries) >= MAX_WRITES:
            self.omitted += 1
            return
        self._order += 1
        self.entries.append((at_ms, self._order, peer, name, value))

    def to_json(self, session_id: int, file: str, state: str, enums: dict[tuple[str, str], list[dict]]) -> dict:
        """What GET api/recording answers for this run. enums: the items of each enum variable, by (peer, name)."""
        text, steps, skipped = (None, 0, 0)
        reason = self.reason
        if self.available:
            if self.entries:
                text, steps, skipped = scenario_yaml(self.entries, f"recorded from {file} (run {session_id})",
                                                     enums, self.omitted)
            if steps == 0:
                reason = ("no write made through this GUI server has been applied in this run yet: write variables "
                          "(the variables screen, or POST api/write), then save again")
        return {"session_id": session_id, "file": file, "state": state, "available": self.available,
                "reason": reason if text is None else None, "writes": len(self.entries) - skipped,
                "omitted": self.omitted + skipped, "steps": steps, "yaml": text}


# ------------------------------------------------------------ YAML


def _escape(text: str) -> str:
    out = []
    for ch in text:
        code = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif 0x20 <= code < 0x7F:
            out.append(ch)
        elif code <= 0xFF:
            out.append(f"\\x{code:02x}")
        elif code <= 0xFFFF:
            out.append(f"\\u{code:04x}")
        else:
            out.append(f"\\U{code:08x}")
    return '"' + "".join(out) + '"'


def quoted(text: str) -> str:
    """A YAML double-quoted scalar of any text: only printable ASCII is written as itself. Raises ValueError for
    a lone surrogate (not a character)."""
    if any(0xD800 <= ord(ch) <= 0xDFFF for ch in text):
        raise ValueError("a lone surrogate is not a character")
    return _escape(text)


def key(text: str) -> str:
    """A mapping key: plain when it cannot be read as anything but this text, else double-quoted."""
    return text if PLAIN_KEY.match(text) else quoted(text)


def number(value: object) -> str:
    """An integer or a finite real as YAML 1.1 and 1.2 both read it (a real always has a '.'). Raises ValueError
    for a value that is not a number or not finite."""
    if isinstance(value, bool):
        raise ValueError("a bool is not a number")
    if isinstance(value, int):
        return str(value)
    if not isinstance(value, float) or not math.isfinite(value):
        raise ValueError(f"{value!r} is not a finite number")
    text = repr(value)  # the shortest text that reads back as the same double: 50.0, 1e+20, 1.5e-07
    mantissa, e, exponent = text.partition("e")
    if "." not in mantissa:
        mantissa += ".0"
    return mantissa + (e + exponent if e else "")


def value_text(value: object, items: list[dict] | None) -> str:
    """A value of a set: true/false, a number, or an enum item's name (quoted)."""
    if isinstance(value, bool):
        return "true" if value else "false"
    if items is not None and isinstance(value, int):
        names = [i["name"] for i in items if i.get("value") == value and isinstance(i.get("name"), str)]
        if len(names) == 1 and sum(i.get("name") == names[0] for i in items) == 1:
            return quoted(names[0])
    return number(value)


def scenario_yaml(entries: list[tuple[int, int, str, str, object]], name: str,
                  enums: dict[tuple[str, str], list[dict]], omitted: int) -> tuple[str | None, int, int]:
    """(the scenario, its number of steps, values skipped): one `set` per time a value was applied, in the order of
    the time (the order of the writes within one time; a variable written twice at one time keeps its last value)."""
    by_time: dict[int, dict[str, str]] = {}
    skipped = 0
    for at_ms, _, peer, var, value in sorted(entries):
        try:
            line_key = key(f"{peer}.{var}")
            text = value_text(value, enums.get((peer, var)))
        except ValueError:
            skipped += 1
            continue
        writes = by_time.setdefault(at_ms, {})
        writes.pop(line_key, None)  # the last value at this time, in its place in the order
        writes[line_key] = text
    if not by_time:
        return None, 0, skipped
    lines = ["# Written by vehicle-sil-gui from the writes that were applied in one run (docs/gui.md).",
             "# Add expect and wait_until steps to check what the ECUs do (docs/scenario.md)."]
    if omitted + skipped:
        lines.append(f"# {omitted + skipped} more applied writes are not in this file (the record keeps at most "
                     f"{MAX_WRITES}).")
    try:
        name_text = quoted(name)
    except ValueError:
        name_text = '"recorded"'
    lines += [f"vehicle_sil_scenario: {FORMAT}", f"name: {name_text}", "steps:"]
    for at_ms in sorted(by_time):
        lines.append(f"  - at_ms: {at_ms}")
        lines.append("    set:")
        lines += [f"      {k}: {v}" for k, v in by_time[at_ms].items()]
    return "\n".join(lines) + "\n", len(by_time), skipped
