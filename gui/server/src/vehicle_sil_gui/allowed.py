"""What this server accepts of a screen now: `allowed.start`, `allowed.stop` and `allowed.force` of the summary.

`rev` goes up with each change. Each is {"ok", "reason", "message"}: ok says whether the server would take the request (POST api/run, POST api/stop
and POST api/stop with force), reason is a code that stays the same (null when ok) and message is one English
sentence for a person (null when ok). The codes of start are those POST api/run answers with (409 or 503); POST api/stop always answers 200, so the
reasons of stop and force are only here.

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
    state ("none", "requested", "forced"). A stop asked for while a run starts is taken and applies once the
    launcher has started."""
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


class Numbered:
    """Gives `allowed` a number (`rev`) that goes up whenever what it says changes, so that a reader who gets it from
    several places (hello, api/session, the summary, the `allowed` message) keeps the one with the larger number.
    One per server."""

    def __init__(self) -> None:
        self.rev = 0
        self._last: dict | None = None

    def stamp(self, answer: dict) -> dict:
        if answer != self._last:
            self._last = answer
            self.rev += 1
        return {"rev": self.rev, **answer}


def allowed(facts: Facts, running: bool, stoppable: bool, stop: str) -> dict:
    return {"start": allowed_start(facts, running), "stop": allowed_stop(facts, stoppable, stop),
            "force": allowed_force(facts, stoppable, stop)}
