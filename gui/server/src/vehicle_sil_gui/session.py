"""One run of the launcher as the screen sees it: the events it wrote, a summary of them, and what to replay.

The server makes no state of its own: the summary is what the launcher's events said (phase, participants,
findings, result), plus what the server noticed about the stream itself (problems: a line that is not an
event, a gap in seq, no result at the end). Nothing here does I/O; the caller feeds lines and gets back the
messages to send to the screens.
"""

from __future__ import annotations

import json
from collections import deque
from dataclasses import dataclass, field
from datetime import UTC, datetime
from typing import Any

EVENTS_FORMAT = 1  # the `vehicle_sil_events` of hello that this server reads


@dataclass(frozen=True)
class Limits:
    log_lines_per_source: int = 2000  # log events kept per source for a replay
    log_sources: int = 64  # sources with their own log; later sources share one
    other_events: int = 5000  # kept events that are neither log nor time
    problems: int = 100  # problems kept in the summary
    detail_chars: int = 300  # the part of a bad line shown in a problem


def now_iso() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


SHARED_LOG = "*"  # the source key of the log shared by the sources after Limits.log_sources


@dataclass
class Session:
    id: int
    file: str
    disable: list[str]
    speed: float | str | None
    start_paused: bool
    limits: Limits = field(default_factory=Limits)
    started_at: str = field(default_factory=now_iso)
    ended_at: str | None = None
    state: str = "running"  # the launcher process: "running" until it has exited and its output is read
    phase: str | None = None
    stop_requests: int = 0
    launcher_pid: int | None = None
    exit_code: int | None = None
    last_seq: int | None = None
    sim_ms: int | None = None
    result: dict | None = None
    hello_seen: bool = False

    def __post_init__(self) -> None:
        self.participants: dict[str, dict] = {}
        self.findings: list[dict] = []
        self.problems: list[dict] = []
        self.problems_omitted = 0
        self._kept: deque[dict] = deque(maxlen=self.limits.other_events)
        self._kept_omitted = 0
        self._logs: dict[str, deque[dict]] = {}
        self._logs_omitted: dict[str, int] = {}
        self._last_time: dict | None = None

    # ------------------------------------------------------------ input

    def feed_line(self, line: str) -> list[dict]:
        """Takes one line of the launcher's standard output; returns the messages for the screens."""
        try:
            event = json.loads(line)
        except ValueError:
            event = None
        if not isinstance(event, dict) or not isinstance(event.get("event"), str):
            return self.problem("launcher-output",
                                "the launcher wrote a line that is not an event (a JSON object with \"event\"): "
                                "the line is ignored; if it repeats, the launcher and this GUI do not match",
                                self._cut(line))
        seq = event.get("seq")
        if not isinstance(seq, int) or isinstance(seq, bool):
            return self.problem("launcher-output",
                                f"the launcher wrote a '{event['event']}' event without an integer seq: it is ignored",
                                self._cut(line))
        out: list[dict] = []
        if self.last_seq is not None and seq <= self.last_seq:
            return self.problem("seq-order",
                                f"the launcher wrote seq {seq} after {self.last_seq}: the event is ignored "
                                "(events must come in order)", self._cut(line))
        expected = 0 if self.last_seq is None else self.last_seq + 1
        if seq != expected:
            missing = f"seq {expected}" if seq == expected + 1 else f"seq {expected} to {seq - 1}"
            out += self.problem("seq-gap", f"events are missing: {missing} never came from the launcher",
                                {"expected": expected, "got": seq})
        if not self.hello_seen and event["event"] != "hello":
            out += self.problem("no-hello", f"the first event is '{event['event']}', not 'hello'", None)
            self.hello_seen = True
        self.last_seq = seq
        out += self._apply(event)
        self._keep(event)
        out.append({"type": "event", "session": self.id, "event": event})
        return out

    def _apply(self, event: dict) -> list[dict]:
        kind = event["event"]
        out: list[dict] = []
        if kind == "hello":
            self.hello_seen = True
            version = event.get("vehicle_sil_events")
            if version != EVENTS_FORMAT:
                out += self.problem("events-format",
                                    f"the launcher writes events in format {version!r}, and this GUI reads format "
                                    f"{EVENTS_FORMAT}: what it shows may be wrong; use a GUI of the same "
                                    "vehicle-sil version as the launcher", {"vehicle_sil_events": version})
            if isinstance(event.get("pid"), int):
                self.launcher_pid = event["pid"]
        elif kind == "phase" and isinstance(event.get("phase"), str):
            self.phase = event["phase"]
        elif kind == "check" and isinstance(event.get("check"), dict):
            for p in event["check"].get("participants") or []:
                if isinstance(p, dict) and isinstance(p.get("name"), str):
                    self._participant(p["name"]).update(required=p.get("required"), enabled=p.get("enabled"))
        elif kind == "participant" and isinstance(event.get("name"), str):
            entry = self._participant(event["name"])
            entry["state"] = event.get("state")
            for key in ("pid", "exit_code", "signal"):
                if key in event:
                    entry[key] = event[key]
        elif kind == "finding":
            self.findings.append({k: v for k, v in event.items() if k not in ("event", "seq", "t_ms")})
        elif kind == "time" and isinstance(event.get("sim_ms"), int):
            self.sim_ms = event["sim_ms"]
        elif kind == "result" and isinstance(event.get("result"), dict):
            self.result = event["result"]
        return out

    def _participant(self, name: str) -> dict:
        if name not in self.participants:
            self.participants[name] = {"name": name, "required": None, "enabled": None, "state": None,
                                       "pid": None, "exit_code": None, "signal": None}
        return self.participants[name]

    def _keep(self, event: dict) -> None:
        kind = event["event"]
        if kind == "time":
            self._last_time = event
        elif kind == "log":
            source = event.get("source") if isinstance(event.get("source"), str) else SHARED_LOG
            if source not in self._logs and len(self._logs) >= self.limits.log_sources:
                source = SHARED_LOG
            log = self._logs.setdefault(source, deque(maxlen=self.limits.log_lines_per_source))
            if len(log) == log.maxlen:
                self._logs_omitted[source] = self._logs_omitted.get(source, 0) + 1
            log.append(event)
        else:
            if len(self._kept) == self._kept.maxlen:
                self._kept_omitted += 1
            self._kept.append(event)

    def problem(self, code: str, message: str, detail: Any) -> list[dict]:
        entry = {"code": code, "message": message, "detail": detail}
        if len(self.problems) < self.limits.problems:
            self.problems.append(entry)
        else:
            self.problems_omitted += 1
        return [{"type": "error", "session": self.id, **entry}]

    def stop_requested(self) -> list[dict]:
        self.stop_requests += 1
        return [self.message()]

    def ended(self, exit_code: int | None) -> list[dict]:
        """The launcher has exited (exit_code None: unknown) and all its output is read."""
        out: list[dict] = []
        if self.result is None:
            out += self.problem("no-result",
                                f"the launcher ended (exit code {exit_code}) without writing its result: the "
                                "participants' states shown may be stale; see the launcher's last lines",
                                {"exit_code": exit_code})
        self.state = "ended"
        self.exit_code = exit_code
        self.ended_at = now_iso()
        return out + [self.message()]

    def _cut(self, line: str) -> str:
        n = self.limits.detail_chars
        return line if len(line) <= n else line[:n] + f"... ({len(line) - n} more characters)"

    # ------------------------------------------------------------ output

    def summary(self) -> dict:
        return {
            "id": self.id, "file": self.file, "disable": self.disable, "speed": self.speed,
            "start_paused": self.start_paused, "state": self.state, "phase": self.phase,
            "stop_requests": self.stop_requests, "started_at": self.started_at, "ended_at": self.ended_at,
            "launcher_pid": self.launcher_pid, "exit_code": self.exit_code, "last_seq": self.last_seq,
            "sim_ms": self.sim_ms, "participants": list(self.participants.values()), "findings": self.findings,
            "result": self.result, "problems": self.problems, "problems_omitted": self.problems_omitted,
        }

    def message(self) -> dict:
        return {"type": "session", "session": self.summary()}

    def replay(self, from_seq: int | None) -> tuple[dict, list[dict]]:
        """(the replay part of hello, the event messages to send), oldest first, from seq from_seq on."""
        events = list(self._kept)
        for log in self._logs.values():
            events += log
        if self._last_time is not None:
            events.append(self._last_time)
        if from_seq is not None:
            events = [e for e in events if e["seq"] >= from_seq]
        events.sort(key=lambda e: e["seq"])
        meta = {"from_seq": from_seq,
                "omitted": {"log": dict(self._logs_omitted), "events": self._kept_omitted}}
        return meta, [{"type": "event", "session": self.id, "event": e} for e in events]
