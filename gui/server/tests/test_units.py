"""The parts without a server: the session model, the hub's slow-client handling, paths, static files, lines."""

from __future__ import annotations

import asyncio
import json
import math
import os
import sys
from pathlib import Path

import pytest
from vehicle_sil_gui import process
from vehicle_sil_gui.allowed import Facts
from vehicle_sil_gui.hub import Client, Hub, encode
from vehicle_sil_gui.launcher import (InvalidRequest, Launcher, check_command, command_line, hide_request,
                                     request_text, serve_command, serve_run_command, speed_argument,
                                     write_request_file)
from vehicle_sil_gui.paths import ConfigRoot, PathRejected
from vehicle_sil_gui.session import Limits, Session
from vehicle_sil_gui.static import StaticFiles


def line(event: str, seq: int, **fields) -> str:
    return json.dumps({"event": event, "seq": seq, "t_ms": seq, **fields})


def feed_line(session: Session, text: str) -> list[dict]:
    size = len(text.encode("utf-8", "surrogatepass"))
    try:
        ev = json.loads(
            text,
            parse_constant=lambda c: (_ for _ in ()).throw(ValueError(c)),
            parse_float=lambda s: float(s) if math.isfinite(float(s)) else (_ for _ in ()).throw(ValueError(s)),
        )
    except (ValueError, RecursionError):
        return session.bad_line(text)
    if not isinstance(ev, dict) or not isinstance(ev.get("event"), str):
        return session.bad_line(text)
    return session.feed_event(ev, size)


def new_session(**limits) -> Session:
    s = Session("1", "simulation.yaml", [], None, False, limits=Limits(**limits))
    s.next_stream_seq = lambda: (s.last_seq if s.last_seq is not None else 0)
    return s


def kinds(messages: list[dict]) -> list[str]:
    return [m["type"] if m["type"] != "error" else f"error:{m['code']}" for m in messages]


# ---------------------------------------------------------------- session


def test_session_follows_the_events() -> None:
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    check = {"participants": [{"name": "A", "required": True, "enabled": True, "variables": []},
                              {"name": "B", "required": False, "enabled": False, "variables": None}]}
    feed_line(s, line("check", 1, check=check))
    feed_line(s, line("phase", 2, phase="running"))
    feed_line(s, line("participant", 3, name="B", state="disabled"))
    feed_line(s, line("participant", 4, name="A", state="started", pid=7, command=["a"], log=None))
    feed_line(s, line("participant", 5, name="A", state="ended", exit_code=3, signal=None))
    feed_line(s, line("finding", 6, severity="error", code="participant-failed", message="m"))
    feed_line(s, line("time", 7, sim_ms=300))
    feed_line(s, line("registry", 8, uri="silkit://x"))
    feed_line(s, line("command", 9, command="stop", accepted=True))
    feed_line(s, line("result", 10, result={"result": "failed"}))
    assert kinds(s.launcher_exited(1)) == ["session"] and s.summary()["state"] == "running"
    out = s.ended()
    summary = s.summary()
    assert summary["launcher"] == {
        "pid": None,
        "running": False,
        "exit_code": 1,
        "signal": None,
        "silent_s": None,
        "mode": "cold",
        "process_alive": False,
    }
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


def test_only_the_last_state_of_the_launcher_is_kept() -> None:
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    assert s.summary()["launcher_state"] is None
    first = {"rev": 1, "phase": "starting", "allowed": {"pause": {"ok": True, "reason": None}}}
    out = feed_line(s, line("state", 1, **first))
    assert kinds(out) == ["event"] and s.summary()["launcher_state"] == {"seq": 1, **first}
    second = {"rev": 2, "phase": "running", "allowed": {"pause": {"ok": False, "reason": "not-running"}}}
    feed_line(s, line("state", 2, **second))
    assert s.summary()["launcher_state"] == {"seq": 2, **second}  # not a history: the one before is gone
    # a state of the wrong shape is not passed on and the one kept stays
    assert kinds(feed_line(s, line("state", 3, rev="x", allowed={}))) == ["error:bad-event"]
    assert kinds(feed_line(s, line("state", 4, rev=3, allowed=[]))) == ["error:bad-event"]
    assert s.summary()["launcher_state"]["rev"] == 2
    # too large to keep: it is shown now, the one kept stays, and the server says so once
    big = {"rev": 4, "allowed": {}, "outside": ["x" * 5000]}
    small = new_session(state_bytes=1000)
    feed_line(small, line("run_start", 0, run="r-1"))
    feed_line(small, line("state", 1, rev=1, allowed={}))
    out = feed_line(small, line("state", 2, **big))
    assert kinds(out) == ["error:kept-limit", "event"] and small.summary()["launcher_state"]["rev"] == 1


def test_the_command_event_returns_the_id_and_the_reason() -> None:
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    feed_line(s, line("command", 1, command="step", accepted=False, message="m", reason="not-paused", id=7))
    assert s.summary()["last_command"] == {"seq": 1, "command": "step", "accepted": False, "message": "m",
                                           "reason": "not-paused", "id": 7}
    assert kinds(feed_line(s, line("command", 2, command="step", accepted=True, id="7"))) == ["error:bad-event"]
    assert kinds(feed_line(s, line("command", 3, command="step", accepted=True, id=None))) == ["event"]


def test_allowed_says_what_this_server_takes() -> None:
    s = new_session()
    ok = {"ok": True, "reason": None, "message": None}

    def reasons(facts: Facts | None = None) -> dict:
        allowed = s.allowed(facts)
        return {k: v["reason"] for k, v in s.summary(allowed=allowed)["allowed"].items() if k != "rev"}

    assert reasons() == {"start": "already-running", "stop": None, "force": None, "save": None}
    assert s.summary()["allowed"]["stop"] == ok and s.allowed()["start"]["message"].startswith("a simulation is running")
    s.request_stop(False)
    assert reasons() == {"start": "already-running", "stop": "already-stopping", "force": None, "save": None}
    s.request_stop(True)
    assert reasons() == {"start": "already-running", "stop": "already-stopping", "force": "already-forced",
                         "save": None}
    s.launcher_exited(0)
    assert reasons()["stop"] == reasons()["force"] == "not-running" and reasons()["start"] == "already-running"
    s.ended()
    assert reasons() == {"start": None, "stop": "not-running", "force": "not-running", "save": None}
    # the server's own facts: the first that holds wins, in this order
    for facts, start, stop in (
            (Facts(launcher_usable=False, launcher_message="no launcher"), "launcher-unusable", "not-running"),
            (Facts(starting=True, launcher_usable=False), "starting", "starting"),
            (Facts(closing=True, starting=True), "shutting-down", "shutting-down")):
        assert (reasons(facts)["start"], reasons(facts)["stop"], reasons(facts)["force"]) == (start, stop, stop), facts
    assert reasons(Facts(closing=True, starting=True))["save"] == "shutting-down"
    no_save = Facts(save=False)
    assert reasons(no_save)["save"] == "save-disabled" and s.allowed(no_save)["save"]["caution"] is None
    allow_cmd = Facts(command_changes_allowed=True)
    assert reasons(allow_cmd)["save"] is None and s.allowed(allow_cmd)["save"]["caution"]["code"] == "command-changes-allowed"
    s = new_session()  # running
    assert reasons(Facts(starting=True))["start"] == "already-running"  # the run just registered, its start not yet answered
    unusable = Facts(launcher_usable=False, launcher_message="no launcher")
    s.launcher_exited(0, facts=unusable)
    s.ended(facts=unusable)
    assert s.summary(allowed=s.allowed(unusable))["allowed"]["start"]["message"] == "no launcher"


def test_allowed_reflects_stop_and_facts_changes() -> None:
    s = new_session()
    first = s.summary()["allowed"]
    assert "rev" not in first
    assert s.summary()["allowed"] == first and s.allowed() == first  # unchanged
    s.request_stop(False)
    second = s.summary()["allowed"]
    assert second["stop"]["reason"] == "already-stopping" and s.allowed() == second
    assert s.allowed(Facts(closing=True))["start"]["reason"] == "shutting-down"
    assert s.allowed() == second


def test_a_signal_is_not_an_exit_code() -> None:
    s = new_session()
    s.launcher_exited(-9)
    assert s.summary()["launcher"] == {
        "pid": None,
        "running": False,
        "exit_code": None,
        "signal": 9,
        "silent_s": None,
        "mode": "cold",
        "process_alive": False,
    }


def test_stop_requests_are_recorded_once() -> None:
    s = new_session()
    assert [s.request_stop(False), s.request_stop(False)] == ["graceful", None] and s.stop == "requested"
    assert [s.request_stop(True), s.request_stop(True), s.request_stop(False)] == ["force", None, None] and s.stop == "forced"
    s = new_session()
    assert s.request_stop(True) == "force" and s.stop == "forced"
    s = new_session()
    s.launcher_exited(0)
    assert s.request_stop(False) is None and s.stop == "none"  # nothing to stop


def test_lines_that_are_not_events_are_reported_and_not_passed_on() -> None:
    s = new_session()
    assert kinds(feed_line(s, "not json")) == ["error:launcher-output"]
    assert kinds(feed_line(s, "[1]")) == ["error:launcher-output"]
    assert kinds(feed_line(s, '{"event": "log"}')) == ["error:launcher-output"]  # no seq
    assert kinds(feed_line(s, '{"event": "log", "seq": true}')) == ["error:launcher-output"]
    assert kinds(feed_line(s, '{"event": "log", "seq": 0, "x": NaN}')) == ["error:launcher-output"]  # not JSON
    assert kinds(feed_line(s, line("log", 0, source="A", line="x"))) == ["event"]
    assert kinds(feed_line(s, line("mystery", 2, colour="blue"))) == ["event"]
    assert kinds(feed_line(s, line("log", 2))) == ["error:seq-order"]
    assert kinds(feed_line(s, line("log", 1))) == ["error:seq-order"]
    long = feed_line(s, "x" * 1000)[0]
    assert len(long["detail"]) < 400 and "more characters" in long["detail"]


# NOT_FINITE stands for numbers that are valid JSON but not finite: they are put in the line as they are.
NOT_FINITE = {"<1e999>": "1e999", "<-1e999>": "-1e999", "<1E400>": "1E400"}
WRONG_VALUES = [None, True, 0, -1, 2.5, "x", "", [], [1], {}, {"a": 1}, [{"name": 1}], "\ud800", 2 ** 64, -(2 ** 64),
                *NOT_FINITE]
KNOWN = {
    "run_start": {"run": "r-1"},
    "phase": {"phase": "running"},
    "check": {"check": {"participants": [{"name": "A", "required": True}]}},
    "registry": {"uri": "u"},
    "participant": {"name": "A", "state": "started", "pid": 3, "exit_code": None},
    "log": {"source": "A", "line": "l"},
    "finding": {"severity": "note", "code": "c", "message": "m"},
    "time": {"sim_ms": 1},
    "command": {"command": "stop", "accepted": True, "id": 3},
    "state": {"rev": 1, "allowed": {"pause": {"ok": True}}},
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
            out = feed_line(s, text)
            seq += 1
            assert all(m["type"] in ("event", "error") for m in out)
            for m in out:
                encode(m)  # strict JSON: no NaN or Infinity
            encode(s.summary())  # the summary can still be sent
    good = feed_line(s, line("phase", seq, phase="ended"))
    assert kinds(good) == ["event"] and s.summary()["phase"] == "ended"
    assert s.summary()["problems_omitted"] > 0  # many were reported (and the summary keeps a bounded number)


def test_numbers_that_are_not_finite_are_not_events() -> None:
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    for n, number in enumerate(("1e999", "-1e999", "1E400", "-0.5e+999")):
        out = feed_line(s, f'{{"event":"time","seq":{n + 1},"t_ms":0,"sim_ms":5,"actual_speed":{number}}}')
        assert kinds(out) == ["error:launcher-output"], number
    assert s.summary()["time"] is None and s.last_seq == 0
    encode(s.summary())
    assert kinds(feed_line(s, line("time", 5, sim_ms=5, actual_speed=1.5))) == ["event"]
    assert s.summary()["time"]["sim_ms"] == 5 and s.last_seq == 5


def test_seq_and_t_ms_are_integers_javascript_reads_exactly() -> None:
    s = new_session(dropped_seqs=4, log_line_bytes=10)
    feed_line(s, line("run_start", 0, run="r-1"))
    for seq in (2 ** 63, 2 ** 64, 2 ** 53 + 1, -1):
        assert kinds(feed_line(s, line("log", seq, source="A", line="x"))) == ["error:launcher-output"], seq
    out = feed_line(s, json.dumps({"event": "log", "seq": 1, "t_ms": 2 ** 64, "source": "A", "line": "x"}))
    assert kinds(out) == ["error:bad-event"] and s.last_t_ms == 0
    out = feed_line(s, line("log", 2 ** 53, source="A", line="a line too long to keep"))
    assert kinds(out) == ["event"]
    assert s.summary()["log"] == {"A": {"kept": 0, "dropped": 1}} and s.last_seq == 2 ** 53


def test_a_known_event_of_the_wrong_shape_is_not_passed_on() -> None:
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    out = feed_line(s, line("check", 1, check={"participants": 5}))
    assert kinds(out) == ["error:bad-event"] and s.summary()["check"] is None and s.last_seq == 1
    assert kinds(feed_line(s, line("log", 2, source=None, line="x"))) == ["error:bad-event"]
    assert s.summary()["log"] == {}


def test_a_different_events_format_is_reported_but_passed_on(tmp_path: Path) -> None:
    from types import SimpleNamespace
    from vehicle_sil_gui.runtime import ServeRunHandle, ServeRuntime, _Resident

    async def main() -> None:
        rt = ServeRuntime(Launcher(Path("vehicle_sil_launch")), cwd=tmp_path)
        proc = SimpleNamespace(returncode=None, pid=1234, stdin=None, stdout=None, stderr=None)
        res = _Resident(config="a.yaml", want_control=False, proc=proc, credentials=None, token=None)  # type: ignore[arg-type]
        handle = ServeRunHandle(res, command_id=1, on_finished=lambda _: None)
        res.pending_run = handle

        rt._handle_stdout_line(res, line("hello", 0, vehicle_sil_events=3, launcher="inst-1"))
        rt._handle_stdout_line(res, line("run_start", 1, run="r-1", id=1))
        # Also verify stream-wide seq-gap detection on ServeRuntime.
        rt._handle_stdout_line(res, line("phase", 4, run="r-1", phase="running"))

        items = []
        while not handle._queue.empty():
            items.append(handle._queue.get_nowait())
        assert items[0][0] == "problem" and items[0][1] == "events-format"
        assert items[0][3] == {"vehicle_sil_events": 3}
        assert items[1][0] == "event" and items[1][1]["event"] == "run_start"
        assert items[2][0] == "problem" and items[2][1] == "seq-gap"
        assert items[2][3] == {"expected": 2, "got": 4}
        assert items[3][0] == "event" and items[3][1]["event"] == "phase"

    asyncio.run(main())


def test_ending_without_a_result_is_reported() -> None:
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    s.launcher_exited(70)
    out = s.ended()
    assert kinds(out) == ["error:no-result", "session"]
    assert s.summary()["launcher"]["exit_code"] == 70 and s.summary()["problems"][0]["detail"] == {"exit_code": 70}
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    feed_line(s, line("result", 1, result={"result": "passed"}))
    s.launcher_exited(0)
    assert kinds(s.ended(output_cut=True)) == ["error:output-left-open", "session"]


def test_problems_are_capped() -> None:
    s = new_session(problems=3)
    for _ in range(5):
        feed_line(s, "bad")
    assert len(s.summary()["problems"]) == 3 and s.summary()["problems_omitted"] == 2
    assert len(s.stderr_line("e" * 100_000)[0]["message"]) < 1100


def test_junk_does_not_hide_the_servers_notices_or_the_last_stderr_lines() -> None:
    s = new_session()
    for n in range(120):  # junk from the launcher: more than its room
        feed_line(s, f"junk {n}")
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
    feed_line(s, line("run_start", 0, run="r-1"))
    seq = 1
    for n in range(5):
        for source in ("A", "B", "C"):  # C is the third source: it shares the log of the later sources
            feed_line(s, line("log", seq, source=source, line=f"{source}{n}"))
            seq += 1
        feed_line(s, line("time", seq, sim_ms=n))
        seq += 1
    dropped, events = s.replay(None)
    lines = [m["event"]["line"] for m in events]
    assert lines == ["A2", "B2", "C2", "A3", "B3", "C3", "A4", "B4", "C4"]  # log only, in seq order
    assert dropped == {"type": "dropped", "run_id": "1", "reason": "not-kept",
                       "log": {"A": 2, "B": 2, "*": 2}, "at_least": [], "events": 0}
    assert s.summary()["log"] == {"A": {"kept": 3, "dropped": 2}, "B": {"kept": 3, "dropped": 2},
                                  "*": {"kept": 3, "dropped": 2}}
    assert s.summary()["time"]["sim_ms"] == 4
    a3 = next(m["stream_seq"] for m in events if m["event"]["line"] == "A3")
    dropped, events = s.replay(a3 - 1)
    assert dropped is None and [m["event"]["line"] for m in events] == ["A3", "B3", "C3", "A4", "B4", "C4"]
    dropped, events = s.replay(1)  # A0 (seq 1) is already received: it was dropped, but the screen has it
    assert dropped["log"] == {"A": 1, "B": 2, "*": 2}


def test_memory_is_bounded_in_bytes() -> None:
    s = new_session(dropped_seqs=100)
    feed_line(s, line("run_start", 0, run="r-1"))
    seq = 1
    for n in range(3000):  # 3000 lines of 2 KiB from 20 sources: 6 MiB in all, 1 MiB per source at most
        feed_line(s, line("log", seq, source=f"S{n % 20}", line="x" * 2048))
        seq += 1
    feed_line(s, line("log", seq, source="S0", line="y" * 100_000))  # too long to keep
    seq += 1
    assert all(log.bytes <= 1024 * 1024 for log in s._logs.values()) and s._log_bytes <= 16 * 1024 * 1024
    assert s.summary()["log"]["S0"]["dropped"] >= 1
    one = new_session()
    for n in range(700):  # 1.4 MiB from one source, under its line limit
        feed_line(one, line("log", n, source="A", line="x" * 2048))
    assert one._logs["A"].bytes <= 1024 * 1024 and one._logs["A"].dropped > 0
    small = new_session(log_bytes=100 * 1024)  # all sources together
    for n in range(200):
        feed_line(small, line("log", n, source=f"S{n % 5}", line="x" * 2048))
    assert small._log_bytes <= 100 * 1024 and sum(log.dropped for log in small._logs.values()) > 0
    big = {"participants": [], "stop_at_ms": 10000, "blob": "z" * (5 * 1024 * 1024)}
    assert "stop_at_ms" not in s.summary()
    out = feed_line(s, line("check", seq, check=big))
    seq += 1
    assert kinds(out) == ["error:kept-limit", "event"] and s.summary()["check"] is None
    assert s.summary()["stop_at_ms"] == 10000
    out = feed_line(s, line("result", seq, result={"result": "passed", "blob": "z" * (5 * 1024 * 1024)}))
    seq += 1
    assert kinds(out) == ["error:kept-limit", "event"] and s.summary()["result"] == {"result": "passed"}
    for n in range(1500):
        feed_line(s, line("finding", seq, severity="note", code="c", message=f"m{n}"))
        seq += 1
        feed_line(s, line("participant", seq, name=f"P{n}", state="started"))
        seq += 1
    for n in range(50):
        feed_line(s, line("mystery", seq, blob="w" * 10_000))  # unknown events are passed on, not kept
        seq += 1
    summary = s.summary()
    assert len(summary["findings"]) == 1000 and summary["findings_omitted"] == 500
    assert len(summary["participants"]) == 256 and summary["participants_omitted"] == 1244
    assert sorted(p["detail"]["what"] for p in summary["problems"] if p["code"] == "kept-limit") == \
        ["check", "findings", "participants", "result"]
    assert len(json.dumps(summary)) < 2 * 1024 * 1024


def test_dropped_counts_are_exact_with_a_line_too_long_to_keep() -> None:
    s = new_session(log_lines_per_source=5)
    feed_line(s, line("run_start", 0, run="r-1"))
    for seq in range(1, 12):  # seq 6 is too long to keep: dropped when it comes, before 1..5
        feed_line(s, line("log", seq, source="A", line="x" * (70_000 if seq == 6 else 1)))
    for after_seq, want in ((None, 6), (0, 6), (3, 3), (5, 1)):
        dropped = s.replay(after_seq)[0]
        assert dropped["log"] == {"A": want} and dropped["at_least"] == [], after_seq
    assert s.replay(6)[0] is None
    small = new_session(log_lines_per_source=2, dropped_seqs=3)  # fewer recorded than dropped
    feed_line(small, line("run_start", 0, run="r-1"))
    for seq in range(1, 9):  # 4 too long: dropped first; kept 7, 8; dropped 1..6
        feed_line(small, line("log", seq, source="A", line="x" * (70_000 if seq == 4 else 1)))
    assert small.replay(3)[0]["log"] == {"A": 3} and small.replay(3)[0]["at_least"] == []  # 4, 5, 6
    assert small.replay(2)[0]["log"] == {"A": 3} and small.replay(2)[0]["at_least"] == ["A"]


def test_log_dropped_is_counted_and_kept_where_it_came() -> None:
    s = new_session(log_lines_per_source=3)
    feed_line(s, line("run_start", 0, run="r-1"))
    feed_line(s, line("log", 1, source="A", line="a1"))
    out = feed_line(s, line("log_dropped", 2, count=125, sources={"A": 120, "vehicle_sil_launch": 5}))
    assert kinds(out) == ["event"]
    feed_line(s, line("log", 3, source="A", line="a3"))
    assert s.summary()["log"] == {"A": {"kept": 2, "dropped": 120}, "vehicle_sil_launch": {"kept": 0, "dropped": 5}}
    _, events = s.replay(None)
    assert [e["event"]["seq"] for e in events] == [1, 2, 3], "the marker once, in its place"
    _, events = s.replay(2)
    assert [e["event"]["seq"] for e in events] == [3]
    for n in range(4, 8):  # the marker leaves with the old lines; the counts stay
        feed_line(s, line("log", n, source="A", line=f"a{n}"))
    assert s.summary()["log"]["A"] == {"kept": 3, "dropped": 120 + 3}
    assert kinds(feed_line(s, line("log_dropped", 8, count="many", sources={}))) == ["error:bad-event"]
    assert kinds(feed_line(s, line("log_dropped", 9, count=1, sources={"A": -1}))) == ["error:bad-event"]


def test_a_launcher_that_writes_nothing_is_reported_as_silent() -> None:
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    start = s.last_line_at
    assert s.check_silence(start + 4.9, 5) == []
    out = s.check_silence(start + 5.2, 5)
    assert kinds(out) == ["error:launcher-silent", "session"] and s.summary()["launcher"]["silent_s"] == 5
    assert s.check_silence(start + 9.0, 5) == [], "the summary changes every 5 s, not on each look"
    assert kinds(s.check_silence(start + 10.1, 5)) == ["session"] and s.summary()["launcher"]["silent_s"] == 10
    out = feed_line(s, line("heartbeat", 1))
    assert kinds(out) == ["session", "event"] and s.summary()["launcher"]["silent_s"] is None
    assert [p["code"] for p in s.problems] == ["launcher-silent"]
    feed_line(s, line("result", 2, result={"result": "passed"}))
    assert s.check_silence(s.last_line_at + 60, 5) == [], "nothing is promised after the result"
    s2 = new_session()
    s2.launcher_exited(0)
    assert s2.check_silence(s2.last_line_at + 60, 5) == [], "nor after the launcher has exited"


def test_a_participant_state_this_server_does_not_know_passes_through() -> None:
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    feed_line(s, line("participant", 1, name="A", state="started", pid=7, command=["a"], log=None))
    out = feed_line(s, line("participant", 2, name="A", state="failed", reason="process_ended", message="m"))
    assert kinds(out) == ["event"]
    feed_line(s, line("participant", 3, name="A", state="ended", exit_code=1, signal=None))
    feed_line(s, line("participant", 4, name="B", state="sleeping", why={"x": 1}))
    assert s.summary()["participants"] == [
        {"name": "A", "state": "ended", "pid": 7, "command": ["a"], "log": None, "reason": "process_ended",
         "message": "m", "exit_code": 1, "signal": None},
        {"name": "B", "state": "sleeping", "why": {"x": 1}}]


def test_the_control_link_of_a_participant_is_kept_in_its_entry() -> None:
    """The control event (a state of the participant's link to its variables) is laid over the participant's entry
    as one field, so that a screen opened later sees it; a state this server does not know passes through."""
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    feed_line(s, line("participant", 1, name="A", state="started", pid=7))
    feed_line(s, line("participant", 2, name="A", state="joined"))
    out = feed_line(s, line("control_link", 3, name="A", state="connected", variables=17))
    assert kinds(out) == ["event"]
    feed_line(s, line("control_link", 4, name="B", state="version_mismatch", version=2))
    feed_line(s, line("control_link", 5, name="C", state="some_new_state"))
    feed_line(s, line("participant", 6, name="A", state="ended", exit_code=0, signal=None))
    assert s.summary()["participants"] == [
        {"name": "A", "state": "ended", "pid": 7, "control_link": {"state": "connected", "variables": 17},
         "exit_code": 0, "signal": None},
        {"name": "B", "control_link": {"state": "version_mismatch", "version": 2}},
        {"name": "C", "control_link": {"state": "some_new_state"}}]
    assert kinds(feed_line(s, line("control_link", 7, name=5, state="connected"))) == ["error:bad-event"]


def test_an_event_this_server_does_not_know_is_passed_on_and_the_reading_goes_on() -> None:
    """A launcher newer than this server may write events it does not know: they reach the screens as they are
    (the screen skips what it does not know), nothing is reported, and the events after them are read as usual."""
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    out = feed_line(s, line("future_thing", 1, payload={"a": [1, 2]}, note="n"))
    assert kinds(out) == ["event"] and out[0]["event"]["payload"] == {"a": [1, 2]}
    out = feed_line(s, line("phase", 2, phase="running"))
    assert kinds(out) == ["event"] and s.summary()["phase"] == "running"
    assert s.summary()["problems"] == [] and s.last_seq == 2


def test_the_essential_fields_of_a_large_small_event_are_kept() -> None:
    s = new_session()
    feed_line(s, line("run_start", 0, run="r-1"))
    long_command = [f"--argument-{n}-" + "x" * 100 for n in range(45)]  # about 5 KiB
    out = feed_line(s, line("participant", 1, name="Engine", state="started", pid=7, command=long_command,
                           log="engine.log"))
    assert kinds(out) == ["error:kept-limit", "event"] and out[0]["detail"] == {"what": "participant"}
    assert s.summary()["participants"] == [{"name": "Engine", "state": "started", "pid": 7, "log": "engine.log",
                                            "omitted_fields": ["command"]}]
    feed_line(s, line("participant", 2, name="Dash", state="started", pid=8, command=["dash"]))
    feed_line(s, line("participant", 3, name="Dash", state="ended", exit_code=0, signal=None, note="n" * 5000))
    feed_line(s, line("participant", 4, name="Engine", state="ended", exit_code=1, signal=None))
    assert s.summary()["participants"] == [
        {"name": "Engine", "state": "ended", "pid": 7, "log": "engine.log", "exit_code": 1, "signal": None,
         "omitted_fields": ["command"]},  # still not known
        {"name": "Dash", "state": "ended", "pid": 8, "command": ["dash"], "exit_code": 0, "signal": None,
         "omitted_fields": ["note"]}]
    feed_line(s, line("participant", 5, name="Engine", state="ended", command=["engine"]))  # known again
    assert "omitted_fields" not in s.summary()["participants"][0]
    feed_line(s, line("phase", 6, phase="running", detail="d" * 5000))
    feed_line(s, line("time", 7, sim_ms=100, detail="d" * 5000))
    feed_line(s, line("command", 8, command="stop", accepted=True, message="m" * 5000))
    feed_line(s, line("registry", 9, uri="silkit://x", detail="d" * 5000))
    summary = s.summary()
    assert summary["phase"] == "running"
    assert summary["time"] == {"event": "time", "seq": 7, "t_ms": 7, "sim_ms": 100, "omitted_fields": ["detail"]}
    assert summary["last_command"] == {"seq": 8, "command": "stop", "accepted": True, "omitted_fields": ["message"]}
    assert summary["registry"] == {"uri": "silkit://x", "omitted_fields": ["detail"]}
    out = feed_line(s, line("participant", 10, name="N" * 5000, state="started"))  # not even the name fits
    assert kinds(out) == ["event"] and len(s.summary()["participants"]) == 2
    # The time with its lateness, however large the event: a screen opened later still sees how late it is
    feed_line(s, line("time", 11, sim_ms=300, actual_speed=0.9, late_steps=1, max_late_ms=200,
                     slipped_ms=200, late_ms=0, detail="d" * 5000))
    time = s.summary()["time"]
    assert {k: time[k] for k in ("sim_ms", "late_steps", "max_late_ms", "slipped_ms", "late_ms")} \
        == {"sim_ms": 300, "late_steps": 1, "max_late_ms": 200, "slipped_ms": 200,
            "late_ms": 0}
    assert time["omitted_fields"] == ["detail"]


def test_dropped_counts_are_exact_while_recorded() -> None:
    s = new_session(log_lines_per_source=10, dropped_seqs=20)
    feed_line(s, line("run_start", 0, run="r-1"))
    for seq in range(1, 101):
        feed_line(s, line("log", seq, source="A", line=str(seq)))
    # kept: seq 91..100; dropped: 1..90, of which 71..90 recorded
    assert s.replay(79)[0]["log"] == {"A": 11} and s.replay(79)[0]["at_least"] == []
    assert s.replay(90)[0] is None
    assert s.replay(49)[0]["log"] == {"A": 20} and s.replay(49)[0]["at_least"] == ["A"]
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
        drained = asyncio.Event()
        orig_next = client.next

        async def next_item() -> object:
            if gate.is_set() and not client.items:
                drained.set()
            return await orig_next()

        client.next = next_item  # type: ignore[method-assign]
        task = asyncio.create_task(hub.serve(client, None, None))
        await asyncio.sleep(0)
        for m in messages:
            hub.broadcast([m], log_key)
            assert len(client.items) <= limit + 1
        gate.set()
        await asyncio.wait_for(drained.wait(), timeout=2.0)
        hub.close_all()
        await task
        assert client not in hub.clients
        return sent, client

    return asyncio.run(main())


def log_event(seq: int, source: str = "A") -> dict:
    return {"type": "event", "run_id": "1", "event": {"event": "log", "seq": seq, "source": source, "line": "x"}}


def test_a_slow_screen_loses_log_lines_not_the_state() -> None:
    messages = [log_event(n, "A" if n % 2 else "B") for n in range(200)]
    messages[50] = {"type": "event", "run_id": "1", "event": {"event": "phase", "seq": 50, "phase": "running"}}
    messages[60] = {"type": "error", "run_id": "1", "code": "seq-gap", "message": "m", "detail": None}
    messages += [{"type": "event", "run_id": "1", "event": {"event": "time", "seq": 200 + n, "sim_ms": n}}
                 for n in range(30)]
    sent, client = run_hub(messages, limit=10)
    types = [m["type"] for m in sent]
    assert "resync" not in types and client.resyncs == 0
    assert any(m["type"] == "event" and m["event"]["event"] == "phase" for m in sent)
    assert any(m["type"] == "error" for m in sent)
    dropped = [m for m in sent if m["type"] == "dropped"]
    log_sent = [m for m in sent if m["type"] == "event" and m["event"]["event"] == "log"]
    assert sum(sum(d["log"].values()) for d in dropped) + len(log_sent) == 198
    assert all(d["reason"] == "slow-client" and d["run_id"] == "1" for d in dropped)
    times = [m["event"]["sim_ms"] for m in sent if m["type"] == "event" and m["event"]["event"] == "time"]
    assert times[-1] == 29 and times == sorted(times)


def test_a_slow_screen_keeps_the_last_state_of_the_launcher() -> None:
    messages = [log_event(n, "A") for n in range(200)]
    messages += [{"type": "event", "run_id": "1", "event": {"event": "state", "seq": 200 + n, "rev": n, "allowed": {}}}
                 for n in range(30)]
    sent, client = run_hub(messages, limit=10)
    assert client.resyncs == 0
    revs = [m["event"]["rev"] for m in sent if m["type"] == "event" and m["event"]["event"] == "state"]
    assert revs and revs[-1] == 29 and revs == sorted(revs)
    # what was dropped is counted for the log lines only: a state that a later one replaces is not "lost"
    assert not any(m["type"] == "dropped" and m["events"] for m in sent)


def test_a_slow_screen_counts_what_the_launcher_dropped_and_loses_no_heartbeat_count() -> None:
    messages = [log_event(n, "A") for n in range(100)]
    messages[40] = {"type": "event", "run_id": "1", "event": {"event": "log_dropped", "seq": 40, "count": 1005,
                                                             "sources": {"A": 1000, "B": 5}}}
    messages[41] = {"type": "event", "run_id": "1", "event": {"event": "heartbeat", "seq": 41}}
    sent, _ = run_hub(messages, limit=10)
    dropped = [m for m in sent if m["type"] == "dropped"]
    markers = [m for m in sent if m["type"] == "event" and m["event"]["event"] == "log_dropped"]
    log_sent = [m for m in sent if m["type"] == "event" and m["event"]["event"] == "log"]
    if not markers:  # shed: its counts are in the dropped lines of its sources, the heartbeat is not counted
        assert sum(d["log"].get("B", 0) for d in dropped) == 5
        assert sum(d["log"].get("A", 0) for d in dropped) + len(log_sent) == 98 + 1000
    assert all(d["events"] == 0 for d in dropped)


def test_a_slow_screen_gets_resync_when_dropping_is_not_enough() -> None:
    messages = [{"type": "event", "run_id": "1", "event": {"event": "finding", "seq": n, "severity": "note",
                                                           "message": "m"}} for n in range(100)]
    messages.append({"type": "event", "run_id": "1", "event": {"event": "phase", "seq": 100, "phase": "after"}})
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


def test_relay_event_handles_non_finite_dict_and_feed_exceptions() -> None:
    from conftest import CONFIGS, REPLAY
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options

    server = GuiServer(Options(root=CONFIGS, launcher=Launcher(REPLAY), port=1, token="t"))
    session = new_session()
    sent: list[dict] = []
    server.ws.hub.broadcast = lambda messages, log_key=None, real=server.ws.hub.broadcast: (
        sent.extend(messages), real(messages, log_key))[1]
    feed = session.feed_event
    calls = iter([
        lambda ev, sz: [{"type": "event", "run_id": "1", "event": {"event": "time", "seq": 1, "x": float("inf")}}],
        lambda ev, sz: 1 / 0,
    ])
    session.feed_event = lambda ev, sz: next(calls, feed)(ev, sz)
    server.runs.relay_event(session, {"event": "time", "seq": 1, "t_ms": 1, "sim_ms": 1}, 40)
    server.runs.relay_event(session, {"event": "time", "seq": 2, "t_ms": 2, "sim_ms": 2}, 40)
    server.runs.relay_event(session, {"event": "run_start", "seq": 0, "t_ms": 0, "run": "r-1"}, 50)
    assert kinds(sent[1:]) == ["error:bad-event", "error:bad-event", "event"]
    assert "ZeroDivisionError" in sent[2]["message"]


def test_session_feed_event_parity_limits_and_zero_json_loads(monkeypatch) -> None:
    import vehicle_sil_gui.session as session_mod

    s_line = new_session(log_line_bytes=20, object_bytes=80, state_bytes=60, findings_bytes=70)
    s_ev = new_session(log_line_bytes=20, object_bytes=80, state_bytes=60, findings_bytes=70)
    events = [
        {"event": "run_start", "seq": 0, "t_ms": 0, "run": "r-1"},
        {"event": "log", "seq": 1, "t_ms": 1, "source": "A", "line": "ok"},
        {"event": "log", "seq": 2, "t_ms": 2, "source": "A", "line": "too long for log_line_bytes"},
        {"event": "log_dropped", "seq": 3, "t_ms": 3, "count": 2, "sources": {"A": 2}},
        {"event": "finding", "seq": 4, "t_ms": 4, "severity": "warning", "message": "small"},
        {"event": "finding", "seq": 5, "t_ms": 5, "severity": "warning", "message": "exceeds findings_bytes " * 3},
        {"event": "state", "seq": 6, "t_ms": 6, "rev": 1, "allowed": {"pause": {"ok": True}}, "pad": "x" * 80},
        {"event": "check", "seq": 7, "t_ms": 7, "check": {"result": "passed", "participants": [], "pad": "x" * 100}},
        {"event": "result", "seq": 8, "t_ms": 8, "result": {"result": "passed", "extra": "x" * 100}},
    ]
    for ev in events:
        raw = json.dumps(ev, separators=(",", ":"))
        feed_line(s_line, raw)

    monkeypatch.setattr(session_mod.json, "loads", lambda *a, **kw: (_ for _ in ()).throw(AssertionError("unexpected json.loads")))
    for ev in events:
        raw = json.dumps(ev, separators=(",", ":"))
        s_ev.feed_event(ev, len(raw.encode("utf-8", "surrogatepass")))

    # Silent recovery clears silent_s cleanly
    s_ev.silent_s = 5
    msgs = s_ev.feed_event({"event": "time", "seq": 9, "t_ms": 9, "sim_ms": 9}, 35)
    assert [m["type"] for m in msgs] == ["session", "event"]
    assert s_ev.silent_s is None

    # Compare summaries on the shared prefix (seq 0..8)
    s_line.silent_s = None
    assert s_line.check == s_ev.check
    assert s_line.result == s_ev.result
    assert s_line.launcher_state == s_ev.launcher_state
    assert s_line.findings == s_ev.findings and s_line.findings_omitted == s_ev.findings_omitted
    assert s_line.log_key("A") == s_ev.log_key("A")
    assert s_line.replay(None) == s_ev.replay(None)


def test_messages_are_valid_utf_8() -> None:
    sent, _ = run_hub([{"type": "event", "run_id": "1", "event": {"event": "log", "seq": 0, "source": "A",
                                                                 "line": "a\ud800b 日"}}])
    assert sent[2]["event"]["line"] == "a\ud800b 日"


# ---------------------------------------------------------------- paths


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    root = tmp_path / "root"
    (root / "sub" / "deeper").mkdir(parents=True)
    (root / ".hidden").mkdir()
    (root / "a.yaml").write_text("vehicle_sil_simulation: 2\n")
    (root / "b.yml").write_text("other: 1\n")
    (root / "notes.txt").write_text("x")
    (root / "sub" / "deeper" / "c.YAML").write_text("vehicle_sil_simulation: 2\n")
    (root / ".hidden" / "h.yaml").write_text("x")
    (tmp_path / "outside.yaml").write_text("vehicle_sil_simulation: 2\n")
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


def test_speeds_and_time_commands_are_written_as_the_launcher_reads_them() -> None:
    # The shortest decimal that is the number: never rounded, and no ".0"
    assert [speed_argument(s) for s in ("max", 2.0, 0.5, 1e-6, 1.2345678, 1e6)] == \
        ["max", "2", "0.5", "1e-06", "1.2345678", "1000000"]
    assert command_line({"command": "pause"}) == b'{"command":"pause"}\n'
    assert command_line({"command": "resume"}) == b'{"command":"resume"}\n'
    assert command_line({"command": "step", "ms": 25}) == b'{"command":"step","ms":25}\n'
    assert command_line({"command": "speed", "value": 2.0}) == b'{"command":"speed","value":2}\n'
    assert command_line({"command": "speed", "value": "max"}) == b'{"command":"speed","value":"max"}\n'
    assert command_line({"command": "pause", "id": 0}) == b'{"command":"pause","id":0}\n'
    assert command_line({"command": "step", "ms": 25, "id": 2 ** 53}) == \
        b'{"command":"step","ms":25,"id":9007199254740992}\n'
    assert command_line({"command": "speed", "value": 0.5, "id": 4}) == \
        b'{"command":"speed","value":0.5,"id":4}\n'
    assert command_line({"command": "speed", "value": "max", "id": 4}) == \
        b'{"command":"speed","value":"max","id":4}\n'
    assert command_line({"command": "speed", "value": 2.0, "id": 4}, run_id="r-1") == \
        b'{"command":"speed","value":2,"id":4,"run":"r-1"}\n'
    assert command_line({"command": "pause"}, run_id="r-1") == b'{"command":"pause","run":"r-1"}\n'


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
    os.symlink(web / "main.js", web / "alias.js")  # a link, even to a file of the directory
    os.symlink(web / "assets", web / "linked")
    s = StaticFiles(web)
    assert s.lookup("/main.js")[1:] == ("text/javascript; charset=utf-8", 200)
    assert s.lookup("/assets/a.svg")[1:] == ("image/svg+xml", 200)
    assert s.lookup("/locale/messages.en.json")[1:] == ("application/json", 200)  # the screen's translations
    assert s.lookup("/")[0].name == "index.html"
    assert s.lookup("/run/some/route")[0].name == "index.html"  # the screen's routes
    for path in ("/api/x", "/api", "/ws", "/ws/x", "/secret.py", "/.env", "/../outside.js", "/assets/../../x.js",
                 "/missing.js", "/locale/messages.xx.json", "/link.js", "/alias.js", "/linked/a.svg", "/a\\b.js",
                 "/C:/x.js"):
        assert s.lookup(path)[2] == 404, path
    assert s.lookup("/run/some/route", page_load=False)[2] == 404  # a fetch under a route is not the page
    assert StaticFiles(None).lookup("/") == (None, None, 200)  # the page that says how to build the screen


@pytest.mark.skipif(sys.platform == "win32", reason="symbolic links need privileges on Windows")
def test_a_static_file_is_read_only_when_it_is_still_the_checked_one(tmp_path: Path, monkeypatch) -> None:
    from vehicle_sil_gui import static
    web = tmp_path / "web"
    (web / "assets").mkdir(parents=True)
    (web / "index.html").write_text("<html>")
    (web / "assets" / "a.js").write_text("inside")
    (tmp_path / "outside.js").write_text("secret")
    s = StaticFiles(web)
    path = s.lookup("/assets/a.js")[0]
    assert s.read(path) == b"inside"
    # Between the check and the reading, the file becomes a link out of the directory: not read.
    (web / "assets" / "a.js").unlink()
    os.symlink(tmp_path / "outside.js", web / "assets" / "a.js")
    assert s.read(path) is None
    # The same with a directory above it.
    (web / "assets" / "a.js").unlink()
    (tmp_path / "out").mkdir()
    (tmp_path / "out" / "a.js").write_text("secret")
    os.rename(web / "assets", tmp_path / "assets-old")
    os.symlink(tmp_path / "out", web / "assets")
    assert s.read(path) is None
    # A file larger than the limit is refused with its own error.
    (web / "big.js").write_text("x" * 11)
    monkeypatch.setattr(static, "MAX_FILE_BYTES", 10)
    with pytest.raises(static.TooLarge):
        s.read(s.lookup("/big.js")[0])


def test_the_docs_show_the_csp_that_the_screens_get() -> None:
    # docs/gui.md tells the authors of their own screens what the CSP allows: it shows the server's own.
    from conftest import REPOSITORY, manual_pages
    from vehicle_sil_gui.server import CSP
    for page in manual_pages("docs/gui.md"):
        name = page.relative_to(REPOSITORY).as_posix()
        assert f"\n{CSP}\n" in page.read_text(encoding="utf-8"), \
            f"{name}, the section on the CSP: copy the CSP of gui/server/.../server.py there"


# ---------------------------------------------------------------- processes


def test_lines_are_split_and_long_lines_skipped() -> None:
    async def main() -> list:
        reader = asyncio.StreamReader()
        reader.feed_data(b"one\r\n" + b"x" * 100 + b"\ntwo\n\xff\nend")
        reader.feed_eof()
        return [x async for x in process.lines(reader, 50)]

    assert asyncio.run(main()) == ["one", None, "two", "\ufffd", "end"]


_EXIT_PROTOCOL_MODES = [
    pytest.param(False, id="default"),
    pytest.param(True, id="exit-protocol"),
]


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX process groups")
@pytest.mark.parametrize("force_exit_protocol", _EXIT_PROTOCOL_MODES)
def test_a_command_exits_even_when_an_escaped_child_keeps_its_output_open(
    tmp_path: Path, monkeypatch, force_exit_protocol: bool,
) -> None:
    """Process exit, not pipe EOF, starts the output grace period (also on Python 3.11/3.12)."""
    from conftest import alive, scaled, wait_until
    import signal

    if force_exit_protocol:
        monkeypatch.setattr(process, "_USE_EXIT_PROTOCOL", True)

    pid_file = tmp_path / "escaped-pid"
    code = ("import os, subprocess, sys\n"
            "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=True)\n"
            f"open({str(pid_file)!r}, 'w').write(str(child.pid))\n"
            "print('parent exited', flush=True)\n")

    async def main() -> None:
        code_result, out, _ = await process.run_to_end([sys.executable, "-c", code], cwd=None, timeout_s=scaled(5))
        assert code_result == 0 and out == b"parent exited\n"

    try:
        asyncio.run(main())
        child = int(pid_file.read_text())
        assert alive(child), "the escaped child still holds the output open"
    finally:
        if pid_file.exists():
            child = int(pid_file.read_text())
            try:
                os.kill(child, signal.SIGKILL)
            except ProcessLookupError:
                pass
            assert wait_until(lambda: not alive(child), 5)


@pytest.mark.parametrize("force_exit_protocol", _EXIT_PROTOCOL_MODES)
def test_shutdown_wait_observes_exit_and_hurry(monkeypatch, force_exit_protocol: bool) -> None:
    from conftest import scaled
    from types import SimpleNamespace

    if force_exit_protocol:
        monkeypatch.setattr(process, "_USE_EXIT_PROTOCOL", True)

    async def main() -> None:
        # Real child exiting on stdin EOF: wait_exit_or_hurry wakes on exit and returns True.
        hurry = asyncio.Event()
        proc = await process.start([sys.executable, "-c", "import sys; sys.stdin.read()"], cwd=None)
        try:
            proc.stdin.close()
            assert await process.wait_exit_or_hurry(proc, hurry, scaled(5.0))
            assert proc.returncode == 0
        finally:
            process.kill_tree(proc)
            await process.wait_bounded(proc, process.KILL_WAIT_S)
            process.close_pipes(proc)

        # Running child with hurry set asynchronously: wakes immediately and returns False.
        proc = await process.start([sys.executable, "-c", "import sys; sys.stdin.read()"], cwd=None)
        try:
            hurry = asyncio.Event()
            asyncio.get_running_loop().call_later(0.02, hurry.set)
            assert not await process.wait_exit_or_hurry(proc, hurry, scaled(5.0))
            assert proc.returncode is None
            # Already-set hurry returns False immediately without waiting for timeout.
            assert not await process.wait_exit_or_hurry(proc, hurry, scaled(5.0))
            # Timeout on a running child returns False.
            assert not await process.wait_exit_or_hurry(proc, asyncio.Event(), 0.02)
            proc.stdin.close()
            async with asyncio.timeout(scaled(5.0)):
                assert await process.wait_exit(proc) == 0
        finally:
            process.kill_tree(proc)
            await process.wait_bounded(proc, process.KILL_WAIT_S)
            process.close_pipes(proc)

        # Same-tick race: if returncode is set as hurry fires, return True (unhashable/non-weakrefable stub also works).
        never = asyncio.Event()

        async def _never_wait() -> int:
            await never.wait()
            return 0

        raced = SimpleNamespace(returncode=None, wait=_never_wait)
        race_hurry = asyncio.Event()

        def _exit_and_hurry() -> None:
            raced.returncode = 0
            race_hurry.set()

        asyncio.get_running_loop().call_later(0.01, _exit_and_hurry)
        assert await process.wait_exit_or_hurry(raced, race_hurry, scaled(1.0))

        # Exceptions from wait_exit() must propagate instead of being misreported as exited=True.
        async def _failing_wait() -> int:
            raise RuntimeError("wait failed")

        failing = SimpleNamespace(returncode=None, wait=_failing_wait)
        with pytest.raises(RuntimeError, match="wait failed"):
            await process.wait_exit_or_hurry(failing, asyncio.Event(), scaled(1.0))

        no_wait = SimpleNamespace(returncode=None)
        with pytest.raises(AttributeError):
            await process.wait_exit_or_hurry(no_wait, asyncio.Event(), scaled(1.0))

    asyncio.run(main())


@pytest.mark.parametrize("force_exit_protocol", _EXIT_PROTOCOL_MODES)
def test_process_waits_observe_exit_without_polling(monkeypatch, force_exit_protocol: bool) -> None:
    from conftest import scaled

    if force_exit_protocol:
        monkeypatch.setattr(process, "_USE_EXIT_PROTOCOL", True)

    async def main() -> None:
        code, out, _ = await process.run_to_end([sys.executable, "-c", "print('done')"], cwd=None, timeout_s=scaled(5))
        assert code == 0 and out == b"done\n"

        # Fast-path on already-exited process: wait_exit, wait_bounded(0), and wait_exit_or_hurry(hurry_set, 0).
        proc = await process.start([sys.executable, "-c", "import sys; sys.stdin.read()"], cwd=None)
        try:
            proc.stdin.close()
            assert await process.wait_bounded(proc, scaled(5.0))
            assert proc.returncode == 0
            assert await process.wait_exit(proc) == 0
            assert await process.wait_bounded(proc, 0.0)
            hurry = asyncio.Event()
            hurry.set()
            assert await process.wait_exit_or_hurry(proc, hurry, 0.0)
        finally:
            process.kill_tree(proc)
            await process.wait_bounded(proc, process.KILL_WAIT_S)
            process.close_pipes(proc)

    asyncio.run(main())


@pytest.mark.parametrize("force_exit_protocol", _EXIT_PROTOCOL_MODES)
def test_process_concurrent_exit_and_hurry_waits(monkeypatch, force_exit_protocol: bool) -> None:
    from conftest import scaled

    if force_exit_protocol:
        monkeypatch.setattr(process, "_USE_EXIT_PROTOCOL", True)

    async def main() -> None:
        # Both wait_exit and wait_exit_or_hurry observe exit together (mirrors _follow_resident + _close_resident).
        proc = await process.start([sys.executable, "-c", "import sys; sys.stdin.read()"], cwd=None)
        try:
            follower = asyncio.create_task(process.wait_exit(proc))
            hurry = asyncio.Event()
            closer = asyncio.create_task(process.wait_exit_or_hurry(proc, hurry, scaled(5.0)))
            await asyncio.sleep(0)
            # An early hurry wakeup cancels only closer's waiter, not follower.
            hurry.set()
            assert not await closer
            assert not follower.done()
            # A second concurrent closer completes alongside follower when the child exits.
            closer2 = asyncio.create_task(process.wait_exit_or_hurry(proc, asyncio.Event(), scaled(5.0)))
            await asyncio.sleep(0)
            proc.stdin.close()
            async with asyncio.timeout(scaled(5.0)):
                code, closed = await asyncio.gather(follower, closer2)
            assert code == 0 and closed
        finally:
            process.kill_tree(proc)
            await process.wait_bounded(proc, process.KILL_WAIT_S)
            process.close_pipes(proc)

    asyncio.run(main())


@pytest.mark.parametrize("force_exit_protocol", _EXIT_PROTOCOL_MODES)
def test_process_wait_timeout_and_cancellation_cleanup(monkeypatch, force_exit_protocol: bool) -> None:
    from conftest import scaled

    if force_exit_protocol:
        monkeypatch.setattr(process, "_USE_EXIT_PROTOCOL", True)

    async def main() -> None:
        proc = await process.start([sys.executable, "-c", "import sys; sys.stdin.read()"], cwd=None)
        try:
            # Cancelled wait_exit cleans up without breaking later waiters.
            t1 = asyncio.create_task(process.wait_exit(proc))
            await asyncio.sleep(0)
            t1.cancel()
            with pytest.raises(asyncio.CancelledError):
                await t1

            # Cancelled wait_exit_or_hurry cancels both internal tasks cleanly.
            t2 = asyncio.create_task(process.wait_exit_or_hurry(proc, asyncio.Event(), scaled(5.0)))
            await asyncio.sleep(0)
            t2.cancel()
            with pytest.raises(asyncio.CancelledError):
                await t2

            # wait_bounded timeout returns False while process stays alive.
            assert not await process.wait_bounded(proc, 0.02)
            assert proc.returncode is None

            proc.stdin.close()
            async with asyncio.timeout(scaled(5.0)):
                assert await process.wait_exit(proc) == 0
        finally:
            process.kill_tree(proc)
            await process.wait_bounded(proc, process.KILL_WAIT_S)
            process.close_pipes(proc)

    asyncio.run(main())


@pytest.mark.parametrize("force_exit_protocol", _EXIT_PROTOCOL_MODES)
def test_process_wait_exit_abnormal_termination(monkeypatch, force_exit_protocol: bool) -> None:
    from conftest import scaled
    import signal

    if force_exit_protocol:
        monkeypatch.setattr(process, "_USE_EXIT_PROTOCOL", True)

    async def main() -> None:
        # Non-zero exit code.
        proc = await process.start([sys.executable, "-c", "import sys; sys.exit(7)"], cwd=None, stdin=False)
        try:
            async with asyncio.timeout(scaled(5.0)):
                assert await process.wait_exit(proc) == 7
        finally:
            process.kill_tree(proc)
            await process.wait_bounded(proc, process.KILL_WAIT_S)
            process.close_pipes(proc)

        # SIGKILL termination (POSIX).
        if sys.platform != "win32":
            proc = await process.start([sys.executable, "-c", "import sys; sys.stdin.read()"], cwd=None)
            try:
                waiter = asyncio.create_task(process.wait_exit(proc))
                await asyncio.sleep(0)
                process.kill_tree(proc)
                async with asyncio.timeout(scaled(5.0)):
                    assert await waiter == -signal.SIGKILL
            finally:
                process.kill_tree(proc)
                await process.wait_bounded(proc, process.KILL_WAIT_S)
                process.close_pipes(proc)

    asyncio.run(main())


def test_process_exit_events_weakref_cleanup(monkeypatch) -> None:
    from conftest import scaled
    import gc
    import weakref

    monkeypatch.setattr(process, "_USE_EXIT_PROTOCOL", True)

    async def main() -> None:
        gc.collect()
        baseline = len(process._EXIT_EVENTS)
        proc = await process.start([sys.executable, "-c", "pass"], cwd=None, stdin=False)
        try:
            assert proc in process._EXIT_EVENTS
            ref = weakref.ref(proc)
            async with asyncio.timeout(scaled(5.0)):
                assert await process.wait_exit(proc) == 0
        finally:
            process.kill_tree(proc)
            await process.wait_bounded(proc, process.KILL_WAIT_S)
            process.close_pipes(proc)
        del proc
        # Drain pending transport connection_lost callbacks before collecting.
        await asyncio.sleep(0)
        await asyncio.sleep(0)
        gc.collect()
        assert ref() is None
        assert len(process._EXIT_EVENTS) == baseline

    asyncio.run(main())


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX process groups")
def test_a_command_that_does_not_end_is_killed_with_its_children(tmp_path: Path) -> None:
    pids = tmp_path / "pids"
    code = ("import os, subprocess, sys, time\n"
            "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
            f"open({str(pids)!r}, 'w').write(f'{{os.getpid()}} {{child.pid}}')\n"
            "time.sleep(60)\n")

    async def main() -> None:
        from conftest import scaled
        # Python has to start both processes and write their IDs before the time limit ends them: scaled.
        with pytest.raises(TimeoutError):
            await process.run_to_end([sys.executable, "-c", code], cwd=None, timeout_s=scaled(2))

    asyncio.run(main())
    from conftest import alive, wait_until
    parent, child = map(int, pids.read_text().split())
    assert wait_until(lambda: not alive(parent) and not alive(child), 5)


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX process groups")
def test_a_cancelled_command_is_killed_with_its_children(tmp_path: Path) -> None:
    from conftest import alive, scaled, wait_until

    async def main() -> tuple[int, int]:
        ready: asyncio.Future[tuple[int, int]] = asyncio.get_running_loop().create_future()

        async def on_ready(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
            try:
                line = (await reader.readline()).decode("utf-8").strip()
                if line and not ready.done():
                    p, c = map(int, line.split())
                    ready.set_result((p, c))
            finally:
                writer.close()
                await writer.wait_closed()

        srv = await asyncio.start_server(on_ready, "127.0.0.1", 0)
        port = srv.sockets[0].getsockname()[1]
        code = ("import os, socket, subprocess, sys, time\n"
                "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
                f"with socket.create_connection(('127.0.0.1', {port})) as s:\n"
                "    s.sendall(f'{os.getpid()} {child.pid}\\n'.encode())\n"
                "time.sleep(60)\n")
        try:
            task = asyncio.create_task(process.run_to_end([sys.executable, "-c", code], cwd=None, timeout_s=60))
            try:
                parent, child = await asyncio.wait_for(ready, timeout=scaled(10.0))
            except BaseException:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
                raise
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await task
            return parent, child
        finally:
            srv.close()
            await srv.wait_closed()

    parent, child = asyncio.run(main())
    assert wait_until(lambda: not alive(parent) and not alive(child), 5)


# ---------------------------------------------------------------- the launcher's version


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX process groups")
def test_the_last_resort_kills_a_version_probe_that_hangs(tmp_path: Path) -> None:
    from conftest import CONFIGS, alive, scaled, wait_until
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options
    path = tmp_path / "launcher.py"

    async def main() -> bool:
        ready: asyncio.Future[int] = asyncio.get_running_loop().create_future()

        async def on_ready(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
            try:
                line = (await reader.readline()).decode("utf-8").strip()
                if line and not ready.done():
                    ready.set_result(int(line))
            finally:
                writer.close()
                await writer.wait_closed()

        srv = await asyncio.start_server(on_ready, "127.0.0.1", 0)
        port = srv.sockets[0].getsockname()[1]
        path.write_text(
            "import os, socket, time\n"
            f"with socket.create_connection(('127.0.0.1', {port})) as s:\n"
            "    s.sendall(f'{os.getpid()}\\n'.encode())\n"
            "time.sleep(60)\n",
        )
        server = GuiServer(Options(root=CONFIGS, launcher=Launcher(path), port=1, token="t"))
        try:
            probe = asyncio.create_task(server.runs.launcher.probe())
            try:
                pid = await asyncio.wait_for(ready, timeout=scaled(10.0))
                server.kill_all()  # what the server's end does when it takes too long: the probe is not left
                gone = await asyncio.to_thread(wait_until, lambda: not alive(pid), 5)
            finally:
                probe.cancel()
                await asyncio.gather(probe, return_exceptions=True)
            return gone
        finally:
            srv.close()
            await srv.wait_closed()

    assert asyncio.run(main())


def test_concurrent_probes_wait_for_the_answer(tmp_path: Path, monkeypatch) -> None:
    from conftest import REPLAY
    from vehicle_sil_gui.launcher import Launcher
    path = tmp_path / "launcher.py"
    path.write_text(REPLAY.read_text())
    launcher = Launcher(path)

    async def main() -> list[bool]:
        await launcher.probe()
        assert launcher.usable
        path.write_text("print('not the version object')\n")  # replaced (new size invalidates cached stamp)
        entered = asyncio.Event()
        release = asyncio.Event()
        calls = 0

        async def gated_run_to_end(*args, **kwargs):
            nonlocal calls
            calls += 1
            entered.set()
            await release.wait()
            return 0, b"not the version object\n", b""

        monkeypatch.setattr(process, "run_to_end", gated_run_to_end)

        async def probe() -> bool:
            await launcher.probe()
            return launcher.usable

        first = asyncio.create_task(probe())
        try:
            await asyncio.wait_for(entered.wait(), timeout=5.0)
            second = asyncio.create_task(probe())
            while not launcher._lock.locked():
                await asyncio.sleep(0)
            await asyncio.sleep(0)
        finally:
            release.set()
        results = await asyncio.gather(first, second)
        assert calls == 1
        return results

    assert asyncio.run(main()) == [False, False]


def test_a_compressed_neighbour_is_not_served(tmp_path: Path) -> None:
    # aiohttp's FileResponse would answer with main.js.gz to a client that accepts gzip; the server reads the
    # file it checked instead.
    from aiohttp.test_utils import make_mocked_request
    from conftest import CONFIGS, REPLAY
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options
    web = tmp_path / "web"
    web.mkdir()
    (web / "index.html").write_text("<html>")
    (web / "main.js").write_text("the file")
    (web / "main.js.gz").write_bytes(b"not this one")
    (web / "main.js.br").write_bytes(b"nor this one")
    server = GuiServer(Options(root=CONFIGS, launcher=Launcher(REPLAY), port=1, token="t", web_dir=web))

    async def main():
        request = make_mocked_request("GET", "/main.js", headers={"Accept-Encoding": "gzip, br"})
        return await server.get_static(request)

    response = asyncio.run(main())
    assert response.body == b"the file" and response.headers["Content-Type"] == "text/javascript; charset=utf-8"
    assert "Content-Encoding" not in response.headers


def test_an_unexpected_error_is_a_500_with_the_usual_headers() -> None:
    from aiohttp.test_utils import make_mocked_request
    from conftest import CONFIGS, REPLAY
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options
    server = GuiServer(Options(root=CONFIGS, launcher=Launcher(REPLAY), port=1, token="t"))

    async def failing(request):
        raise RuntimeError("a bug")

    async def main():
        return await server._headers(make_mocked_request("GET", "/api/app"), failing)

    response = asyncio.run(main())
    assert response.status == 500 and json.loads(response.body)["error"]["code"] == "internal"
    assert "script-src 'self'" in response.headers["Content-Security-Policy"]
    assert response.headers["Cache-Control"] == "no-store"


def test_the_intent_becomes_a_request_file_and_the_launchers_arguments(tmp_path: Path) -> None:
    launcher = Launcher(Path("vehicle_sil_launch"))
    intent = {"variant": "-small", "params": {"gain": 0.5}, "scenario": "scenarios/s.yaml", "disable": ["-x"],
              "speed": 2.0, "stop_at_ms": 1003}
    assert json.loads(request_text(intent)) == {"vehicle_sil_run_request": 1, **intent}
    request = write_request_file(intent)
    try:
        body = json.loads(request.read_text(encoding="utf-8"))
        assert body["variant"] == "-small" and body["stop_at_ms"] == 1003
        if os.name == "posix":
            assert request.stat().st_mode & 0o777 == 0o600  # the other users of this computer cannot read it
    finally:
        request.unlink()
    with pytest.raises(InvalidRequest):
        write_request_file({"variant": "\ud800"})
    # The arguments are the launcher's side only: the intent is not among them
    assert check_command(launcher, "f.yaml", request)[1:] == ["check", "--json", "--request", str(request), "--",
                                                              "f.yaml"]
    assert serve_command(launcher, "f.yaml")[1:] == ["serve", "--", "f.yaml"]
    assert serve_command(launcher, "f.yaml", once=True)[1:] == ["serve", "--once", "--", "f.yaml"]
    credentials = tmp_path / "c.txt"
    assert serve_command(launcher, "f.yaml", credentials, once=True)[1:] == [
        "serve", "--once", "--control", "0", "--control-credentials", str(credentials),
        "--", "f.yaml"]
    assert json.loads(serve_run_command(request, command_id=1)) == {
        "command": "run", "request": str(request), "id": 1
    }


def test_the_name_of_the_request_file_is_hidden_from_what_the_launcher_says(tmp_path: Path) -> None:
    path = tmp_path / "vehicle-sil-gui-request-x.json"
    finding = {"severity": "error", "code": "usage", "file": str(path), "line": 5,
               "message": f"{path}, line 5: params has a name that is not a name; variant in {path}"}
    other = {"severity": "note", "code": "x", "file": "a.yaml", "line": 3, "message": "a.yaml, line 3: fine"}
    hidden = hide_request({"findings": [finding, other]}, path)
    assert hidden["findings"][0] == {"severity": "error", "code": "usage", "file": "the request", "line": None,
                                     "message": "the request: params has a name that is not a name; variant in "
                                                "the request"}
    assert hidden["findings"][1] == other

    from vehicle_sil_gui.launcher import hide_request_in_event
    unrelated = {"event": "time", "seq": 1, "sim_ms": 10}
    assert hide_request_in_event(unrelated, path) is unrelated
    nested = {"event": "check", "check": {"findings": [dict(finding), dict(other)]}}
    snapshot = json.dumps(nested)
    redacted = hide_request_in_event(nested, path)
    assert redacted is not nested
    assert redacted["check"]["findings"][0]["file"] == "the request"
    assert redacted["check"]["findings"][0]["line"] is None
    assert redacted["check"]["findings"][1] == other
    assert json.dumps(nested) == snapshot


def test_hide_request_in_event_deep_nesting_without_recursion_error(tmp_path: Path) -> None:
    from vehicle_sil_gui.launcher import hide_request_in_event

    path = tmp_path / "vehicle-sil-gui-request-deep.json"

    def make_deep_event(leaf: object, depth: int = 1000) -> dict:
        node: object = leaf
        for i in range(depth):
            node = [node] if i % 2 else {"nested": node}
        return {"event": "finding", "seq": 1, "t_ms": 1, "data": node}

    def unwrap(ev: dict, depth: int = 1000) -> dict:
        cur: object = ev["data"]
        for i in range(depth - 1, -1, -1):
            cur = cur[0] if i % 2 else cur["nested"]  # type: ignore[index]
        assert isinstance(cur, dict)
        return cur

    unrelated = make_deep_event({"file": "a.yaml", "line": 3, "message": "ok"})
    assert hide_request_in_event(unrelated, path) is unrelated

    leaf = {"file": str(path), "line": 42, "message": f"{path}, line 42: deep error"}
    with_req = make_deep_event(dict(leaf))
    redacted = hide_request_in_event(with_req, path)
    assert redacted is not with_req
    assert unwrap(redacted) == {"file": "the request", "line": None, "message": "the request: deep error"}
    assert unwrap(with_req) == leaf


def test_relay_event_guards_redaction_exception_and_redacts_detail(tmp_path: Path, monkeypatch) -> None:
    from conftest import CONFIGS, REPLAY
    from vehicle_sil_gui import run_manager
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options

    srv = GuiServer(Options(root=CONFIGS, launcher=Launcher(REPLAY), port=1, token="t"))
    session = new_session()
    sent: list[dict] = []
    srv.ws.hub.broadcast = lambda messages, log_key=None, real=srv.ws.hub.broadcast: (
        sent.extend(messages), real(messages, log_key))[1]
    req_path = tmp_path / "vehicle-sil-gui-request-secret.json"
    fake_run = type("_R", (), {"request": req_path, "session": session})()

    def boom(_ev, _path):
        raise RuntimeError("redaction boom")

    monkeypatch.setattr(run_manager, "hide_request_in_event", boom)
    srv.runs.relay_event(
        session,
        {"event": "finding", "seq": 0, "t_ms": 0, "message": f"{req_path}, line 9: secret"},
        64,
        fake_run,  # type: ignore[arg-type]
    )
    assert kinds(sent) == ["error:bad-event"]
    assert "RuntimeError" in sent[0]["message"]
    assert str(req_path) not in str(sent[0]["detail"])
    assert "the request" in str(sent[0]["detail"])



def test_a_request_file_that_cannot_be_made_is_said_so(monkeypatch) -> None:
    """No space, or no directory that can be written: 500 request-file with what to do (not "a bug"). The
    stand-in cannot make the temporary directory unusable (tempfile falls back to others), so the write fails here."""
    from vehicle_sil_gui import config_service
    from vehicle_sil_gui.refusal import Refused

    def full(intent):
        raise OSError(28, "No space left on device")

    monkeypatch.setattr(config_service, "write_request_file", full)
    with pytest.raises(Refused) as e:
        config_service.make_request_file({})
    assert (e.value.status, e.value.code) == (500, "request-file")
    assert "No space left on device" in str(e.value) and "TMPDIR" in str(e.value)


def test_timeout_scale(monkeypatch) -> None:
    import conftest

    for text, expected in (("3", 3.0), ("0.5", 0.5), (".5", 0.5), ("5.", 5.0), ("007", 7.0), ("0.001", 0.001)):
        monkeypatch.setenv("VEHICLE_SIL_TEST_TIMEOUT_SCALE", text)
        assert conftest._timeout_scale() == expected

    monkeypatch.delenv("VEHICLE_SIL_TEST_TIMEOUT_SCALE", raising=False)
    assert conftest._timeout_scale() == 1.0
    monkeypatch.setenv("VEHICLE_SIL_TEST_TIMEOUT_SCALE", "")
    assert conftest._timeout_scale() == 1.0

    for bad in ("0", "0.0", "000", "000.0", ".", "-1", "+3", "abc", "3x", " ", " 3", "3 ", "3\n",
                "1e3", "1e-3", "0x10", "1_000", "1.2.3", "inf", "nan", "Infinity", "NaN", "٢", "²",
                "9" * 400, "0." + ("0" * 400) + "1"):
        monkeypatch.setenv("VEHICLE_SIL_TEST_TIMEOUT_SCALE", bad)
        with pytest.raises(pytest.UsageError) as exc:
            conftest._timeout_scale()
        assert f'VEHICLE_SIL_TEST_TIMEOUT_SCALE="{bad}"' in str(exc.value)

    monkeypatch.setattr(conftest, "TIMEOUT_SCALE", 2.5)
    assert conftest.scaled(4.0) == 10.0


def test_stop_command_lines() -> None:
    from vehicle_sil_gui.launcher import stop_command_line

    assert stop_command_line() == b'{"command":"stop"}\n'
    assert stop_command_line("r-1") == b'{"command":"stop","run":"r-1"}\n'
    assert stop_command_line("r-1", force=True) == b'{"command":"stop","run":"r-1","force":true}\n'
    assert stop_command_line("r-1", force=False) == b'{"command":"stop","run":"r-1","force":false}\n'


def test_allowed_run_facts_and_stop_requests_projection() -> None:
    from vehicle_sil_gui.allowed import Facts, RunFacts, allowed, project_run_facts, stop_of

    assert stop_of("none", None) == "none"
    assert stop_of("none", 0) == "none"
    assert stop_of("none", 1) == "requested"
    assert stop_of("none", 2) == "forced"
    assert stop_of("none", 5) == "forced"
    assert stop_of("requested", 0) == "requested"
    assert stop_of("requested", 2) == "forced"
    assert stop_of("forced", 1) == "forced"

    # Cold mode ignores execution_status and preserves exact Session boolean semantics.
    cold_facts = project_run_facts(
        True,
        True,
        "none",
        launcher_mode="cold",
        execution_status={"launcher_state": "ready", "run_state": {"stop": {"requests": 2}}},
    )
    assert cold_facts == RunFacts(running=True, stoppable=True, stop="none")

    # Hot mode with running launcher_state and C++ stop.requests == 1.
    hot_stopping = project_run_facts(
        True,
        True,
        "none",
        launcher_mode="hot",
        execution_status={"launcher_state": "running", "run_state": {"stop": {"requests": 1}}},
    )
    assert hot_stopping == RunFacts(running=True, stoppable=True, stop="requested")
    ans = allowed(Facts(), hot_stopping)
    assert ans["start"]["reason"] == "already-running"
    assert ans["stop"]["reason"] == "already-stopping"
    assert ans["force"]["ok"] is True

    # Hot post-result async drain window: session is still running, but C++ launcher_state left "running".
    hot_draining = project_run_facts(
        True,
        True,
        "none",
        launcher_mode="hot",
        execution_status={"launcher_state": "ready", "run_state": None},
    )
    assert hot_draining == RunFacts(running=True, stoppable=False, stop="none")
    ans_drain = allowed(Facts(), hot_draining)
    assert ans_drain["start"]["reason"] == "already-running"
    assert ans_drain["stop"]["reason"] == "not-running"
    assert ans_drain["force"]["reason"] == "not-running"

    # Closing overrides everything.
    ans_closing = allowed(Facts(closing=True), hot_stopping)
    assert ans_closing["start"]["reason"] == "shutting-down"
    assert ans_closing["stop"]["reason"] == "shutting-down"
    assert ans_closing["force"]["reason"] == "shutting-down"

    # Hot mode with execution_status=None (e.g. launcher_ends=True async drain) never falls back to Cold stoppable=True.
    assert project_run_facts(
        True,
        True,
        "none",
        launcher_mode="hot",
        execution_status=None,
    ) == RunFacts(running=True, stoppable=False, stop="none")


def test_session_allowed_uses_hot_execution_status_projection() -> None:
    s = Session("1", "simulation.yaml", [], None, False, launcher_mode="hot")
    cur_status: dict | None = {"launcher_state": "running", "run_state": {"stop": {"requests": 0}}}

    a1 = s.allowed(execution_status=cur_status)
    assert a1["start"]["reason"] == "already-running"
    assert a1["stop"]["ok"] is True
    assert a1["force"]["ok"] is True

    # C++ reports 2 stop requests via ExecutionStatus -> force becomes already-forced without local request_stop().
    cur_status = {"launcher_state": "running", "run_state": {"stop": {"requests": 2}}}
    a2 = s.allowed(execution_status=cur_status)
    assert a2["stop"]["reason"] == "already-stopping"
    assert a2["force"]["reason"] == "already-forced"

    # C++ result arrived before session.ended(): stoppable drops to False while start stays already-running.
    cur_status = {"launcher_state": "ready", "run_state": None}
    a3 = s.allowed(execution_status=cur_status)
    assert a3["start"]["reason"] == "already-running"
    assert a3["stop"]["reason"] == "not-running"
    assert a3["force"]["reason"] == "not-running"
    assert s.allowed(execution_status=cur_status) == a3


def test_serve_runtime_projected_status_covers_all_cpp_launcher_states(tmp_path: Path) -> None:
    from types import SimpleNamespace
    from vehicle_sil_gui.runtime import ServeRuntime, _Resident

    rt = ServeRuntime(Launcher(Path("vehicle_sil_launch")), cwd=tmp_path)
    assert rt.status() == {"mode": "hot", "state": "idle", "config": None, "reason": None, "message": None}

    fake_proc = SimpleNamespace(returncode=None, pid=1234, stdin=None, stdout=None, stderr=None)
    res = _Resident(config="a.yaml", want_control=False, proc=fake_proc, credentials=None, token=None)  # type: ignore[arg-type]
    rt._resident = res

    for cpp_state, expected_state, expected_cfg in (
        ("unprepared", "ready", "a.yaml"),
        ("preparing", "preparing", "a.yaml"),
        ("ready", "ready", "a.yaml"),
        ("running", "running", "a.yaml"),
        ("ending", "idle", None),
        ("contaminated", "idle", None),
    ):
        res.execution_status = {
            "launcher_state": cpp_state,
            "config": "a.yaml",
            "eligibility": {"eligible": True, "reason": None, "message": None},
            "launcher_allowed": {"prepare": {"ok": True}, "run": {"ok": True}},
        }
        assert rt.status() == {"mode": "hot", "state": expected_state, "config": expected_cfg, "reason": None, "message": None}

    res.execution_status = {
        "launcher_state": "cold_only",
        "config": "fixed.yaml",
        "eligibility": {"eligible": False, "reason": "fixed-registry", "message": "fixed registry"},
        "launcher_allowed": {"run": {"ok": False, "reason": "fixed-registry"}},
    }
    assert rt.status() == {"mode": "cold", "state": "cold", "config": "fixed.yaml", "reason": "fixed-registry", "message": "fixed registry"}

    rt._resident = None
    rt._cold_status = ("a.yaml", "resident-failed", "boom")
    assert rt.status() == {"mode": "cold", "state": "cold", "config": "a.yaml", "reason": "resident-failed", "message": "boom"}

    # Active Cold resident (once=True) exposes execution_status() while keeping mode="cold" and resident_pid=None.
    cold_res = _Resident(
        config="a.yaml", want_control=False, proc=fake_proc, credentials=None, token=None, once=True,  # type: ignore[arg-type]
    )
    cold_res.execution_status = {
        "launcher_state": "running",
        "config": "a.yaml",
        "eligibility": {"eligible": False, "reason": "fixed-registry", "message": "fixed"},
        "launcher_allowed": {"prepare": {"ok": False}, "run": {"ok": False}},
    }
    rt._active_resident = cold_res
    rt._resident = cold_res  # even if assigned, explicit resident.once guard keeps Cold projection and resident_pid=None
    assert rt.resident_pid is None
    assert rt.status() == {"mode": "cold", "state": "cold", "config": "a.yaml", "reason": "resident-failed", "message": "boom"}
    assert rt.execution_status() == cold_res.execution_status


def test_events_v4_validators_reject_contract_drift() -> None:
    from fake_runtime import validate_events_v4_event, validate_version_json

    valid_version = {
        "vehicle_sil_launch": 1,
        "vehicle_sil": "0.4.0-rc.1",
        "formats": {"check": 1, "run": 2, "events": 4, "describe": 1, "test": 1, "timeline": 1, "connections": 1, "control_metrics": 1},
        "features": ["events", "serve"],
    }
    validate_version_json(valid_version)
    bad_formats = {**valid_version, "formats": {**valid_version["formats"], "run_request": 1}}
    del bad_formats["formats"]["describe"]
    with pytest.raises(AssertionError):
        validate_version_json(bad_formats)

    valid_allowed = {"ok": True, "reason": None, "message": None, "caution": None}
    valid_status = {
        "event": "status", "seq": 1, "t_ms": 10, "id": None,
        "instance": "feed000000000001", "status_rev": 0, "launcher_state": "unprepared",
        "config": "sim.yaml", "eligibility": {"eligible": False, "reason": None, "message": None},
        "launcher_allowed": {"prepare": dict(valid_allowed), "run": dict(valid_allowed)},
        "active_run_id": None, "last_run_id": None, "run_state": None,
    }
    validate_events_v4_event(valid_status)
    no_caution = {
        **valid_status,
        "launcher_allowed": {
            "prepare": {"ok": True, "reason": None, "message": None},
            "run": dict(valid_allowed),
        },
    }
    with pytest.raises(AssertionError):
        validate_events_v4_event(no_caution)

    validate_events_v4_event({"event": "run_start", "seq": 2, "t_ms": 20, "run": "inst-1", "id": 1})
    with pytest.raises(AssertionError):
        validate_events_v4_event(
            {"event": "run_start", "seq": 2, "t_ms": 20, "run": "inst-1", "id": 1, "timeline": "/tmp/t.json"}
        )

    validate_events_v4_event({
        "event": "command", "seq": 3, "t_ms": 30, "run": "inst-1",
        "command": "stop", "source": "stdin", "accepted": True, "message": None, "reason": None, "id": 2,
    })
    with pytest.raises(AssertionError):
        validate_events_v4_event({
            "event": "command", "seq": 3, "t_ms": 30, "run": "inst-1",
            "command": "stop", "source": "stdin", "accepted": True, "id": 2,
        })

    with pytest.raises(AssertionError):
        validate_events_v4_event({"event": "unknown_event_kind", "seq": 4, "t_ms": 40})


def test_stub_launcher_events_v4_contract_and_graceful_stop_idempotency(tmp_path: Path) -> None:
    import queue
    import subprocess
    import threading
    from conftest import CONFIGS, REPLAY, scaled
    from fake_runtime import validate_events_v4_event, validate_version_json

    timeout_s = scaled(15)
    env = {**os.environ, "REPLAY_HEARTBEAT_MS": "0", "REPLAY_NO_SLEEPERS": "1"}
    ver_proc = subprocess.run([sys.executable, str(REPLAY), "version", "--json"],
                              capture_output=True, text=True, timeout=timeout_s, check=True, env=env)
    validate_version_json(json.loads(ver_proc.stdout))

    # Two stop commands without force on ignore-stop.yaml stay graceful (requests=1) until explicit force=True.
    req = tmp_path / "req.json"
    req.write_text(json.dumps({"vehicle_sil_run_request": 1}), encoding="utf-8")
    proc = subprocess.Popen(
        [sys.executable, str(REPLAY), "serve", "--", str(CONFIGS / "ignore-stop.yaml")],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env,
    )
    lines: queue.Queue[str | None] = queue.Queue()

    def _pump_stdout() -> None:
        assert proc.stdout is not None
        try:
            for ln in proc.stdout:
                lines.put(ln)
        finally:
            lines.put(None)

    def read_event() -> dict:
        try:
            ln = lines.get(timeout=timeout_s)
        except queue.Empty as exc:
            err = proc.stderr.read() if proc.stderr and proc.poll() is not None else ""
            raise TimeoutError(f"stub_launcher stdout timed out after {timeout_s:g} s: {err}") from exc
        assert ln, proc.stderr.read() if proc.stderr else ""
        ev = json.loads(ln)
        validate_events_v4_event(ev)
        return ev

    threading.Thread(target=_pump_stdout, daemon=True).start()
    try:
        assert proc.stdin is not None and proc.stdout is not None
        hello = read_event()
        init_status = read_event()
        assert hello["event"] == "hello" and init_status["event"] == "status"
        assert init_status["launcher_state"] == "unprepared"
        assert init_status["eligibility"] == {"eligible": False, "reason": None, "message": None}
        assert init_status["launcher_allowed"]["run"]["ok"] is True

        proc.stdin.write(json.dumps({"command": "run", "request": str(req), "id": 1}) + "\n")
        proc.stdin.flush()
        run_id = None
        while True:
            ev = read_event()
            if ev["event"] == "run_start":
                run_id = ev["run"]
            elif ev["event"] == "state" and ev.get("phase") == "running":
                break
        assert run_id is not None

        for cid in (2, 3):
            proc.stdin.write(json.dumps({"command": "stop", "run": run_id, "id": cid}) + "\n")
            proc.stdin.flush()
            while True:
                ev = read_event()
                if ev["event"] == "state" and ev.get("phase") == "stopping":
                    assert ev["stop"] == {"requests": 1, "by": "stop command"}, ev
                    break

        proc.stdin.write(json.dumps({"command": "stop", "run": run_id, "force": True, "id": 4}) + "\n")
        proc.stdin.flush()
        while True:
            ev = read_event()
            if ev["event"] == "result":
                res_ev = ev
                assert ev["launcher_ends"] is False
                assert ev["status"]["launcher_state"] == "unprepared"
                assert ev["status"]["active_run_id"] is None
                assert ev["status"]["last_run_id"] == run_id
                break

        # Immediately query status: verifies no unsolicited duplicate post-result status event was emitted.
        proc.stdin.write(json.dumps({"command": "status", "id": 5}) + "\n")
        proc.stdin.flush()
        next_ev = read_event()
        assert next_ev["event"] == "status" and next_ev["id"] == 5, next_ev
        assert next_ev["status_rev"] == res_ev["status"]["status_rev"], next_ev

        proc.stdin.close()
        assert proc.wait(timeout=timeout_s) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


def test_variables_close_awaits_cancelled_background_tasks(caplog: pytest.LogCaptureFixture) -> None:
    from vehicle_sil_gui.variables import Variables

    async def main() -> None:
        v = Variables("1", lambda _msgs: None)
        started_reconcile = asyncio.Event()
        started_read = asyncio.Event()

        async def pending_reconcile() -> None:
            started_reconcile.set()
            await asyncio.Future()

        async def failing_read() -> None:
            started_read.set()
            try:
                await asyncio.Future()
            except asyncio.CancelledError:
                raise RuntimeError("cleanup-boom") from None

        reconcile_task = asyncio.create_task(pending_reconcile())
        read_task = asyncio.create_task(failing_read())
        v._reconciling = reconcile_task
        v._last_read = read_task
        await started_reconcile.wait()
        await started_read.wait()

        with caplog.at_level("ERROR", logger="vehicle_sil_gui"):
            await v.close()

        assert reconcile_task.done() and reconcile_task.cancelled()
        assert read_task.done() and not read_task.cancelled()
        assert v._reconciling is None and v._last_read is None
        assert "cleanup-boom" in caplog.text

    asyncio.run(main())


def test_gui_server_shutdown_awaits_cancelled_ticker_and_evicting_tasks(
    caplog: pytest.LogCaptureFixture,
) -> None:
    from conftest import CONFIGS, REPLAY
    from vehicle_sil_gui.server import GuiServer, Options

    async def main() -> None:
        server = GuiServer(Options(root=CONFIGS, launcher=Launcher(REPLAY), port=1, token="t"))
        ticker_started = asyncio.Event()
        evict_started = asyncio.Event()

        async def fake_tick() -> None:
            ticker_started.set()
            await asyncio.Future()

        async def fake_evict() -> None:
            evict_started.set()
            try:
                await asyncio.Future()
            except asyncio.CancelledError:
                raise RuntimeError("evict-boom") from None

        ticker_task = asyncio.create_task(fake_tick())
        evict_task = asyncio.create_task(fake_evict())
        server.ws._ticker = ticker_task
        server.ws._evicting.add(evict_task)
        evict_task.add_done_callback(server.ws._evicting.discard)
        await ticker_started.wait()
        await evict_started.wait()

        with caplog.at_level("ERROR", logger="vehicle_sil_gui"):
            await server.shutdown()

        assert ticker_task.done() and ticker_task.cancelled()
        assert evict_task.done() and not evict_task.cancelled()
        assert server.ws._ticker is None
        assert not server.ws._evicting
        assert "evict-boom" in caplog.text

    asyncio.run(main())


# ---------------------------------------------------------------- cli.bind


def test_cli_bind_socket_options(monkeypatch: pytest.MonkeyPatch) -> None:
    import socket as socket_module
    from vehicle_sil_gui import cli

    class FakeSocket:
        def __init__(self, family: int, kind: int) -> None:
            self.family = family
            self.kind = kind
            self.opts: list[tuple[int, int, int]] = []
            self.bound: tuple[str, int] | None = None
            self.backlog: int | None = None
            self.blocking: bool | None = None
            self.closed = False

        def setsockopt(self, level: int, optname: int, value: int) -> None:
            self.opts.append((level, optname, value))

        def bind(self, address: tuple[str, int]) -> None:
            self.bound = address

        def listen(self, backlog: int) -> None:
            self.backlog = backlog

        def setblocking(self, flag: bool) -> None:
            self.blocking = flag

        def close(self) -> None:
            self.closed = True

    monkeypatch.setattr(cli.socket, "socket", FakeSocket)
    monkeypatch.setattr(cli.socket, "SO_EXCLUSIVEADDRUSE", 9999, raising=False)

    # POSIX fixed port sets SO_REUSEADDR (never SO_REUSEPORT or SO_EXCLUSIVEADDRUSE).
    monkeypatch.setattr(cli.sys, "platform", "linux")
    fixed = cli.bind(8765)
    assert fixed.opts == [(socket_module.SOL_SOCKET, socket_module.SO_REUSEADDR, 1)]
    assert fixed.bound == ("127.0.0.1", 8765)
    assert fixed.backlog == 128 and fixed.blocking is False and not fixed.closed

    # POSIX port == 0 never sets SO_REUSEADDR so parallel ephemeral binds stay isolated.
    ephemeral = cli.bind(0)
    assert ephemeral.opts == []
    assert ephemeral.bound == ("127.0.0.1", 0)

    # Windows sets SO_EXCLUSIVEADDRUSE and never SO_REUSEADDR for both fixed and ephemeral ports.
    monkeypatch.setattr(cli.sys, "platform", "win32")
    for port in (8765, 0):
        win_sock = cli.bind(port)
        assert win_sock.opts == [(socket_module.SOL_SOCKET, 9999, 1)]
        assert win_sock.bound == ("127.0.0.1", port)


def test_cli_bind_real_posix_socket_reuseaddr() -> None:
    import socket as socket_module
    from vehicle_sil_gui import cli

    if sys.platform == "win32":
        pytest.skip("POSIX SO_REUSEADDR check")

    ephemeral = cli.bind(0)
    try:
        assert ephemeral.getsockopt(socket_module.SOL_SOCKET, socket_module.SO_REUSEADDR) == 0
    finally:
        ephemeral.close()

    fixed = None
    for _ in range(5):
        probe = socket_module.socket(socket_module.AF_INET, socket_module.SOCK_STREAM)
        try:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        finally:
            probe.close()
        try:
            fixed = cli.bind(port)
            break
        except OSError:
            continue
    assert fixed is not None
    try:
        assert fixed.getsockopt(socket_module.SOL_SOCKET, socket_module.SO_REUSEADDR) != 0
    finally:
        fixed.close()
