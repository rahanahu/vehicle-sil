"""What this server accepts of a screen now: `allowed.start`, `allowed.stop`, `allowed.force` and `allowed.save` of the
summary.

Each is {"ok", "reason", "message"}: ok says whether the server would take the request (POST api/run, POST api/stop-session
and POST api/stop-session with force), reason is a code that stays the same (null when ok) and message is one English
sentence for a person (null when ok). The codes of start are those POST api/run answers with (409 or 503); POST api/stop-session
answers 200 for the current run, so the reasons of stop and force are only here. save (POST api/config) also has caution: null,
or {"code", "message"} when the server takes the request but there is something to know first (the same shape as the
launcher's `caution`).

Only what the server knows is decided here: whether the launcher can be used, whether a run is starting, running
or ending, and how many stop requests it has taken. What the launcher accepts (pause, step, speed, writes) comes
from its own `state` event, which the summary carries as it is. Nothing here does I/O.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Facts:
    """What the server knows about itself, apart from the run."""
    closing: bool = False  # the server is ending
    starting: bool = False  # a launcher is being started for POST api/run
    launcher_usable: bool = True  # the launcher the server found can be used
    launcher_message: str = ""  # why not, when it cannot
    save: bool = True  # POST api/config saves (not --no-save)
    command_changes_allowed: bool = False  # it also saves what changes the commands that run (--allow-command-changes)
    auth: bool = True  # token authentication is enabled (not --no-auth)


@dataclass(frozen=True)
class RunFacts:
    """Projected UI-facing state of the active run (from C++ ExecutionStatus in Hot mode or Session in Cold mode)."""
    running: bool = False
    stoppable: bool = False
    stop: str = "none"


_STOP_RANK = {"none": 0, "requested": 1, "forced": 2}


def stop_of(local_stop: str, stop_requests: int | None = None) -> str:
    """Combines the server's local stop state with C++ `run_state.stop.requests`."""
    rank = _STOP_RANK.get(local_stop, 0)
    if isinstance(stop_requests, int) and not isinstance(stop_requests, bool):
        if stop_requests >= 2:
            rank = max(rank, 2)
        elif stop_requests == 1:
            rank = max(rank, 1)
    return ("none", "requested", "forced")[rank]


def project_run_facts(
    session_running: bool = False,
    launcher_running: bool = False,
    local_stop: str = "none",
    *,
    launcher_mode: str = "cold",
    execution_status: dict | None = None,
    run_state: dict | None = None,
) -> RunFacts:
    """Projects RunFacts from C++ ExecutionStatus in Hot mode or Session state in Cold mode."""
    running = bool(session_running)
    if launcher_mode == "hot":
        if not isinstance(execution_status, dict):
            return RunFacts(
                running=running,
                stoppable=False,
                stop=local_stop,
            )
        stoppable = running and execution_status.get("launcher_state") == "running"
        rs = run_state if isinstance(run_state, dict) else execution_status.get("run_state")
        stop_info = rs.get("stop") if isinstance(rs, dict) else None
        requests = stop_info.get("requests") if isinstance(stop_info, dict) else None
        return RunFacts(
            running=running,
            stoppable=stoppable,
            stop=stop_of(local_stop, requests if type(requests) is int else None),
        )
    return RunFacts(
        running=running,
        stoppable=running and bool(launcher_running),
        stop=local_stop,
    )


def _answer(reason: str | None = None, message: str | None = None) -> dict:
    return {"ok": reason is None, "reason": reason, "message": message}


def allowed_start(facts: Facts, running: bool) -> dict:
    """running: a run of this server has not ended (its launcher runs, or its output is being read)."""
    if facts.closing:
        return _answer("shutting-down", "the GUI server is stopping: it starts no new run")
    if running:  # also the run that has just been registered, while its start has not answered yet
        return _answer("already-running", "a simulation is running: stop it and wait for it to end "
                                          "before starting another")
    if facts.starting:
        return _answer("starting", "a run is starting: wait until it has started, "
                                   "then stop it before starting another")
    if not facts.launcher_usable:
        return _answer("launcher-unusable", facts.launcher_message or "the launcher is not usable")
    return _answer()


def allowed_stop(facts: Facts, stoppable: bool, stop: str) -> dict:
    """stoppable: its launcher runs (Session.request_stop writes a stop only then). stop: the session's stop
    state ("none", "requested", "forced")."""
    if facts.closing:
        return _answer("shutting-down", "the GUI server is stopping: it stops the run itself")
    if facts.starting:
        return _answer()
    if not stoppable:
        return _answer("not-running", "no simulation is running: there is nothing to stop")
    if stop != "none":
        return _answer("already-stopping", "a stop was already asked for: wait for the run to end, or stop it "
                                           "without waiting (force)")
    return _answer()


def allowed_force(facts: Facts, stoppable: bool, stop: str) -> dict:
    if facts.closing:
        return _answer("shutting-down", "the GUI server is stopping: it stops the run itself")
    if facts.starting:
        return _answer()
    if not stoppable:
        return _answer("not-running", "no simulation is running: there is nothing to stop")
    if stop == "forced":
        return _answer("already-forced", "the stop was already forced: wait for the run to end")
    return _answer()


def allowed_save(facts: Facts) -> dict:
    """POST api/config. Which texts it saves (the commands that run stay the same) is decided per request, from the
    launcher's check of the text: not here."""
    if facts.closing:
        answer = _answer("shutting-down", "the GUI server is stopping: it saves no file")
    elif not facts.save:
        answer = _answer("save-disabled", "the GUI server was started with --no-save: it saves no configuration "
                                          "file; save the text in a text editor, or start the GUI without --no-save")
    else:
        answer = _answer()
    caution = None
    if answer["ok"] and facts.command_changes_allowed:
        actor = "whoever has its token" if facts.auth else "any local process on this computer"
        caution = {"code": "command-changes-allowed",
                   "message": "the GUI server was started with --allow-command-changes: it saves an edit that changes "
                              f"the commands that run, and new files, so {actor} can run any command as "
                              "this user"}
    return {**answer, "caution": caution}


def allowed(
    facts: Facts,
    running: bool | RunFacts = False,
    stoppable: bool = False,
    stop: str = "none",
) -> dict:
    run = running if isinstance(running, RunFacts) else RunFacts(running=running, stoppable=stoppable, stop=stop)
    return {"start": allowed_start(facts, run.running), "stop": allowed_stop(facts, run.stoppable, run.stop),
            "force": allowed_force(facts, run.stoppable, run.stop), "save": allowed_save(facts)}
