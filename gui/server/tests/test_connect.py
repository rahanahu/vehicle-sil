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
        status, reply = await c.post("api/run", {"file": FILE, "connect": {"ETH_EXT": "fake0"}, "stop_at_ms": 1000})
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


def stand_in(tmp_path: Path, broken: Path | None = None) -> Path:
    """A launcher that plays the recordings with every target of ETH_EXT (but none) as the recorded one, fake0: the
    guard decides before that, and the tests here are about the guard, not about what the launcher says of a target.
    With broken: while that file is there, connections writes no list."""
    launcher = tmp_path / "vehicle_sil_launch.py"
    launcher.write_text(
        "import json, os, runpy, sys\n"
        "a = sys.argv[1:]\n"
        f"if a[:1] == ['connections'] and {str(broken)!r} != 'None' and os.path.exists({str(broken)!r}):\n"
        "    print('not json'); sys.exit(1)\n"
        "if '--request' in a:\n"
        "    p = a[a.index('--request') + 1]\n"
        "    r = json.load(open(p, encoding='utf-8'))\n"
        "    if r.get('connect', {}).get('ETH_EXT', 'none') != 'none':\n"
        "        r['connect']['ETH_EXT'] = 'fake0'\n"
        "        json.dump(r, open(p, 'w', encoding='utf-8'))\n"
        f"sys.argv[0] = {str(REPLAY)!r}\n"
        f"runpy.run_path({str(REPLAY)!r}, run_name='__main__')\n")
    return launcher


def test_a_target_that_the_last_list_did_not_give_as_usable_is_refused(tmp_path: Path) -> None:
    """The browser's value reaches a bridge's device only through the guard: listed for that file and that network,
    from the file as it is now, usable, and written exactly so. none needs no list."""
    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)
    shutil.copy(root / FILE, root / "other.yaml")  # another file with the same networks

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
                assert (f"the last list of 'ETH_EXT' of connect.yaml does not have {target!r} as usable (the usable "
                        'ones: "fake0", "fake1", "fake-echo", "fake-wifi", "fake-down" and 10 more)') \
                    in error["message"], error
        # Another network, another file: their own lists
        refused(*await c.post("api/check", {"file": FILE, "connect": {"J1939_CAN": "fake0"}}), "target-not-listed")
        refused(*await c.post("api/check", {"file": "other.yaml", "connect": {"ETH_EXT": "fake0"}}),
                "target-not-listed")
        status, reply = await c.post("api/check", {"file": FILE, "connect": {"ETH_EXT": "fake0"}})
        assert status == 200, reply
        # The file has changed since the list: listed again before it is passed on
        touch(root / FILE)
        error = refused(*await c.post("api/run", {"file": FILE, "connect": {"ETH_EXT": "fake0"}, "stop_at_ms": 1000}),
                        "target-not-listed")
        assert "connect.yaml has changed since 'ETH_EXT' was listed" in error["message"]
        await listed(c)
        status, reply = await c.post("api/check", {"file": FILE, "connect": {"ETH_EXT": "fake0"}})
        assert status == 200, reply
        status, body = await c.get("api/session")
        assert body["session"] is None  # nothing was started by any of the refused ones
    with gui_server(tmp_path, root=root, launcher=stand_in(tmp_path)) as gui:
        run_async(main, gui)


def touch(path: Path) -> None:
    """A newer modification time: the file's stamp changes, its text does not (the recordings still match)."""
    stat = path.stat()
    os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000))


def test_a_run_compares_its_targets_right_before_the_launcher_starts(tmp_path: Path) -> None:
    """While a run is preparing before the launcher starts, if the file changes or the network is listed again
    and fails, the run is refused and nothing starts."""
    from aiohttp.test_utils import TestServer
    import aiohttp
    from conftest import Gui
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options

    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)
    broken = tmp_path / "broken"
    run = {"file": FILE, "connect": {"ETH_EXT": "fake0"}}

    async def main() -> None:
        launcher_path = stand_in(tmp_path, broken)
        launcher = Launcher(launcher_path)
        await launcher.probe()
        server = GuiServer(Options(root=root, launcher=launcher, port=0, token="test-token", variables=False))
        test_server = TestServer(server.app(), host="127.0.0.1")
        await test_server.start_server()
        assert test_server.port is not None
        server.hosts = {f"127.0.0.1:{test_server.port}"}
        server.origins = {f"http://127.0.0.1:{test_server.port}"}
        gui = Gui(proc=None, port=test_server.port, token="test-token",
                  url=f"http://127.0.0.1:{test_server.port}/#token=test-token", stderr=tmp_path / ".err")
        orig_no_vars = server._no_variables

        async def slow_no_vars():
            await asyncio.sleep(0.35)
            return await orig_no_vars()

        server._no_variables = slow_no_vars  # type: ignore[method-assign]
        try:
            async with aiohttp.ClientSession() as http:
                c = Client(gui, http)
                await listed(c)
                starting = asyncio.ensure_future(c.post("api/run", run))
                await asyncio.sleep(0.1)
                touch(root / FILE)
                refused(*await starting, "target-not-listed")

                await listed(c)
                broken.touch()
                starting = asyncio.ensure_future(c.post("api/run", run))
                await asyncio.sleep(0.1)
                status, reply = await c.post("api/connections", LIST)
                assert (status, reply["error"]["code"]) == (502, "launcher-output"), reply
                error = refused(*await starting, "target-not-listed")
                assert "there is no list of where 'ETH_EXT'" in error["message"]
                status, body = await c.get("api/session")
                assert body["session"] is None
        finally:
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_a_network_that_the_file_lacks_is_left_to_the_launcher(tmp_path: Path) -> None:
    """A request file of a test bench may connect networks that this configuration does not have: the launcher
    leaves them out with a note, as for run --request, and connects nothing for them."""
    async def main(c: Client) -> None:
        req = {"file": FILE, "connect": {"CAN1": "physical:1"}, "stop_at_ms": 1000}
        status, reply = await c.post("api/check", req)
        assert status == 200 and reply["check"]["result"] == "passed", reply
        assert "connect-network-unused" in {f["code"] for f in reply["check"]["findings"]}
        sock = await c.ws()
        await sock.until(is_synced)
        status, reply = await c.post("api/run", req)
        assert status == 200, reply
        got = await sock.until(is_ended)
        assert got[-1]["session"]["result"]["result"] == "passed"
    with gui_server(tmp_path) as gui:
        run_async(main, gui)


def test_a_run_whose_file_changes_during_its_check_connects_nothing(tmp_path: Path) -> None:
    """A network that the file lacks is left to the launcher, but only for the file as it was read: if the file
    changes before the launcher starts, it may have that network now, so the run is refused."""
    from aiohttp.test_utils import TestServer
    import aiohttp
    from conftest import Gui
    from vehicle_sil_gui.launcher import Launcher
    from vehicle_sil_gui.server import GuiServer, Options

    root = tmp_path / "root"
    shutil.copytree(CONFIGS, root)

    async def main() -> None:
        launcher = Launcher(stand_in(tmp_path))
        await launcher.probe()
        server = GuiServer(Options(root=root, launcher=launcher, port=0, token="test-token", variables=False))
        test_server = TestServer(server.app(), host="127.0.0.1")
        await test_server.start_server()
        assert test_server.port is not None
        server.hosts = {f"127.0.0.1:{test_server.port}"}
        server.origins = {f"http://127.0.0.1:{test_server.port}"}
        gui = Gui(proc=None, port=test_server.port, token="test-token",
                  url=f"http://127.0.0.1:{test_server.port}/#token=test-token", stderr=tmp_path / ".err")
        orig_no_vars = server._no_variables

        async def slow_no_vars():
            await asyncio.sleep(0.35)
            return await orig_no_vars()

        server._no_variables = slow_no_vars  # type: ignore[method-assign]
        try:
            async with aiohttp.ClientSession() as http:
                c = Client(gui, http)
                starting = asyncio.ensure_future(c.post("api/run", {"file": FILE, "connect": {"CAN1": "physical:1"}}))
                await asyncio.sleep(0.1)
                touch(root / FILE)
                error = refused(*await starting, "target-not-listed")
                assert "connect.yaml has changed since this run was checked" in error["message"]
                assert error["detail"] == {"network": "CAN1", "target": "physical:1"}
                status, body = await c.get("api/session")
                assert body["session"] is None
        finally:
            await server.shutdown()
            await test_server.close()

    asyncio.run(main())


def test_the_list_is_of_a_network_of_the_file(tmp_path: Path) -> None:
    async def main(c: Client) -> None:
        error = refused(*await c.post("api/connections", {"file": FILE, "network": "NOPE"}), "unknown-network")
        assert error["detail"]["network"] == "NOPE" and error["detail"]["findings"][0]["code"] == "usage"
        assert error["message"] == ("connect.yaml has no network 'NOPE' to list (the launcher says: --network names "
                                    "the network 'NOPE', which the file does not have: the networks are J1939_CAN, "
                                    "ETH_EXT): give the name of a can or ethernet network of the file, as "
                                    "networks[].name of its check")
        for body in ({"file": FILE}, {"file": FILE, "network": ""}, {"file": FILE, "network": ["ETH_EXT"]},
                     {"file": FILE, "network": None}, {**LIST, "connect": {"ETH_EXT": "fake0"}}):
            refused(*await c.post("api/connections", body), "invalid-field")
        status, reply = await c.post("api/connections", {"file": "../connect.yaml", "network": "ETH_EXT"})
        assert (status, reply["error"]["code"]) == (400, "outside-root"), reply
        # A file with variants: listed without a check first (its networks are the same in every variant)
        reply = await listed(c, {"file": "variants.yaml", "network": "J1939_CAN"})
        assert reply["exit_code"] == 0 and reply["connections"]["networks"][0]["name"] == "J1939_CAN"
    with gui_server(tmp_path) as gui:
        run_async(main, gui)


def test_non_list_networks_yaml_fails_closed_on_connect(tmp_path: Path) -> None:
    """When networks cannot be read from YAML metadata, non-'none' connect targets fail closed unless listed."""
    from vehicle_sil_gui.server import _read_config_metadata

    cfg = tmp_path / "weird.yaml"
    cfg.write_text("vehicle_sil_simulation: 2\nnetworks: not-a-list\nparticipants:\n  - name: Engine\n",
                   encoding="utf-8")
    _, participants, networks = _read_config_metadata(cfg, None)
    assert participants == ["Engine"] and networks is None

    root = tmp_path / "root"
    root.mkdir()
    (root / "weird.yaml").write_text("vehicle_sil_simulation: 2\nnetworks: not-a-list\nparticipants: []\n",
                                      encoding="utf-8")
    with gui_server(tmp_path, root=root) as gui:
        async def refuse(c: Client) -> None:
            error = refused(*await c.post("api/run", {"file": "weird.yaml", "connect": {"CAN1": "fake0"}}),
                            "target-not-listed")
            assert error["detail"] == {"network": "CAN1", "target": "fake0"}
        run_async(refuse, gui)


def test_lists_wait_in_the_queue_of_the_checks(tmp_path: Path) -> None:
    async def main(c: Client) -> None:
        replies = await asyncio.gather(*(c.post("api/connections", LIST) for _ in range(8)))
        codes = sorted(r[1]["error"]["code"] if r[0] != 200 else "ok" for r in replies)
        assert "ok" in codes and "check-busy" in codes and set(codes) == {"ok", "check-busy"}, codes
    with gui_server(tmp_path, env={"REPLAY_CHECK_DELAY_MS": "300"}) as gui:
        run_async(main, gui)


def test_a_list_that_is_not_one_is_said_so_and_names_no_target(tmp_path: Path) -> None:
    broken = tmp_path / "broken"

    async def main(c: Client) -> None:
        await listed(c)
        broken.touch()  # the next list fails: the one before it is no longer used either
        status, reply = await c.post("api/connections", LIST)
        assert (status, reply["error"]["code"]) == (502, "launcher-output"), reply
        refused(*await c.post("api/check", {"file": FILE, "connect": {"ETH_EXT": "fake0"}}), "target-not-listed")
    with gui_server(tmp_path, launcher=stand_in(tmp_path, broken)) as gui:
        run_async(main, gui)
