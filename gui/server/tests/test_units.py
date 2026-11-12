"""The parts without a server: the session model, the hub's slow-client handling, paths, static files, lines."""

from __future__ import annotations

import asyncio
import json
import os
import sys
from pathlib import Path

import pytest
from vehicle_sil_gui import process
from vehicle_sil_gui.hub import Client, Hub, encode
from vehicle_sil_gui.launcher import (InvalidRequest, check_disable, check_speed, check_time_command, command_line,
                                     speed_argument)
from vehicle_sil_gui.paths import ConfigRoot, PathRejected
from vehicle_sil_gui.session import Limits, Session
from vehicle_sil_gui.static import StaticFiles


def line(event: str, seq: int, **fields) -> str:
    return json.dumps({"event": event, "seq": seq, "t_ms": seq, **fields})


def new_session(**limits) -> Session:
    return Session(1, "simulation.yaml", [], None, False, limits=Limits(**limits))


def kinds(messages: list[dict]) -> list[str]:
    return [m["type"] if m["type"] != "error" else f"error:{m['code']}" for m in messages]


# ---------------------------------------------------------------- session


def test_session_follows_the_events() -> None:
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1, pid=42))
    check = {"participants": [{"name": "A", "required": True, "enabled": True, "variables": []},
                              {"name": "B", "required": False, "enabled": False, "variables": None}]}
    s.feed_line(line("check", 1, check=check))
    s.feed_line(line("phase", 2, phase="running"))
    s.feed_line(line("participant", 3, name="B", state="disabled"))
    s.feed_line(line("participant", 4, name="A", state="started", pid=7, command=["a"], log=None))
    s.feed_line(line("participant", 5, name="A", state="ended", exit_code=3, signal=None))
    s.feed_line(line("finding", 6, severity="error", code="participant-failed", message="m"))
    s.feed_line(line("time", 7, sim_ms=300))
    s.feed_line(line("registry", 8, uri="silkit://x"))
    s.feed_line(line("command", 9, command="stop", accepted=True))
    s.feed_line(line("result", 10, result={"result": "failed"}))
    assert kinds(s.launcher_exited(1)) == ["session"] and s.summary()["state"] == "running"
    out = s.ended()
    summary = s.summary()
    assert summary["launcher"] == {"pid": None, "running": False, "exit_code": 1, "signal": None, "silent_s": None}
    assert summary["phase"] == "running" and summary["time"]["sim_ms"] == 300 and summary["check"] == check
    assert summary["last_seq"] == 10 and summary["last_t_ms"] == 10 and summary["stop"] == "none"
    assert summary["participants"] == [{"name": "B", "state": "disabled"},
                                       {"name": "A", "state": "ended", "pid": 7, "command": ["a"], "log": None,
                                        "exit_code": 3, "signal": None}]
    assert summary["findings"] == [{"severity": "error", "code": "participant-failed", "message": "m"}]
    assert summary["registry"] == {"uri": "silkit://x"}
    assert summary["last_command"] == {"seq": 9, "command": "stop", "accepted": True}
    assert summary["result"] == {"result": "failed"} and summary["problems"] == []
    assert kinds(out) == ["session"] and out[0]["session"]["state"] == "ended"


def test_a_signal_is_not_an_exit_code() -> None:
    s = new_session()
    s.launcher_exited(-9)
    assert s.summary()["launcher"] == {"pid": None, "running": False, "exit_code": None, "signal": 9, "silent_s": None}


def test_stop_requests_are_recorded_once() -> None:
    s = new_session()
    assert [s.request_stop(False), s.request_stop(False)] == [1, 0] and s.stop == "requested"
    assert [s.request_stop(True), s.request_stop(True), s.request_stop(False)] == [1, 0, 0] and s.stop == "forced"
    s = new_session()
    assert s.request_stop(True) == 2 and s.stop == "forced"
    s = new_session()
    s.launcher_exited(0)
    assert s.request_stop(False) == 0 and s.stop == "none"  # nothing to stop


def test_lines_that_are_not_events_are_reported_and_not_passed_on() -> None:
    s = new_session()
    assert kinds(s.feed_line("not json")) == ["error:launcher-output"]
    assert kinds(s.feed_line("[1]")) == ["error:launcher-output"]
    assert kinds(s.feed_line('{"event": "log"}')) == ["error:launcher-output"]  # no seq
    assert kinds(s.feed_line('{"event": "log", "seq": true}')) == ["error:launcher-output"]
    assert kinds(s.feed_line('{"event": "log", "seq": 0, "x": NaN}')) == ["error:launcher-output"]  # not JSON
    assert kinds(s.feed_line(line("log", 0, source="A", line="x"))) == ["error:no-hello", "event"]
    assert kinds(s.feed_line(line("mystery", 2, colour="blue"))) == ["error:seq-gap", "event"]
    assert kinds(s.feed_line(line("log", 2))) == ["error:seq-order"]
    assert kinds(s.feed_line(line("log", 1))) == ["error:seq-order"]
    long = s.feed_line("x" * 1000)[0]
    assert len(long["detail"]) < 400 and "more characters" in long["detail"]


# NOT_FINITE stands for numbers that are valid JSON but not finite: they are put in the line as they are.
NOT_FINITE = {"<1e999>": "1e999", "<-1e999>": "-1e999", "<1E400>": "1E400"}
WRONG_VALUES = [None, True, 0, -1, 2.5, "x", "", [], [1], {}, {"a": 1}, [{"name": 1}], "\ud800", 2 ** 64, -(2 ** 64),
                *NOT_FINITE]
KNOWN = {
    "hello": {"vehicle_sil_events": 1, "pid": 1},
    "phase": {"phase": "running"},
    "check": {"check": {"participants": [{"name": "A", "required": True}]}},
    "registry": {"uri": "u"},
    "participant": {"name": "A", "state": "started", "pid": 3, "exit_code": None},
    "log": {"source": "A", "line": "l"},
    "finding": {"severity": "note", "code": "c", "message": "m"},
    "time": {"sim_ms": 1},
    "command": {"command": "stop", "accepted": True},
    "result": {"result": {"result": "passed", "participants": []}},
}


def test_no_field_of_the_wrong_type_stops_the_reading() -> None:
    # Every field of every known event, and every field of the objects in them, replaced by values of other
    # types: feeding never raises, and the next good event is still read.
    def variants(fields: dict):
        for key, value in fields.items():
            for wrong in WRONG_VALUES:
                yield {**fields, key: wrong}
            yield {k: v for k, v in fields.items() if k != key}
            if isinstance(value, dict):
                for inner in variants(value):
                    yield {**fields, key: inner}
            if isinstance(value, list) and value and isinstance(value[0], dict):
                for inner in variants(value[0]):
                    yield {**fields, key: [inner]}

    s = new_session()
    seq = 0
    for kind, fields in KNOWN.items():
        for variant in variants(fields):
            text = json.dumps({"event": kind, "seq": seq, "t_ms": seq, **variant})
            for marker, number in NOT_FINITE.items():
                text = text.replace(json.dumps(marker), number)
            out = s.feed_line(text)
            seq += 1
            assert all(m["type"] in ("event", "error") for m in out)
            for m in out:
                encode(m)  # strict JSON: no NaN or Infinity
            encode(s.summary())  # the summary can still be sent
    good = s.feed_line(line("phase", seq, phase="ended"))
    assert kinds(good) == ["event"] and s.summary()["phase"] == "ended"
    assert s.summary()["problems_omitted"] > 0  # many were reported (and the summary keeps a bounded number)


def test_numbers_that_are_not_finite_are_not_events() -> None:
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    for n, number in enumerate(("1e999", "-1e999", "1E400", "-0.5e+999")):
        out = s.feed_line(f'{{"event":"time","seq":{n + 1},"t_ms":0,"sim_ms":5,"actual_speed":{number}}}')
        assert kinds(out) == ["error:launcher-output"], number
    assert s.summary()["time"] is None and s.last_seq == 0
    encode(s.summary())
    assert kinds(s.feed_line(line("time", 5, sim_ms=5, actual_speed=1.5))) == ["error:seq-gap", "event"]


def test_seq_and_t_ms_are_integers_javascript_reads_exactly() -> None:
    s = new_session(dropped_seqs=4, log_line_bytes=10)
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    for seq in (2 ** 63, 2 ** 64, 2 ** 53 + 1, -1):
        assert kinds(s.feed_line(line("log", seq, source="A", line="x"))) == ["error:launcher-output"], seq
    out = s.feed_line(json.dumps({"event": "log", "seq": 1, "t_ms": 2 ** 64, "source": "A", "line": "x"}))
    assert kinds(out) == ["error:bad-event"] and s.last_t_ms == 0
    out = s.feed_line(line("log", 2 ** 53, source="A", line="a line too long to keep"))
    assert kinds(out) == ["error:seq-gap", "event"]
    assert s.summary()["log"] == {"A": {"kept": 0, "dropped": 1}} and s.last_seq == 2 ** 53


def test_a_known_event_of_the_wrong_shape_is_not_passed_on() -> None:
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    out = s.feed_line(line("check", 1, check={"participants": 5}))
    assert kinds(out) == ["error:bad-event"] and s.summary()["check"] is None and s.last_seq == 1
    assert kinds(s.feed_line(line("log", 2, source=None, line="x"))) == ["error:bad-event"]
    assert s.summary()["log"] == {}


def test_a_different_events_format_is_reported_but_passed_on() -> None:
    s = new_session()
    out = s.feed_line(line("hello", 0, vehicle_sil_events=2))
    assert kinds(out) == ["error:events-format", "event"]


def test_ending_without_a_result_is_reported() -> None:
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    s.launcher_exited(70)
    out = s.ended()
    assert kinds(out) == ["error:no-result", "session"]
    assert s.summary()["launcher"]["exit_code"] == 70 and s.summary()["problems"][0]["detail"] == {"exit_code": 70}
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    s.feed_line(line("result", 1, result={"result": "passed"}))
    s.launcher_exited(0)
    assert kinds(s.ended(output_cut=True)) == ["error:output-left-open", "session"]


def test_problems_are_capped() -> None:
    s = new_session(problems=3)
    for _ in range(5):
        s.feed_line("bad")
    assert len(s.summary()["problems"]) == 3 and s.summary()["problems_omitted"] == 2
    assert len(s.stderr_line("e" * 100_000)[0]["message"]) < 1100


def test_junk_does_not_hide_the_servers_notices_or_the_last_stderr_lines() -> None:
    s = new_session()
    for n in range(120):  # junk from the launcher: more than its room
        s.feed_line(f"junk {n}")
    stderr = [f"stderr {n}" for n in range(100)]
    live = [m for text in stderr for m in s.stderr_line(text)]
    assert [m["message"] for m in live] == [f"the launcher wrote: stderr {n}" for n in range(20)]  # the first
    s.launcher_exited(70)
    out = s.ended(output_cut=True)
    codes = [m["code"] for m in out if m["type"] == "error"]
    assert codes == ["launcher-stderr"] * 31 + ["output-left-open", "no-result"]
    assert out[0]["detail"] == {"skipped": 50} and out[30]["message"] == "the launcher wrote: stderr 99"  # the last
    problems = s.summary()["problems"]
    assert [p["code"] for p in problems].count("launcher-output") == 100 and s.summary()["problems_omitted"] == 20
    kept = [p["message"] for p in problems if p["code"] == "launcher-stderr"]
    assert kept[0].endswith("stderr 0") and kept[-1].endswith("stderr 99") and len(kept) == 51
    assert [p["code"] for p in problems][-2:] == ["output-left-open", "no-result"]


def test_the_log_is_kept_per_source_and_bounded() -> None:
    s = new_session(log_lines_per_source=3, log_sources=2)
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    seq = 1
    for n in range(5):
        for source in ("A", "B", "C"):  # C is the third source: it shares the log of the later sources
            s.feed_line(line("log", seq, source=source, line=f"{source}{n}"))
            seq += 1
        s.feed_line(line("time", seq, sim_ms=n))
        seq += 1
    dropped, events = s.replay(None)
    lines = [m["event"]["line"] for m in events]
    assert lines == ["A2", "B2", "C2", "A3", "B3", "C3", "A4", "B4", "C4"]  # log only, in seq order
    assert dropped == {"type": "dropped", "session": 1, "reason": "not-kept", "log": {"A": 2, "B": 2, "*": 2},
                       "at_least": [], "events": 0}
    assert s.summary()["log"] == {"A": {"kept": 3, "dropped": 2}, "B": {"kept": 3, "dropped": 2},
                                  "*": {"kept": 3, "dropped": 2}}
    assert s.summary()["time"]["sim_ms"] == 4
    a3 = next(m["event"]["seq"] for m in events if m["event"]["line"] == "A3")
    dropped, events = s.replay(a3)
    assert dropped is None and [m["event"]["line"] for m in events] == ["A3", "B3", "C3", "A4", "B4", "C4"]
    dropped, events = s.replay(2)  # A0 (seq 1) is before: it was dropped, but the screen has it
    assert dropped["log"] == {"A": 1, "B": 2, "*": 2}


def test_memory_is_bounded_in_bytes() -> None:
    s = new_session(dropped_seqs=100)
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    seq = 1
    for n in range(3000):  # 3000 lines of 2 KiB from 20 sources: 6 MiB in all, 1 MiB per source at most
        s.feed_line(line("log", seq, source=f"S{n % 20}", line="x" * 2048))
        seq += 1
    s.feed_line(line("log", seq, source="S0", line="y" * 100_000))  # too long to keep
    seq += 1
    assert all(log.bytes <= 1024 * 1024 for log in s._logs.values()) and s._log_bytes <= 16 * 1024 * 1024
    assert s.summary()["log"]["S0"]["dropped"] >= 1
    one = new_session()
    for n in range(700):  # 1.4 MiB from one source, under its line limit
        one.feed_line(line("log", n, source="A", line="x" * 2048))
    assert one._logs["A"].bytes <= 1024 * 1024 and one._logs["A"].dropped > 0
    small = new_session(log_bytes=100 * 1024)  # all sources together
    for n in range(200):
        small.feed_line(line("log", n, source=f"S{n % 5}", line="x" * 2048))
    assert small._log_bytes <= 100 * 1024 and sum(log.dropped for log in small._logs.values()) > 0
    big = {"participants": [], "blob": "z" * (5 * 1024 * 1024)}
    out = s.feed_line(line("check", seq, check=big))
    seq += 1
    assert kinds(out) == ["error:kept-limit", "event"] and s.summary()["check"] is None
    out = s.feed_line(line("result", seq, result={"result": "passed", "blob": "z" * (5 * 1024 * 1024)}))
    seq += 1
    assert kinds(out) == ["error:kept-limit", "event"] and s.summary()["result"] == {"result": "passed"}
    for n in range(1500):
        s.feed_line(line("finding", seq, severity="note", code="c", message=f"m{n}"))
        seq += 1
        s.feed_line(line("participant", seq, name=f"P{n}", state="started"))
        seq += 1
    for n in range(50):
        s.feed_line(line("mystery", seq, blob="w" * 10_000))  # unknown events are passed on, not kept
        seq += 1
    summary = s.summary()
    assert len(summary["findings"]) == 1000 and summary["findings_omitted"] == 500
    assert len(summary["participants"]) == 256 and summary["participants_omitted"] == 1244
    assert sorted(p["detail"]["what"] for p in summary["problems"] if p["code"] == "kept-limit") == \
        ["check", "findings", "participants", "result"]
    assert len(json.dumps(summary)) < 2 * 1024 * 1024


def test_dropped_counts_are_exact_with_a_line_too_long_to_keep() -> None:
    s = new_session(log_lines_per_source=5)
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    for seq in range(1, 12):  # seq 6 is too long to keep: dropped when it comes, before 1..5
        s.feed_line(line("log", seq, source="A", line="x" * (70_000 if seq == 6 else 1)))
    for from_seq, want in ((None, 6), (1, 6), (4, 3), (6, 1)):
        dropped = s.replay(from_seq)[0]
        assert dropped["log"] == {"A": want} and dropped["at_least"] == [], from_seq
    assert s.replay(7)[0] is None
    small = new_session(log_lines_per_source=2, dropped_seqs=3)  # fewer recorded than dropped
    small.feed_line(line("hello", 0, vehicle_sil_events=1))
    for seq in range(1, 9):  # 4 too long: dropped first; kept 7, 8; dropped 1..6
        small.feed_line(line("log", seq, source="A", line="x" * (70_000 if seq == 4 else 1)))
    assert small.replay(4)[0]["log"] == {"A": 3} and small.replay(4)[0]["at_least"] == []  # 4, 5, 6
    assert small.replay(3)[0]["log"] == {"A": 3} and small.replay(3)[0]["at_least"] == ["A"]


def test_log_dropped_is_counted_and_kept_where_it_came() -> None:
    s = new_session(log_lines_per_source=3)
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    s.feed_line(line("log", 1, source="A", line="a1"))
    out = s.feed_line(line("log_dropped", 2, count=125, sources={"A": 120, "vehicle_sil_launch": 5}))
    assert kinds(out) == ["event"]
    s.feed_line(line("log", 3, source="A", line="a3"))
    assert s.summary()["log"] == {"A": {"kept": 2, "dropped": 120}, "vehicle_sil_launch": {"kept": 0, "dropped": 5}}
    _, events = s.replay(None)
    assert [e["event"]["seq"] for e in events] == [1, 2, 3], "the marker once, in its place"
    _, events = s.replay(3)
    assert [e["event"]["seq"] for e in events] == [3]
    for n in range(4, 8):  # the marker leaves with the old lines; the counts stay
        s.feed_line(line("log", n, source="A", line=f"a{n}"))
    assert s.summary()["log"]["A"] == {"kept": 3, "dropped": 120 + 3}
    assert kinds(s.feed_line(line("log_dropped", 8, count="many", sources={}))) == ["error:bad-event"]
    assert kinds(s.feed_line(line("log_dropped", 9, count=1, sources={"A": -1}))) == ["error:bad-event"]


def test_a_launcher_that_writes_nothing_is_reported_as_silent() -> None:
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    start = s.last_line_at
    assert s.check_silence(start + 4.9, 5) == []
    out = s.check_silence(start + 5.2, 5)
    assert kinds(out) == ["error:launcher-silent", "session"] and s.summary()["launcher"]["silent_s"] == 5
    assert s.check_silence(start + 9.0, 5) == [], "the summary changes every 5 s, not on each look"
    assert kinds(s.check_silence(start + 10.1, 5)) == ["session"] and s.summary()["launcher"]["silent_s"] == 10
    out = s.feed_line(line("heartbeat", 1))
    assert kinds(out) == ["session", "event"] and s.summary()["launcher"]["silent_s"] is None
    assert [p["code"] for p in s.problems] == ["launcher-silent"]
    s.feed_line(line("result", 2, result={"result": "passed"}))
    assert s.check_silence(s.last_line_at + 60, 5) == [], "nothing is promised after the result"
    s2 = new_session()
    s2.launcher_exited(0)
    assert s2.check_silence(s2.last_line_at + 60, 5) == [], "nor after the launcher has exited"


def test_a_participant_state_this_server_does_not_know_passes_through() -> None:
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    s.feed_line(line("participant", 1, name="A", state="started", pid=7, command=["a"], log=None))
    out = s.feed_line(line("participant", 2, name="A", state="failed", reason="process_ended", message="m"))
    assert kinds(out) == ["event"]
    s.feed_line(line("participant", 3, name="A", state="ended", exit_code=1, signal=None))
    s.feed_line(line("participant", 4, name="B", state="sleeping", why={"x": 1}))
    assert s.summary()["participants"] == [
        {"name": "A", "state": "ended", "pid": 7, "command": ["a"], "log": None, "reason": "process_ended",
         "message": "m", "exit_code": 1, "signal": None},
        {"name": "B", "state": "sleeping", "why": {"x": 1}}]


def test_the_control_link_of_a_participant_is_kept_in_its_entry() -> None:
    """The control event (a state of the participant's link to its variables) is laid over the participant's entry
    as one field, so that a screen opened later sees it; a state this server does not know passes through."""
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    s.feed_line(line("participant", 1, name="A", state="started", pid=7))
    s.feed_line(line("participant", 2, name="A", state="joined"))
    out = s.feed_line(line("control_link", 3, name="A", state="connected", variables=17))
    assert kinds(out) == ["event"]
    s.feed_line(line("control_link", 4, name="B", state="version_mismatch", version=2))
    s.feed_line(line("control_link", 5, name="C", state="some_new_state"))
    s.feed_line(line("participant", 6, name="A", state="ended", exit_code=0, signal=None))
    assert s.summary()["participants"] == [
        {"name": "A", "state": "ended", "pid": 7, "control_link": {"state": "connected", "variables": 17},
         "exit_code": 0, "signal": None},
        {"name": "B", "control_link": {"state": "version_mismatch", "version": 2}},
        {"name": "C", "control_link": {"state": "some_new_state"}}]
    assert kinds(s.feed_line(line("control_link", 7, name=5, state="connected"))) == ["error:bad-event"]


def test_an_event_this_server_does_not_know_is_passed_on_and_the_reading_goes_on() -> None:
    """A launcher newer than this server may write events it does not know: they reach the screens as they are
    (the screen skips what it does not know), nothing is reported, and the events after them are read as usual."""
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    out = s.feed_line(line("future_thing", 1, payload={"a": [1, 2]}, note="n"))
    assert kinds(out) == ["event"] and out[0]["event"]["payload"] == {"a": [1, 2]}
    out = s.feed_line(line("phase", 2, phase="running"))
    assert kinds(out) == ["event"] and s.summary()["phase"] == "running"
    assert s.summary()["problems"] == [] and s.last_seq == 2


def test_the_essential_fields_of_a_large_small_event_are_kept() -> None:
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    long_command = [f"--argument-{n}-" + "x" * 100 for n in range(45)]  # about 5 KiB
    out = s.feed_line(line("participant", 1, name="Engine", state="started", pid=7, command=long_command,
                           log="engine.log"))
    assert kinds(out) == ["error:kept-limit", "event"] and out[0]["detail"] == {"what": "participant"}
    assert s.summary()["participants"] == [{"name": "Engine", "state": "started", "pid": 7, "log": "engine.log",
                                            "omitted_fields": ["command"]}]
    s.feed_line(line("participant", 2, name="Dash", state="started", pid=8, command=["dash"]))
    s.feed_line(line("participant", 3, name="Dash", state="ended", exit_code=0, signal=None, note="n" * 5000))
    s.feed_line(line("participant", 4, name="Engine", state="ended", exit_code=1, signal=None))
    assert s.summary()["participants"] == [
        {"name": "Engine", "state": "ended", "pid": 7, "log": "engine.log", "exit_code": 1, "signal": None,
         "omitted_fields": ["command"]},  # still not known
        {"name": "Dash", "state": "ended", "pid": 8, "command": ["dash"], "exit_code": 0, "signal": None,
         "omitted_fields": ["note"]}]
    s.feed_line(line("participant", 5, name="Engine", state="ended", command=["engine"]))  # known again
    assert "omitted_fields" not in s.summary()["participants"][0]
    s.feed_line(line("phase", 6, phase="running", detail="d" * 5000))
    s.feed_line(line("time", 7, sim_ms=100, detail="d" * 5000))
    s.feed_line(line("command", 8, command="stop", accepted=True, message="m" * 5000))
    s.feed_line(line("registry", 9, uri="silkit://x", detail="d" * 5000))
    summary = s.summary()
    assert summary["phase"] == "running"
    assert summary["time"] == {"event": "time", "seq": 7, "t_ms": 7, "sim_ms": 100, "omitted_fields": ["detail"]}
    assert summary["last_command"] == {"seq": 8, "command": "stop", "accepted": True, "omitted_fields": ["message"]}
    assert summary["registry"] == {"uri": "silkit://x", "omitted_fields": ["detail"]}
    out = s.feed_line(line("participant", 10, name="N" * 5000, state="started"))  # not even the name fits
    assert kinds(out) == ["event"] and len(s.summary()["participants"]) == 2
    # The time with its lateness, however large the event: a screen opened later still sees how late it is
    s.feed_line(line("time", 11, sim_ms=300, speed=1, paused=False, actual_speed=0.9, late_steps=1, max_late_ms=200,
                     slipped_ms=200, late_ms=0, detail="d" * 5000))
    time = s.summary()["time"]
    assert {k: time[k] for k in ("sim_ms", "speed", "paused", "late_steps", "max_late_ms", "slipped_ms", "late_ms")} \
        == {"sim_ms": 300, "speed": 1, "paused": False, "late_steps": 1, "max_late_ms": 200, "slipped_ms": 200,
            "late_ms": 0}
    assert time["omitted_fields"] == ["detail"]


def test_dropped_counts_are_exact_while_recorded() -> None:
    s = new_session(log_lines_per_source=10, dropped_seqs=20)
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    for seq in range(1, 101):
        s.feed_line(line("log", seq, source="A", line=str(seq)))
    # kept: seq 91..100; dropped: 1..90, of which 71..90 recorded
    assert s.replay(80)[0]["log"] == {"A": 11} and s.replay(80)[0]["at_least"] == []
    assert s.replay(91)[0] is None
    assert s.replay(50)[0]["log"] == {"A": 20} and s.replay(50)[0]["at_least"] == ["A"]
    assert s.replay(None)[0]["log"] == {"A": 90} and s.replay(None)[0]["at_least"] == []


# ---------------------------------------------------------------- hub


def run_hub(messages: list[dict], limit: int = 10, max_bytes: int = 1 << 30,
            log_key=None) -> tuple[list[dict], Client]:
    """A screen that reads nothing until all messages are broadcast, then everything."""
    async def main() -> tuple[list[dict], Client]:
        snapshots = []

        def snapshot(session, from_seq):
            snapshots.append((session, from_seq))
            return [{"type": "hello", "n": len(snapshots)}, {"type": "synced"}]

        hub = Hub(snapshot, limit=limit, max_bytes=max_bytes)
        sent: list[dict] = []
        gate = asyncio.Event()

        async def send(text: str) -> None:
            await gate.wait()
            text.encode("utf-8")  # what a WebSocket text frame needs
            sent.append(json.loads(text))

        client = hub.client(send)
        task = asyncio.create_task(hub.serve(client, None, None))
        await asyncio.sleep(0)
        for m in messages:
            hub.broadcast([m], log_key)
            assert len(client.items) <= limit + 1
        gate.set()
        await asyncio.sleep(0.05)
        hub.close_all()
        await task
        assert client not in hub.clients
        return sent, client

    return asyncio.run(main())


def log_event(seq: int, source: str = "A") -> dict:
    return {"type": "event", "session": 1, "event": {"event": "log", "seq": seq, "source": source, "line": "x"}}


def test_a_slow_screen_loses_log_lines_not_the_state() -> None:
    messages = [log_event(n, "A" if n % 2 else "B") for n in range(200)]
    messages[50] = {"type": "event", "session": 1, "event": {"event": "phase", "seq": 50, "phase": "running"}}
    messages[60] = {"type": "error", "session": 1, "code": "seq-gap", "message": "m", "detail": None}
    messages += [{"type": "event", "session": 1, "event": {"event": "time", "seq": 200 + n, "sim_ms": n}}
                 for n in range(30)]
    sent, client = run_hub(messages, limit=10)
    types = [m["type"] for m in sent]
    assert "resync" not in types and client.resyncs == 0
    assert any(m["type"] == "event" and m["event"]["event"] == "phase" for m in sent)
    assert any(m["type"] == "error" for m in sent)
    dropped = [m for m in sent if m["type"] == "dropped"]
    log_sent = [m for m in sent if m["type"] == "event" and m["event"]["event"] == "log"]
    assert sum(sum(d["log"].values()) for d in dropped) + len(log_sent) == 198
    assert all(d["reason"] == "slow-client" and d["session"] == 1 for d in dropped)
    times = [m["event"]["sim_ms"] for m in sent if m["type"] == "event" and m["event"]["event"] == "time"]
    assert times[-1] == 29 and times == sorted(times)


def test_a_slow_screen_counts_what_the_launcher_dropped_and_loses_no_heartbeat_count() -> None:
    messages = [log_event(n, "A") for n in range(100)]
    messages[40] = {"type": "event", "session": 1, "event": {"event": "log_dropped", "seq": 40, "count": 1005,
                                                             "sources": {"A": 1000, "B": 5}}}
    messages[41] = {"type": "event", "session": 1, "event": {"event": "heartbeat", "seq": 41}}
    sent, _ = run_hub(messages, limit=10)
    dropped = [m for m in sent if m["type"] == "dropped"]
    markers = [m for m in sent if m["type"] == "event" and m["event"]["event"] == "log_dropped"]
    log_sent = [m for m in sent if m["type"] == "event" and m["event"]["event"] == "log"]
    if not markers:  # shed: its counts are in the dropped lines of its sources, the heartbeat is not counted
        assert sum(d["log"].get("B", 0) for d in dropped) == 5
        assert sum(d["log"].get("A", 0) for d in dropped) + len(log_sent) == 98 + 1000
    assert all(d["events"] == 0 for d in dropped)


def test_a_slow_screen_gets_resync_when_dropping_is_not_enough() -> None:
    messages = [{"type": "event", "session": 1, "event": {"event": "finding", "seq": n, "severity": "note",
                                                           "message": "m"}} for n in range(100)]
    messages.append({"type": "event", "session": 1, "event": {"event": "phase", "seq": 100, "phase": "after"}})
    sent, client = run_hub(messages, limit=10)
    types = [m["type"] for m in sent]
    assert "resync" in types and client.resyncs >= 1
    # What came after the overflow is in the state the resync sends (the server's snapshot has the summary).
    assert types[-3:] == ["resync", "hello", "synced"]


def test_a_slow_screen_is_told_the_sources_the_summary_names() -> None:
    messages = [log_event(n, f"S{n}") for n in range(200)]
    named = {"S0", "S1", "S2", "S3", "S4"}
    sent, _ = run_hub(messages, limit=10, log_key=lambda source: source if source in named else "*")
    keys = {k for m in sent if m["type"] == "dropped" for k in m["log"]}
    assert keys and keys <= {"S0", "S1", "S2", "S3", "S4", "*"}, keys
    sent, _ = run_hub(messages, limit=10)  # without names from the summary: a bounded number of keys
    assert all(len(m["log"]) <= 65 for m in sent if m["type"] == "dropped")


def test_a_message_that_cannot_be_sent_does_not_stop_the_others() -> None:
    bad = {"type": "event", "session": 1, "event": {"event": "time", "seq": 1, "x": float("inf")}}
    sent, _ = run_hub([log_event(0), bad, log_event(2)], limit=100)
    assert [m["event"]["seq"] for m in sent if m["type"] == "event"] == [0, 2]


def test_one_line_that_cannot_be_passed_on_does_not_stop_the_reading() -> None:
    from conftest import CONFIGS, FAKE
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options
    server = GuiServer(Options(root=CONFIGS, launcher=Launcher(FAKE), port=1, token="t"))
    session = new_session()
    sent: list[dict] = []
    server.hub.broadcast = lambda messages, log_key=None, real=server.hub.broadcast: (
        sent.extend(messages), real(messages, log_key))[1]
    feed = session.feed_line
    calls = iter([
        lambda text: [{"type": "event", "session": 1, "event": {"event": "time", "seq": 1, "x": float("inf")}}],
        lambda text: 1 / 0,
    ])
    session.feed_line = lambda text: next(calls, feed)(text)
    server.relay(session, line("time", 1, sim_ms=1))  # a message that cannot be turned into JSON
    server.relay(session, line("time", 2, sim_ms=2))  # an exception while reading it
    server.relay(session, line("hello", 0, vehicle_sil_events=1))
    assert kinds(sent[1:]) == ["error:bad-event", "error:bad-event", "event"]
    assert "ZeroDivisionError" in sent[2]["message"]


def test_messages_are_valid_utf_8() -> None:
    sent, _ = run_hub([{"type": "event", "session": 1, "event": {"event": "log", "seq": 0, "source": "A",
                                                                 "line": "a\ud800b 日"}}])
    assert sent[2]["event"]["line"] == "a\ud800b 日"


# ---------------------------------------------------------------- paths


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    root = tmp_path / "root"
    (root / "sub" / "deeper").mkdir(parents=True)
    (root / ".hidden").mkdir()
    (root / "a.yaml").write_text("vehicle_sil_simulation: 1\n")
    (root / "b.yml").write_text("other: 1\n")
    (root / "notes.txt").write_text("x")
    (root / "sub" / "deeper" / "c.YAML").write_text("vehicle_sil_simulation: 1\n")
    (root / ".hidden" / "h.yaml").write_text("x")
    (tmp_path / "outside.yaml").write_text("vehicle_sil_simulation: 1\n")
    (tmp_path / "outdir").mkdir()
    (tmp_path / "outdir" / "o.yaml").write_text("x")
    os.symlink(tmp_path / "outside.yaml", root / "link-out.yaml")
    os.symlink(tmp_path / "outdir", root / "linkdir")
    os.symlink(root / "a.yaml", root / "link-in.yaml")
    os.symlink(root / "notes.txt", root / "notes.yaml")
    return root


@pytest.mark.skipif(sys.platform == "win32", reason="symbolic links need privileges on Windows")
def test_only_configuration_files_under_the_root_are_accepted(tree: Path) -> None:
    r = ConfigRoot(tree)
    assert r.resolve("a.yaml").relative == "a.yaml"
    assert r.resolve("sub/deeper/c.YAML").absolute == (tree / "sub/deeper/c.YAML").resolve()
    assert r.resolve("link-in.yaml").absolute == (tree / "a.yaml").resolve()
    rejected = {
        "../outside.yaml": "outside-root", "sub/../a.yaml": "outside-root", "./a.yaml": "outside-root",
        "sub//deeper/c.YAML": "outside-root", "/etc/passwd.yaml": "outside-root",
        str(tree / "a.yaml"): "outside-root", "C:/x.yaml": "outside-root",
        "link-out.yaml": "outside-root", "linkdir/o.yaml": "outside-root",
        "notes.txt": "not-a-configuration", "notes.yaml": "not-a-configuration", "sub": "not-a-configuration",
        "missing.yaml": "file-not-found", "sub\\deeper\\c.YAML": "invalid-path", "a.yaml\x00": "invalid-path",
        "": "invalid-path", 5: "invalid-path", None: "invalid-path", "x" * 2000 + ".yaml": "invalid-path",
        "a.yaml:b.yaml": "invalid-path", "sub:x/c.yaml": "invalid-path", "a\ud800.yaml": "invalid-path",
    }
    for path, code in rejected.items():
        with pytest.raises(PathRejected) as e:
            r.resolve(path)
        assert e.value.code == code, (path, e.value.code, str(e.value))


@pytest.mark.skipif(sys.platform == "win32", reason="symbolic links need privileges on Windows")
def test_the_listing_stays_under_the_root(tree: Path) -> None:
    files, truncated = ConfigRoot(tree).list()
    assert [f["path"] for f in files] == ["a.yaml", "b.yml", "link-in.yaml", "sub/deeper/c.YAML"]
    assert [f["simulation"] for f in files] == [True, False, True, True]
    assert not truncated


# ---------------------------------------------------------------- the command lines


def test_disable_and_speed_are_checked() -> None:
    assert check_disable(["A"], ["A", "B"]) == ["A"]
    assert check_disable(None, []) == []
    for bad in (["C"], ["--json"], "A", [1], ["A", "A"]):
        with pytest.raises(InvalidRequest):
            check_disable(bad, ["A", "B"])
    assert check_speed(None) is None and check_speed("max") == "max" and check_speed(2) == 2.0
    for bad in (True, 0, -1, 1e9, 1e-7, float("nan"), float("inf"), "fast", [1]):
        with pytest.raises(InvalidRequest):
            check_speed(bad)
    assert check_speed(0.000001) == 1e-6 and check_speed(1000000) == 1e6


def test_speeds_and_time_commands_are_written_as_the_launcher_reads_them() -> None:
    # The shortest decimal that is the number: never rounded, and no ".0"
    assert [speed_argument(s) for s in ("max", 2.0, 0.5, 1e-6, 1.2345678, 1e6)] == \
        ["max", "2", "0.5", "1e-06", "1.2345678", "1000000"]
    assert command_line(check_time_command({"command": "pause"})) == b'{"command":"pause"}\n'
    assert command_line(check_time_command({"command": "resume"})) == b'{"command":"resume"}\n'
    assert command_line(check_time_command({"command": "step", "ms": 25})) == b'{"command":"step","ms":25}\n'
    assert command_line(check_time_command({"command": "speed", "value": 2})) == b'{"command":"speed","value":2}\n'
    assert command_line(check_time_command({"command": "speed", "value": "max"})) == \
        b'{"command":"speed","value":"max"}\n'
    for bad in ({}, {"command": None}, {"command": "stop"}, {"command": "step", "ms": 0}, {"command": "step"},
                {"command": "speed", "value": None}, {"command": "resume", "value": 1}):
        with pytest.raises(InvalidRequest):
            check_time_command(bad)


# ---------------------------------------------------------------- static files


def test_static_files_stay_in_their_directory(tmp_path: Path) -> None:
    web = tmp_path / "web"
    (web / "assets").mkdir(parents=True)
    (web / "locale").mkdir()
    (web / "index.html").write_text("<html>")
    (web / "locale" / "messages.en.json").write_text("{}")
    (web / "main.js").write_text("x")
    (web / "assets" / "a.svg").write_text("<svg/>")
    (web / "secret.py").write_text("x")
    (web / ".env").write_text("x")
    (tmp_path / "outside.js").write_text("x")
    os.symlink(tmp_path / "outside.js", web / "link.js")
    s = StaticFiles(web)
    assert s.lookup("/main.js")[1:] == ("text/javascript; charset=utf-8", 200)
    assert s.lookup("/assets/a.svg")[1:] == ("image/svg+xml", 200)
    assert s.lookup("/locale/messages.en.json")[1:] == ("application/json", 200)  # the screen's translations
    assert s.lookup("/")[0].name == "index.html"
    assert s.lookup("/run/some/route")[0].name == "index.html"  # the screen's routes
    for path in ("/api/x", "/api", "/ws", "/ws/x", "/secret.py", "/.env", "/../outside.js", "/assets/../../x.js",
                 "/missing.js", "/locale/messages.xx.json", "/link.js", "/a\\b.js", "/C:/x.js"):
        assert s.lookup(path)[2] == 404, path
    assert StaticFiles(None).lookup("/") == (None, None, 200)  # the page that says how to build the screen


# ---------------------------------------------------------------- processes


def test_lines_are_split_and_long_lines_skipped() -> None:
    async def main() -> list:
        reader = asyncio.StreamReader()
        reader.feed_data(b"one\r\n" + b"x" * 100 + b"\ntwo\n\xff\nend")
        reader.feed_eof()
        return [x async for x in process.lines(reader, 50)]

    assert asyncio.run(main()) == ["one", None, "two", "\ufffd", "end"]


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX process groups")
def test_a_command_that_does_not_end_is_killed_with_its_children(tmp_path: Path) -> None:
    pids = tmp_path / "pids"
    code = ("import os, subprocess, sys, time\n"
            "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
            f"open({str(pids)!r}, 'w').write(f'{{os.getpid()}} {{child.pid}}')\n"
            "time.sleep(60)\n")

    async def main() -> None:
        with pytest.raises(TimeoutError):
            await process.run_to_end([sys.executable, "-c", code], cwd=None, timeout_s=2)

    asyncio.run(main())
    from conftest import alive, wait_until
    parent, child = map(int, pids.read_text().split())
    assert wait_until(lambda: not alive(parent) and not alive(child), 5)


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX process groups")
def test_a_cancelled_command_is_killed_with_its_children(tmp_path: Path) -> None:
    pids = tmp_path / "pids"
    code = ("import os, subprocess, sys, time\n"
            "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
            f"open({str(pids)!r}, 'w').write(f'{{os.getpid()}} {{child.pid}}')\n"
            "time.sleep(60)\n")

    async def main() -> None:
        task = asyncio.create_task(process.run_to_end([sys.executable, "-c", code], cwd=None, timeout_s=60))
        while not pids.exists() or not pids.read_text():
            await asyncio.sleep(0.05)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(main())
    from conftest import alive, wait_until
    parent, child = map(int, pids.read_text().split())
    assert wait_until(lambda: not alive(parent) and not alive(child), 5)


# ---------------------------------------------------------------- the launcher's version


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX process groups")
def test_the_last_resort_kills_a_version_probe_that_hangs(tmp_path: Path) -> None:
    from conftest import CONFIGS, alive
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options
    pid_file = tmp_path / "pid"
    path = tmp_path / "launcher.py"
    path.write_text(f"import os, time\nopen({str(pid_file)!r}, 'w').write(str(os.getpid()))\ntime.sleep(60)\n")
    server = GuiServer(Options(root=CONFIGS, launcher=Launcher(path), port=1, token="t"))

    async def main() -> bool:
        probe = asyncio.create_task(server.launcher.probe())
        while not pid_file.exists() or not pid_file.read_text():
            await asyncio.sleep(0.05)
        server.kill_all()  # what the server's end does when it takes too long: the probe is not left
        pid = int(pid_file.read_text())
        for _ in range(60):
            if not alive(pid):
                break
            await asyncio.sleep(0.05)
        gone = not alive(pid)
        probe.cancel()
        await asyncio.gather(probe, return_exceptions=True)
        return gone

    assert asyncio.run(main())


def test_concurrent_probes_wait_for_the_answer(tmp_path: Path) -> None:
    from conftest import FAKE
    from vehicle_sil_gui.launcher import Launcher
    path = tmp_path / "launcher.py"
    path.write_text(FAKE.read_text())
    launcher = Launcher(path)

    async def main() -> list[bool]:
        await launcher.probe()
        assert launcher.usable
        path.write_text("import time\ntime.sleep(0.5)\nprint('not the version object')\n")  # replaced

        async def probe() -> bool:
            await launcher.probe()
            return launcher.usable
        return await asyncio.gather(probe(), probe())

    assert asyncio.run(main()) == [False, False]


def test_a_compressed_neighbour_is_not_served(tmp_path: Path) -> None:
    # aiohttp's FileResponse would answer with main.js.gz to a client that accepts gzip; the server reads the
    # file it checked instead.
    from aiohttp.test_utils import make_mocked_request
    from conftest import CONFIGS, FAKE
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options
    web = tmp_path / "web"
    web.mkdir()
    (web / "index.html").write_text("<html>")
    (web / "main.js").write_text("the file")
    (web / "main.js.gz").write_bytes(b"not this one")
    (web / "main.js.br").write_bytes(b"nor this one")
    server = GuiServer(Options(root=CONFIGS, launcher=Launcher(FAKE), port=1, token="t", web_dir=web))

    async def main():
        request = make_mocked_request("GET", "/main.js", headers={"Accept-Encoding": "gzip, br"})
        return await server.get_static(request)

    response = asyncio.run(main())
    assert response.body == b"the file" and response.headers["Content-Type"] == "text/javascript; charset=utf-8"
    assert "Content-Encoding" not in response.headers


def test_an_unexpected_error_is_a_500_with_the_usual_headers() -> None:
    from aiohttp.test_utils import make_mocked_request
    from conftest import CONFIGS, FAKE
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options
    server = GuiServer(Options(root=CONFIGS, launcher=Launcher(FAKE), port=1, token="t"))

    async def failing(request):
        raise RuntimeError("a bug")

    async def main():
        return await server._headers(make_mocked_request("GET", "/api/app"), failing)

    response = asyncio.run(main())
    assert response.status == 500 and json.loads(response.body)["error"]["code"] == "internal"
    assert "script-src 'self'" in response.headers["Content-Security-Policy"]
    assert response.headers["Cache-Control"] == "no-store"
