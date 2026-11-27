"""control.GrpcSource (the launcher's control server as the source of the variables) against the stand-in for
that server (fake_control.py), in this process: the threads, the token, losing the connection, a flood."""

from __future__ import annotations

import asyncio
import os
import stat
import threading
import time

import pytest
from conftest import free_port, scaled
from fake_control import variables
from vehicle_sil_gui import control
from vehicle_sil_gui.source import SnapshotInfo, SourceError, TimeInfo

MS = 1_000_000
TOKEN = "a-token-of-this-test-0123456789"


class Sink:
    def __init__(self):
        self.calls: list[tuple] = []
        self.news = asyncio.Event()
        self.thread = threading.get_ident()

    def _note(self, *call) -> None:
        assert threading.get_ident() == self.thread, "the sink is called on the event loop only"
        self.calls.append(call)
        self.news.set()

    def on_peers(self, peers, removed):
        self._note("peers", [p.name for p in peers], removed)
        self.peers = getattr(self, "peers", {}) | {p.name: p for p in peers}

    def on_samples(self, samples):
        self._note("samples", samples)

    def on_operations(self, operations):
        self._note("operations", operations)

    def on_time(self, time_):
        self._note("time", time_)

    def on_synced(self):
        self._note("synced")

    def on_dropped(self):
        self._note("dropped")

    def on_lost(self, reason, fatal):
        self._note("lost", reason, fatal)

    def on_availability(self, peer, incarnation, states):
        self._note("availability", peer, incarnation, states)

    def kinds(self) -> list[str]:
        return [c[0] for c in self.calls]

    async def until(self, kind: str, timeout: float = 10) -> tuple:
        async with asyncio.timeout(timeout):
            while True:
                found = [c for c in self.calls if c[0] == kind]
                if found:
                    return found[-1]
                self.news.clear()
                await self.news.wait()


@pytest.fixture
def fake(control_env):
    import fake_control
    made = []

    def make(**options):
        server = fake_control.FakeControl({"Engine": variables("Engine"), "Body": variables("Body")}, **options)
        server.start()
        made.append(server)
        return server
    yield make
    for server in made:
        server.stop()


def library() -> control.Library:
    return control.load()


def test_the_state_the_values_a_write_and_its_result(fake) -> None:
    server = fake(token=TOKEN)

    async def main() -> None:
        source = control.GrpcSource(library(), server.address, TOKEN)
        sink = Sink()
        source.start(sink)
        await sink.until("synced")
        kinds = sink.kinds()  # the peers (in one or more parts), the time, and then the word that this is all
        assert kinds[-2:] == ["time", "synced"] and set(kinds[:-2]) == {"peers"}
        # What it asked the server to send along: the operations (but the scenario's), the time and the availability,
        # not the values.
        asked = server.requests[0]
        assert (asked.values, asked.events, asked.operations, asked.time, asked.availability) == \
            (False, False, True, True, True)
        assert list(asked.operation_clients_excluded) == ["scenario"]
        assert [p.name for p in await source.read_peers(2)] == ["Engine", "Body"]
        engine = sink.peers["Engine"]
        # every ECU has ecu.connected first (vehicle-sil adds it), then its own variables
        assert (engine.incarnation, engine.connected) == ("1", True)
        assert [v["name"] for v in engine.variables] == ["ecu.connected", "engine.demand", "engine.speed",
                                                         "engine.mode"]
        assert engine.variables[0]["builtin"] is True
        assert engine.variables[1] == {"name": "engine.demand", "type": "real", "role": "input", "unit": "%",
                                       "min": 0.0, "max": 100.0, "step": None, "description": "requested level",
                                       "enum": [], "builtin": False, "availability": None}
        assert engine.variables[3]["enum"] == [("off", 0), ("on", 1)] and engine.variables[3]["role"] == "parameter"
        # Before the first step: the time, and for every name asked for that there is no sample (a peer that
        # has none yet, a name that is no peer's).
        assert await source.read_snapshot(["Engine", "Nobody"], 2) == SnapshotInfo(
            TimeInfo(0, False, False, False), {"Engine": None, "Nobody": None}, False)
        assert await source.read_operation(99, 2) is None

        seq = await source.write([("Engine", "engine.demand", 12.5), ("Engine", "engine.mode", 1),
                                  ("Body", "body.demand", 3.0)], 100 * MS, "reject", 2)
        assert seq == 1
        queued = await source.wait_queued(1, 2)
        assert queued is not None and not queued.done
        assert [(t.peer, t.status) for t in queued.targets] == [("Engine", "queued"), ("Body", "queued")]
        assert await source.wait_queued(99, 2) is None
        operations = (await sink.until("operations"))[1]
        assert [(t.peer, t.status) for t in operations[0].targets] == [("Engine", "queued"), ("Body", "queued")]
        server.step(0)
        server.step(100)
        server.set_time(200, True)
        del sink.calls[:]
        await sink.until("time")
        done = (await sink.until("operations"))[1][0]
        assert done.done and [(t.status, t.applied_at_ns) for t in done.targets] == [("applied", 100 * MS)] * 2
        assert await source.read_operation(1, 2) == done
        assert "samples" not in sink.kinds()  # the values are read when asked for, not sent along
        # Standing still at 200: the time and the values of that moment, in one call.
        read = await source.read_snapshot(["Body", "Engine", "Body"], 2)
        assert (read.time, read.consistent, list(read.samples)) == (TimeInfo(200 * MS, True, True, False), True,
                                                                    ["Body", "Engine"])
        sample = read.samples["Engine"]
        types = [v["type"] for v in engine.variables]
        assert (sample.incarnation, sample.time_ns, sample.count) == ("1", 100 * MS, 4)
        assert sample.decode(types) == [True, 12.5, 225, 1]
        # The server reads the time first: a step that runs before it reads the values is in them, and then they
        # are not those of the time read.
        server.between_reads = lambda: server.step(200)
        read = await source.read_snapshot(["Engine"], 2)
        server.between_reads = None
        assert (read.time.now_ns, read.samples["Engine"].time_ns, read.consistent) == (200 * MS, 200 * MS, True)
        server.between_reads = lambda: server.step(210)
        read = await source.read_snapshot(["Engine"], 2)
        server.between_reads = None
        assert (read.time.now_ns, read.samples["Engine"].time_ns, read.consistent) == (200 * MS, 210 * MS, False)
        server.set_time(210, True)
        assert (await source.read_snapshot(["Engine"], 2)).consistent is True
        # An answer that would be too large says so (fewer peers may fit).
        server.max_snapshot_bytes = 10
        with pytest.raises(SourceError) as e:
            await source.read_snapshot(["Engine", "Body"], 2)
        assert e.value.kind == "too-large"
        server.max_snapshot_bytes = 4 * 1024 * 1024
        server.set_time(220, False, running=False, ended=True)
        assert (await source.read_snapshot(["Engine"], 2)).consistent is False
        await source.close()
        await source.close()
        assert not source._thread.is_alive()
        with pytest.raises(SourceError) as e:
            await source.read_snapshot(["Engine"], 1)
        assert e.value.kind == "unavailable"
    asyncio.run(main())


def test_the_availability_of_the_variables_comes_after_the_peers(fake) -> None:
    server = fake(availability={"Engine": {"engine.speed": ("unknown", "active"), "engine.demand": ("unknown",
                                                                                                    "inactive")}})

    async def main() -> None:
        source = control.GrpcSource(library(), server.address, None)
        sink = Sink()
        source.start(sink)
        await sink.until("synced")
        # The peers declare which variables report and how they start; the states now come after the peers, for
        # Engine only (Body reports none), before the word that this is all.
        engine = sink.peers["Engine"]
        assert [v["availability"] for v in engine.variables] == [None, "unknown", "unknown", None]
        assert {v["availability"] for v in sink.peers["Body"].variables} == {None}
        kinds = sink.kinds()
        assert kinds.index("availability") > kinds.index("peers") and kinds[-1] == "synced"
        assert [c[1:] for c in sink.calls if c[0] == "availability"] == \
            [("Engine", "1", ["active", "unknown", "unknown", "active"])]
        del sink.calls[:]
        server.step(0)  # sets the states its availability names
        assert (await sink.until("availability"))[1:] == ("Engine", "1", ["active", "inactive", "active", "active"])
        await source.close()
    asyncio.run(main())


def test_a_control_server_without_the_snapshot_is_not_used(fake) -> None:
    server = fake(snapshot=False)

    async def main() -> None:
        source = control.GrpcSource(library(), server.address, None)
        sink = Sink()
        source.start(sink)
        await sink.until("synced")
        with pytest.raises(SourceError) as e:
            await source.read_snapshot(["Engine"], 2.0)
        assert e.value.kind == "refused" and "UNIMPLEMENTED" in str(e.value)
        await source.close()
        assert not source._thread.is_alive()
    asyncio.run(main())


def test_a_wrong_token_is_refused_for_good_and_never_shown(fake) -> None:
    server = fake(token=TOKEN + "-other")

    async def main() -> None:
        source = control.GrpcSource(library(), server.address, TOKEN)
        sink = Sink()
        source.start(sink)
        _, reason, fatal = await sink.until("lost")
        assert fatal and "refused this GUI server (UNAUTHENTICATED" in reason and TOKEN not in reason
        with pytest.raises(SourceError) as e:
            await source.write([("Engine", "engine.demand", 1.0)], None, "reject", 2)
        assert e.value.kind == "refused" and TOKEN not in str(e.value)
        assert source._thread_done is not None and source._thread is not None
        await asyncio.wait_for(source._thread_done.wait(), timeout=scaled(2.0))
        await asyncio.to_thread(source._thread.join, scaled(2.0))
        assert server.calls.count("Watch") == 1 and not source._thread.is_alive()  # it does not try again
        await source.close()
    asyncio.run(main())


def test_a_credential_that_may_only_read_cannot_write(fake) -> None:
    server = fake(token=TOKEN, role="viewer")

    async def main() -> None:
        source = control.GrpcSource(library(), server.address, TOKEN)
        sink = Sink()
        source.start(sink)
        await sink.until("synced")
        with pytest.raises(SourceError) as e:
            await source.write([("Engine", "engine.demand", 1.0)], None, "reject", 2)
        assert e.value.kind == "refused" and "PERMISSION_DENIED" in str(e.value) and TOKEN not in str(e.value)
        await source.close()
    asyncio.run(main())


def test_what_the_control_server_says_is_turned_into_the_servers_words(control_env) -> None:
    lib = library()
    pb = lib.pb
    source = control.GrpcSource(lib, "127.0.0.1:1", None)
    statuses = ["sent", "queued", "disconnected", "applied", "late", "rejected", "cancelled", "invalid", "busy",
                "unknown"]
    targets = [pb.TargetResult(peer=name, status=getattr(pb, "TARGET_STATUS_" + name.upper())) for name in statuses]
    targets[3].applied_at_ns = 5 * MS
    targets[4].late, targets[5].reason = True, "a setter threw"
    targets[3].applied_writes = 2
    targets[5].applied_writes, targets[5].failed_write = 0, 0  # 0 is told apart from not set
    targets.append(pb.TargetResult(peer="none"))  # unspecified
    targets.append(pb.TargetResult(peer="later", status=99))  # a status of a newer server
    op = source._operation(pb.Operation(seq=7, targets=targets, done=True))
    assert (op.seq, op.done) == (7, True)
    assert [t.status for t in op.targets] == statuses + ["unknown", "unknown"]
    assert [t.applied_at_ns for t in op.targets[:5]] == [None, None, None, 5 * MS, None]
    assert op.targets[4].late and op.targets[5].reason == "a setter threw"
    assert [(t.applied_writes, t.failed_write) for t in op.targets[2:6]] == [(None, None), (2, None), (None, None), (0, 0)]
    peer = source._peer(pb.PeerInfo(participant="P", incarnation="i", connected=False, schema=[
        pb.VariableInfo(name="a", type=pb.VALUE_TYPE_BOOL, causality=pb.CAUSALITY_INPUT, builtin=True),
        pb.VariableInfo(name="b", type=pb.VALUE_TYPE_INTEGER, causality=pb.CAUSALITY_OUTPUT, min=-1, max=2, step=0.5,
                        builtin=False),
        pb.VariableInfo(name="c", type=pb.VALUE_TYPE_REAL, causality=pb.CAUSALITY_PARAMETER, unit="V"),
        pb.VariableInfo(name="d", type=pb.VALUE_TYPE_ENUM, items=[pb.EnumItem(name="x", value=-3)]),
        pb.VariableInfo(name="e", type=77, causality=55),  # of a newer server: no type, read-only
    ]))
    assert (peer.name, peer.incarnation, peer.connected) == ("P", "i", False)
    assert [(v["type"], v["role"], v["builtin"]) for v in peer.variables] == [
        ("bool", "input", True), ("integer", "output", False), ("real", "parameter", False),
        ("enum", "output", False), ("", "output", False)]
    assert (peer.variables[1]["min"], peer.variables[1]["max"], peer.variables[1]["step"]) == (-1, 2, 0.5)
    assert (peer.variables[0]["min"], peer.variables[2]["unit"], peer.variables[3]["enum"]) == (None, "V", [("x", -3)])
    snapshot = pb.Snapshot(time_ns=9, sequence=4, values=[
        pb.Value(b=True), pb.Value(i=-5), pb.Value(d=float("nan")), pb.Value(i=-3), pb.Value(d=float("inf")),
        pb.Value(d=1.5), pb.Value()])
    sample = control._Snapshot("i", snapshot)
    assert (sample.time_ns, sample.sequence, sample.count) == (9, 4, 7)
    assert sample.decode(["bool", "integer", "real", "enum", "", "", ""]) == [True, -5, None, -3, None, 1.5, None]
    assert control._time(pb.TimeState(now_ns=3, paused=True, running=False, ended=True)) == TimeInfo(3, True, False, True)


def test_a_server_that_takes_the_call_and_says_nothing_is_given_up_and_tried_again(fake, monkeypatch) -> None:
    server = fake()
    monkeypatch.setattr(control, "SYNC_TIMEOUT_S", 0.3)

    async def main() -> None:
        with server.changed:  # the server's Watch waits for this lock: it sends nothing
            source = control.GrpcSource(library(), server.address, None)
            sink = Sink()
            source.start(sink)
            _, reason, fatal = await sink.until("lost")
            assert not fatal and reason.endswith("did not send its state in 0.3 s")
        await sink.until("synced")  # it answers again: the next try gets through
        await source.close()
    asyncio.run(main())


def test_a_server_that_is_not_there_yet_is_tried_again_and_one_that_goes_is_noticed(fake, control_env) -> None:
    import fake_control
    port = free_port()  # nothing there yet; the server starts there later

    async def main() -> None:
        source = control.GrpcSource(library(), f"127.0.0.1:{port}", None)
        sink = Sink()
        source.start(sink)
        _, reason, fatal = await sink.until("lost")
        assert not fatal and reason == f"the control server at 127.0.0.1:{port} cannot be reached"
        with pytest.raises(SourceError) as e:
            await source.read_snapshot(["Engine"], 0.5)
        assert e.value.kind in ("unavailable", "timeout")
        server = fake_control.FakeControl({"Engine": variables("Engine")})
        server.start(f"127.0.0.1:{port}")
        try:
            await sink.until("synced")
            del sink.calls[:]
            started = time.monotonic()
            server.stop()
            await sink.until("lost")
            assert time.monotonic() - started < 3
            # the loop stays free while it tries again
            for _ in range(20):
                before = time.monotonic()
                await asyncio.sleep(0.01)
                assert time.monotonic() - before < 0.2
        finally:
            server.stop()
            await source.close()
        assert not source._thread.is_alive()
    asyncio.run(main())


def test_a_call_that_gets_no_answer_ends_in_its_time_and_does_not_hold_the_loop(fake) -> None:
    server = fake()

    async def main() -> None:
        source = control.GrpcSource(library(), server.address, None)
        sink = Sink()
        source.start(sink)
        await sink.until("synced")
        with server.changed:  # the server's calls wait for this lock: no answer
            started = time.monotonic()
            call = asyncio.create_task(source.read_snapshot(["Engine"], 0.3))
            worst = 0.0
            while not call.done():
                before = time.monotonic()
                await asyncio.sleep(0.005)
                worst = max(worst, time.monotonic() - before)
            with pytest.raises(SourceError) as e:
                call.result()
            assert e.value.kind == "timeout" and 0.2 < time.monotonic() - started < 2 and worst < 0.2
        await source.close()
    asyncio.run(main())


def test_the_credentials_file_is_private_and_holds_one_operator(tmp_path) -> None:
    path, token = control.credentials_file()
    try:
        assert path.read_text() == f"operator vehicle-sil-gui {token}\n" and len(token) >= 32
        if os.name == "posix":
            assert stat.S_IMODE(path.stat().st_mode) == 0o600
        other, other_token = control.credentials_file()
        assert other != path and other_token != token
        control.remove(other)
    finally:
        control.remove(path)
        control.remove(path)  # twice is fine
    assert not path.exists() and not other.exists()


@pytest.mark.parametrize("address", ["127.0.0.1:0", "127.0.0.1:65536", "localhost:5", "control.example:8510",
                                     "127.0.0.1:80@evil.example", "unix:/tmp/x", "[::1]:5", "", None, 5,
                                     "127.0.0.1:5\n", "10.0.0.1:5"])
def test_only_this_computers_address_is_connected_to(address) -> None:
    with pytest.raises(ValueError):
        control.check_address(address)
    assert control.check_address("127.0.0.1:34089") == "127.0.0.1:34089"


def test_close_wakes_on_reader_exit_without_sleep_polling(fake, monkeypatch) -> None:
    server = fake(token=TOKEN)
    sleeps: list[float] = []
    real_sleep = asyncio.sleep

    async def spy_sleep(delay, *args, **kwargs):
        sleeps.append(delay)
        return await real_sleep(delay, *args, **kwargs)

    async def main() -> None:
        source = control.GrpcSource(library(), server.address, TOKEN)
        sink = Sink()
        source.start(sink)
        await sink.until("synced")
        monkeypatch.setattr(asyncio, "sleep", spy_sleep)
        began = time.monotonic()
        await source.close()
        elapsed = time.monotonic() - began
        assert sleeps == [], f"close() should wake via event, not sleep-poll: {sleeps}"
        assert not source._thread.is_alive()
        assert elapsed < scaled(1.0)
    asyncio.run(main())


def test_close_respects_close_wait_s_when_reader_is_stuck(fake, monkeypatch) -> None:
    server = fake(token=TOKEN)
    gate = threading.Event()
    monkeypatch.setattr(control, "CLOSE_WAIT_S", 0.05)

    async def main() -> None:
        source = control.GrpcSource(library(), server.address, TOKEN)
        monkeypatch.setattr(source, "_read_loop", lambda: gate.wait(5.0))
        sink = Sink()
        source.start(sink)
        began = time.monotonic()
        await source.close()
        elapsed = time.monotonic() - began
        assert 0.04 <= elapsed < scaled(1.0)
        gate.set()
        source._thread.join(timeout=2.0)
        assert not source._thread.is_alive()
    asyncio.run(main())


def test_close_while_reader_waits_for_taken_has_no_lost_wakeup(fake, monkeypatch) -> None:
    """Closing while _read_loop transitions from _follow() into the _taken wait wakes immediately under _cond."""
    server = fake(token=TOKEN)

    async def main() -> None:
        # 1) close() arrives while _read_loop is blocked waiting for _taken_flag (simulating a delayed _take()).
        source = control.GrpcSource(library(), server.address, TOKEN)
        sink = Sink()
        take_called = asyncio.Event()

        def hold_take() -> None:
            # Take pending state without setting _taken_flag so _read_loop stays parked in _cond.wait_for.
            with source._cond:
                source._scheduled = False
                source._lost = None
            take_called.set()

        monkeypatch.setattr(source, "_follow", lambda call, watchdog: (_ for _ in ()).throw(RuntimeError("drop")))
        monkeypatch.setattr(source, "_take", hold_take)
        source.start(sink)
        await asyncio.wait_for(take_called.wait(), timeout=scaled(2.0))
        began = time.monotonic()
        await source.close()
        elapsed = time.monotonic() - began
        assert not source._thread.is_alive()
        assert elapsed < scaled(0.2)

        # 2) close() arrives right after _follow() returns, before _read_loop acquires _cond to clear _taken_flag.
        source2 = control.GrpcSource(library(), server.address, TOKEN)
        after_follow = asyncio.Event()
        proceed = threading.Event()
        loop = asyncio.get_running_loop()

        def gated_follow(call, watchdog) -> bool:
            loop.call_soon_threadsafe(after_follow.set)
            proceed.wait(timeout=5.0)
            return False

        monkeypatch.setattr(source2, "_follow", gated_follow)
        source2.start(Sink())
        await asyncio.wait_for(after_follow.wait(), timeout=scaled(2.0))
        close_task = asyncio.create_task(source2.close())
        while not source2._closed:
            await asyncio.sleep(0)
        began = time.monotonic()
        proceed.set()
        await asyncio.wait_for(close_task, timeout=scaled(2.0))
        elapsed = time.monotonic() - began
        assert not source2._thread.is_alive()
        assert elapsed < scaled(0.2)
    asyncio.run(main())


def test_close_from_other_loop_wakes_without_sleep_polling_or_blocking_loop(fake, monkeypatch) -> None:
    """Closing a GrpcSource on a different event loop uses wrap_future/to_thread with zero asyncio.sleep polling
    and without blocking the caller's event loop during thread.join()."""
    server = fake(token=TOKEN)
    source = control.GrpcSource(library(), server.address, TOKEN)

    async def start_on_loop_a() -> None:
        sink = Sink()
        source.start(sink)
        await sink.until("synced")

    # Start on loop A, then close loop A while the reader thread is still running.
    asyncio.run(start_on_loop_a())
    assert source._thread is not None and source._thread.is_alive()

    # Add a brief delay after _notify_thread_done() so close() must run thread.join() while loop B stays responsive.
    real_notify = source._notify_thread_done

    def slow_os_exit() -> None:
        real_notify()
        time.sleep(0.08)

    monkeypatch.setattr(source, "_notify_thread_done", slow_os_exit)

    async def close_on_loop_b() -> None:
        sleeps: list[float] = []
        real_sleep = asyncio.sleep
        ticks = 0
        worst_gap = 0.0

        async def ticker() -> None:
            nonlocal ticks, worst_gap
            while True:
                before = time.monotonic()
                await real_sleep(0.005)
                worst_gap = max(worst_gap, time.monotonic() - before)
                ticks += 1

        async def spy_sleep(delay, *args, **kwargs):
            sleeps.append(delay)
            return await real_sleep(delay, *args, **kwargs)

        probe = asyncio.create_task(ticker())
        monkeypatch.setattr(asyncio, "sleep", spy_sleep)
        try:
            began = time.monotonic()
            await source.close()
            elapsed = time.monotonic() - began
        finally:
            monkeypatch.setattr(asyncio, "sleep", real_sleep)
            probe.cancel()
            await asyncio.gather(probe, return_exceptions=True)

        assert sleeps == [], f"cross-loop close() must not sleep-poll: {sleeps}"
        assert not source._thread.is_alive()
        assert elapsed < scaled(1.0)
        assert ticks >= 2 and worst_gap < scaled(0.07), (ticks, worst_gap)

    asyncio.run(close_on_loop_b())
