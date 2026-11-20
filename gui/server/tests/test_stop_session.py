"""A delayed stop must not affect a newer run."""

import asyncio
from pathlib import Path

from conftest import Client, gui_server, is_ended, is_synced, run_async


def test_targeted_stop_does_not_stop_a_later_run(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_TICK_MS": "50"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            status, first = await c.post("api/run", {"file": "forever.yaml"})
            assert status == 200, first
            first_id = first["session"]["id"]
            instance = first["instance"]
            await sock.until(lambda m: m.get("event", {}).get("event") == "time")
            status, _ = await c.post("api/stop-session", {"session": first_id, "instance": instance})
            assert status == 200
            await sock.until(is_ended)
            status, second = await c.post("api/run", {"file": "forever.yaml"})
            assert status == 200, second
            second_id = second["session"]["id"]
            assert second_id != first_id
            status, stale = await c.post("api/stop-session", {"session": first_id, "instance": instance})
            assert status == 409 and stale["error"]["code"] == "session-changed"
            status, current = await c.get("api/session")
            assert status == 200 and current["session"]["id"] == second_id
            assert current["session"]["stop"] == "none"
            status, stale = await c.post("api/stop-session", {"session": second_id, "instance": "old-server"})
            assert status == 409 and stale["error"]["code"] == "session-changed"
            status, stopped = await c.post("api/stop-session", {"session": second_id, "instance": instance})
            assert status == 200 and stopped["session"]["stop"] == "requested"
            await sock.until(is_ended)
            await sock.close()
        run_async(main, gui)


def test_targeted_stop_rejects_invalid_session_ids(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
        async def main(c: Client) -> None:
            for value in (None, 0, -1, True, "1", 1.5):
                status, reply = await c.post("api/stop-session", {"session": value, "instance": "x"})
                assert status == 400 and reply["error"]["code"] == "invalid-field"
            status, reply = await c.post("api/stop-session", {"session": 1})
            assert status == 400 and reply["error"]["code"] == "invalid-field"
            status, reply = await c.post("api/stop-session", {"session": 1, "instance": "x", "forse": True})
            assert status == 400 and reply["error"]["code"] == "invalid-field"
        run_async(main, gui)


def test_targeted_stop_cannot_queue_a_stop_for_another_start(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_CHECK_DELAY_MS": "500", "REPLAY_TICK_MS": "50"}) as gui:
        async def main(c: Client) -> None:
            sock = await c.ws()
            await sock.until(is_synced)
            status, app = await c.get("api/app")
            assert status == 200
            start = asyncio.create_task(c.post("api/run", {"file": "forever.yaml"}))
            await sock.until(lambda m: m.get("type") == "allowed" and
                             m.get("allowed", {}).get("start", {}).get("reason") == "starting")
            status, stale = await c.post("api/stop-session", {"session": 999, "instance": app["instance"]})
            assert status == 409 and stale["error"]["code"] == "session-changed"
            status, answer = await start
            assert status == 200 and answer["session"]["stop"] == "none"
            await sock.until(lambda m: m.get("event", {}).get("event") == "time")
            status, stopped = await c.post("api/stop-session", {"session": answer["session"]["id"], "instance": app["instance"]})
            assert status == 200 and stopped["session"]["stop"] == "requested"
            await sock.until(is_ended)
            await sock.close()
        run_async(main, gui)
