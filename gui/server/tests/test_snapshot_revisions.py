"""Capture revisions order complete session and variable views with availability deltas."""

import asyncio
import json

from vehicle_sil_gui.session import Limits, Session
from vehicle_sil_gui.source import PeerInfo
from vehicle_sil_gui.variables import Variables


def test_session_snapshot_revision_and_list_capture():
    session = Session(1, "sim.yaml", [], None, False)
    first = session.summary()
    session.findings.append({"severity": "note", "message": "later"})
    session.problems.append({"code": "later", "message": "later"})
    session.launcher_running = False
    second = session.summary()
    assert first["snapshot_rev"] < second["snapshot_rev"]
    assert first["findings"] == [] and first["problems"] == []
    assert len(second["findings"]) == len(second["problems"]) == 1
    assert second["launcher"]["running"] is False


def test_variable_snapshot_and_availability_capture_order():
    async def run():
        sent = []
        variables = Variables(1, sent.extend)
        variables.expect()
        schema = [{"name": "x", "type": "real", "role": "input", "availability": "active"}]
        variables.on_peers([PeerInfo("ECU", "incarnation", True, schema)], [])
        variables.on_synced()
        first = variables.summary()
        variables.on_availability("ECU", "incarnation", ["inactive"])
        variables._tell_availability()
        delta = next(message for message in sent if message["type"] == "availability")
        second = variables.summary()
        assert first["rev"] < delta["rev"] < second["rev"]
        assert delta["peers"]["ECU"]["x"] == "inactive"
        assert second["peers"][0]["variables"][0]["availability"] == "inactive"
        await variables.close()

    asyncio.run(run())


def test_problem_sequence_is_shared_by_error_and_summary():
    session = Session(1, "sim.yaml", [], None, False)
    first = session.problem("stop-not-delivered", "failed", None)[0]
    second = session.problem("stop-not-delivered", "failed", None)[0]
    summary = session.summary()
    assert (first["problem_seq"], second["problem_seq"]) == (1, 2)
    assert summary["problem_seq"] == 2
    assert [problem["problem_seq"] for problem in summary["problems"]] == [1, 2]


def test_retained_findings_carry_their_event_sequence_and_respect_limits():
    session = Session(1, "sim.yaml", [], None, False, limits=Limits(findings=2))
    for seq in (1, 2, 3):
        session.feed_line(json.dumps({"event": "finding", "seq": seq, "t_ms": seq,
                                      "severity": "warning", "message": "same"}))
    summary = session.summary()
    assert [finding["event_seq"] for finding in summary["findings"]] == [1, 2]
    assert summary["findings_omitted"] == 1
