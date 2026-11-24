"""Single-stream sequencing, session summary list capture, and variable availability revision ordering."""

import asyncio

from vehicle_sil_gui.session import Limits, Session
from vehicle_sil_gui.source import PeerInfo
from vehicle_sil_gui.variables import Variables


def test_session_summary_list_capture_and_stream_seq():
    session = Session("inst-1", "sim.yaml", [], None, False)
    first = session.summary()
    assert "snapshot_rev" not in first and "problem_seq" not in first
    session.findings.append({"severity": "note", "message": "later"})
    session.problems.append({"code": "later", "message": "later"})
    session.launcher_running = False
    second = session.summary()
    assert first["findings"] == [] and first["problems"] == []
    assert len(second["findings"]) == len(second["problems"]) == 1
    assert second["launcher"]["running"] is False

    m1 = session.message()
    m2 = session.problem("stop-not-delivered", "failed", None)[0]
    m3 = session.feed_event({"event": "hello", "seq": 0, "t_ms": 0, "vehicle_sil_events": 4}, 48)[0]
    assert [m1["stream_seq"], m2["stream_seq"], m3["stream_seq"]] == [1, 2, 3]
    assert m1["session"]["id"] == m2["run_id"] == m3["run_id"] == "inst-1"


def test_variable_snapshot_and_availability_capture_order():
    async def run():
        sent = []
        variables = Variables("inst-1", sent.extend)
        variables.expect()
        schema = [{"name": "x", "type": "real", "role": "input", "availability": "active"}]
        variables.on_peers([PeerInfo("ECU", "incarnation", True, schema)], [])
        variables.on_synced()
        first = variables.summary()
        assert "rev" not in first
        assert first["peers"][0]["variables"][0]["availability"] == "active"
        variables.on_availability("ECU", "incarnation", ["inactive"])
        variables._tell_availability()
        delta = next(message for message in sent if message["type"] == "availability")
        second = variables.summary()
        assert "rev" not in delta and "rev" not in second
        assert delta["run_id"] == "inst-1"
        assert delta["peers"]["ECU"]["x"] == "inactive"
        assert second["peers"][0]["variables"][0]["availability"] == "inactive"
        await variables.close()

    asyncio.run(run())


def test_retained_findings_respect_limits():
    session = Session("inst-1", "sim.yaml", [], None, False, limits=Limits(findings=2))
    session.feed_event({"event": "hello", "seq": 0, "t_ms": 0, "vehicle_sil_events": 4}, 48)
    for seq in (1, 2, 3):
        session.feed_event(
            {"event": "finding", "seq": seq, "t_ms": seq, "severity": "warning", "message": f"m{seq}"},
            64,
        )
    summary = session.summary()
    assert [finding["message"] for finding in summary["findings"]] == ["m1", "m2"]
    assert all("event_seq" not in finding for finding in summary["findings"])
    assert summary["findings_omitted"] == 1
