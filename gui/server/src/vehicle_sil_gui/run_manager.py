"""Active run lifecycle, `allowed()` projection, AsyncExitStack start pipeline, and shutdown coordination.

Owns:
- `current`, `_runs`, `_starting`, `_starts_in_flight`/`_started`, `_prepares_in_flight`/`_prepared`
- `_next_session_id`, `_stream_seq`, `_closing`, `_hurry`
- `Timelines`, `LauncherRuntime` (`ServeRuntime`), `_library`/`_no_library`/`_library_lock`
- `facts()`, `run_facts()`, `allowed()` (sole owner — no circular callbacks from `Session`)
- `Run.silence_task` and `_RunSink` event/finish/cleanup delivery

Does not import `aiohttp`, `WsGateway`, or `server`.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import time
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass, field
from pathlib import Path

from . import __version__, control, process
from .allowed import Facts, RunFacts, allowed as allowed_of, allowed_start
from .config_service import CHECK_TIMEOUT_S, ConfigService, make_request_file
from .contracts import (PROTOCOL, PrepareRequest, RunRequest, StopSessionRequest, parse_time_command_request)
from .hub import Hub, encode
from .launcher import (FEATURE_TIMELINE, NO_CONNECTION, SPEED_MAX, SPEED_MIN, STEP_MS_MAX, Launcher,
                       hide_request, hide_request_in_event)
from .paths import ConfigFile
from .recording import Recording
from .refusal import Refused
from .runtime import HotDecision, LauncherRuntime, RunHandle, RunSpec, ServeRuntime
from .session import SILENT_AFTER_S, Limits, Session, _Staging
from .source import Source
from .timelines import KEPT as TIMELINES_KEPT, Timelines, api_path, written_whole
from .variables import AT_MS_MAX, Limits as VariableLimits, Rejected, Variables

MAX_LINE = 16 * 1024 * 1024  # one line of the launcher's events (a check with many variables is long)
STDIN_DRAIN_S = 2  # writing a stop command to a launcher that does not read its input
ESCALATE_WAIT_S = 5  # at the server's end, after asking the group to stop, before killing it
START_WAIT_S = 5  # at the server's end, for a launcher being started to be registered
VARIABLES_CLOSE_S = 3  # at a run's end, for the connection to its control server to close
SOCKETS_CLOSE_S = 3  # at the server's end, for the WebSocket connections to close
# The longest the server's end takes after --stop-timeout (see cli.py): the waits above, the output read
# after the launcher's exit, and aiohttp's own end (twice its shutdown_timeout).
RUNNER_SHUTDOWN_S = 1.0
END_BUDGET_S = (START_WAIT_S + ESCALATE_WAIT_S + process.KILL_WAIT_S + process.OUTPUT_GRACE_S + 1
                + VARIABLES_CLOSE_S + SOCKETS_CLOSE_S + 2 * RUNNER_SHUTDOWN_S)

log = logging.getLogger("vehicle_sil_gui")


def _variables_ending(session: Session) -> bool:
    return (
        session.phase in ("stopping", "ended")
        or session.stop != "none"
        or session.result is not None
        or not session.launcher_running
    )


def _noop() -> None:
    return None


@dataclass(eq=False)
class Run:
    """One active or completed run and the session it feeds."""
    session: Session
    handle: RunHandle
    variables: Variables
    silence_task: asyncio.Task | None = None
    features: tuple[str, ...] = ()  # of the launcher that runs it (version --json when it started)
    credentials: Path | None = None  # the control server's credentials file, until the launcher has read it
    request: Path | None = None  # the request file (what the browser chose); its name is hidden from the output
    request_gone: bool = False  # the file is removed (when the launcher has ended)
    token: str | None = None  # of that file
    recording: Recording = field(default_factory=Recording)  # the applied writes made through this server
    timeline: Path | None = None  # the file the launcher writes the scenario's timeline to (--timeline)
    preflight_timing: dict = field(default_factory=dict)


@dataclass(frozen=True)
class _ResolvedRun:
    config: ConfigFile
    scenario: str | None
    intent: dict
    disable: list[str]
    manual: list[str]
    hot_decision: HotDecision
    known_networks: list[str] | None
    checked_stamp: tuple
    checked_sha256: str | None


class _RunSink:
    """Routes runtime lifecycle callbacks for one run to RunManager, Session, Variables, and Hub."""

    def __init__(self, manager: RunManager, run: Run):
        self._runs = manager
        self._run = run
        self.check_decision = asyncio.Event()
        self.cleanup_done = asyncio.Event()

    def on_stdout_event(self, event: dict, size: int) -> None:
        run = self._run
        if run.session.discarded:
            return
        self._runs.relay_event(run.session, event, size, run)
        st = run.session._staging
        if st is not None and st.decision:
            self.check_decision.set()

    def on_stdout_line(self, line: str | None) -> None:
        run, session = self._run, self._run.session
        if session.discarded:
            return
        if line is None:
            if session.staged:
                session.stage_line_too_long()
                return
            messages = session.problem("line-too-long", f"the launcher wrote a line longer than "
                                       f"{MAX_LINE} bytes: it is skipped", None)
            if session.check is None and not session.check_timing and run.preflight_timing:
                # Check and run use the same intent, including its stop time.
                session.check_timing = run.preflight_timing
                messages.append(self._runs.session_message(session))
            self._runs._hub.broadcast(messages)
        elif line.strip():
            raw = hide_request(line, run.request) if run.request is not None else line
            text = raw if isinstance(raw, str) else line
            if session.silent_s is not None:
                messages = session.bad_line(text, allowed=self._runs.allowed(session))
            else:
                messages = session.bad_line(text)
            if messages:
                self._runs._hub.broadcast(messages)

    def on_stderr_line(self, line: str) -> None:
        if not line or self._run.session.discarded:
            return
        run = self._run
        raw = hide_request(line, run.request) if run.request is not None else line
        text = raw if isinstance(raw, str) else line
        messages = run.session.stderr_line(text)
        if messages:
            self._runs._hub.broadcast(messages)

    def on_problem(self, code: str, message: str, detail: object = None) -> None:
        session = self._run.session
        if session.discarded:
            return
        messages = session.problem(code, message, detail)
        if messages:
            self._runs._hub.broadcast(messages)

    def on_process_exited(self, returncode: int | None) -> None:
        run = self._run
        if run.session.staged:
            run.session.stage_exit(returncode)
            return
        if run.silence_task is not None:
            run.silence_task.cancel()
        # Recorded now (api/session and a stop see it), sent with the end: a summary sent before the
        # launcher's last lines are read would be older than the events that follow it.
        run.session.launcher_exited(returncode, tell=False)
        run.request_gone = control.remove(run.request)  # the launcher has ended: it needs the file no more

    async def on_run_finished(self, *, output_cut: bool = False, launcher_alive: bool = False) -> None:
        run = self._run
        if run.session.staged:
            run.session.stage_finish(output_cut, launcher_alive)
            if not run.session.discarded:
                self.check_decision.set()
            return
        if run.silence_task is not None:
            run.silence_task.cancel()
        if not run.request_gone:
            run.request_gone = control.remove(run.request)
        await self._runs._close_variables(run)  # before the end: the summary sent with it has their last state
        await self._runs._keep_timeline(run)  # before the end too: the summary says where the file is
        self._runs._hub.broadcast(run.session.ended(
            output_cut=output_cut, launcher_alive=launcher_alive,
            facts=self._runs.facts(), execution_status=self._runs.runtime.execution_status(),
        ))

    async def on_run_cleanup(self) -> None:
        session = self._run.session
        if session.staged:
            if not session.discarded:
                session.mark_cleanup_staged()
                self.cleanup_done.set()
                return
            await self._cleanup_run()
            self.cleanup_done.set()
            return
        await self._cleanup_run()

    async def _cleanup_run(self) -> None:
        run = self._run
        if run.silence_task is not None:
            run.silence_task.cancel()
            await asyncio.gather(run.silence_task, return_exceptions=True)
            run.silence_task = None
        self._runs._remove_credentials(run, last=True)
        if not run.request_gone and not control.remove(run.request):
            log.warning("the request file %s of run %s could not be removed: remove it by hand", run.request,
                        run.session.id)
        if not run.variables.closed:  # cancelled on the way: the connection is still closed
            await asyncio.shield(self._runs._close_variables(run))
        self._runs._runs.discard(run)


class RunManager:
    """Single owner of active run lifecycle, `allowed()` projection, timelines, and runtime supervision."""

    def __init__(
        self,
        *,
        instance: str,
        launcher: Launcher,
        configs: ConfigService,
        runtime: LauncherRuntime | None = None,
        initial_file: str | None = None,
        stop_timeout_s: float | None = None,
        participant_start_timeout_ms: int = 30000,
        participant_stop_timeout_ms: int = 10000,
        check_timeout_s: float = CHECK_TIMEOUT_S,
        silent_after_s: float = SILENT_AFTER_S,
        limits: Limits | None = None,
        variables: bool = True,
        variable_limits: VariableLimits | None = None,
        source: Callable[[str, str], Source] | None = None,
        save: bool = True,
        command_changes_allowed: bool = False,
        auth: bool = True,
        hub: Hub | None = None,
        on_variables_changed: Callable[[], None] = _noop,
    ):
        self.instance = instance
        self.launcher = launcher
        self._configs = configs
        self._initial_file = initial_file
        self._stop_timeout_s = stop_timeout_s
        self._check_timeout_s = check_timeout_s
        self._silent_after_s = silent_after_s
        self._limits = limits or Limits()
        self._variables_enabled = variables
        self._variable_limits = variable_limits or VariableLimits()
        self._source = source
        self._save = save
        self._command_changes_allowed = command_changes_allowed
        self._auth = auth
        self.timelines = Timelines()
        self.current: Run | None = None  # the last run started (the one the screens see)
        self._runs: set[Run] = set()  # every launcher not yet followed to its end
        self._starting = False
        self._starts_in_flight = 0  # post_run() calls not yet past registration and buffered-event drain
        self._started = asyncio.Event()  # set when no post_run() is in flight
        self._started.set()
        self._prepares_in_flight = 0
        self._prepared = asyncio.Event()  # set when no post_prepare() is in flight
        self._prepared.set()
        self._next_session_id = 1
        self._stream_seq = 0
        self._hurry = asyncio.Event()
        self._closing = False
        self._library: control.Library | None = None
        self._no_library: str | None = None  # why the gRPC library cannot be used, once known
        self._library_lock = asyncio.Lock()
        self._hub: Hub = hub if hub is not None else Hub(lambda *_: [], next_stream_seq=self.next_stream_seq)
        self._on_variables_changed = on_variables_changed
        self.runtime: LauncherRuntime = runtime or ServeRuntime(
            self.launcher, cwd=configs.root.root,
            participant_start_timeout_ms=participant_start_timeout_ms,
            participant_stop_timeout_ms=participant_stop_timeout_ms,
            check_timeout_s=check_timeout_s,
            max_line=MAX_LINE, escalate_wait_s=ESCALATE_WAIT_S,
            finish_timeout_s=process.OUTPUT_GRACE_S + 1 + VARIABLES_CLOSE_S,
            on_status=self._on_runtime_status,
            track=configs.checks,
        )
        self.runtime.sync_launcher()

    def bind_hub(self, hub: Hub, on_variables_changed: Callable[[], None]) -> None:
        """Connects the WebSocket gateway's Hub and watch wakeup callback."""
        self._hub = hub
        self._on_variables_changed = on_variables_changed

    @property
    def session(self) -> Session | None:
        return self.current.session if self.current else None

    @property
    def closing(self) -> bool:
        return self._closing

    @property
    def stream_seq(self) -> int:
        return self._stream_seq

    def next_stream_seq(self) -> int:
        self._stream_seq += 1
        return self._stream_seq

    def not_closing(self) -> None:
        if self._closing:
            raise Refused(503, "shutting-down", "the GUI server is stopping: start it again to go on")

    # ------------------------------------------------------------ facts & allowed (single owner)

    def facts(self) -> Facts:
        """What `allowed` needs of the server itself (the launcher is as the last probe found it)."""
        usable = self.launcher.usable
        return Facts(closing=self._closing, starting=self._starting, launcher_usable=usable,
                     launcher_message="" if usable else "; ".join(p["message"] for p in self.launcher.problems),
                     save=self._save, command_changes_allowed=self._command_changes_allowed, auth=self._auth)

    def run_facts(self, session: Session | None = None) -> RunFacts:
        s = session if session is not None else self.session
        if s is None:
            return RunFacts()
        return s.run_facts(self.runtime.execution_status())

    def allowed(self, session: Session | None = None) -> dict:
        """The `allowed` of the given/current run; with no run, what the server accepts (start)."""
        return allowed_of(self.facts(), self.run_facts(session))

    def session_summary(self, session: Session | None = None, *, variables: bool = True) -> dict | None:
        s = session if session is not None else self.session
        return s.summary(variables=variables, allowed=self.allowed(s)) if s is not None else None

    def session_message(self, session: Session) -> dict:
        return session.message(allowed=self.allowed(session))

    def _on_runtime_status(self, status: dict) -> None:
        self._hub.broadcast([{"type": "runtime", "runtime": status}])
        if not self._starting:
            self._push_allowed()

    def _push_allowed(self) -> None:
        """Tells the screens an `allowed` that changed with no `session` message to carry it."""
        answer = self.allowed()
        if answer != self._hub.last_allowed:
            self._hub.broadcast([{"type": "allowed", "run_id": self.session.id if self.session else None,
                                  "allowed": answer}])

    async def ensure_usable(self) -> None:
        await self.launcher.probe()
        self.runtime.sync_launcher()
        if not self.launcher.usable:
            raise Refused(503, "launcher-unusable",
                          "; ".join(p["message"] for p in self.launcher.problems) or "the launcher is not usable")

    # ------------------------------------------------------------ read endpoints

    async def get_app(self) -> dict:
        await self.launcher.probe()
        self.runtime.sync_launcher()
        self._push_allowed()  # the probe may have made the launcher usable or not
        why_not = await self._no_variables()
        v = self._variable_limits
        return {
            "vehicle_sil_gui": PROTOCOL, "version": __version__, "instance": self.instance,
            "root": str(self._configs.root.root), "initial_file": self._initial_file,
            "launcher": self.launcher.info(),
            "variables": {"available": why_not is None, "reason": why_not},
            "limits": {"log_lines_per_source": self._limits.log_lines_per_source,
                       "check_timeout_s": self._check_timeout_s, "speed_min": SPEED_MIN, "speed_max": SPEED_MAX,
                       "step_ms_max": STEP_MS_MAX,
                       "watches": v.watches, "watch_variables": v.watch_variables,
                       "watch_period_ms_min": v.period_ms_min, "watch_period_ms_max": v.period_ms_max,
                       "writes_per_request": v.writes_per_request, "write_at_ms_max": AT_MS_MAX,
                       "write_timeout_s": v.write_timeout_s},
        }

    def get_session_payload(self) -> str:
        # Strict JSON encoding matching WebSocket (never bare NaN or Infinity).
        return encode({"instance": self.instance, "session": self.session_summary(),
                       "allowed": self.allowed(), "runtime": self.runtime.status()})

    def get_recording(self) -> dict:
        run = self.current
        if run is None:
            return {"recording": None}
        enums = {(peer, name): entry["enum"] for peer, p in run.variables.peers.items()
                 for name, entry in p.index.items() if entry.get("enum")}
        sim_ms = (run.session.time or {}).get("sim_ms")  # how far the run has gone (its last time event)
        run_ms = sim_ms if isinstance(sim_ms, int) and not isinstance(sim_ms, bool) else None
        return {"recording": run.recording.to_json(run.session.id, run.session.file,
                                                   run.session.state, enums, run_ms)}

    async def get_timeline_file(self, run_id: str) -> tuple[Path, str]:
        path = await asyncio.to_thread(self.timelines.file_of, run_id) if run_id else None
        if path is None:
            raise Refused(404, "no-timeline",
                          f"run {run_id} has no timeline file on this server: a scenario run started from this GUI "
                          "server records one when the launcher has the feature 'timeline', and it can be read once "
                          f"the run has ended; the server keeps the files of the last {TIMELINES_KEPT} such runs "
                          "until it stops", {"run_id": run_id})
        return path

    # ------------------------------------------------------------ prepare, start, stop, time, write

    async def release_config(self, relative: str) -> None:
        await self.runtime.release_config(relative)

    @contextlib.asynccontextmanager
    async def prepare_guard(self) -> AsyncIterator[None]:
        self._prepares_in_flight += 1
        self._prepared.clear()
        try:
            self.not_closing()
            start = allowed_start(self.facts(), self.run_facts().running)
            if start["reason"] in ("already-running", "starting"):
                raise Refused(409, "already-running", "a simulation is running: stop it before preparing another")
            yield
        finally:
            self._prepares_in_flight -= 1
            if self._prepares_in_flight == 0:
                self._prepared.set()

    async def prepare(self, req: PrepareRequest, raw_file: object) -> dict:
        config = self._configs.resolve(raw_file)
        await self.ensure_usable()
        why_not = await self._no_variables()
        await self.runtime.prepare(
            config.relative,
            variant=req.variant,
            disable=tuple(req.disable or ()),
            enable=tuple(req.enable or ()),
            manual=tuple(req.manual or ()),
            want_control=why_not is None,
        )
        return {"instance": self.instance}

    async def start_run(self, req: RunRequest, body: dict) -> dict:
        self.not_closing()
        start = allowed_start(self.facts(), self.run_facts().running)
        if start["reason"] in ("already-running", "starting"):
            raise Refused(409, start["reason"], start["message"])
        # From here until the launcher is registered (or the start fails), no other run may start.
        self._starting = True
        self._starts_in_flight += 1
        self._started.clear()
        self._push_allowed()
        registered = False

        def finish_starting() -> None:
            nonlocal registered
            registered = True
            self._starting = False

        try:
            return await self._start(req, body, finish_starting)
        finally:
            if not registered:
                self._starting = False
                self._push_allowed()
            self._starts_in_flight -= 1
            if self._starts_in_flight == 0:
                self._started.set()

    async def _start(self, req: RunRequest, body: dict, finish_starting: Callable[[], None]) -> dict:
        resolved = await self._resolve_run_intent(req, body)
        run, sink = await self._launch_run(resolved)
        if run.session.staged:
            await self._await_staged_check(run, sink, resolved)
        return await self._commit_run(run, sink, finish_starting)

    async def _resolve_run_intent(self, req: RunRequest, body: dict) -> _ResolvedRun:
        config = self._configs.resolve(body.get("file"))
        await self.ensure_usable()
        scenario = self._configs.resolve_scenario(config, body.get("scenario"))
        raw_disable = list(req.disable or ())
        raw_manual = list(req.manual or ())
        raw_enable = list(req.enable or ())
        raw_connect = req.connect or {}
        has_external_connect = any(target != NO_CONNECTION for target in raw_connect.values())
        need_metadata = bool(raw_disable or raw_manual or has_external_connect)
        want_control_hint = (
            self._variables_enabled
            and control.FEATURE in self.launcher.features
            and (self._source is not None or self._no_library is None)
        )
        checked_stamp = self._configs.stamp(config.relative)
        inspection = await self.runtime.inspect_config(
            config.relative,
            variant=req.variant,
            disable=tuple(raw_disable),
            enable=tuple(raw_enable),
            manual=tuple(raw_manual),
            want_control=want_control_hint,
            need_metadata=need_metadata,
        )
        known_participants = inspection.participants
        known_networks = inspection.networks
        checked_sha256 = inspection.file_sha256
        if (raw_disable or raw_manual) and known_participants is None:
            cached = self._configs.cached_names(config.relative, req.variant, checked_stamp)
            if cached is not None:
                known_participants = cached
            else:
                base = await self.runtime.inspect_config(
                    config.relative,
                    variant=req.variant,
                    want_control=want_control_hint,
                    need_metadata=True,
                    allow_prepare_on_resident=False,
                )
                known_participants = base.participants
                if known_networks is None:
                    known_networks = base.networks
                if checked_sha256 is None:
                    checked_sha256 = base.file_sha256
        if known_networks is None:
            known_networks = self._configs.cached_networks(config.relative, req.variant, checked_stamp)
        intent, disable, manual = await self._configs.build_intent(
            config, req, scenario, known_participants=known_participants, fetch_names=False,
        )
        return _ResolvedRun(
            config, scenario, intent, disable, manual, inspection.hot,
            known_networks, checked_stamp, checked_sha256,
        )

    async def _launch_run(self, r: _ResolvedRun) -> tuple[Run, _RunSink]:
        why_not = await self._no_variables()
        self.not_closing()
        async with contextlib.AsyncExitStack() as stack:
            credentials: Path | None = None
            token: str | None = None
            timeline: Path | None = None
            if why_not is None:
                try:
                    credentials, token = await process.run_thread_reclaimable(
                        control.credentials_file, lambda pair: control.remove(pair[0]),
                    )
                    stack.callback(lambda: control.remove(credentials))
                except OSError as e:
                    why_not = f"the file with the control server's credentials could not be made ({e})"
            if r.scenario is not None and FEATURE_TIMELINE in self.launcher.features:
                timeline = await process.run_thread_reclaimable(self.timelines.new_file, control.remove)
                stack.callback(control.remove, timeline)
            self.not_closing()
            # Last await is above: from here to launcher start no other request runs (TOCTOU guard).
            self._configs.validate_connect_targets(
                r.config.relative, r.intent.get("connect", {}), r.known_networks, r.checked_stamp, r.checked_sha256,
            )
            request = make_request_file(r.intent)
            stack.callback(control.remove, request)
            handle = await self.runtime.start_run(RunSpec(
                config=r.config.relative, request=request, credentials=credentials, timeline=timeline,
                variant=r.intent.get("variant"), disable=tuple(r.disable), enable=tuple(r.intent.get("enable", ())),
                manual=tuple(r.manual), hot_decision=r.hot_decision,
            ))

            def abort_handle(h: RunHandle = handle) -> None:
                h.send_stop(force=True)
                if (abort := getattr(h, "abort", None)) is not None:
                    abort()

            stack.callback(abort_handle)
            run_credentials, run_token = credentials, token
            if handle.control_token is not None:
                control.remove(run_credentials)
                credentials = run_credentials = None
                run_token = handle.control_token
            run, sink = self._build_run(r, handle, why_not, run_credentials, run_token, timeline, request)
            self._runs.add(run)
            stack.callback(self._runs.discard, run)
            handle.start(sink)
            stack.pop_all()
            return run, sink

    def _build_run(
        self,
        r: _ResolvedRun,
        handle: RunHandle,
        why_not: str | None,
        credentials: Path | None,
        token: str | None,
        timeline: Path | None,
        request: Path,
    ) -> tuple[Run, _RunSink]:
        session = Session(
            f"{self.instance}-{self._next_session_id}", r.config.relative, r.disable, r.intent.get("speed"),
            r.intent.get("start_paused", False), limits=self._limits, launcher_pid=handle.pid,
            launcher_mode=handle.mode, scenario=r.scenario, manual=r.manual, staged=handle.waits_for_check,
        )
        session.next_stream_seq = self.next_stream_seq
        variables = Variables(
            session.id, lambda msgs: None if session.discarded else self._tell_variables(msgs),
            limits=self._variable_limits, secret=token, ending=lambda: _variables_ending(session),
        )
        variables.expect() if why_not is None else variables.set_off(why_not)
        session.variables = variables.summary
        recording = Recording() if why_not is None and r.scenario is None else Recording(
            available=False, reason="this run plays a scenario: its writes are the scenario's own" if r.scenario
            is not None else f"this run has no variables to write: {why_not}")
        preflight_timing = {k: r.intent[k] for k in ("stop_at_ms", "late_tolerance_ms") if k in r.intent}
        run = Run(session, handle, variables, features=tuple(self.launcher.features), credentials=credentials,
                  token=token, recording=recording, timeline=timeline, request=request,
                  preflight_timing=preflight_timing)
        return run, _RunSink(self, run)

    async def _abort_staged_run(self, run: Run, sink: _RunSink, *, force_stop: bool, wait_s: float) -> None:
        handle = run.handle
        try:
            if force_stop:
                handle.send_stop(force=True)
            if not sink.cleanup_done.is_set():
                with contextlib.suppress(TimeoutError, Exception):
                    async with asyncio.timeout(wait_s):
                        await sink.cleanup_done.wait()
            if not sink.cleanup_done.is_set():
                abort = getattr(handle, "abort", None)
                if abort is not None:
                    abort()
                with contextlib.suppress(TimeoutError, Exception):
                    async with asyncio.timeout(process.KILL_WAIT_S + 1.0):
                        await sink.cleanup_done.wait()
        finally:
            await asyncio.shield(self._discard_failed_start(run))

    async def _await_staged_check(self, run: Run, sink: _RunSink, resolved: _ResolvedRun) -> None:
        session = run.session
        st = session._staging
        assert st is not None
        wait_cap = 5.0 if self._stop_timeout_s is None else self._stop_timeout_s
        try:
            async with asyncio.timeout(self._check_timeout_s):
                await sink.check_decision.wait()
        except BaseException as exc:
            session.discard_staging()
            await self._abort_staged_run(run, sink, force_stop=True, wait_s=min(1.0, wait_cap))
            if isinstance(exc, TimeoutError):
                raise Refused(504, "check-timeout",
                              f"the check did not end in {self._check_timeout_s:g} s (it was stopped): "
                              "an ECU's --describe may hang") from None
            raise
        if st.failed_check is not None:
            session.discard_staging()
            await self._abort_staged_run(run, sink, force_stop=False, wait_s=min(5.0, wait_cap))
            self.not_closing()
            checked = hide_request(st.failed_check, run.request) if run.request is not None else st.failed_check
            raise Refused(422, "check-failed", self._check_failed_message(resolved.config.relative, checked),
                          {"result": checked.get("result"), "findings": checked.get("findings", [])})
        if self._closing:
            session.discard_staging()
            await asyncio.shield(self._discard_failed_start(run))
            raise Refused(503, "shutting-down", "the GUI server is stopping: the run that was starting is stopped")
        if isinstance(st.passed_check, dict):
            names = [p["name"] for p in st.passed_check.get("participants", [])
                     if isinstance(p, dict) and isinstance(p.get("name"), str)]
            if names or st.passed_check.get("result") not in ("usage",):
                self._configs.remember_names(
                    resolved.config.relative, resolved.intent.get("variant"), resolved.checked_stamp, names,
                )

    async def _commit_run(self, run: Run, sink: _RunSink, finish_starting: Callable[[], None]) -> dict:
        session = run.session
        staged = session.take_staged()
        self._next_session_id += 1
        self.current = run
        run.silence_task = asyncio.create_task(self._watch_silence(session))
        # Registration is complete: clear starting state before broadcasting the session and draining
        # staged callbacks so an already-exited launcher neither corrupts `allowed` while draining
        # nor triggers a spurious `allowed` push when start_run() exits.
        finish_starting()
        # Step 1: initial session (clean pre-event summary + allowed) followed by variables.
        self._hub.broadcast([self.session_message(session)])
        self._tell_variables([run.variables.message()])
        if staged is not None:
            if staged.omitted_problems:
                session.problems_omitted += staged.omitted_problems
            # Step 2: out-of-band prestart dropped notice (no stream_seq) with watermark S_vars + 1.
            vars_sseq = self._stream_seq
            dropped = session.apply_prestart_dropped(staged, prestart_seq=vars_sseq + 1)
            if dropped:
                self._hub.broadcast(dropped)
            # Step 3: one-by-one merged dispatch preserving _push_allowed() and per-event bad-event isolation.
            await self._replay_staged(run, sink, staged)
            if dropped and self._stream_seq == vars_sseq:
                session.clamp_prestart_seq(vars_sseq)
        if self._closing:  # the end began while the launcher was starting: it is stopped with the others
            raise Refused(503, "shutting-down", "the GUI server is stopping: the run that was starting is stopped")
        return {"instance": self.instance, "run_id": session.id}

    async def _replay_staged(self, run: Run, sink: _RunSink, staged: _Staging) -> None:
        session = run.session
        for item in staged.merged_items():
            kind = item[0]
            if kind == "event":
                sink.on_stdout_event(item[1], item[2])
            elif kind == "line_too_long":
                sink.on_stdout_line(None)
            elif kind == "stderr_first":
                self._hub.broadcast(session._stderr_problem(item[1]))
            elif kind == "problem":
                sink.on_problem(item[1], item[2], item[3])
            elif kind == "exit":
                sink.on_process_exited(item[1])
            elif kind == "finish":
                if staged.last_seq is not None and (session.last_seq is None or staged.last_seq > session.last_seq):
                    session.last_seq = staged.last_seq
                await sink.on_run_finished(output_cut=item[1], launcher_alive=item[2])
        if staged.last_seq is not None and (session.last_seq is None or staged.last_seq > session.last_seq):
            session.last_seq = staged.last_seq
        if staged.cleanup_called:
            await sink.on_run_cleanup()

    async def _discard_failed_start(self, run: Run) -> None:
        self._remove_credentials(run)
        if not run.request_gone:
            run.request_gone = control.remove(run.request)
        control.remove(run.timeline)
        run.timeline = None
        if not run.variables.closed:
            await self._close_variables(run)
        self._runs.discard(run)

    async def stop_session(self, req: StopSessionRequest) -> dict:
        """Stop one identified run, never a run that replaced it."""
        if (
            req.instance != self.instance
            or self._starting
            or self.current is None
            or self.current.session.id != req.run_id
        ):
            raise Refused(409, "session-changed", "the run has changed: no stop was sent")
        run = self.current
        if not self._closing and self._write_stop(run, req.force):  # decided, recorded and written before any await
            await run.handle.drain(STDIN_DRAIN_S)
        return {"instance": self.instance, "run_id": run.session.id}

    async def send_time(self, body: dict) -> dict:
        """Forwards one validated time command to the running launcher's standard input."""
        try:
            command, run_id, instance = parse_time_command_request(body)
        except ValueError as exc:
            raise Refused(400, "invalid-field", str(exc)) from None
        targeted = run_id is not None
        run = self.current
        if targeted and (instance != self.instance or self._starting or run is None or run_id != run.session.id):
            raise Refused(409, "session-changed", "the run has changed: no time command was sent")
        if run is None or self._starting or run.session.state != "running" or not run.session.launcher_running \
                or not run.handle.can_send():
            raise Refused(409, "not-running", "no simulation is running: time commands go to the run that is "
                                              "running")
        said = self._launcher_refuses(run, command["command"])
        if said is not None and said[0] == "not-running":
            raise Refused(409, "not-running", said[1])
        gone = Refused(409, "not-running", "the launcher's input is closed (it is ending): the command was not sent")
        if not run.handle.send_time(command):  # written before any await: in the order the requests came
            raise gone
        if command["command"] in ("resume", "step"):
            run.variables.unsettle()  # it runs on: the values are not called paused until they are read again
        if not await run.handle.drain(STDIN_DRAIN_S):
            raise gone
        return {"run_id": run.session.id, "command": command}

    async def write_variables(self, body: dict) -> dict:
        """Validates and forwards a variable write to the running simulation's control server."""
        meant = body.get("run_id") if isinstance(body, dict) else None
        if not isinstance(meant, str) or not meant:
            raise Refused(400, "invalid-field", "a write needs run_id: the id of the run it is meant for (the "
                                                "id of the summary of GET api/session, or of hello)")
        if "instance" in body:
            inst = body.get("instance")
            if not isinstance(inst, str) or not inst:
                raise Refused(400, "invalid-field",
                              "instance must be the server instance from the run or session answer")
            if inst != self.instance:
                raise Refused(409, "session-changed",
                              "the server instance has changed: nothing was written; look at the variables of "
                              "this run before writing to it")
        run = self.current
        if run is None or self._starting or run.session.state != "running" or not run.session.launcher_running:
            raise Refused(409, "not-running", "no simulation is running: variables are written to the run that "
                                              "is running")
        if meant != run.session.id:
            raise Refused(409, "session-changed", f"the write is meant for the run {meant}, but the run that is "
                                                  f"running is {run.session.id}: nothing was written; look at "
                                                  "the variables of this run before writing to it")
        said = self._launcher_refuses(run, "write")
        if said is None and run.session.launcher_state is None and run.session.scenario is not None:
            said = ("scenario-running", f"the run {run.session.id} plays the scenario {run.session.scenario}: "
                                        "variables cannot be written while it runs (the scenario must give the "
                                        "same result every time); read them, or start a run without a scenario "
                                        "to write")
        if said is not None and said[0] != "no-control-server":
            raise Refused(409, said[0], said[1])
        if control.FEATURE not in run.features:
            raise Refused(400, "unsupported", "the launcher of this run was built without a control server: "
                                              "its ECUs' variables cannot be written")
        try:
            writes, at_ns, late = run.variables.check_write(body)
            seq = await run.variables.write(writes, at_ns, late)
        except Rejected as e:
            raise Refused(e.status, e.code, str(e), e.detail) from None
        run.recording.wrote(seq, writes, run.variables.operations.get(seq))
        return {"run_id": run.session.id, "operation": seq}

    # ------------------------------------------------------------ variables, events, helpers

    async def _no_variables(self) -> str | None:
        """Why a run started now would have no variables (None: it would have them). The launcher is probed."""
        if not self._variables_enabled:
            return "the GUI server was started without variables"
        if control.FEATURE not in self.launcher.features:
            return "this launcher was built without a control server, so the variables of the ECUs cannot be read"
        if self._source is not None:
            return None
        async with self._library_lock:
            if self._library is None and self._no_library is None:
                try:
                    self._library = await asyncio.to_thread(control.load)  # imports gRPC: not on the loop
                except control.Unavailable as e:
                    self._no_library = str(e)
        return self._no_library

    @staticmethod
    def _launcher_refuses(run: Run, name: str) -> tuple[str, str] | None:
        state = run.session.launcher_state
        item = state["allowed"].get(name) if state else None
        if not isinstance(item, dict) or item.get("ok") is not False or not isinstance(item.get("reason"), str):
            return None
        message = item.get("message")
        return item["reason"], message if isinstance(message, str) else f"the launcher does not accept {name} now"

    @staticmethod
    def _check_failed_message(relative: str, result: dict) -> str:
        errors = result.get("errors")
        what = f"{errors} error(s)" if isinstance(errors, int) and errors else f"the result {result.get('result')!r}"
        return (f"the check of {relative} found {what}, so no run was started: fix what detail.findings lists "
                "(the same findings as POST api/check), then start again")

    def _tell_variables(self, messages: list[dict]) -> None:
        self._hub.broadcast(messages)
        for m in messages:
            if m.get("type") == "operation":
                for run in self._runs:
                    if run.session.id == m.get("run_id"):
                        run.recording.on_operation(m)
        if any(m.get("type") == "variables" for m in messages):
            self._on_variables_changed()

    def _write_stop(self, run: Run, force: bool) -> bool:
        """Records a stop request and writes its commands to the launcher's input (no await)."""
        session = run.session
        run_facts = self.run_facts(session)
        if not run_facts.stoppable or run_facts.stop == "forced" or (not force and run_facts.stop != "none"):
            return False
        mode = session.request_stop(force)
        if mode is None:
            return False
        delivered = run.handle.send_stop(force=mode == "force")
        messages = []
        if not delivered:
            messages = session.problem("stop-not-delivered", "the stop could not be written to the launcher's "
                                                             "input (it is closed): the launcher may be ending; "
                                                             "if it is not, stop the GUI server", None)
        messages.append(self.session_message(session))
        self._hub.broadcast(messages)
        return delivered

    async def _keep_timeline(self, run: Run) -> None:
        if run.timeline is None:
            return
        whole = written_whole(run.session.result)
        if await asyncio.to_thread(self.timelines.finish, run.session.id, run.timeline, whole,
                                   run.session.scenario or ""):
            run.session.timeline_url = api_path(run.session.id)

    def _remove_credentials(self, run: Run, last: bool = False) -> None:
        if control.remove(run.credentials):
            run.credentials = None
        elif last:
            log.warning("the credentials file %s of run %s could not be removed: remove it by hand (its token is "
                        "of no use after the run)", run.credentials, run.session.id)

    async def _close_variables(self, run: Run) -> None:
        self._remove_credentials(run)
        try:
            async with asyncio.timeout(VARIABLES_CLOSE_S):
                await run.variables.close()
        except TimeoutError:
            log.warning("the connection to the control server of run %s did not close in time", run.session.id)

    def _control_started(self, run: Run, event: dict) -> None:
        self._remove_credentials(run)
        variables = run.variables
        if variables.state != "connecting" or variables.source is not None or variables.closed:
            return
        try:
            address = control.check_address(event.get("address"))
            if event.get("tls") is True:
                raise ValueError("the launcher's control server asks for TLS, which this GUI server did not ask for")
            if self._source is not None:
                source = self._source(address, run.token or "")
            else:
                source = control.GrpcSource(self._library, address, run.token, run_id=run.handle.run_id)
        except Exception as e:
            variables.fail(f"{e}: the variables of this run are not read")
            return
        variables.connect(source)

    async def _watch_silence(self, session: Session) -> None:
        while True:
            await asyncio.sleep(min(1.0, self._silent_after_s / 4))
            self._hub.broadcast(session.check_silence(
                time.monotonic(), self._silent_after_s, allowed=self.allowed(session),
            ))

    def _dispatch_messages(self, session: Session, messages: list[dict], run: Run | None) -> bool:
        sent = self._hub.broadcast(messages, session.log_key)
        if run is not None:
            for m in messages:
                if m["type"] != "event":
                    continue
                if m["event"]["event"] == control.EVENT:
                    self._control_started(run, m["event"])
                elif m["event"]["event"] == "phase" and m["event"].get("phase") == "stopping":
                    run.variables.read_last()  # the simulation is over: its control server closes soon
                elif m["event"]["event"] == "state":
                    self._push_allowed()
        return sent

    def relay_event(self, session: Session, event: dict, size: int, run: Run | None = None) -> None:
        """One already-parsed launcher event: into the summary and to the screens."""
        try:
            if run is not None and run.request is not None:
                cleaned = (
                    hide_request_in_event(event, run.request)
                    if isinstance(event, dict)
                    else hide_request(event, run.request)
                )
            else:
                cleaned = event
            if session.silent_s is not None:
                messages = session.feed_event(cleaned, size, allowed=self.allowed(session))
            else:
                messages = session.feed_event(cleaned, size)
            if not messages or self._dispatch_messages(session, messages, run):
                return
            error_msg = "a message for the screens could not be made of it"
        except Exception as e:  # a shape the checks did not foresee
            log.exception("reading an event")
            error_msg = type(e).__name__
        detail = session._cut(repr(event))
        if run is not None and run.request is not None:
            redacted = hide_request(detail, run.request)
            detail = redacted if isinstance(redacted, str) else detail
        problem = session.problem("bad-event", f"the server could not pass on an event ({error_msg}): it is "
                                               "ignored", detail)
        if problem:
            self._hub.broadcast(problem)

    # ------------------------------------------------------------ shutdown phases

    def begin_shutdown(self) -> bool:
        """Starts shutdown (returning True), or hurries an in-progress shutdown (returning False)."""
        if self._closing:
            self._hurry.set()
            return False
        self._closing = True
        self._push_allowed()
        return True

    async def shutdown_runtime(self) -> None:
        try:
            async with asyncio.timeout(START_WAIT_S):
                await self._started.wait()
        except TimeoutError:
            log.warning("a run was still starting when the server stopped")
        await self.runtime.shutdown(self._hurry, self._stop_timeout_s)
        try:
            async with asyncio.timeout(START_WAIT_S):
                await self._prepared.wait()
        except TimeoutError:
            log.warning("a prepare was still running when the server stopped")

    def warn_active_pids(self) -> None:
        for pid in self.runtime.active_pids():
            log.warning("the launcher (pid %s) could not be stopped: stop it by hand", pid)

    async def cleanup_timelines(self) -> None:
        await asyncio.to_thread(self.timelines.remove_all)

    def kill_all(self) -> None:
        self.runtime.kill_all()
