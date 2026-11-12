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
from array import array
from bisect import bisect_left
from collections import deque
from dataclasses import dataclass, field
from datetime import UTC, datetime
from itertools import islice
from typing import Any

EVENTS_FORMAT = 1  # the `vehicle_sil_events` of hello that this server reads
SHARED_LOG = "*"  # the source key of the log shared by the sources after Limits.log_sources
ENVELOPE = ("event", "seq", "t_ms")  # what is removed from an event when its fields are kept in the summary
STOP_NONE, STOP_REQUESTED, STOP_FORCED = "none", "requested", "forced"


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
    problems: int = 100
    detail_chars: int = 300  # the part of a bad line shown in a problem
    message_chars: int = 1000  # the part of a launcher's stderr line shown in a problem


def now_iso() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def _no_constant(name: str) -> Any:
    raise ValueError(f"{name} is not JSON")


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
    if kind == "command":
        return None if _str_or_none(event.get("command")) and isinstance(event.get("accepted"), bool) \
            else "command is not a string, or accepted is not true or false"
    if kind == "result":
        return None if isinstance(event.get("result"), dict) else "result is not an object"
    return None


class _SeqRing:
    """The seqs of the last `size` dropped lines of one source, oldest first (they are dropped in seq order)."""

    def __init__(self, size: int):
        self.size = size
        self.data = array("q")
        self.start = 0  # index of the oldest when full

    def add(self, seq: int) -> None:
        if self.size <= 0:
            return
        if len(self.data) < self.size:
            self.data.append(seq)
        else:
            self.data[self.start] = seq
            self.start = (self.start + 1) % self.size

    def __len__(self) -> int:
        return len(self.data)

    def _at(self, i: int) -> int:
        return self.data[(self.start + i) % len(self.data)]

    def oldest(self) -> int | None:
        return self._at(0) if self.data else None

    def count_from(self, seq: int) -> int:
        """How many of the recorded seqs are >= seq."""
        lo, hi = 0, len(self.data)
        while lo < hi:
            mid = (lo + hi) // 2
            if self._at(mid) < seq:
                lo = mid + 1
            else:
                hi = mid
        return len(self.data) - lo


class _Log:
    __slots__ = ("lines", "bytes", "dropped", "ring")

    def __init__(self, ring_size: int):
        self.lines: deque[tuple[int, int, dict]] = deque()  # (seq, size, event)
        self.bytes = 0
        self.dropped = 0
        self.ring = _SeqRing(ring_size)

    def drop_oldest(self) -> int:
        seq, size, _ = self.lines.popleft()
        self.bytes -= size
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
        if self.dropped > len(self.ring) and (not len(self.ring) or self.ring.oldest() >= seq):
            return n, False  # older dropped lines are not recorded: some of them may be >= seq
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
        self.hello_seen = False
        self._logs: dict[str, _Log] = {}
        self._log_bytes = 0
        self._limit_reported: set[str] = set()

    # ------------------------------------------------------------ input

    def feed_line(self, line: str) -> list[dict]:
        """Takes one line of the launcher's standard output; returns the messages for the screens."""
        try:
            event = json.loads(line, parse_constant=_no_constant)
        except (ValueError, RecursionError):
            event = None
        if not isinstance(event, dict) or not isinstance(event.get("event"), str):
            return self.problem("launcher-output",
                                "the launcher wrote a line that is not an event (a JSON object with \"event\"): "
                                "the line is ignored; if it repeats, the launcher and this GUI do not match",
                                self._cut(line))
        seq = event.get("seq")
        if not _is_int(seq):
            return self.problem("launcher-output",
                                f"the launcher wrote a '{self._cut(event['event'])}' event without an integer seq: "
                                "it is ignored", self._cut(line))
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
        if _is_int(event.get("t_ms")):
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
        elif kind == "check":
            if size <= self.limits.object_bytes:
                self.check = event["check"]
            else:
                out += self._limit("check", f"the launcher's check is larger than {self.limits.object_bytes} "
                                            "bytes: it is shown now, but not kept for a screen opened later")
        elif kind == "result":
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
        elif kind in ("phase", "time", "registry", "command", "participant") and size > self.limits.small_bytes:
            out += self._limit(kind, f"the launcher wrote a '{kind}' event larger than {self.limits.small_bytes} "
                                     "bytes: it is shown now, but not kept for a screen opened later")
        elif kind == "phase":
            self.phase = event["phase"]
        elif kind == "time":
            self.time = event
        elif kind == "registry":
            self.registry = {k: v for k, v in event.items() if k not in ENVELOPE}
        elif kind == "command":
            self.last_command = {k: v for k, v in event.items() if k not in ENVELOPE}
        elif kind == "participant":
            name = event["name"]
            if name in self.participants or len(self.participants) < self.limits.participants:
                entry = self.participants.setdefault(name, {"name": name})
                entry.update((k, v) for k, v in event.items() if k not in ENVELOPE)
                if len(json.dumps(entry)) > self.limits.small_bytes:  # fields piled up over many events
                    self.participants[name] = {k: entry.get(k) for k in ("name", "state", "pid", "exit_code",
                                                                         "signal")}
            else:
                self.participants_omitted += 1
                out += self._limit("participants", f"more participants than {self.limits.participants}: the "
                                                   "later ones are shown now, but not kept for a screen opened later")
        return out

    def _keep_log(self, event: dict, size: int) -> None:
        source = event["source"]
        if source not in self._logs and len(self._logs) >= self.limits.log_sources:
            source = SHARED_LOG
        log = self._logs.get(source)
        if log is None:
            log = self._logs[source] = _Log(self.limits.dropped_seqs)
        if size > self.limits.log_line_bytes:
            log.drop(event["seq"])
            return
        log.lines.append((event["seq"], size, event))
        log.bytes += size
        self._log_bytes += size
        while len(log.lines) > self.limits.log_lines_per_source or log.bytes > self.limits.log_bytes_per_source:
            self._log_bytes -= log.drop_oldest()
        while self._log_bytes > self.limits.log_bytes:  # all sources: the oldest line first
            oldest = min((l for l in self._logs.values() if l.lines), key=lambda l: l.lines[0][0])
            self._log_bytes -= oldest.drop_oldest()

    def _limit(self, what: str, message: str) -> list[dict]:
        """A kept-limit problem, once per kind of thing."""
        if what in self._limit_reported:
            return []
        self._limit_reported.add(what)
        return self.problem("kept-limit", message, {"what": what})

    def problem(self, code: str, message: str, detail: Any) -> list[dict]:
        entry = {"code": code, "message": message, "detail": detail}
        if len(self.problems) < self.limits.problems:
            self.problems.append(entry)
        else:
            self.problems_omitted += 1
        return [{"type": "error", "session": self.id, **entry}]

    def stderr_line(self, line: str) -> list[dict]:
        n = self.limits.message_chars
        cut = line if len(line) <= n else line[:n] + f"... ({len(line) - n} more characters)"
        return self.problem("launcher-stderr", f"the launcher wrote: {cut}", None)

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

    def launcher_exited(self, returncode: int | None) -> list[dict]:
        """The launcher process has exited (its output may still be being read). A negative returncode is the
        POSIX signal that ended it."""
        self.launcher_running = False
        if returncode is not None and returncode < 0:
            self.exit_code, self.signal = None, -returncode
        else:
            self.exit_code, self.signal = returncode, None
        return [self.message()]

    def ended(self, output_cut: bool = False) -> list[dict]:
        """The launcher has exited and its output is read (or, output_cut, given up on)."""
        out: list[dict] = []
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

    def _cut(self, text: str) -> str:
        n = self.limits.detail_chars
        return text if len(text) <= n else text[:n] + f"... ({len(text) - n} more characters)"

    # ------------------------------------------------------------ output

    def summary(self) -> dict:
        return {
            "id": self.id, "file": self.file, "disable": self.disable, "speed": self.speed,
            "start_paused": self.start_paused, "started_at": self.started_at, "ended_at": self.ended_at,
            "state": self.state,
            "launcher": {"pid": self.launcher_pid, "running": self.launcher_running, "exit_code": self.exit_code,
                         "signal": self.signal},
            "stop": self.stop, "phase": self.phase, "last_seq": self.last_seq, "last_t_ms": self.last_t_ms,
            "time": self.time, "check": self.check, "registry": self.registry,
            "participants": list(self.participants.values()), "participants_omitted": self.participants_omitted,
            "findings": self.findings, "findings_omitted": self.findings_omitted,
            "last_command": self.last_command, "result": self.result,
            "log": {source: {"kept": len(log.lines), "dropped": log.dropped} for source, log in self._logs.items()},
            "problems": self.problems, "problems_omitted": self.problems_omitted,
        }

    def message(self) -> dict:
        return {"type": "session", "session": self.summary()}

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
        events = [{"type": "event", "session": self.id, "event": e}
                  for e in heapq.merge(*streams, key=lambda e: e["seq"])]
        dropped = None
        if counts or at_least:
            dropped = {"type": "dropped", "session": self.id, "reason": "not-kept", "log": counts,
                       "at_least": at_least, "events": 0}
        return dropped, events
