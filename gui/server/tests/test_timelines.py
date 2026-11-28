"""The timeline of a scenario run started from the GUI: the timeline field in serve's run command (only with a
launcher that has the feature), the file served under api/timeline/<run_id> with the token, how many files are kept,
and the page (gui/timeline) served under timeline/ without a token."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from conftest import REPOSITORY, Client, gui_server, is_ended, is_synced, run_async, stop_server

from vehicle_sil_gui.launcher import serve_run_command
from vehicle_sil_gui.server import CSP
from vehicle_sil_gui.timelines import KEPT, Timelines, api_path, find_page_dir, written_whole

PASS, FAIL = "scenarios/throttle.yaml", "scenarios/fails.yaml"
WITHOUT_TIMELINE = (
    "events,stdin-commands,request,check-variables,speed,start-paused,time-commands,"
    "stdin-run-until,state,control-links,scenario,manual-participants,serve,check-file-only,serve-prepare"
)


def is_run_start_event(m: dict) -> bool:
    return m["type"] == "event" and m["event"]["event"] == "run_start"


async def run_to_end(c: Client, body: dict) -> tuple[dict, dict]:
    """Starts a run, waits for its end: (the summary at the end, the run_start event)."""
    ws = await c.ws()
    await ws.until(is_synced)
    status, started = await c.post("api/run", body)
    assert status == 200, started
    got = await ws.until(is_ended)
    run_start = next(m["event"] for m in got if is_run_start_event(m))
    await ws.close()
    _, now = await c.get("api/session")
    return now["session"], run_start


async def get_raw(c: Client, path: str, **headers) -> tuple[int, bytes, dict]:
    async with c.http.get(f"{c.gui.base}/{path}", headers=c.gui.headers(**headers), allow_redirects=False) as r:
        return r.status, await r.read(), dict(r.headers)


def test_a_scenario_run_records_its_timeline_and_the_server_serves_it(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
        where: list[Path] = []

        async def talk(c: Client) -> None:
            session, run_start = await run_to_end(c, {"file": "simulation.yaml", "scenario": FAIL})
            assert "timeline" not in run_start
            path = Path(session["result"]["scenario"]["timeline"])
            assert path.is_absolute() and path.parent.name.startswith("vehicle-sil-gui-timelines-")
            where.append(path.parent)
            url = session["scenario"]["timeline_url"]
            assert url == api_path(session["id"]) == f"api/timeline/{session['id']}"
            assert session["timeline_url"] == url

            status, body, headers = await get_raw(c, url)
            assert status == 200, body
            assert json.loads(body)["vehicle_sil_timeline"] == 1
            assert headers["Content-Type"].startswith("application/json")
            assert headers["Content-Disposition"] == 'inline; filename="fails.timeline.json"'
            assert headers["Cache-Control"] == "no-store" and headers["Content-Security-Policy"] == CSP

            # The token is needed, as for every api/ request
            for token in (None, "wrong"):
                status, body, headers = await get_raw(c, url, token=token)
                assert status == 401 and json.loads(body)["error"]["code"] == "unauthorized"

            # Another run: a manual run without record has none; an id the server never gave has none
            other, run_start_other = await run_to_end(c, {"file": "simulation.yaml", "stop_at_ms": 1000})
            assert "timeline" not in run_start_other and other["scenario"] is None and "scenario" not in other["result"]
            assert other.get("timeline_url") is None
            for wrong in (other["id"], "999", "x", "-1", "1e3", "１"):
                status, body = await c.get(f"api/timeline/{wrong}")
                assert (status, body["error"]["code"]) == (404, "no-timeline"), (wrong, body)
                assert "the feature 'timeline'" in body["error"]["message"]
            # The first run's file is still there
            status, _, _ = await get_raw(c, url)
            assert status == 200
        run_async(talk, gui)
        assert stop_server(gui.proc) == 0
    assert where and not where[0].exists(), "the server removes its timeline files when it ends"


def test_a_manual_run_with_record_writes_and_serves_a_timeline(tmp_path: Path) -> None:
    with gui_server(tmp_path) as gui:
        async def talk(c: Client) -> None:
            session, run_start = await run_to_end(
                c,
                {
                    "file": "simulation.yaml",
                    "stop_at_ms": 1000,
                    "record": ["Engine.engine.speed", "Dashboard.dashboard.speed"],
                },
            )
            assert "timeline" not in run_start
            assert session["scenario"] is None
            assert "scenario" not in session["result"]
            path = Path(session["result"]["timeline"])
            assert path.is_absolute() and path.parent.name.startswith("vehicle-sil-gui-timelines-")
            url = session["timeline_url"]
            assert url == api_path(session["id"])

            status, body, headers = await get_raw(c, url)
            assert status == 200, body
            tl = json.loads(body)
            assert tl["vehicle_sil_timeline"] == 1
            assert tl["scenario"] is None
            assert tl["recorded_to_ms"] == 1000
            assert tl["steps"] == [] and tl["lines"] == []
            assert [v["name"] for v in tl["variables"]] == ["Engine.engine.speed", "Dashboard.dashboard.speed"]
            assert all(v["recorded"] is True and v["written"] is False and v["watched"] is False for v in tl["variables"])
            assert headers["Content-Disposition"] == 'inline; filename="simulation.timeline.json"'
        run_async(talk, gui)


def test_a_manual_run_with_record_gets_no_timeline_without_manual_timeline_feature(tmp_path: Path) -> None:
    features = f"{WITHOUT_TIMELINE},timeline,run-stop-at,record-signals"
    with gui_server(tmp_path, env={"REPLAY_FEATURES": features}) as gui:
        async def talk(c: Client) -> None:
            session, _ = await run_to_end(
                c,
                {
                    "file": "simulation.yaml",
                    "stop_at_ms": 1000,
                    "record": ["Engine.engine.speed"],
                },
            )
            assert session["scenario"] is None
            assert session.get("timeline_url") is None
            assert "timeline" not in session["result"]
            status, body = await c.get(api_path(session["id"]))
            assert (status, body["error"]["code"]) == (404, "no-timeline")
        run_async(talk, gui)


def test_a_launcher_without_the_feature_is_given_no_timeline(tmp_path: Path) -> None:
    with gui_server(tmp_path, env={"REPLAY_FEATURES": WITHOUT_TIMELINE}) as gui:
        async def talk(c: Client) -> None:
            session, run_start = await run_to_end(c, {"file": "simulation.yaml", "scenario": PASS})
            assert "timeline" not in run_start
            assert session["result"]["scenario"]["timeline"] is None
            assert session["scenario"]["status"] == "passed" and session["scenario"]["timeline_url"] is None
            status, body = await c.get(api_path(session["id"]))
            assert (status, body["error"]["code"]) == (404, "no-timeline")
        run_async(talk, gui)


def test_the_page_is_served_without_a_token_under_the_servers_checks(tmp_path: Path) -> None:
    page = find_page_dir()
    assert page == REPOSITORY / "gui" / "timeline"
    with gui_server(tmp_path) as gui:
        async def talk(c: Client) -> None:
            for path, media, name in (("timeline/", "text/html", "timeline.html"),
                                      ("timeline/timeline.html", "text/html", "timeline.html"),
                                      ("timeline/vehicle-sil-timeline.js", "text/javascript",
                                       "vehicle-sil-timeline.js"),
                                      ("timeline/page.js", "text/javascript", "page.js")):
                status, body, headers = await get_raw(c, path, token=None)
                assert status == 200, (path, body[:200])
                assert body == (page / name).read_bytes()
                assert headers["Content-Type"].startswith(media)
                assert headers["Content-Security-Policy"] == CSP and headers["X-Content-Type-Options"] == "nosniff"
            status, _, headers = await get_raw(c, "timeline", token=None)
            assert status == 308 and headers["Location"] == "/timeline/"
            # The page's query (the file and the language) goes along
            status, _, headers = await get_raw(c, "timeline?src=/api/timeline/inst-3&lang=ja", token=None)
            assert status == 308 and headers["Location"] == "/timeline/?src=/api/timeline/inst-3&lang=ja"
            # The sources of the one file, and the files to work on it, are not served
            for path in ("timeline/eslint.config.js", "timeline/tests/viewer.test.js", "timeline/..%2Fweb",
                         "timeline/x.js", "timeline/viewer.js", "timeline/messages.js", "timeline/bundle.js",
                         "timeline/timeline.css"):
                status, body, _ = await get_raw(c, path, token=None)
                assert status == 404, (path, body[:200])
            status, body, _ = await get_raw(c, "timeline/", token=None, host="localhost:1")
            assert status == 403 and json.loads(body)["error"]["code"] == "forbidden-host"
        run_async(talk, gui)


# ---------------------------------------------------------------- without a server


def test_serve_run_command_names_the_timeline_file(tmp_path: Path) -> None:
    file, request = tmp_path / "t.timeline.json", tmp_path / "r.json"
    with_tl = json.loads(serve_run_command(request, timeline=file, command_id=1))
    assert with_tl == {"command": "run", "request": str(request), "timeline": str(file), "id": 1}
    without_tl = json.loads(serve_run_command(request, command_id=1))
    assert "timeline" not in without_tl


def test_a_file_is_kept_only_when_the_result_says_it_was_written_whole() -> None:
    assert written_whole({"result": "failed", "findings": [{"code": "scenario-failed"}]}) is True
    assert written_whole({"result": "passed"}) is True
    assert written_whole({"findings": [{"code": "scenario-failed"}, {"code": "timeline-write"}]}) is False
    assert written_whole(None) is False  # no result: the launcher ended before it wrote the file


def test_the_files_of_the_last_runs_are_kept_and_the_rest_removed(tmp_path: Path) -> None:
    timelines = Timelines()
    try:
        files = []
        for idx in range(1, KEPT + 3):
            run_id = f"inst-{idx}"
            path = timelines.new_file()
            assert path.parent == timelines.directory and not path.exists()
            files.append(path)
            if idx == 2:  # the launcher wrote nothing (it ended before the run)
                assert timelines.finish(run_id, path, True, FAIL) is False
                continue
            path.write_text('{"vehicle_sil_timeline": 1}\n')
            if idx == 3:  # a file written only in part (the result reports timeline-write)
                assert timelines.finish(run_id, path, False, FAIL) is False
                assert not path.exists()
                continue
            assert timelines.finish(run_id, path, True, "scenarios/a b.yaml" if idx == 4 else
                                    "scenarios/ü.yaml") is True
        assert KEPT == 4  # the number in docs/gui.md and gui/protocol/README.md
        kept = sorted(timelines.ready)
        assert kept == ["inst-1", "inst-4", "inst-5", "inst-6"]
        assert sorted(p.name for p in timelines.directory.iterdir()) == sorted(
            timelines.ready[i][0].name for i in kept)
        assert (timelines.file_of("inst-4")[1], timelines.file_of("inst-5")[1]) == (
            "a_b.timeline.json", "_.timeline.json"
        )
        # one more: the oldest goes
        path = timelines.new_file()
        path.write_text("{}")
        timelines.finish("inst-7", path, True, PASS)
        assert timelines.file_of("inst-1") is None and not files[0].exists() and timelines.file_of("inst-7")[1] == \
            "throttle.timeline.json"
        directory = timelines.directory
    finally:
        timelines.remove_all()
    assert not directory.exists() and timelines.ready == {}


@pytest.mark.parametrize("name", ["timeline.html", "vehicle-sil-timeline.js", "page.js"])
def test_the_page_dir_is_found_only_with_all_its_files(tmp_path: Path, monkeypatch, name: str) -> None:
    import vehicle_sil_gui.timelines as module
    fake = tmp_path / "vehicle_sil_gui"
    (fake / "timeline").mkdir(parents=True)
    for other in ("timeline.html", "vehicle-sil-timeline.js", "page.js"):
        if other != name:
            (fake / "timeline" / other).write_text("x")
    monkeypatch.setattr(module, "__file__", str(fake / "timelines.py"))
    assert find_page_dir() is None  # neither the packaged one (one file missing) nor a repository above tmp_path
    (fake / "timeline" / name).write_text("x")
    assert find_page_dir() == fake / "timeline"
