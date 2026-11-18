"""Connecting a network outside the simulation: the list of one network (POST api/connections), the intent's connect
and time_policy, and the guard that passes on only a target that the last list gave as usable (target-not-listed).
connect.yaml's ETH_EXT is recorded with the Ethernet bridge on the fake pcap library (record_launcher.py)."""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import sys
from pathlib import Path

import pytest
from conftest import CONFIGS, REPLAY, Client, gui_server, is_ended, is_synced, run_async

POSIX = sys.platform != "win32"
FILE = "connect.yaml"
LIST = {"file": FILE, "network": "ETH_EXT"}


async def listed(c: Client, body: dict = LIST) -> dict:
    status, reply = await c.post("api/connections", body)
    assert status == 200, reply
    return reply


def refused(status: int, reply: dict, code: str) -> dict:
    assert (status, reply["error"]["code"]) == (400, code), reply
    return reply["error"]


def test_the_list_of_one_network_and_a_run_connected_to_a_target_of_it(tmp_path: Path) -> None:
    async def main(c: Client) -> None:
        reply = await listed(c)
        assert (reply["file"], reply["network"], reply["exit_code"]) == (FILE, "ETH_EXT", 0)
        networks = reply["connections"]["networks"]
        assert {n["name"] for n in networks} == {"ETH_EXT"} and {n["backend"] for n in networks} == {"packet", "tap"}
        values = {t["value"]: t["usable"] for n in networks for t in n["list"]["targets"]}
        assert values["fake0"] is True and values["tap:lo"] is False
        # The check relays the launcher's decision about the time: locked to real time, or not with free
        status, reply = await c.post("api/check", {"file": FILE, "connect": {"ETH_EXT": "fake0"}})
        assert status == 200 and reply["check"]["time_lock"] == {"locked": True, "because": ["ETH_EXT"]}, reply
        assert reply["check"]["networks"][1]["outside"]["target"] == "fake0"
        status, reply = await c.post("api/check", {"file": FILE, "connect": {"ETH_EXT": "fake0"},
                                                   "time_policy": "free"})
        assert status == 200 and reply["check"]["time_lock"] == {"locked": False, "because": ["ETH_EXT"]}, reply
        sock = await c.ws()
        await sock.until(is_synced)
        status, reply = await c.post("api/run", {"file": FILE, "connect": {"ETH_EXT": "fake0"}})
        assert status == 200, reply
        got = await sock.until(is_ended)
        assert got[-1]["session"]["result"]["result"] == "passed"
        outside = [m["event"] for m in got if m["type"] == "event" and m["event"]["event"] == "outside"]
        assert outside and outside[0]["network"] == "ETH_EXT"  # the launcher's event, passed on as it is
    with gui_server(tmp_path) as gui:
        run_async(main, gui)


@pytest.mark.skipif(not POSIX, reason="the stand-in launcher is a script")
def test_connect_and_time_policy_go_to_the_launcher_in_the_request_file(tmp_path: Path) -> None:
    seen = tmp_path / "seen"
    seen.mkdir()
    launcher = tmp_path / "vehicle_sil_launch"
    launcher.write_text(f"#!{sys.executable}\nimport json, os, runpy, sys\n"
                        f"a = sys.argv[1:]\nif '--request' in a:\n    p = a[a.index('--request') + 1]\n"
                        f"    json.dump({{'argv': a, 'text': open(p).read()}}, "
                        f"open('{seen}/' + str(len(os.listdir('{seen}'))) + '.json', 'w'))\n"
                        f"runpy.run_path({str(REPLAY)!r}, run_name='__main__')\n")
    launcher.chmod(0o755)

    async def main(c: Client) -> None:
        await listed(c)
        status, reply = await c.post("api/check", {"file": FILE, "connect": {"ETH_EXT": "fake0"},
                                                   "time_policy": "free"})
        assert status == 200, reply
        last = json.loads(sorted(seen.iterdir(), key=lambda f: int(f.stem))[-1].read_text())
        assert not {"--connect", "--time-policy"} & set(last["argv"])  # the intent is not among the arguments
        assert json.loads(last["text"]) == {"vehicle_sil_run_request": 1, "connect": {"ETH_EXT": "fake0"},
                                            "time_policy": "free"}
    with gui_server(tmp_path, launcher=launcher) as gui:
        run_async(main, gui)


def test_a_target_that_the_last_list_did_not_give_as_usable_is_refused(tmp_path: Path) -> None:
    """The browser's value reaches a bridge's device only through the guard: listed for that file and that network,
    from the file as it is now, usable, and written exactly so. none needs no list."""
    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)

    async def main(c: Client) -> None:
        for path in ("api/check", "api/run"):  # before any list
            error = refused(*await c.post(path, {"file": FILE, "connect": {"ETH_EXT": "fake0"}}), "target-not-listed")
            assert error["detail"] == {"network": "ETH_EXT", "target": "fake0"}
            assert "there is no list of where 'ETH_EXT' of connect.yaml can be connected" in error["message"]
            assert 'POST api/connections {"file": "connect.yaml", "network": "ETH_EXT"}' in error["message"]
        status, reply = await c.post("api/check", {"file": FILE, "connect": {"ETH_EXT": "none"}})
        assert status == 200 and reply["check"]["result"] == "passed", reply  # none: SIL Kit only, no list needed
        await listed(c)
        for target in ("enp3s0", "tap:lo", "FAKE0", "fake0 ", "packet:fake0", ""):  # not listed, not usable, not so
            for path in ("api/check", "api/run"):
                error = refused(*await c.post(path, {"file": FILE, "connect": {"ETH_EXT": target}}),
                                "target-not-listed")
                assert error["detail"] == {"network": "ETH_EXT", "target": target}
                assert f"does not have {target!r} as usable" in error["message"]
        # Another network, another file: their own lists
        refused(*await c.post("api/check", {"file": FILE, "connect": {"J1939_CAN": "fake0"}}), "target-not-listed")
        refused(*await c.post("api/check", {"file": "simulation.yaml", "connect": {"ETH_EXT": "fake0"}}),
                "target-not-listed")
        status, reply = await c.post("api/check", {"file": FILE, "connect": {"ETH_EXT": "fake0"}})
        assert status == 200, reply
        # The file has changed since the list: listed again before it is passed on
        stat = (root / FILE).stat()
        os.utime(root / FILE, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000))
        error = refused(*await c.post("api/run", {"file": FILE, "connect": {"ETH_EXT": "fake0"}}),
                        "target-not-listed")
        assert "connect.yaml has changed since 'ETH_EXT' was listed" in error["message"]
        await listed(c)
        status, reply = await c.post("api/check", {"file": FILE, "connect": {"ETH_EXT": "fake0"}})
        assert status == 200, reply
        status, body = await c.get("api/session")
        assert body["session"] is None  # nothing was started by any of the refused ones
    with gui_server(tmp_path, root=root) as gui:
        run_async(main, gui)


def test_the_list_is_of_a_network_of_the_files_last_check(tmp_path: Path) -> None:
    async def main(c: Client) -> None:
        error = refused(*await c.post("api/connections", {"file": FILE, "network": "NOPE"}), "unknown-network")
        assert error["detail"] == {"network": "NOPE", "networks": ["J1939_CAN", "ETH_EXT"]}
        assert "the networks are J1939_CAN, ETH_EXT" in error["message"]
        for body in ({"file": FILE}, {"file": FILE, "network": ""}, {"file": FILE, "network": ["ETH_EXT"]},
                     {"file": FILE, "network": None}, {**LIST, "connect": {"ETH_EXT": "fake0"}}):
            refused(*await c.post("api/connections", body), "invalid-field")
        status, reply = await c.post("api/connections", {"file": "../connect.yaml", "network": "ETH_EXT"})
        assert (status, reply["error"]["code"]) == (400, "outside-root"), reply
    with gui_server(tmp_path) as gui:
        run_async(main, gui)


def test_lists_wait_in_the_queue_of_the_checks(tmp_path: Path) -> None:
    async def main(c: Client) -> None:
        await listed(c)  # the check that names the networks is done: the ones below are lists only
        replies = await asyncio.gather(*(c.post("api/connections", LIST) for _ in range(8)))
        codes = sorted(r[1]["error"]["code"] if r[0] != 200 else "ok" for r in replies)
        assert "ok" in codes and "check-busy" in codes and set(codes) == {"ok", "check-busy"}, codes
    with gui_server(tmp_path, env={"REPLAY_CHECK_DELAY_MS": "300"}) as gui:
        run_async(main, gui)


@pytest.mark.skipif(not POSIX, reason="the stand-in launcher is a script")
def test_a_list_that_is_not_one_is_said_so_and_names_no_target(tmp_path: Path) -> None:
    launcher = tmp_path / "vehicle_sil_launch"
    broken = tmp_path / "broken"
    launcher.write_text(f"#!{sys.executable}\nimport os, runpy, sys\n"
                        f"if sys.argv[1:2] == ['connections'] and os.path.exists({str(broken)!r}):\n"
                        "    print('not json'); sys.exit(1)\n"
                        f"runpy.run_path({str(REPLAY)!r}, run_name='__main__')\n")
    launcher.chmod(0o755)

    async def main(c: Client) -> None:
        await listed(c)
        broken.touch()  # the next list fails: the one before it is no longer used either
        status, reply = await c.post("api/connections", LIST)
        assert (status, reply["error"]["code"]) == (502, "launcher-output"), reply
        refused(*await c.post("api/check", {"file": FILE, "connect": {"ETH_EXT": "fake0"}}), "target-not-listed")
    with gui_server(tmp_path, launcher=launcher) as gui:
        run_async(main, gui)
