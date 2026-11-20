"""Capture revisions order complete session and variable views with availability deltas."""

import asyncio

from vehicle_sil_gui.session import Session
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
