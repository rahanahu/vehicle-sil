"""One run of the launcher as the screen sees it: a summary of the events the launcher wrote, and its recent log.

The server makes no state of its own: the summary is what the launcher's events said (phase, check,
participants, findings, result), what the server did to the launcher (started it, asked it to stop) and noticed
about it (it exited; a line that is not an event, a gap in seq, no result at the end). The summary is enough to
rebuild a screen; of the events themselves only log lines are kept, for a screen that reconnects. Everything
kept has a limit in count and in bytes, and what is dropped is counted. Nothing here does I/O; the caller feeds
lines and gets back the messages to send to the screens.
"""

from __future__ import annotations

import heapq
import json
import math
import time
from array import array
from bisect import bisect_left, insort
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import UTC, datetime
from itertools import islice
from typing import Any

from .launcher import CONTROL_EVENT, CONTROL_SUMMARY_FIELD, SCENARIO_STEP_EVENT

EVENTS_FORMAT = 1  # the `vehicle_sil_events` of hello that this server reads
SHARED_LOG = "*"  # the source key of the log shared by the sources after Limits.log_sources
ENVELOPE = ("event", "seq", "t_ms")  # what is removed from an event when its fields are kept in the summary
MAX_COUNT = 2 ** 53  # seq and t_ms: integers that JavaScript reads exactly
# The fields of a small event that the summary keeps however large the event is (the others are left out of
# the summary when the event is larger than Limits.small_bytes, and named in `omitted_fields`).
ESSENTIAL = {
    "participant": ("name", "state", "pid", "exit_code", "signal", "reason", "stop_requested", CONTROL_SUMMARY_FIELD),
    "time": ("event", "seq", "t_ms", "sim_ms", "speed", "paused", "late_steps", "max_late_ms", "slipped_ms", "late_ms"),
    "registry": ("uri",),
    "command": ("seq", "command", "accepted", "ms"),
}
# Problems that the server notices about the run, as opposed to what the launcher wrote: each has its own room
# in the summary, so that a launcher that writes junk cannot push them out.
SERVER_PROBLEMS = frozenset({"kept-limit", "no-result", "output-left-open", "stop-not-delivered",
                             "stop-escalated", "launcher-silent"})
# The launcher writes a line (a heartbeat if nothing else) at least once a second from hello to its result; this
# long without any line while its process runs, and it is reported as silent (it may hang).
SILENT_AFTER_S = 5.0
STDERR = "launcher-stderr"
STOP_NONE, STOP_REQUESTED, STOP_FORCED = "none", "requested", "forced"
# The summary's `variables` of a run without any (variables.Variables.summary makes the others).
# The fields of a scenario step that the summary keeps (the launcher's scenario_step event and result.scenario.steps[])
SCENARIO_STEP_FIELDS = ("index", "line", "kind", "text", "status", "start_ms", "at_ms", "end_ms", "code", "message")
NO_VARIABLES = {"state": "off", "reason": None, "peers": [], "peers_omitted": 0, "variables_omitted": 0,
                "operations": []}


@dataclass(frozen=True)
class Limits:
    log_lines_per_source: int = 2000  # log lines kept per source for a reconnecting screen
    log_bytes_per_source: int = 1024 * 1024
    log_bytes: int = 16 * 1024 * 1024  # all sources together
    log_line_bytes: int = 64 * 1024  # a longer log event is passed on but not kept
    log_sources: int = 64  # sources with their own log; later sources share one
    dropped_seqs: int = 10_000  # per source: the seqs of the last dropped lines, to count what a screen missed
    object_bytes: int = 4 * 1024 * 1024  # the check and the result in the summary
    small_bytes: int = 4 * 1024  # phase, time, registry, command, and one participant's entry
    findings: int = 1000
    findings_bytes: int = 1024 * 1024
    participants: int = 256
    problems: int = 100  # problems from what the launcher wrote (not its standard error)
    server_problems: int = 50  # problems the server noticed itself (SERVER_PROBLEMS)
    stderr_first: int = 20  # the launcher's standard error: the first lines, sent at once
    stderr_last: int = 30  # and the last lines, sent when the run ends
    detail_chars: int = 300  # the part of a bad line shown in a problem
    message_chars: int = 1000  # the part of a launcher's stderr line shown in a problem
    scenario_steps: int = 1000  # steps of a scenario kept (by index); text and message are cut at message_chars


def _size(value: object) -> int:
    return len(json.dumps(value))


def now_iso() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def _no_constant(name: str) -> Any:
    raise ValueError(f"{name} is not JSON")


def _finite(text: str) -> float:
    """A JSON number with a fraction or an exponent: 1e999 is valid JSON, but not a number JSON can carry on."""
    value = float(text)
    if not math.isfinite(value):
        raise ValueError(f"{text} is not a finite number")
    return value


def _is_count(value: object) -> bool:
    return _is_int(value) and 0 <= value <= MAX_COUNT


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _str_or_none(value: object) -> bool:
    return value is None or isinstance(value, str)


def _int_or_none(value: object) -> bool:
    return value is None or _is_int(value)


def _shape_problem(event: dict) -> str | None:
    """What is wrong with a known event's fields (None: nothing, or an unknown event). Only what the server and
    the screen read is checked; other fields pass as they are."""
    kind = event["event"]
    if kind == "hello":
        return None if _int_or_none(event.get("pid")) else "pid is not an integer"
    if kind == "phase":
        return None if isinstance(event.get("phase"), str) else "phase is not a string"
    if kind == "check":
        check = event.get("check")
        if not isinstance(check, dict):
            return "check is not an object"
        participants = check.get("participants", [])
        if not isinstance(participants, list) or not all(
                isinstance(p, dict) and isinstance(p.get("name"), str) for p in participants):
            return "check.participants is not a list of objects with a name"
        return None
    if kind == "registry":
        return None if _str_or_none(event.get("uri")) else "uri is not a string"
    if kind == "participant":
        if not isinstance(event.get("name"), str) or not isinstance(event.get("state"), str):
            return "name or state is not a string"
        if not (_int_or_none(event.get("pid")) and _int_or_none(event.get("exit_code"))):
            return "pid or exit_code is not an integer"
        return None
    if kind == "log":
        return None if isinstance(event.get("source"), str) and isinstance(event.get("line"), str) \
            else "source or line is not a string"
    if kind == "finding":
        return None if isinstance(event.get("severity"), str) and isinstance(event.get("message"), str) \
            else "severity or message is not a string"
    if kind == "time":
        return None if _is_int(event.get("sim_ms")) else "sim_ms is not an integer"
    if kind == "log_dropped":
        sources = event.get("sources")
        if not _is_count(event.get("count")) or not isinstance(sources, dict) or \
                not all(_is_count(n) for n in sources.values()):
            return "count or sources is not a count, or an object of counts"
        return None
    if kind == "command":
        return None if _str_or_none(event.get("command")) and isinstance(event.get("accepted"), bool) \
            else "command is not a string, or accepted is not true or false"
    if kind == "result":
        return None if isinstance(event.get("result"), dict) else "result is not an object"
    if kind == CONTROL_EVENT:
        return None if isinstance(event.get("name"), str) and isinstance(event.get("state"), str) \
            else "name or state is not a string"
    if kind == SCENARIO_STEP_EVENT:
        index = event.get("index")
        return None if _is_int(index) and index >= 1 and isinstance(event.get("status"), str) \
            else "index is not a whole number from 1, or status is not a string"
    return None


class _DroppedSeqs:
    """The seqs of the last `size` dropped lines of one source (the largest seqs), in seq order. Lines are
    mostly dropped in seq order (the oldest kept line first); a line too long to keep is dropped when it comes,
    before older ones, and is put in its place."""

    def __init__(self, size: int):
        self.size = size
        self.data = array("q")
        self.start = 0  # data[start:] are recorded; data[:start] were pushed out and are removed in bulk

    def add(self, seq: int) -> None:
        if self.size <= 0:
            return
        if len(self) and seq < self.data[-1]:
            insort(self.data, seq, lo=self.start)
        else:
            self.data.append(seq)
        if len(self) > self.size:
            self.start += 1
        if self.start >= self.size:
            del self.data[:self.start]
            self.start = 0

    def __len__(self) -> int:
        return len(self.data) - self.start

    def oldest(self) -> int | None:
        return self.data[self.start] if len(self) else None

    def count_from(self, seq: int) -> int:
        """How many of the recorded seqs are >= seq."""
        return len(self.data) - bisect_left(self.data, seq, lo=self.start)


class _Log:
    """The lines kept of one source, with the launcher's log_dropped events among them (markers: where the
    launcher could not write lines of this source; one event can stand in the logs of several sources)."""

    __slots__ = ("lines", "bytes", "dropped", "ring", "markers", "launcher_dropped")

    def __init__(self, ring_size: int):
        self.lines: deque[tuple[int, int, dict]] = deque()  # (seq, size, event)
        self.bytes = 0
        self.dropped = 0  # lines this server did not keep
        self.ring = _DroppedSeqs(ring_size)
        self.markers = 0  # log_dropped events in lines
        self.launcher_dropped = 0  # lines the launcher did not write (log_dropped)

    @property
    def kept(self) -> int:
        return len(self.lines) - self.markers

    def drop_oldest(self) -> int:
        seq, size, event = self.lines.popleft()
        self.bytes -= size
        if event["event"] == "log_dropped":
            self.markers -= 1
        else:
            self.drop(seq)
        return size

    def drop(self, seq: int) -> None:
        self.dropped += 1
        self.ring.add(seq)

    def dropped_from(self, seq: int | None) -> tuple[int, bool]:
        """(dropped lines with a seq >= seq, whether the count is exact)."""
        if seq is None or self.dropped == 0:
            return self.dropped, True
        n = self.ring.count_from(seq)
        if self.dropped > len(self.ring) and (not len(self.ring) or self.ring.oldest() > seq):
            return n, False  # the dropped lines not recorded are older than the oldest recorded: some may be >= seq
        return n, True


@dataclass
class Session:
    id: int
    file: str
    disable: list[str]
    speed: float | str | None
    start_paused: bool
    limits: Limits = field(default_factory=Limits)
    launcher_pid: int | None = None
    started_at: str = field(default_factory=now_iso)
    scenario: str | None = None  # the scenario file given to --scenario (root-relative), or None

    def __post_init__(self) -> None:
        self.ended_at: str | None = None
        self.state = "running"  # until the launcher has exited and its output is read
        self.launcher_running = True
        self.exit_code: int | None = None
        self.signal: int | None = None
        self.stop = STOP_NONE
        self.phase: str | None = None
        self.last_seq: int | None = None
        self.last_t_ms: int | None = None
        self.time: dict | None = None
        self.check: dict | None = None
        self.registry: dict | None = None
        self.participants: dict[str, dict] = {}
        self.participants_omitted = 0
        self.findings: list[dict] = []
        self.findings_bytes = 0
        self.findings_omitted = 0
        self.last_command: dict | None = None
        self.result: dict | None = None
        self.problems: list[dict] = []
        self.problems_omitted = 0
        self._problem_counts = {"launcher": 0, "server": 0}
        self._stderr_lines = 0
        self._stderr_last: deque[str] = deque(maxlen=self.limits.stderr_last)
        self.hello_seen = False
        self._logs: dict[str, _Log] = {}
        self._log_bytes = 0
        self._limit_reported: set[str] = set()
        self._small_problems: list[dict] = []
        self.last_line_at = time.monotonic()  # when the launcher last wrote a line (or started)
        self.silent_s: int | None = None  # how long it has written nothing, once that is SILENT_AFTER_S or more
        # The ECUs' variables of the run (the schema, the state, the last operations), made by the server's
        # variables.Variables when a summary is asked for. Not in the `session` messages (see message).
        self.variables: Callable[[], dict] | None = None
        # The scenario's progress (a run with --scenario): from the check, the scenario_step events, the result.
        self.scenario_name: str | None = None
        self.scenario_step_count: int | None = None
        self.scenario_steps: dict[int, dict] = {}
        self.scenario_steps_omitted = 0
        self.scenario_status: str | None = None
        self.scenario_over_at_ms: float | None = None

    # ------------------------------------------------------------ input

    def feed_line(self, line: str) -> list[dict]:
        """Takes one line of the launcher's standard output; returns the messages for the screens."""
        self.last_line_at = time.monotonic()
        if self.silent_s is not None:  # it was silent, and writes again
            self.silent_s = None
            return [self.message(), *self._feed(line)]
        return self._feed(line)

    def _feed(self, line: str) -> list[dict]:
        try:
            event = json.loads(line, parse_constant=_no_constant, parse_float=_finite)
        except (ValueError, RecursionError):
            event = None
        if not isinstance(event, dict) or not isinstance(event.get("event"), str):
            return self.problem("launcher-output",
                                "the launcher wrote a line that is not an event (a JSON object with \"event\"): "
                                "the line is ignored; if it repeats, the launcher and this GUI do not match",
                                self._cut(line))
        seq = event.get("seq")
        if not _is_count(seq):
            return self.problem("launcher-output",
                                f"the launcher wrote a '{self._cut(event['event'])}' event without a seq that is an "
                                f"integer from 0 to 2^53: it is ignored", self._cut(line))
        if self.last_seq is not None and seq <= self.last_seq:
            return self.problem("seq-order",
                                f"the launcher wrote seq {seq} after {self.last_seq}: the event is ignored "
                                "(events must come in order)", self._cut(line))
        out: list[dict] = []
        expected = 0 if self.last_seq is None else self.last_seq + 1
        if seq != expected:
            missing = f"seq {expected}" if seq == expected + 1 else f"seq {expected} to {seq - 1}"
            out += self.problem("seq-gap", f"events are missing: {missing} never came from the launcher",
                                {"expected": expected, "got": seq})
        kind = event["event"]
        if not self.hello_seen and kind != "hello":
            out += self.problem("no-hello", f"the first event is '{self._cut(kind)}', not 'hello'", None)
        self.hello_seen = True
        self.last_seq = seq
        wrong = None if "t_ms" not in event or _is_count(event["t_ms"]) else "t_ms is not an integer from 0 to 2^53"
        if wrong is None:
            if "t_ms" in event:
                self.last_t_ms = event["t_ms"]
            wrong = _shape_problem(event)
        if wrong is not None:
            return out + self.problem("bad-event", f"the launcher wrote a '{kind}' event whose {wrong}: it is "
                                                   "ignored (not shown); the launcher and this GUI may not match",
                                      self._cut(line))
        out += self._apply(event, len(line))
        out.append({"type": "event", "session": self.id, "event": event})
        return out

    def _apply(self, event: dict, size: int) -> list[dict]:
        kind = event["event"]
        out: list[dict] = []
        if kind == "hello":
            version = event.get("vehicle_sil_events")
            if version != EVENTS_FORMAT:
                out += self.problem("events-format",
                                    f"the launcher writes events in format {self._cut(repr(version))}, and this GUI "
                                    f"reads format {EVENTS_FORMAT}: what it shows may be wrong; use a GUI of the "
                                    "same vehicle-sil version as the launcher",
                                    {"vehicle_sil_events": version if _is_int(version) else None})
        elif kind == "log":
            self._keep_log(event, size)
        elif kind == "log_dropped":
            self._keep_marker(event, size)
        elif kind == SCENARIO_STEP_EVENT:
            self._keep_step(event)
        elif kind == "check":
            self._scenario_of_check(event["check"])
            if size <= self.limits.object_bytes:
                self.check = event["check"]
            else:
                out += self._limit("check", f"the launcher's check is larger than {self.limits.object_bytes} "
                                            "bytes: it is shown now, but not kept for a screen opened later")
        elif kind == "result":
            self._scenario_of_result(event["result"])
            if size <= self.limits.object_bytes:
                self.result = event["result"]
            else:
                self.result = {k: v for k, v in event["result"].items()
                               if not isinstance(v, (dict, list)) and not (isinstance(v, str) and len(v) > 1024)}
                out += self._limit("result", f"the launcher's result is larger than {self.limits.object_bytes} "
                                             "bytes: only its short plain fields are kept for a screen opened later")
        elif kind == "finding":
            entry = {k: v for k, v in event.items() if k not in ENVELOPE}
            if len(self.findings) < self.limits.findings and self.findings_bytes + size <= self.limits.findings_bytes:
                self.findings.append(entry)
                self.findings_bytes += size
            else:
                self.findings_omitted += 1
                out += self._limit("findings", f"more findings than {self.limits.findings} (or "
                                               f"{self.limits.findings_bytes} bytes): later ones are shown now, but "
                                               "not kept for a screen opened later")
        elif kind == "phase":
            if len(event["phase"]) <= self.limits.small_bytes:
                self.phase = event["phase"]
            else:
                out += self._limit(kind, f"the launcher wrote a phase name longer than {self.limits.small_bytes} "
                                         "characters: it is shown now, but not kept for a screen opened later")
        elif kind == "time":
            kept = self._small(kind, event, None)
            self.time = kept if kept is not None else self.time
        elif kind == "registry":
            kept = self._small(kind, {k: v for k, v in event.items() if k not in ENVELOPE}, None)
            self.registry = kept if kept is not None else self.registry
        elif kind == "command":
            # with its seq: a screen tells a new answer from the same one sent again in a later summary
            kept = self._small(kind, {k: v for k, v in event.items() if k not in ("event", "t_ms")}, None)
            self.last_command = kept if kept is not None else self.last_command
        elif kind in ("participant", CONTROL_EVENT):
            name = event["name"]
            if name in self.participants or len(self.participants) < self.limits.participants:
                fields = {k: v for k, v in event.items() if k not in ENVELOPE}
                if kind == CONTROL_EVENT:
                    # the participant's control link, kept as one field of its entry
                    fields = {"name": name, CONTROL_SUMMARY_FIELD: {k: v for k, v in fields.items() if k != "name"}}
                elif event["state"] == "stop_requested":
                    # kept after later states: a participant asked to stop that then ends is not a failure
                    fields["stop_requested"] = True
                kept = self._small("participant", fields, self.participants.get(name, {"name": name}))
                if kept is not None:
                    self.participants[name] = kept
            else:
                self.participants_omitted += 1
                out += self._limit("participants", f"more participants than {self.limits.participants}: the "
                                                   "later ones are shown now, but not kept for a screen opened later")
        out += self._small_problems
        self._small_problems = []
        return out

    def _small(self, kind: str, fields: dict, old: dict | None) -> dict | None:
        """What the summary keeps of a small event's fields (laid over `old`, the participant's entry so far):
        all of them when they fit in Limits.small_bytes; else the essential ones and then as many of the others
        as fit, the rest named in `omitted_fields` (a kept-limit problem says so, once). None when not even the
        essential fields fit: then nothing is kept."""
        omitted = [k for k in (old or {}).get("omitted_fields", []) if k not in fields]  # still not known
        entry = {k: v for k, v in (old or {}).items() if k != "omitted_fields"}
        entry.update(fields)
        limit = self.limits.small_bytes
        if not omitted and _size(entry) <= limit:
            return entry
        essential = ESSENTIAL[kind]
        kept = {k: entry[k] for k in essential if k in entry}
        if _size(kept) > limit:
            self._small_problems += self._limit(kind, f"the launcher wrote a '{kind}' event whose "
                                                      f"{', '.join(essential)} are longer than {limit} bytes: it is "
                                                      "shown now, but not kept for a screen opened later")
            return None
        room = limit - _size(kept) - _size({"omitted_fields": [*omitted, *entry]})
        for k, v in entry.items():
            if k in kept:
                continue
            size = _size({k: v})
            if size <= room:
                kept[k] = v
                room -= size
            elif k not in omitted:
                omitted.append(k)
        if omitted:
            kept["omitted_fields"] = omitted
            self._small_problems += self._limit(kind, f"the launcher wrote a '{kind}' event larger than {limit} "
                                                      f"bytes: only its {', '.join(essential)} and the fields "
                                                      "that fit are kept for a screen opened later (the others "
                                                      "are named in omitted_fields)")
        return kept

    def _keep_step(self, step: object) -> None:
        if self.scenario is None or not isinstance(step, dict):
            return
        index = step.get("index")
        if not _is_int(index) or index < 1:
            return
        if index > self.limits.scenario_steps:
            self.scenario_steps_omitted = max(self.scenario_steps_omitted, index - self.limits.scenario_steps)
            return
        kept = {}
        for k in SCENARIO_STEP_FIELDS:
            v = step.get(k)
            kept[k] = self._cut(v, self.limits.message_chars) if isinstance(v, str) else v \
                if v is None or isinstance(v, (int, float)) else None
        self.scenario_steps[index] = kept

    def _scenario_of_check(self, check: dict) -> None:
        found = check.get("scenario")
        if self.scenario is None or not isinstance(found, dict):
            return
        if isinstance(found.get("name"), str):
            self.scenario_name = self._cut(found["name"], self.limits.message_chars)
        if _is_count(found.get("steps")):
            self.scenario_step_count = found["steps"]

    def _scenario_of_result(self, result: dict) -> None:
        found = result.get("scenario")
        if self.scenario is None or not isinstance(found, dict):
            return
        if isinstance(found.get("status"), str):
            self.scenario_status = self._cut(found["status"], 100)
        over = found.get("over_at_ms")
        self.scenario_over_at_ms = over if isinstance(over, (int, float)) and not isinstance(over, bool) else None
        if isinstance(found.get("name"), str):
            self.scenario_name = self._cut(found["name"], self.limits.message_chars)
        steps = found.get("steps")
        if isinstance(steps, list):
            for step in steps:
                self._keep_step(step)
            if self.scenario_step_count is None:
                self.scenario_step_count = len(steps)

    def scenario_summary(self) -> dict | None:
        if self.scenario is None:
            return None
        return {"file": self.scenario, "name": self.scenario_name, "step_count": self.scenario_step_count,
                "steps": [self.scenario_steps[i] for i in sorted(self.scenario_steps)],
                "steps_omitted": self.scenario_steps_omitted, "status": self.scenario_status,
                "over_at_ms": self.scenario_over_at_ms}

    def _keep_log(self, event: dict, size: int) -> None:
        log = self._log_of(event["source"])
        if size > self.limits.log_line_bytes:
            log.drop(event["seq"])
            return
        log.lines.append((event["seq"], size, event))
        log.bytes += size
        self._log_bytes += size
        self._trim(log)

    def _log_of(self, source: str) -> _Log:
        if source not in self._logs and len(self._logs) >= self.limits.log_sources:
            source = SHARED_LOG
        log = self._logs.get(source)
        if log is None:
            log = self._logs[source] = _Log(self.limits.dropped_seqs)
        return log

    def _keep_marker(self, event: dict, size: int) -> None:
        """A log_dropped event: counted in the dropped lines of each source it names, and kept in their logs
        where it came (once per log), so that a screen that connects later shows the gap in the same place."""
        counts: dict[int, list] = {}
        for source, count in event["sources"].items():
            log = self._log_of(source)
            counts.setdefault(id(log), [log, 0])[1] += count
        for log, count in counts.values():
            log.launcher_dropped += count
            if size > self.limits.log_line_bytes:
                continue
            log.lines.append((event["seq"], size, event))
            log.markers += 1
            log.bytes += size
            self._log_bytes += size
            self._trim(log)

    def _trim(self, log: _Log) -> None:
        while len(log.lines) > self.limits.log_lines_per_source or log.bytes > self.limits.log_bytes_per_source:
            self._log_bytes -= log.drop_oldest()
        while self._log_bytes > self.limits.log_bytes:  # all sources: the oldest line first
            oldest = min((l for l in self._logs.values() if l.lines), key=lambda l: l.lines[0][0])
            self._log_bytes -= oldest.drop_oldest()

    def log_key(self, source: str) -> str:
        """The name under which the log lines of a source are counted in the summary."""
        return source if source in self._logs else SHARED_LOG

    def _limit(self, what: str, message: str) -> list[dict]:
        """A kept-limit problem, once per kind of thing."""
        if what in self._limit_reported:
            return []
        self._limit_reported.add(what)
        return self.problem("kept-limit", message, {"what": what})

    def problem(self, code: str, message: str, detail: Any) -> list[dict]:
        """A problem for the screens, kept in the summary while its room has space: the server's own problems,
        the launcher's standard error (bounded by stderr_line) and the rest of what the launcher wrote each have
        their own room."""
        entry = {"code": code, "message": message, "detail": detail}
        room = "server" if code in SERVER_PROBLEMS else None if code == STDERR else "launcher"
        if room is None or self._problem_counts[room] < (self.limits.server_problems if room == "server"
                                                         else self.limits.problems):
            self.problems.append(entry)
            if room is not None:
                self._problem_counts[room] += 1
        else:
            self.problems_omitted += 1
        return [{"type": "error", "session": self.id, **entry}]

    def stderr_line(self, line: str) -> list[dict]:
        """A line of the launcher's standard error. The first stderr_first lines are problems at once; of the
        later ones only the last stderr_last are kept, and they become problems when the run ends (the last
        lines of a launcher that fails say why)."""
        self._stderr_lines += 1
        if self._stderr_lines > self.limits.stderr_first:
            self._stderr_last.append(line)
            return []
        return self._stderr_problem(line)

    def _stderr_problem(self, line: str) -> list[dict]:
        n = self.limits.message_chars
        cut = line if len(line) <= n else line[:n] + f"... ({len(line) - n} more characters)"
        return self.problem(STDERR, f"the launcher wrote: {cut}", None)

    def _stderr_end(self) -> list[dict]:
        out: list[dict] = []
        skipped = self._stderr_lines - self.limits.stderr_first - len(self._stderr_last)
        if skipped > 0:
            out += self.problem(STDERR, f"the launcher wrote {skipped} more lines to its standard error here: "
                                        f"they are not shown (only the first {self.limits.stderr_first} and the "
                                        f"last {self.limits.stderr_last} are)", {"skipped": skipped})
        for line in self._stderr_last:
            out += self._stderr_problem(line)
        self._stderr_last.clear()
        return out

    # ------------------------------------------------------------ what the server did and saw

    def request_stop(self, force: bool) -> int:
        """How many stop commands to write to the launcher now (0, 1 or 2); records the request."""
        if self.state != "running" or not self.launcher_running:
            return 0
        if not force:
            if self.stop != STOP_NONE:
                return 0
            self.stop = STOP_REQUESTED
            return 1
        if self.stop == STOP_FORCED:
            return 0
        n = 2 if self.stop == STOP_NONE else 1
        self.stop = STOP_FORCED
        return n

    def check_silence(self, now: float, after_s: float = SILENT_AFTER_S) -> list[dict]:
        """Whether the launcher has written nothing for after_s or more while its process runs and before its
        result (when it writes a heartbeat every second): the summary says for how long (`launcher.silent_s`,
        updated every after_s), and a problem says so once each time it falls silent."""
        if self.state != "running" or not self.launcher_running or self.result is not None:
            return []
        silent = now - self.last_line_at
        if silent < after_s:
            return []
        shown = int(silent // after_s * after_s)
        if self.silent_s is not None and shown <= self.silent_s:
            return []
        out = []
        if self.silent_s is None:
            out = self.problem("launcher-silent",
                               f"the launcher has written nothing for {shown} s, not even the heartbeat it writes "
                               "every second: it may hang; stop the run, or stop the GUI server if that does not "
                               "end it", {"seconds": shown})
        self.silent_s = shown
        return out + [self.message()]

    def launcher_exited(self, returncode: int | None) -> list[dict]:
        """The launcher process has exited (its output may still be being read). A negative returncode is the
        POSIX signal that ended it. The message is for a caller that sends it at once."""
        self.launcher_running = False
        self.silent_s = None
        if returncode is not None and returncode < 0:
            self.exit_code, self.signal = None, -returncode
        else:
            self.exit_code, self.signal = returncode, None
        return [self.message()]

    def ended(self, output_cut: bool = False) -> list[dict]:
        """The launcher has exited and its output is read (or, output_cut, given up on)."""
        out: list[dict] = self._stderr_end()
        if output_cut:
            out += self.problem("output-left-open",
                                "the launcher has exited, but a process it started still holds its output open: "
                                "the rest of the output is not read; that process may still be running", None)
        if self.result is None:
            how = f"exit code {self.exit_code}" if self.signal is None else f"signal {self.signal}"
            out += self.problem("no-result",
                                f"the launcher ended ({how}) without writing its result: the participants' states "
                                "shown may be stale; see the launcher's last lines",
                                {"exit_code": self.exit_code})
        self.state = "ended"
        self.launcher_running = False
        self.ended_at = now_iso()
        return out + [self.message()]

    def _cut(self, text: str, n: int | None = None) -> str:
        n = n or self.limits.detail_chars
        return text if len(text) <= n else text[:n] + f"... ({len(text) - n} more characters)"

    # ------------------------------------------------------------ output

    def summary(self, variables: bool = True) -> dict:
        out = {
            "id": self.id, "file": self.file, "disable": self.disable, "speed": self.speed,
            "start_paused": self.start_paused, "started_at": self.started_at, "ended_at": self.ended_at,
            "state": self.state,
            "launcher": {"pid": self.launcher_pid, "running": self.launcher_running, "exit_code": self.exit_code,
                         "signal": self.signal, "silent_s": self.silent_s},
            "stop": self.stop, "phase": self.phase, "last_seq": self.last_seq, "last_t_ms": self.last_t_ms,
            "time": self.time, "check": self.check, "registry": self.registry,
            "participants": list(self.participants.values()), "participants_omitted": self.participants_omitted,
            "findings": self.findings, "findings_omitted": self.findings_omitted,
            "last_command": self.last_command, "result": self.result,
            "log": {source: {"kept": log.kept, "dropped": log.dropped + log.launcher_dropped}
                    for source, log in self._logs.items()},
            "problems": self.problems, "problems_omitted": self.problems_omitted,
            "scenario": self.scenario_summary(),
        }
        if variables:
            out["variables"] = self.variables() if self.variables is not None else dict(NO_VARIABLES)
        return out

    def message(self) -> dict:
        """The summary for the screens that are connected. Without `variables`: they are large, change seldom,
        and have a message of their own when they do (`variables`); hello and the HTTP answers have them."""
        return {"type": "session", "session": self.summary(variables=False)}

    def replay(self, from_seq: int | None) -> tuple[dict | None, list[dict]]:
        """(the dropped message, or None when nothing is missing; the log events to send), for a screen that
        has everything before from_seq (None: nothing)."""
        counts, at_least = {}, []
        for source, log in self._logs.items():
            n, exact = log.dropped_from(from_seq)
            if n:
                counts[source] = n
            if not exact:
                at_least.append(source)
        start = from_seq or 0
        streams = []
        for log in self._logs.values():
            first = bisect_left(log.lines, start, key=lambda line: line[0]) if start else 0
            streams.append([e for _, _, e in islice(log.lines, first, None)])
        events, last = [], None
        for e in heapq.merge(*streams, key=lambda e: e["seq"]):
            if e["seq"] != last:  # a log_dropped kept in the logs of several sources is sent once
                events.append({"type": "event", "session": self.id, "event": e})
            last = e["seq"]
        dropped = None
        if counts or at_least:
            dropped = {"type": "dropped", "session": self.id, "reason": "not-kept", "log": counts,
                       "at_least": at_least, "events": 0}
        return dropped, events
