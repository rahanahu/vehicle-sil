"""The parts without a server: the session model, the hub's slow-client handling, paths, static files, lines."""

from __future__ import annotations

import asyncio
import json
import os
import sys
from pathlib import Path

import pytest
from vehicle_sil_gui import process
from vehicle_sil_gui.hub import Client, Hub
from vehicle_sil_gui.launcher import InvalidRequest, check_disable, check_speed
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
    s.feed_line(line("check", 1, check={"participants": [{"name": "A", "required": True, "enabled": True},
                                                          {"name": "B", "required": False, "enabled": False}]}))
    s.feed_line(line("phase", 2, phase="running"))
    s.feed_line(line("participant", 3, name="A", state="ended", exit_code=3, signal=None))
    s.feed_line(line("finding", 4, severity="error", code="participant-failed", message="m"))
    s.feed_line(line("time", 5, sim_ms=300))
    s.feed_line(line("result", 6, result={"result": "failed"}))
    out = s.ended(1)
    summary = s.summary()
    assert summary["launcher_pid"] == 42 and summary["phase"] == "running" and summary["sim_ms"] == 300
    assert summary["participants"][0] == {"name": "A", "required": True, "enabled": True, "state": "ended",
                                          "pid": None, "exit_code": 3, "signal": None}
    assert summary["participants"][1]["enabled"] is False
    assert summary["findings"] == [{"severity": "error", "code": "participant-failed", "message": "m"}]
    assert summary["result"] == {"result": "failed"} and summary["problems"] == []
    assert kinds(out) == ["session"] and out[0]["session"]["state"] == "ended"


def test_lines_that_are_not_events_are_reported_and_not_passed_on() -> None:
    s = new_session()
    assert kinds(s.feed_line("not json")) == ["error:launcher-output"]
    assert kinds(s.feed_line("[1]")) == ["error:launcher-output"]
    assert kinds(s.feed_line('{"event": "log"}')) == ["error:launcher-output"]  # no seq
    assert kinds(s.feed_line('{"event": "log", "seq": true}')) == ["error:launcher-output"]
    assert kinds(s.feed_line(line("log", 0, source="A", line="x"))) == ["error:no-hello", "event"]
    assert kinds(s.feed_line(line("mystery", 2, colour="blue"))) == ["error:seq-gap", "event"]
    assert kinds(s.feed_line(line("log", 2))) == ["error:seq-order"]
    assert kinds(s.feed_line(line("log", 1))) == ["error:seq-order"]
    long = s.feed_line("x" * 1000)[0]
    assert len(long["detail"]) < 400 and "more characters" in long["detail"]


def test_a_different_events_format_is_reported_but_passed_on() -> None:
    s = new_session()
    out = s.feed_line(line("hello", 0, vehicle_sil_events=2))
    assert kinds(out) == ["error:events-format", "event"]


def test_ending_without_a_result_is_reported() -> None:
    s = new_session()
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    out = s.ended(70)
    assert kinds(out) == ["error:no-result", "session"]
    assert s.summary()["exit_code"] == 70 and s.summary()["problems"][0]["detail"] == {"exit_code": 70}


def test_problems_are_capped() -> None:
    s = new_session(problems=3)
    for _ in range(5):
        s.feed_line("bad")
    assert len(s.summary()["problems"]) == 3 and s.summary()["problems_omitted"] == 2


def test_the_replay_keeps_recent_log_lines_per_source_and_the_last_time() -> None:
    s = new_session(log_lines_per_source=3, log_sources=2)
    s.feed_line(line("hello", 0, vehicle_sil_events=1))
    seq = 1
    for n in range(5):
        for source in ("A", "B", "C"):  # C is the third source: it shares the log of the later sources
            s.feed_line(line("log", seq, source=source, line=f"{source}{n}"))
            seq += 1
        s.feed_line(line("time", seq, sim_ms=n))
        seq += 1
    meta, events = s.replay(None)
    lines = [m["event"]["line"] for m in events if m["event"]["event"] == "log"]
    assert lines == ["A2", "B2", "A3", "B3", "A4", "B4", "C2", "C3", "C4"] or \
        sorted(lines) == sorted(["A2", "A3", "A4", "B2", "B3", "B4", "C2", "C3", "C4"])
    assert [m["event"]["seq"] for m in events] == sorted(m["event"]["seq"] for m in events)
    assert [m["event"]["sim_ms"] for m in events if m["event"]["event"] == "time"] == [4]
    assert meta["omitted"]["log"] == {"A": 2, "B": 2, "*": 2}
    meta, events = s.replay(seq - 2)
    assert meta["from_seq"] == seq - 2 and all(m["event"]["seq"] >= seq - 2 for m in events)


# ---------------------------------------------------------------- hub


def test_a_slow_screen_gets_resync_and_the_queue_stays_bounded() -> None:
    async def main() -> None:
        snapshots = []

        def snapshot(session, from_seq):
            snapshots.append((session, from_seq))
            return [{"type": "hello", "n": len(snapshots)}, {"type": "synced"}]

        hub = Hub(snapshot, limit=10)
        sent: list[dict] = []
        gate = asyncio.Event()

        async def send(m: dict) -> None:
            await gate.wait()  # a screen that does not read
            sent.append(m)

        client = Client(send, limit=10)
        task = asyncio.create_task(hub.serve(client, None, None))
        await asyncio.sleep(0)
        sizes = []
        for n in range(1000):
            hub.broadcast([{"type": "event", "n": n}])
            sizes.append(client.queue.qsize())
        assert max(sizes) <= 11
        gate.set()
        await asyncio.sleep(0.05)
        hub.broadcast([{"type": "event", "n": "after"}])
        await asyncio.sleep(0.05)
        hub.close_all()
        await task
        types = [m["type"] for m in sent]
        assert "resync" in types
        i = types.index("resync")
        assert types[i + 1:i + 3] == ["hello", "synced"] and sent[-1] == {"type": "event", "n": "after"}
        assert snapshots[0] == (None, None) and len(snapshots) == 2
        assert client not in hub.clients

    asyncio.run(main())


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
    for bad in (True, 0, -1, 1e9, float("nan"), float("inf"), "fast", [1]):
        with pytest.raises(InvalidRequest):
            check_speed(bad)


# ---------------------------------------------------------------- static files


def test_static_files_stay_in_their_directory(tmp_path: Path) -> None:
    web = tmp_path / "web"
    (web / "assets").mkdir(parents=True)
    (web / "index.html").write_text("<html>")
    (web / "main.js").write_text("x")
    (web / "assets" / "a.svg").write_text("<svg/>")
    (web / "secret.py").write_text("x")
    (web / ".env").write_text("x")
    (tmp_path / "outside.js").write_text("x")
    os.symlink(tmp_path / "outside.js", web / "link.js")
    s = StaticFiles(web)
    assert s.lookup("/main.js")[1:] == ("text/javascript; charset=utf-8", 200)
    assert s.lookup("/assets/a.svg")[1:] == ("image/svg+xml", 200)
    assert s.lookup("/")[0].name == "index.html"
    assert s.lookup("/run/some/route")[0].name == "index.html"  # the screen's routes
    for path in ("/api/x", "/api", "/ws", "/ws/x", "/secret.py", "/.env", "/../outside.js", "/assets/../../x.js",
                 "/missing.js", "/link.js", "/a\\b.js", "/C:/x.js"):
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
