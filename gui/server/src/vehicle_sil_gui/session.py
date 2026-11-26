"""One run of the launcher as the screen sees it: a summary of the events the launcher wrote, and its recent log.

The server makes no state of its own: the summary is what the launcher's events said (phase, check,
participants, findings, result), what the server did to the launcher (started it, asked it to stop) and noticed
about it (it exited; a line that is not an event, out-of-order seq, no result at the end). The summary is enough to
rebuild a screen; of the events themselves only log lines are kept, for a screen that reconnects. Everything
kept has a limit in count and in bytes, and what is dropped is counted. Nothing here does I/O; the caller feeds
lines and gets back the messages to send to the screens.
"""

from __future__ import annotations

import heapq
import itertools
import json
import time
from array import array
from bisect import bisect_left, insort
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import UTC, datetime
from itertools import islice
from typing import Any, Literal

from .allowed import Facts, allowed as allowed_of, project_run_facts
from .launcher import CONTROL_EVENT, CONTROL_SUMMARY_FIELD, SCENARIO_STEP_EVENT, STEP_MS_MAX

SHARED_LOG = "*"  # the source key of the log shared by the sources after Limits.log_sources
ENVELOPE = ("event", "seq", "t_ms", "run")  # what is removed from an event when its fields are kept in the summary
MAX_COUNT = 2 ** 53  # seq and t_ms: integers that JavaScript reads exactly
# The fields of a small event that the summary keeps however large the event is (the others are left out of
# the summary when the event is larger than Limits.small_bytes, and named in `omitted_fields`).
ESSENTIAL = {
    "participant": ("name", "state", "pid", "exit_code", "signal", "reason", "stop_requested", CONTROL_SUMMARY_FIELD),
    "time": ("event", "seq", "t_ms", "sim_ms", "late_steps", "max_late_ms", "slipped_ms", "late_ms"),
    "registry": ("uri",),
    "command": ("seq", "command", "accepted", "ms", "at_ms", "reason", "id"),
}
# Problems that the server notices about the run, as opposed to what the launcher wrote: each has its own room
# in the summary, so that a launcher that writes junk cannot push them out.
SERVER_PROBLEMS = frozenset({"kept-limit", "no-result", "output-left-open", "stop-not-delivered",
                             "stop-escalated", "launcher-silent"})
# The launcher writes a line (a heartbeat if nothing else) at least once a second from run_start to its result; this
# long without any line while its process runs, and it is reported as silent (it may hang).
SILENT_AFTER_S = 5.0
STDERR = "launcher-stderr"
STOP_NONE, STOP_REQUESTED, STOP_FORCED = "none", "requested", "forced"
StopMode = Literal["graceful", "force"]
# The summary's `variables` of a run without any (variables.Variables.summary makes the others).
# The fields of a scenario step that the summary keeps (the launcher's scenario_step event and result.scenario.steps[])
SCENARIO_STEP_FIELDS = ("index", "line", "kind", "text", "status", "start_ms", "at_ms", "end_ms", "code", "message")
NO_VARIABLES = {"state": "off", "reason": None, "peers": [], "peers_omitted": 0, "variables_omitted": 0,
                "operations": [], "operations_from_seq": 0, "operations_open_omitted": 0}


def check_timing(check: dict) -> dict:
    """The bounded timing fields that survive losing a full check."""
    if "stop_at_ms" in check:
        stop_at = check["stop_at_ms"]
        if stop_at is None or (type(stop_at) is int and 1 <= stop_at <= STEP_MS_MAX):
            return {"stop_at_ms": stop_at}
    return {}


@dataclass(frozen=True)
class Limits:
    log_lines_per_source: int = 2000  # log lines kept per source for a reconnecting screen
    log_bytes_per_source: int = 1024 * 1024
    log_bytes: int = 16 * 1024 * 1024  # all sources together
    log_line_bytes: int = 64 * 1024  # a longer log event is passed on but not kept
    log_sources: int = 64  # sources with their own log; later sources share one
    dropped_seqs: int = 10_000  # per source: the seqs of the last dropped lines, to count what a screen missed
    object_bytes: int = 4 * 1024 * 1024  # the check and the result in the summary
    state_bytes: int = 64 * 1024  # the launcher's last state in the summary (about 2 KiB now)
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
    if kind == "run_start":
        return None if isinstance(event.get("run"), str) and bool(event.get("run")) else "run is not a string"
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
                not all(isinstance(s, str) and _is_count(n) for s, n in sources.items()):
            return "count or sources is not a count, or an object of counts"
        return None
    if kind == "command":
        if not (_str_or_none(event.get("command")) and isinstance(event.get("accepted"), bool)):
            return "command is not a string, or accepted is not true or false"
        return None if event.get("id") is None or _is_count(event["id"]) else "id is not null or an integer from 0 to 2^53"
    if kind == "state":
        return None if _is_count(event.get("rev")) and isinstance(event.get("allowed"), dict) \
            else "rev is not an integer from 0 to 2^53, or allowed is not an object"
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

    __slots__ = ("lines", "bytes", "dropped", "ring", "markers", "launcher_dropped",
                 "prestart_dropped", "prestart_seq")

    def __init__(self, ring_size: int):
        self.lines: deque[tuple[int, int, dict]] = deque()  # (seq, size, event)
        self.bytes = 0
        self.dropped = 0  # lines this server did not keep
        self.ring = _DroppedSeqs(ring_size)
        self.markers = 0  # log_dropped events in lines
        self.launcher_dropped = 0  # lines the launcher did not write (log_dropped)
        self.prestart_dropped = 0  # lines dropped while pre-start buffering before session registration
        self.prestart_seq = 0  # stream_seq boundary at which prestart_dropped was accounted

    @property
    def kept(self) -> int:
        return len(self.lines) - self.markers

    def drop_oldest(self, *, record_ring: bool = True) -> int:
        seq, size, event = self.lines.popleft()
        self.bytes -= size
        if event["event"] == "log_dropped":
            self.markers -= 1
        else:
            self.drop(seq, record_ring=record_ring)
        return size

    def drop(self, seq: int, *, record_ring: bool = True) -> None:
        self.dropped += 1
        if record_ring:
            self.ring.add(seq)

    def dropped_from(self, seq: int | None) -> tuple[int, bool]:
        """(dropped lines with a seq >= seq, whether the count is exact)."""
        if seq is None:
            return self.dropped + self.prestart_dropped, True
        n = self.ring.count_from(seq)
        exact = not (self.dropped > len(self.ring) and (not len(self.ring) or self.ring.oldest() > seq))
        if self.prestart_dropped and self.prestart_seq >= seq:
            n += self.prestart_dropped
        return n, exact


@dataclass
class _Staging:
    """Pre-start staging buffers; allocated on Session only while staged=True and detached on commit."""

    order: int = 0
    last_seq: int | None = None
    items: deque[tuple[int, tuple]] = field(default_factory=deque)
    markers: deque[tuple[int, int, dict]] = field(default_factory=deque)
    marker_bytes: int = 0
    heartbeat: tuple[int, dict, int] | None = None
    problem_counts: dict[str, int] = field(default_factory=lambda: {"launcher": 0, "server": 0})
    omitted_problems: int = 0
    line_too_long_seen: bool = False
    decision: bool = False
    passed_check: dict | None = None
    failed_check: dict | None = None
    discarded: bool = False
    cleanup_called: bool = False
    logs: list[tuple[int, tuple]] = field(default_factory=list)
    source_order: tuple[str, ...] = ()
    prestart_counts: tuple[tuple[str, int], ...] = ()

    def next_order(self) -> int:
        self.order += 1
        return self.order

    def settle(self, *, passed: dict | None = None, failed: dict | None = None) -> None:
        if not self.decision:
            self.passed_check, self.failed_check, self.decision = passed, failed, True

    def merged_items(self) -> list[tuple]:
        streams: list[object] = [self.items]
        if self.logs:
            streams.append(self.logs)
        if self.markers:
            streams.append(((o, ("event", ev, sz)) for o, sz, ev in self.markers))
        if self.heartbeat is not None:
            streams.append([(self.heartbeat[0], ("event", self.heartbeat[1], self.heartbeat[2]))])
        return [item for _, item in heapq.merge(*streams, key=lambda entry: entry[0])]


@dataclass
class Session:
    id: str
    file: str
    disable: list[str]
    speed: float | str | None
    start_paused: bool
    limits: Limits = field(default_factory=Limits)
    launcher_pid: int | None = None
    launcher_mode: str = "cold"
    started_at: str = field(default_factory=now_iso)
    scenario: str | None = None  # the scenario file given to --scenario (root-relative), or None
    manual: list[str] = field(default_factory=list)  # the names given to --manual (the user starts them)
    staged: bool = False  # pre-start buffering until the initial check settles on pass or fail

    def __post_init__(self) -> None:
        self.id = str(self.id)
        self.ended_at: str | None = None
        self.state = "running"  # until the run ends and its output is read
        self.launcher_running = True
        self.launcher_process_alive = True
        self.exit_code: int | None = None
        self.signal: int | None = None
        self.stop = STOP_NONE
        self.phase: str | None = None
        self.last_seq: int | None = None
        self.last_t_ms: int | None = None
        self.time: dict | None = None
        self.check: dict | None = None
        self.check_timing: dict = {}  # kept only when the full check cannot be retained
        self.registry: dict | None = None
        self.participants: dict[str, dict] = {}
        self.participants_omitted = 0
        self.findings: list[dict] = []
        self.findings_bytes = 0
        self.findings_omitted = 0
        self.last_command: dict | None = None
        self.launcher_state: dict | None = None  # the last `state` event of the launcher (what it accepts now)
        self.facts: Callable[[], Facts] = Facts  # what the server knows apart from the run (the server sets it)
        self.execution_status: Callable[[], dict | None] = lambda: None
        self.next_stream_seq: Callable[[], int] = itertools.count(1).__next__
        self.result: dict | None = None
        self.problems: list[dict] = []
        self.problems_omitted = 0
        self._problem_counts = {"launcher": 0, "server": 0}
        self._stderr_lines = 0
        self._stderr_last: deque[str] = deque(maxlen=self.limits.stderr_last)
        self.run_id: str | None = None
        self.run_t0_ms: int = 0
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
        # Where the server serves the run's timeline file (api/timeline/<id>), once the run has ended with one.
        self.timeline_url: str | None = None
        self._staging: _Staging | None = _Staging() if self.staged else None

    # ------------------------------------------------------------ pre-start staging

    @property
    def discarded(self) -> bool:
        return self._staging is not None and self._staging.discarded

    def _inspect_check_decision(self, st: _Staging, ev: object) -> None:
        if st.decision or not isinstance(ev, dict):
            return
        kind = ev.get("event")
        if kind == "check" and isinstance(ev.get("check"), dict):
            chk = ev["check"]
            st.settle(passed=chk) if chk.get("result") == "passed" else st.settle(failed=chk)
        elif kind == "result" and isinstance(ev.get("result"), dict):
            res, chk = ev["result"], ev["result"].get("check")
            if isinstance(chk, dict) and chk.get("result") != "passed":
                st.settle(failed=chk)
            elif res.get("result") in ("check_failed", "usage"):
                st.settle(failed={
                    "result": res["result"],
                    "errors": res.get("errors") if isinstance(res.get("errors"), int) else 1,
                    "findings": res.get("findings") if isinstance(res.get("findings"), list) and res.get("findings") else [{
                        "severity": "error", "code": res["result"], "file": self.file, "line": None,
                        "participant": None, "network": None, "message": res.get("message") or "check failed",
                    }],
                })
            else:
                st.settle(passed=chk if isinstance(chk, dict) else None)
        elif (kind == "phase" and ev.get("phase") not in ("checking", "ended")) or kind in (
            "registry", "control_server", "participant", "time"
        ):
            st.settle()

    def _stage_problem_room(self, st: _Staging, room: str | None) -> bool:
        if room is None:
            return True
        limit = self.limits.server_problems if room == "server" else self.limits.problems
        if st.problem_counts[room] < limit:
            st.problem_counts[room] += 1
            return True
        st.omitted_problems += 1
        return False

    def stage_line_too_long(self) -> None:
        st = self._staging
        if st is None or st.discarded:
            return
        if not st.line_too_long_seen:
            st.line_too_long_seen = True
            st.items.append((st.next_order(), ("line_too_long",)))
        elif self._stage_problem_room(st, "launcher"):
            st.items.append((st.next_order(), ("line_too_long",)))

    def stage_exit(self, returncode: int | None) -> None:
        if (st := self._staging) is not None and not st.discarded:
            st.items.append((st.next_order(), ("exit", returncode)))

    def stage_finish(self, output_cut: bool, launcher_alive: bool) -> None:
        if (st := self._staging) is not None and not st.discarded:
            st.settle()
            st.items.append((st.next_order(), ("finish", output_cut, launcher_alive)))

    def mark_cleanup_staged(self) -> None:
        if self._staging is not None:
            self._staging.cleanup_called = True

    def discard_staging(self) -> None:
        """Discards staged pre-start payloads when a start fails, times out, or is cancelled."""
        if self._staging is not None:
            self._staging = _Staging(discarded=True)
            self._logs.clear()
            self._log_bytes = 0
            self._stderr_last.clear()

    def take_staged(self) -> _Staging | None:
        """Detaches staging buffers and switches to live mode, leaving pre-event summary clean."""
        st = self._staging
        if st is None:
            return None
        st.source_order = tuple(src for src in self._logs if src != SHARED_LOG)
        st.prestart_counts = tuple(
            (src, n) for src, log in self._logs.items() if (n := log.dropped + log.prestart_dropped) > 0
        )
        st.logs = sorted(
            ((o, ("event", ev, sz)) for log in self._logs.values() for o, sz, ev in log.lines),
            key=lambda entry: entry[0],
        )
        self.staged = False
        self._staging = None
        self._logs.clear()
        self._log_bytes = 0
        return st

    def apply_prestart_dropped(self, st: _Staging, *, prestart_seq: int) -> list[dict]:
        """Restores pre-start log buckets when drops occurred and returns an out-of-band `dropped` notice."""
        if not st.prestart_counts:
            return []
        for src in st.source_order:
            self._log_of(src)
        reported: dict[str, int] = {}
        for src, count in st.prestart_counts:
            log = self._log_of(src)
            log.prestart_dropped += count
            log.prestart_seq = prestart_seq
            reported[src] = reported.get(src, 0) + count
        return [{"type": "dropped", "run_id": self.id, "reason": "not-kept", "log": reported,
                 "at_least": [], "events": 0}]

    def clamp_prestart_seq(self, seq: int) -> None:
        """Clamps prestart_seq to the current stream head when zero stamped frames followed `variables`."""
        for log in self._logs.values():
            if log.prestart_seq > seq:
                log.prestart_seq = seq

    # ------------------------------------------------------------ input

    def _with_silence(self, fn: Callable[[], list[dict]]) -> list[dict]:
        self.last_line_at = time.monotonic()
        if self.silent_s is not None:  # it was silent, and writes again
            self.silent_s = None
            return [self.message(), *fn()]
        return fn()

    def bad_line(self, line: str) -> list[dict]:
        """Records a non-event line from the launcher's standard output as a launcher-output problem."""
        return self._with_silence(lambda: self.problem(
            "launcher-output",
            "the launcher wrote a line that is not an event (a JSON object with \"event\"): "
            "the line is ignored; if it repeats, the launcher and this GUI do not match",
            self._cut(line),
        ))

    def feed_event(self, event: dict, size: int) -> list[dict]:
        """Takes one already-parsed event dict and its UTF-8 byte size; returns the messages for the screens."""
        return self._with_silence(lambda: self._feed_event(event, size))

    def _feed_event(self, event: dict, size: int, *, detail: str | None = None) -> list[dict]:
        st = self._staging
        if st is not None:
            if st.discarded:
                return []
            self._inspect_check_decision(st, event)

        def problem_detail() -> str:
            return self._cut(detail if detail is not None else repr(event))

        if not isinstance(event, dict) or not isinstance(event.get("event"), str):
            return self.problem("launcher-output",
                                "the launcher wrote a line that is not an event (a JSON object with \"event\"): "
                                "the line is ignored; if it repeats, the launcher and this GUI do not match",
                                problem_detail())
        seq = event.get("seq")
        if not _is_count(seq):
            return self.problem("launcher-output",
                                f"the launcher wrote a '{self._cut(event['event'])}' event without a seq that is an "
                                "integer from 0 to 2^53: it is ignored", problem_detail())
        prev_seq = st.last_seq if st is not None else self.last_seq
        if prev_seq is not None and seq <= prev_seq:
            return self.problem("seq-order",
                                f"the launcher wrote seq {seq} after {prev_seq}: the event is ignored "
                                "(events must come in order)", problem_detail())
        kind = event["event"]
        if st is not None:
            st.last_seq = seq
        else:
            self.last_seq = seq
        wrong = None if "t_ms" not in event or _is_count(event["t_ms"]) else "t_ms is not an integer from 0 to 2^53"
        if wrong is None:
            if st is None and "t_ms" in event:
                self.last_t_ms = event["t_ms"]
            wrong = _shape_problem(event)
        if wrong is not None:
            return self.problem("bad-event", f"the launcher wrote a '{kind}' event whose {wrong}: it is "
                                             "ignored (not shown); the launcher and this GUI may not match",
                                problem_detail())
        if st is not None:
            if kind == "log":
                self._keep_log(event, size, st.next_order())
            elif kind == "log_dropped":
                self._stage_marker(st, event, size)
            elif kind == "heartbeat":
                st.heartbeat = (st.next_order(), event, size)
            else:
                st.items.append((st.next_order(), ("event", event, size)))
            return []
        out = self._apply(event, size)
        sseq = self.next_stream_seq()
        if kind == "log":
            self._keep_log(event, size, sseq)
        elif kind == "log_dropped":
            self._keep_marker(event, size, sseq)
        out.append({"type": "event", "stream_seq": sseq, "run_id": self.id, "event": event})
        return out

    def _apply(self, event: dict, size: int) -> list[dict]:
        kind = event["event"]
        out: list[dict] = []
        if kind == "run_start":
            if isinstance(event.get("run"), str):
                self.run_id = event["run"]
            if _is_count(event.get("t_ms")):
                self.run_t0_ms = event["t_ms"]
        elif kind == SCENARIO_STEP_EVENT:
            self._keep_step(event)
        elif kind == "check":
            check = event["check"]
            self._scenario_of_check(check)
            self.check_timing = {}
            if size <= self.limits.object_bytes:
                self.check = check
            else:
                self.check = None
                self.check_timing = check_timing(check)
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
            kept = self._small(kind, {k: v for k, v in event.items() if k != "run"}, None)
            self.time = kept if kept is not None else self.time
        elif kind == "state":
            # Only the last one is kept (the launcher writes one whenever what it accepts changes); with its seq
            # and rev, so that a screen tells a new one from the same one sent again in a later summary.
            if size <= self.limits.state_bytes:
                self.launcher_state = {k: v for k, v in event.items() if k not in ("event", "t_ms", "run")}
            else:
                out += self._limit("state", f"the launcher's state is larger than {self.limits.state_bytes} "
                                            "bytes: it is shown now, but not kept for a screen opened later")
        elif kind == "registry":
            kept = self._small(kind, {k: v for k, v in event.items() if k not in ENVELOPE}, None)
            self.registry = kept if kept is not None else self.registry
        elif kind == "command":
            # with its seq: a screen tells a new answer from the same one sent again in a later summary
            kept = self._small(kind, {k: v for k, v in event.items() if k not in ("event", "t_ms", "run", "asked_run")}, None)
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
                "over_at_ms": self.scenario_over_at_ms, "timeline_url": self.timeline_url}

    def _keep_log(self, event: dict, size: int, stream_seq: int) -> None:
        log = self._log_of(event["source"])
        cap_bytes = min(self.limits.log_line_bytes, self.limits.log_bytes_per_source, self.limits.log_bytes)
        if self.limits.log_lines_per_source <= 0 or size > cap_bytes:
            log.drop(stream_seq, record_ring=not self.staged)
            return
        log.lines.append((stream_seq, size, event))
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

    def _stage_marker(self, st: _Staging, event: dict, size: int) -> None:
        """Holds staged log_dropped markers in one bounded deque so oversized/evicted markers fold once into
        prestart_dropped without touching per-source log.lines or launcher_dropped."""
        def fold(ev: dict) -> None:
            for source, count in ev["sources"].items():
                if count > 0:
                    self._log_of(source).prestart_dropped += count

        max_bytes = min(self.limits.log_bytes_per_source, self.limits.log_bytes)
        if self.limits.log_lines_per_source <= 0 or size > self.limits.log_line_bytes or size > max_bytes:
            fold(event)
            return
        st.markers.append((st.next_order(), size, event))
        st.marker_bytes += size
        while st.markers and (len(st.markers) > self.limits.log_lines_per_source or st.marker_bytes > max_bytes):
            _, sz, old = st.markers.popleft()
            st.marker_bytes -= sz
            fold(old)

    def _keep_marker(self, event: dict, size: int, stream_seq: int) -> None:
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
            log.lines.append((stream_seq, size, event))
            log.markers += 1
            log.bytes += size
            self._log_bytes += size
            self._trim(log)

    def _trim(self, log: _Log) -> None:
        record = not self.staged
        while len(log.lines) > self.limits.log_lines_per_source or log.bytes > self.limits.log_bytes_per_source:
            self._log_bytes -= log.drop_oldest(record_ring=record)
        while self._log_bytes > self.limits.log_bytes:  # all sources: the oldest line first
            oldest = min((l for l in self._logs.values() if l.lines), key=lambda l: l.lines[0][0])
            self._log_bytes -= oldest.drop_oldest(record_ring=record)

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
        room = "server" if code in SERVER_PROBLEMS else None if code == STDERR else "launcher"
        st = self._staging
        if st is not None:
            if not st.discarded and self._stage_problem_room(st, room):
                st.items.append((st.next_order(), ("problem", code, message, detail)))
            return []
        entry = {"code": code, "message": message, "detail": detail}
        limit = self.limits.server_problems if room == "server" else self.limits.problems
        if room is None or self._problem_counts[room] < limit:
            self.problems.append(entry)
            if room is not None:
                self._problem_counts[room] += 1
        else:
            self.problems_omitted += 1
        return [{"type": "error", "stream_seq": self.next_stream_seq(), "run_id": self.id, **entry}]

    def stderr_line(self, line: str) -> list[dict]:
        """A line of the launcher's standard error. The first stderr_first lines are problems at once; of the
        later ones only the last stderr_last are kept, and they become problems when the run ends (the last
        lines of a launcher that fails say why)."""
        st = self._staging
        if st is not None and st.discarded:
            return []
        self._stderr_lines += 1
        if self._stderr_lines > self.limits.stderr_first:
            self._stderr_last.append(line)
            return []
        if st is not None:
            st.items.append((st.next_order(), ("stderr_first", line)))
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

    def request_stop(self, force: bool) -> StopMode | None:
        """Records a stop request and returns the StopMode to send, or None when nothing new is requested."""
        if self.state != "running" or not self.launcher_running:
            return None
        if not force:
            if self.stop != STOP_NONE:
                return None
            self.stop = STOP_REQUESTED
            return "graceful"
        if self.stop == STOP_FORCED:
            return None
        self.stop = STOP_FORCED
        return "force"

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

    def launcher_exited(self, returncode: int | None, *, tell: bool = True) -> list[dict]:
        """The launcher process has exited (its output may still be being read). A negative returncode is the
        POSIX signal that ended it. The message is for a caller that sends it at once."""
        self.launcher_running = False
        self.launcher_process_alive = False
        self.silent_s = None
        if returncode is not None and returncode < 0:
            self.exit_code, self.signal = None, -returncode
        else:
            self.exit_code, self.signal = returncode, None
        return [self.message()] if tell else []

    def ended(self, output_cut: bool = False, *, launcher_alive: bool = False) -> list[dict]:
        """The run has ended and its output is read (or, output_cut, given up on)."""
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
        self.launcher_process_alive = bool(launcher_alive)
        self.ended_at = now_iso()
        return out + [self.message()]

    def _cut(self, text: str, n: int | None = None) -> str:
        n = n or self.limits.detail_chars
        return text if len(text) <= n else text[:n] + f"... ({len(text) - n} more characters)"

    # ------------------------------------------------------------ output

    def summary(self, variables: bool = True) -> dict:
        out = {
            "id": self.id, "run_t0_ms": self.run_t0_ms, "file": self.file, "disable": self.disable,
            "manual": self.manual, "speed": self.speed,
            "start_paused": self.start_paused, "started_at": self.started_at, "ended_at": self.ended_at,
            "state": self.state,
            "launcher": {"pid": self.launcher_pid, "running": self.launcher_running, "exit_code": self.exit_code,
                         "signal": self.signal, "silent_s": self.silent_s,
                         "mode": self.launcher_mode, "process_alive": self.launcher_process_alive},
            "stop": self.stop, "phase": self.phase, "last_seq": self.last_seq, "last_t_ms": self.last_t_ms,
            "time": self.time, "check": self.check, "registry": self.registry,
            "participants": list(self.participants.values()), "participants_omitted": self.participants_omitted,
            "findings": list(self.findings), "findings_omitted": self.findings_omitted,
            "last_command": self.last_command, "launcher_state": self.launcher_state,
            "allowed": self.allowed(), "result": self.result,
            "log": {source: {"kept": log.kept, "dropped": log.dropped + log.prestart_dropped + log.launcher_dropped}
                    for source, log in self._logs.items()},
            "problems": list(self.problems), "problems_omitted": self.problems_omitted,
            "scenario": self.scenario_summary(),
            **self.check_timing,
        }
        if variables:
            out["variables"] = self.variables() if self.variables is not None else dict(NO_VARIABLES)
        return out

    def allowed(self) -> dict:
        """What this server accepts now: start, stop, force (see allowed.py)."""
        run = project_run_facts(
            self.state == "running",
            self.launcher_running,
            self.stop,
            launcher_mode=self.launcher_mode,
            execution_status=self.execution_status(),
            run_state=self.launcher_state,
        )
        return allowed_of(self.facts(), run)

    def message(self) -> dict:
        """The summary for the screens that are connected. Without `variables`: they are large, change seldom,
        and have a message of their own when they do (`variables`); hello and the HTTP answers have them."""
        return {"type": "session", "stream_seq": self.next_stream_seq(), "session": self.summary(variables=False)}

    def replay(self, after_stream_seq: int | None) -> tuple[dict | None, list[dict]]:
        """(the dropped message, or None when nothing is missing; the log events to send), for a screen that
        has everything up to after_stream_seq (None: nothing)."""
        from_seq = None if after_stream_seq is None else after_stream_seq + 1
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
            streams.append(islice(log.lines, first, None))
        events, last = [], -1
        for sseq, _, e in heapq.merge(*streams, key=lambda item: item[0]):
            if sseq != last:  # a log_dropped kept in the logs of several sources is sent once
                events.append({"type": "event", "stream_seq": sseq, "run_id": self.id, "event": e})
                last = sseq
        dropped = None
        if counts or at_least:
            dropped = {"type": "dropped", "run_id": self.id, "reason": "not-kept", "log": counts,
                       "at_least": at_least, "events": 0}
        return dropped, events
