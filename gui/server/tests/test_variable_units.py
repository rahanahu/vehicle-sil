"""variables.Variables and the hub's deferred messages, with a source that the test scripts (no gRPC, no process)."""

from __future__ import annotations

import asyncio
import json
import math

import pytest
from vehicle_sil_gui import variables as variables_mod
from vehicle_sil_gui.hub import Client, Hub, Outgoing
from vehicle_sil_gui.source import ListSample, OperationInfo, PeerInfo, SnapshotInfo, SourceError, TargetInfo, TimeInfo
from vehicle_sil_gui.variables import Limits, Rejected, Variables, Watch, check_watch

MS = 1_000_000
SCHEMA = [
    {"name": "throttle", "type": "real", "role": "input", "unit": "%", "min": 0, "max": 100, "step": None,
     "description": "", "enum": []},
    {"name": "rpm", "type": "integer", "role": "output", "unit": "rpm", "min": None, "max": None, "step": None,
     "description": "", "enum": []},
    {"name": "gear", "type": "enum", "role": "input", "unit": "", "min": None, "max": None, "step": None,
     "description": "", "enum": [("P", 0), ("D", 3)]},
    {"name": "lamp", "type": "bool", "role": "input", "unit": "", "min": None, "max": None, "step": None,
     "description": "", "enum": []},
    {"name": "count", "type": "integer", "role": "parameter", "unit": "", "min": -5, "max": 5, "step": None,
     "description": "", "enum": []},
    {"name": "free", "type": "real", "role": "input", "unit": "", "min": None, "max": None, "step": None,
     "description": "", "enum": []},
]


def running(ms: int) -> TimeInfo:
    return TimeInfo(ms * MS, False, True, False)


def standing(ms: int) -> TimeInfo:
    return TimeInfo(ms * MS, True, True, False)


def sample(ms: int, sequence: int, rpm: int = 0) -> ListSample:
    return ListSample("a", ms * MS, sequence, [0.0, rpm, 0, False, 0, 0.0])


BODY = PeerInfo("Body", "b", True, SCHEMA[:1])


def body(ms: int, sequence: int) -> ListSample:
    return ListSample("b", ms * MS, sequence, [float(sequence)])


class Scripted:
    """A source whose answers the test sets: a read gives `time` and, of `values`, the peers asked for, with
    `consistent` as the control server decides it (standing still, not ended, no sample after the time). `room`:
    how many samples one answer holds (more: too-large). With `gate`, each read waits until the test lets it go."""

    def __init__(self):
        self.sink = None
        self.time = running(0)
        self.values: dict[str, ListSample] = {}
        self.operations: dict[int, OperationInfo] = {}
        self.peers: list[PeerInfo] = []
        self.written: list = []
        self.write_error: SourceError | None = None
        self.write_gate: asyncio.Event | None = None
        self.wait_queued_calls: list[tuple[int, float]] = []
        self.wait_queued_gates: dict[int, asyncio.Event] = {}
        self.wait_queued_errors: dict[int, SourceError] = {}
        self.wait_queued_answers: dict[int, OperationInfo | None] = {}
        self.gate: asyncio.Event | None = None
        self.read_error: Exception | None = None
        self.room: int | None = None
        self.reads: list[list[str]] = []
        self.closed = False

    def start(self, sink) -> None:
        self.sink = sink

    async def read_snapshot(self, peers: list[str], timeout_s: float) -> SnapshotInfo:
        self.reads.append(list(peers))
        if self.gate is not None:
            await self.gate.wait()
        if self.read_error is not None:
            raise self.read_error
        time = self.time
        samples = {name: self.values.get(name) for name in peers}
        if self.room is not None and sum(s is not None for s in samples.values()) > self.room:
            raise SourceError("too-large", "RESOURCE_EXHAUSTED: ask for fewer peers")
        newer = any(s is not None and s.time_ns > time.now_ns for s in samples.values())
        return SnapshotInfo(time, samples, time.paused and not time.ended and not newer)

    async def read_operation(self, seq: int, timeout_s: float):
        return self.operations.get(seq)

    async def wait_queued(self, seq: int, timeout_s: float) -> OperationInfo | None:
        self.wait_queued_calls.append((seq, timeout_s))
        gate = self.wait_queued_gates.get(seq)
        if gate is not None:
            try:
                async with asyncio.timeout(timeout_s):
                    await gate.wait()
            except TimeoutError:
                raise SourceError("timeout", "DEADLINE_EXCEEDED: WaitOperation timed out") from None
        if seq in self.wait_queued_errors:
            raise self.wait_queued_errors[seq]
        if seq in self.wait_queued_answers:
            return self.wait_queued_answers[seq]
        if seq in self.operations:
            return self.operations[seq]
        writes, _, _ = self.written[seq - 1]
        peers = dict.fromkeys(w[0] for w in writes)
        return OperationInfo(seq, False, [TargetInfo(peer, "queued") for peer in peers])

    async def read_peers(self, timeout_s: float):
        return list(self.peers)

    async def write(self, writes, at_ns, late, timeout_s: float) -> int:
        if self.write_gate is not None:
            await self.write_gate.wait()
        if self.write_error is not None:
            raise self.write_error
        self.written.append((writes, at_ns, late))
        return len(self.written)

    async def close(self) -> None:
        self.closed = True


def make(limits: Limits | None = None, ending=lambda: False, secret: str | None = None):
    sent: list[dict] = []
    v = Variables(7, sent.extend, limits=limits, ending=ending, secret=secret)
    v.expect()
    source = Scripted()
    v.connect(source)
    v.on_peers([PeerInfo("Engine", "a", True, SCHEMA)], [])
    return v, source, sent


def message(v: Variables, wanted=None) -> dict:
    return v.values_message(Watch(1, wanted, 0.02, 0.0, lambda w: None))


async def settle() -> None:
    for _ in range(20):
        await asyncio.sleep(0)


def install_fake_clock(monkeypatch, start: float = 100.0):
    """Advances the loop time seen by vehicle_sil_gui.variables without affecting asyncio's real scheduler."""
    now = [start]

    class _LoopProxy:
        def __init__(self, real):
            self._real = real

        def time(self) -> float:
            return now[0]

        def __getattr__(self, name: str):
            return getattr(self._real, name)

    class _AsyncioProxy:
        def get_running_loop(self):
            return _LoopProxy(asyncio.get_running_loop())

        def __getattr__(self, name: str):
            return getattr(asyncio, name)

    monkeypatch.setattr(variables_mod, "asyncio", _AsyncioProxy())

    def advance(seconds: float) -> None:
        now[0] += seconds

    return advance


# ---------------------------------------------------------------- paused, sim_ms and the values belong together


def test_paused_is_what_one_read_says_of_the_time_and_the_values(monkeypatch) -> None:
    advance = install_fake_clock(monkeypatch)

    async def main() -> None:
        v, source, _ = make()
        v.on_time(running(90))
        v.on_synced()
        assert message(v) == {"type": "values", "id": 1, "watch_id": 1, "run_id": "7", "sim_ms": 90, "paused": False, "peers": {}}
        # While it runs: the values with the time they were read with (they are not older than it).
        source.time, source.values["Engine"] = running(80), sample(80, 8, rpm=80)
        await v.refresh({"Engine"})
        m = message(v)
        assert (m["paused"], m["sim_ms"], m["peers"]["Engine"]["sample_ms"]) == (False, 80, 80)
        # It stands still at 100. The news of that says nothing of the values; the read does, in one call.
        source.time, source.values["Engine"] = standing(100), sample(90, 9, rpm=90)
        v.on_time(standing(100))
        assert message(v)["paused"] is False and message(v)["sim_ms"] == 80
        del source.reads[:]
        await v.refresh({"Engine"})
        m = message(v)
        assert (m["paused"], m["sim_ms"]) == (True, 100) and source.reads == [["Engine"]]
        assert m["peers"]["Engine"] == {"sample_ms": 90, "values": {"throttle": 0.0, "rpm": 90, "gear": 0,
                                                                    "lamp": False, "count": 0, "free": 0.0}}
        # The step at 100 itself comes while it stands still: a second answer for the same time, with other
        # values. It is not asked for at every period (nothing else can come), but it is found, and it is news.
        watch = Watch(1, None, 0.02, 0.0, lambda w: None)
        v.values_message(watch)
        source.values["Engine"] = sample(100, 10, rpm=100)
        await v.refresh({"Engine"})
        assert source.reads == [["Engine"]] and v.stamp(watch) == watch.sent
        advance(variables_mod.STANDING_READ_S + 0.05)
        await v.refresh({"Engine"})
        m = message(v)
        assert (m["paused"], m["sim_ms"], m["peers"]["Engine"]["sample_ms"]) == (True, 100, 100)
        assert v.stamp(watch) != watch.sent
        # Standing still on, read again: the same message, so nothing to send.
        v.values_message(watch)
        v.on_time(TimeInfo(100 * MS, True, True, False))  # told again: no news
        advance(variables_mod.STANDING_READ_S + 0.05)
        await v.refresh({"Engine"})
        assert v.stamp(watch) == watch.sent and len(source.reads) == 3
        # A sample that comes by itself is of no read: nothing is said of its moment until the peer is read.
        v.on_samples({"Engine": sample(100, 11, rpm=101)})
        assert message(v)["paused"] is False and message(v)["peers"]["Engine"]["values"]["rpm"] == 101
        source.values["Engine"] = sample(100, 11, rpm=101)
        await v.refresh({"Engine"})  # not put off: the peer does not stand as read
        assert message(v)["paused"] is True and len(source.reads) == 4
        await v.close()
        assert message(v)["paused"] is False  # after the run: its last values
    asyncio.run(main())


def test_a_read_that_the_simulation_went_on_during_is_not_called_paused() -> None:
    async def main() -> None:
        v, source, _ = make()
        v.on_synced()
        # The time was read standing at 100, then it was stepped on before the values were read.
        source.time, source.values["Engine"] = standing(100), sample(110, 11)
        await v.refresh()
        m = message(v)
        assert (m["paused"], m["sim_ms"], m["peers"]["Engine"]["sample_ms"]) == (False, 100, 110)
        source.time = standing(110)
        await v.refresh()
        assert (message(v)["paused"], message(v)["sim_ms"]) == (True, 110)
        # Ended: its last values, not a standstill.
        source.time = TimeInfo(110 * MS, True, False, True)
        v.on_time(source.time)
        await settle()
        assert (message(v)["paused"], message(v)["sim_ms"]) == (False, 110)
        await v.close()
    asyncio.run(main())


def test_peers_read_by_different_calls_are_paused_together_only_at_one_time(monkeypatch) -> None:
    advance = install_fake_clock(monkeypatch)

    async def main() -> None:
        v, source, _ = make()
        v.on_peers([BODY], [])
        v.on_synced()
        engine, both = [("Engine", "rpm")], [("Engine", "rpm"), ("Body", "throttle")]
        source.time = standing(100)
        source.values = {"Engine": sample(90, 9), "Body": body(90, 9)}
        await v.refresh({"Engine"})
        assert message(v, engine)["paused"] is True
        assert (message(v, both)["paused"], message(v, both)["sim_ms"]) == (False, 100)  # Body was never read
        await v.refresh({"Body"})
        # Each was read standing at 100, by its own call: every step before 100 is in both.
        assert (message(v, both)["paused"], message(v, both)["sim_ms"]) == (True, 100)
        # A step to 110, and only the engine is read there: the body's values are still those of 100.
        source.time = standing(110)
        source.values = {"Engine": sample(100, 10), "Body": body(100, 10)}
        v.on_time(standing(110))
        await v.refresh({"Engine"})
        assert (message(v, engine)["paused"], message(v, engine)["sim_ms"]) == (True, 110)
        m = message(v, both)
        assert (m["paused"], m["sim_ms"], m["peers"]["Body"]["sample_ms"]) == (False, 100, 90)
        assert message(v, [("Body", "throttle")])["paused"] is False  # read at 100, and it does not stand there now
        del source.reads[:]
        await v.refresh({"Body"})  # not put off, although the engine was read just now
        assert source.reads == [["Body"]] and (message(v, both)["paused"], message(v, both)["sim_ms"]) == (True, 110)
        # A peer that is another process than the one known here: its sample is not taken, and nothing is said.
        source.values["Body"] = ListSample("c", 100 * MS, 1, [5.0])
        advance(variables_mod.STANDING_READ_S + 0.05)
        await v.refresh({"Body", "Engine"})
        assert message(v, engine)["paused"] is True and message(v, both)["paused"] is False
        assert message(v, both)["peers"]["Body"]["sample_ms"] == 100
        await v.close()
    asyncio.run(main())


def test_a_command_passed_on_ends_paused_until_it_is_read_again() -> None:
    async def main() -> None:
        v, source, _ = make()
        source.time = standing(100)
        v.on_synced()
        await v.refresh()
        watch = Watch(1, None, 0.02, 0.0, lambda w: None)
        assert v.values_message(watch)["paused"] is True
        # A step was written to the launcher, which has not taken it yet: the source still says standing.
        del source.reads[:]
        v.unsettle()
        assert message(v)["paused"] is False
        await v.refresh()  # read at once, not put off
        assert source.reads == [["Engine"]] and message(v)["paused"] is True and v.stamp(watch) == watch.sent
        # The news of the time makes the next read find the step, without waiting.
        source.time, source.values["Engine"] = standing(110), sample(100, 10)
        v.on_time(standing(110))
        assert message(v)["sim_ms"] == 100
        await v.refresh()
        assert (message(v)["paused"], message(v)["sim_ms"]) == (True, 110) and len(source.reads) == 2
        # A resume: it runs.
        v.unsettle()
        source.time, source.values["Engine"] = running(130), sample(130, 13)
        await v.refresh()
        assert (message(v)["paused"], message(v)["sim_ms"]) == (False, 130)
        await v.close()
    asyncio.run(main())


def test_a_read_that_fails_changes_nothing_and_one_read_is_made_at_a_time() -> None:
    async def main() -> None:
        v, source, sent = make()
        source.time, source.values["Engine"] = standing(100), sample(90, 9)
        v.on_synced()
        await v.refresh()
        before = message(v)
        assert before["paused"] is True
        source.time, source.values["Engine"] = standing(110), sample(100, 10)
        for error in (SourceError("timeout", "DEADLINE_EXCEEDED: no answer"), RuntimeError("a bug in a source")):
            source.read_error = error
            v.on_time(running(100) if isinstance(error, SourceError) else standing(100))  # news; maybe old
            await v.refresh()
            assert message(v) == before
        source.read_error = None
        # A second read waits for the first, and what they give is taken in that order.
        source.gate = asyncio.Event()
        v.unsettle()
        del source.reads[:]
        first, second = asyncio.create_task(v.refresh()), asyncio.create_task(v.refresh())
        await settle()
        assert source.reads == [["Engine"]]
        source.gate.set()
        await asyncio.gather(first, second)
        assert (message(v)["paused"], message(v)["sim_ms"]) == (True, 110) and len(source.reads) == 2
        v.on_lost("the control server at 127.0.0.1:1 cannot be reached", False)
        assert v.state == "connecting" and not v.has_values and message(v)["paused"] is False
        assert sent[-1]["variables"]["reason"].endswith("connecting again")
        assert sent[-1]["variables"]["peers"][0]["connected"] is False
        v.on_peers([PeerInfo("Engine", "a", True, SCHEMA)], [])
        v.on_synced()
        assert v.state == "ready" and message(v)["paused"] is False  # read before the connection was lost
        await v.refresh()
        assert message(v)["paused"] is True
        await v.close()
    asyncio.run(main())


def test_news_that_the_time_went_on_ends_paused_even_when_no_read_works() -> None:
    async def main() -> None:
        v, source, sent = make()
        source.time, source.values["Engine"] = standing(100), sample(90, 9)
        v.on_synced()
        await v.refresh()
        watch = Watch(1, None, 0.02, 0.0, lambda w: None)
        assert v.values_message(watch)["paused"] is True
        # Another client resumed it, and the control server does not answer the reads.
        source.read_error = SourceError("timeout", "DEADLINE_EXCEEDED: no answer")
        v.on_time(running(100))  # may be news from before the standstill: says nothing
        assert v.stamp(watch) == watch.sent
        v.on_time(running(110))  # past the standstill: that cannot be old news
        assert v.stamp(watch) != watch.sent
        m = message(v)
        assert (m["paused"], m["sim_ms"], m["peers"]["Engine"]["sample_ms"]) == (False, 100, 90)
        # The reads that keep failing are told, with the state still ready (the rest works).
        del sent[:]
        for n in range(3):
            assert sent == []
            v.on_time(running(120 + n))
            await v.refresh()
        assert [(m["type"], m["variables"]["state"]) for m in sent] == [("variables", "ready")]
        assert sent[0]["variables"]["reason"].startswith(
            "the values cannot be read from the control server (timeout: DEADLINE_EXCEEDED: no answer)")
        v.on_time(running(130))
        await v.refresh()
        assert len(sent) == 1
        source.read_error = None
        source.time = standing(140)
        v.on_time(standing(140))
        await v.refresh()
        assert (sent[-1]["variables"]["state"], sent[-1]["variables"]["reason"]) == ("ready", None) and len(sent) == 2
        assert (message(v)["paused"], message(v)["sim_ms"]) == (True, 140)
        # The end of the run, told while it stood: not paused, read or not.
        source.read_error = SourceError("unavailable", "UNAVAILABLE: gone")
        v.on_time(TimeInfo(140 * MS, True, False, True))
        assert message(v)["paused"] is False
        await v.close()
    asyncio.run(main())


def test_an_answer_from_before_a_command_does_not_say_paused_after_it(monkeypatch) -> None:
    advance = install_fake_clock(monkeypatch)

    async def main() -> None:
        v, source, _ = make()
        source.time = standing(100)
        v.on_synced()
        await v.refresh()
        assert message(v)["paused"] is True
        advance(variables_mod.STANDING_READ_S + 0.05)
        source.gate = asyncio.Event()
        reading = asyncio.create_task(v.refresh())
        await settle()
        assert len(source.reads) == 2
        v.unsettle()  # a step is passed on while the read is on its way
        source.gate.set()
        await reading
        assert message(v)["paused"] is False
        await v.refresh()  # asked after the command: it stands (the launcher has not taken the step yet)
        assert message(v)["paused"] is True and len(source.reads) == 3
        await v.close()
    asyncio.run(main())


def test_values_that_do_not_fit_one_answer_are_read_with_as_many_calls_as_it_takes(caplog) -> None:
    async def main() -> None:
        v, source, _ = make()
        cabin = PeerInfo("Cabin", "b", True, SCHEMA[:1])
        v.on_peers([BODY, cabin], [])
        v.on_synced()
        both, cabin_only = [("Body", "throttle"), ("Cabin", "throttle")], [("Cabin", "throttle")]
        source.time = standing(100)
        source.values = {"Engine": sample(90, 9), "Body": body(90, 9), "Cabin": body(90, 4)}
        source.room = 2
        await v.refresh()
        # The call that did not fit is asked again in two halves. Each stands at 100: so do they all.
        m = message(v)
        assert m["peers"].keys() == {"Engine", "Body", "Cabin"} and (m["paused"], m["sim_ms"]) == (True, 100)
        assert source.reads == [["Engine", "Body", "Cabin"], ["Engine"], ["Body", "Cabin"]]
        # The next reads begin with calls of the size that fitted; a small watch is one call, and says paused.
        del source.reads[:]
        v.on_time(standing(100))
        await v.refresh()
        assert source.reads == [["Engine"], ["Body"], ["Cabin"]] and message(v)["paused"] is True
        v.unsettle()
        await v.refresh({"Cabin"})
        assert (message(v, cabin_only)["paused"], message(v)["paused"]) == (True, False)
        await v.refresh()
        assert message(v)["paused"] is True
        # Calls that give different times (it runs) are not of one moment.
        times = iter([running(110), running(120), running(130)])
        answer = source.read_snapshot

        async def moving(peers, timeout_s):
            source.time = next(times)
            return await answer(peers, timeout_s)
        source.read_snapshot = moving
        v.unsettle()
        await v.refresh()
        assert (message(v)["paused"], message(v)["sim_ms"]) == (False, 110)
        source.read_snapshot = answer
        # Other peers or variables: one call is tried again.
        source.room, source.time = None, standing(140)
        v.on_peers([cabin], [])
        del source.reads[:]
        await v.refresh()
        assert source.reads == [["Engine", "Body", "Cabin"]] and (message(v)["paused"], message(v)["sim_ms"]) == (True, 140)
        # A peer whose values no answer holds is left out and not asked for again; the others are read.
        source.room = 0
        source.values = {"Engine": sample(100, 10), "Body": None, "Cabin": None}
        source.time = standing(150)
        v.on_time(standing(150))
        del source.reads[:]
        await v.refresh()
        assert source.reads == [["Engine", "Body", "Cabin"], ["Engine"], ["Body", "Cabin"]]
        assert message(v)["peers"]["Engine"]["sample_ms"] == 90 and message(v)["paused"] is False
        assert message(v, both)["paused"] is False  # their samples here are not what was read (none)
        v.unsettle()
        await v.refresh()
        assert source.reads[3:] == [["Body"], ["Cabin"]]
        assert [r.getMessage() for r in caplog.records if "Engine" in r.getMessage()] == [
            "the values of Engine do not fit one answer of the control server: they are not read"]
        # Until it is another process (or has other variables).
        source.room = None
        v.on_peers([PeerInfo("Engine", "a2", True, SCHEMA)], [])
        del source.reads[:]
        v.unsettle()
        await v.refresh()
        assert source.reads == [["Engine", "Body", "Cabin"]]
        await v.close()
    asyncio.run(main())


def test_after_a_reconnection_gone_peers_are_dropped_and_open_operations_read_again() -> None:
    async def main() -> None:
        v, source, sent = make()
        v.on_peers([PeerInfo("Body", "b", True, SCHEMA[:1])], [])
        v.on_synced()
        v.on_operations([OperationInfo(3, False, [TargetInfo("Engine", "queued")]),
                         OperationInfo(4, True, [TargetInfo("Engine", "applied", 0)])])
        v.on_lost("the control server ended the connection", False)
        # While it was away: the operation was applied, and Body left.
        source.operations[3] = OperationInfo(3, True, [TargetInfo("Engine", "applied", 50 * MS)])
        del sent[:]
        v.on_peers([PeerInfo("Engine", "a", True, SCHEMA)], [])
        v.on_synced()
        await settle()
        assert [p["name"] for p in v.summary()["peers"]] == ["Engine"]
        assert [(m["seq"], m["state"]) for m in sent if m["type"] == "operation"] == [(3, "applied")]
        # Changes were lost on the way: the peers are read again too.
        source.peers = [PeerInfo("Engine", "a", False, SCHEMA), PeerInfo("Cabin", "c", True, SCHEMA[:1])]
        v.on_dropped()
        assert v._reconciling is not None
        await v._reconciling
        assert [(p["name"], p["connected"]) for p in v.summary()["peers"]] == [("Engine", False), ("Cabin", True)]
        await v.close()
    asyncio.run(main())


def test_a_schema_that_changes_under_one_incarnation_drops_the_sample_read_with_the_old_one() -> None:
    async def main() -> None:
        v, source, _ = make()
        v.on_synced()
        v.on_samples({"Engine": sample(10, 1, rpm=7)})
        assert message(v)["peers"]["Engine"]["values"]["rpm"] == 7
        v.on_peers([PeerInfo("Engine", "a", True, SCHEMA)], [])  # the same schema again: the sample stays
        assert message(v)["peers"]["Engine"]["values"]["rpm"] == 7
        v.on_peers([PeerInfo("Engine", "a", True, [SCHEMA[1], SCHEMA[0], *SCHEMA[2:]])], [])
        assert message(v)["peers"] == {}  # not the old values under the new names
        await v.close()
    asyncio.run(main())


# ---------------------------------------------------------------- the states


def test_states_at_the_end_of_a_run() -> None:
    async def main() -> None:
        # ready, then the run ends: ended, the values stay
        v, source, sent = make()
        v.on_samples({"Engine": sample(10, 1, rpm=5)})
        v.on_time(running(10))
        v.on_synced()
        await v.close()
        assert (v.state, v.reason, source.closed) == ("ended", None, True) and v.has_values
        assert message(v)["peers"]["Engine"]["values"]["rpm"] == 5 and message(v)["paused"] is False
        assert [m["variables"]["state"] for m in sent if m["type"] == "variables"] == ["ready", "ended"]
        # the connection ends while the launcher is stopping: the end, not a failure
        ending = [False]
        v, source, sent = make(ending=lambda: ending[0])
        v.on_synced()
        ending[0] = True
        v.on_lost("the control server ended the connection", False)
        assert (v.state, v.reason) == ("ended", None)
        await v.close()
        assert [m["variables"]["state"] for m in sent if m["type"] == "variables"] == ["ready", "ended"]
        # lost in the middle and never back: ended, and the reason says the values may be old
        v, source, sent = make()
        v.on_synced()
        v.on_lost("the control server at 127.0.0.1:1 cannot be reached", False)
        await v.close()
        assert v.state == "ended" and "may be older" in v.reason
        # never read at all
        v, source, sent = make()
        v.on_lost("the control server at 127.0.0.1:1 cannot be reached", False)
        assert (v.state, v.reason) == ("connecting", "the control server at 127.0.0.1:1 cannot be reached: "
                                                     "connecting again")
        await v.close()
        assert (v.state, v.reason) == ("failed", "the control server at 127.0.0.1:1 cannot be reached until the run "
                                                 "ended")
        # refused: failed, stays failed
        v, source, sent = make(secret="s3cr3t-token-of-the-run")
        v.on_lost("the control server refused this GUI server (UNAUTHENTICATED: bad s3cr3t-token-of-the-run)", True)
        assert v.state == "failed" and "s3cr3t" not in v.reason and "<token>" in v.reason
        await v.close()
        assert v.state == "failed" and "s3cr3t" not in json.dumps(sent)
        # the control server never started
        sent = []
        v = Variables(1, sent.extend)
        v.expect()
        await v.close()
        assert (v.state, v.reason) == ("off", "the run ended before its control server started")
        # no variables at all: nothing is told at the end
        sent = []
        v = Variables(1, sent.extend)
        v.set_off("no control server")
        await v.close()
        assert sent == [] and v.summary()["state"] == "off"
    asyncio.run(main())


# ---------------------------------------------------------------- the schema's limits


def test_the_schema_is_kept_up_to_its_limits_and_the_rest_is_counted() -> None:
    async def main() -> None:
        v, source, sent = make(Limits(peers=2, variables=8, enum_items=2, name_chars=10, text_chars=5))
        odd = [dict(SCHEMA[0], name="x" * 11), dict(SCHEMA[0], name="dup"), dict(SCHEMA[0], name="dup"),
               dict(SCHEMA[2], name="many", enum=[("a", 0), ("b", 1), ("c", 2)]),
               dict(SCHEMA[0], name="long", unit="kilometres", description="d" * 9),
               dict(SCHEMA[0], name="nan", min=math.nan, max=math.inf), dict(SCHEMA[0], name="what", type="text")]
        v.on_peers([PeerInfo("Body", "b", True, odd), PeerInfo("Third", "c", True, SCHEMA)], [])
        s = v.summary()
        assert [p["name"] for p in s["peers"]] == ["Engine", "Body"] and s["peers_omitted"] == 1
        body = s["peers"][1]["variables"]
        assert [x["name"] for x in body] == ["dup", "long"]  # 6 of Engine and 2 here: the limit of 8
        assert body[1]["unit"] == "kilom..." and body[1]["description"] == "ddddd..."
        assert s["variables_omitted"] == len(SCHEMA) + len(odd) - 2
        # the values still fit the names that are kept (the source's order has the left-out ones too)
        v.on_synced()
        v.on_samples({"Body": ListSample("b", 0, 1, [1.0, 2.0, 3.0, 0, 5.0, 6.0, 7.0])})
        assert message(v)["peers"]["Body"]["values"] == {"dup": 2.0, "long": 5.0}
        # a sample that does not fit the schema is not taken
        v.on_samples({"Body": ListSample("b", 0, 2, [1.0]), "Engine": ListSample("other", 0, 1, [0] * 6)})
        assert message(v)["peers"].keys() == {"Body"}
        v.on_peers([], ["Body"])
        assert v.summary()["variables_omitted"] == len(SCHEMA) and "Body" not in message(v)["peers"]
        await v.close()
    asyncio.run(main())


def test_a_peer_that_is_another_process_now_loses_its_values(monkeypatch) -> None:
    monkeypatch.setattr(variables_mod, "TOLD_TOGETHER_S", 0.0)

    async def main() -> None:
        sent: list[dict] = []
        second_variables = asyncio.Event()

        def emit(messages: list[dict]) -> None:
            sent.extend(messages)
            if sum(1 for m in sent if m["type"] == "variables") >= 2:
                second_variables.set()

        v = Variables(7, emit)
        v.expect()
        source = Scripted()
        v.connect(source)
        v.on_peers([PeerInfo("Engine", "a", True, SCHEMA)], [])
        v.on_synced()
        v.on_samples({"Engine": sample(10, 5)})
        v.on_peers([PeerInfo("Engine", "b", True, SCHEMA)], [])
        assert message(v)["peers"] == {}
        v.on_samples({"Engine": ListSample("b", 0, 1, [1.0, 1, 0, True, 0, 0.0])})
        assert message(v)["peers"]["Engine"]["values"]["lamp"] is True
        # the changed peer is told once, a little later (peers come one by one)
        v.on_peers([PeerInfo("Engine", "b", False, SCHEMA)], [])
        assert [m["type"] for m in sent] == ["variables"]
        await asyncio.wait_for(second_variables.wait(), timeout=2.0)
        assert [m["type"] for m in sent] == ["variables"] * 2 and sent[-1]["variables"]["peers"][0]["connected"] is False
        await v.close()
    asyncio.run(main())


# ---------------------------------------------------------------- watches


def test_a_watch_message_is_checked() -> None:
    limits = Limits(watch_variables=2)
    assert check_watch({"id": 3, "variables": "all"}, limits) == (3, None, 0.02)
    assert check_watch({"id": 0, "variables": [{"peer": "A", "name": "x"}, {"peer": "A", "name": "x"}],
                        "period_ms": 250}, limits) == (0, [("A", "x")], 0.25)
    for bad in ({"variables": "all"}, {"id": -1, "variables": "all"}, {"id": True, "variables": "all"},
                {"id": 1.5, "variables": "all"}, {"id": 2 ** 53 + 1, "variables": "all"}, {"id": 1},
                {"id": 1, "variables": "everything"}, {"id": 1, "variables": [{"peer": "A"}]},
                {"id": 1, "variables": [["A", "x"]]}, {"id": 1, "variables": {"A": "x"}},
                {"id": 1, "variables": "all", "period_ms": 19}, {"id": 1, "variables": "all", "period_ms": 60_001},
                {"id": 1, "variables": "all", "period_ms": 20.5}, {"id": 1, "variables": "all", "period_ms": True},
                {"id": 1, "variables": "all", "period_ms": 10 ** 400},
                {"id": 1, "variables": [{"peer": "A", "name": str(n)} for n in range(3)]}):
        with pytest.raises(Rejected) as e:
            check_watch(bad, limits)
        assert e.value.code == "invalid-watch" and "send" in str(e.value) or "at most" in str(e.value)


def test_the_stamp_of_a_watch_changes_only_with_what_it_shows() -> None:
    v, source, _ = make()
    v.on_peers([PeerInfo("Body", "b", True, SCHEMA[:1])], [])
    v.on_synced()
    engine = Watch(1, [("Engine", "rpm"), ("Engine", "nothing"), ("Nobody", "x")], 0.02, 0.0, lambda w: None)
    assert engine.sent != v.stamp(engine)
    assert v.values_message(engine)["peers"] == {}
    assert engine.sent == v.stamp(engine)
    v.on_samples({"Body": ListSample("b", 0, 1, [1.0])})
    assert engine.sent == v.stamp(engine)  # another peer's sample
    v.on_samples({"Engine": sample(10, 1, rpm=7)})
    assert engine.sent != v.stamp(engine)
    assert v.values_message(engine)["peers"] == {"Engine": {"sample_ms": 10, "values": {"rpm": 7}}}
    v.on_time(running(20))
    assert engine.sent != v.stamp(engine)  # the time is in the message


def test_a_deferred_message_waits_once_and_is_made_when_it_is_sent() -> None:
    async def main() -> None:
        texts: list[str] = []
        state = {"n": 0}
        rendered = asyncio.Event()
        sent_two = asyncio.Event()

        class Item:
            size = 0

            def render(self):
                rendered.set()
                return None if state["n"] < 0 else json.dumps({"n": state["n"]})

        async def send(text: str) -> None:
            texts.append(text)
            if len(texts) >= 2:
                sent_two.set()
        hub = Hub(lambda session, from_seq: [{"type": "hello"}], limit=10)
        client = hub.client(send)
        item = Item()
        for n in range(1000):  # a screen that does not read: the values change a thousand times
            state["n"] = n
            client.put_deferred(item)
        assert len(client.items) == 1
        task = asyncio.create_task(hub.serve(client, None, None))
        await asyncio.wait_for(sent_two.wait(), timeout=2.0)
        assert texts == ['{"type":"hello"}', '{"n": 999}'] and not client.deferred
        # nothing for an item that has nothing to say any more
        state["n"] = -1
        rendered.clear()
        client.put_deferred(item)
        await asyncio.wait_for(rendered.wait(), timeout=2.0)
        await asyncio.sleep(0)
        assert len(texts) == 2 and not client.deferred
        # a queue that overflows with what cannot be dropped is sent again from the start; the deferred item is
        # not what overflows it, and is forgotten with the rest
        state["n"] = 5
        task.cancel()
        client.put_deferred(item)
        for _ in range(10):
            client.put(Outgoing({"type": "session", "session": {}}))
        assert len(client.items) == 11 and client.resyncs == 0
        client.put(Outgoing({"type": "session", "session": {}}))
        assert client.resyncs == 1 and not client.deferred
        client.put_deferred(item)  # nothing is taken until the resync is sent
        assert len(client.items) == 1
    asyncio.run(main())


# ---------------------------------------------------------------- writes


def ready():
    v, source, sent = make()
    v.on_synced()
    return v, source, sent


@pytest.mark.parametrize("value, expected", [
    ({"throttle": 50}, 50.0), ({"throttle": 0}, 0.0), ({"throttle": 100.0}, 100.0), ({"rpm": 1}, "not-writable"),
    ({"throttle": 100.5}, "invalid-field"), ({"throttle": -1}, "invalid-field"), ({"throttle": "50"}, "invalid-field"),
    ({"throttle": True}, "invalid-field"), ({"throttle": None}, "invalid-field"), ({"throttle": [1]}, "invalid-field"),
    ({"throttle": math.nan}, "invalid-field"), ({"throttle": math.inf}, "invalid-field"),
    ({"throttle": 10 ** 400}, "invalid-field"), ({"free": 10 ** 400}, "invalid-field"), ({"free": -1e308}, -1e308),
    ({"free": 2 ** 70}, float(2 ** 70)), ({"free": -math.inf}, "invalid-field"),
    ({"gear": 3}, 3), ({"gear": "D"}, 3), ({"gear": 1}, "invalid-field"), ({"gear": "R"}, "invalid-field"),
    ({"gear": 3.0}, "invalid-field"), ({"gear": True}, "invalid-field"),
    ({"lamp": True}, True), ({"lamp": 1}, "invalid-field"), ({"lamp": "true"}, "invalid-field"),
    ({"count": 5}, 5), ({"count": -5.0}, -5), ({"count": 6}, "invalid-field"), ({"count": 1.5}, "invalid-field"),
    ({"count": 10 ** 30}, "invalid-field"), ({"count": True}, "invalid-field"), ({"count": 1e300}, "invalid-field"),
    ({"nothing": 1}, "unknown-variable"), ({}, "invalid-field"),
])
def test_a_write_is_checked_against_the_schema(value: dict, expected) -> None:
    v, _, _ = ready()
    try:
        writes, at_ns, late = v.check_write({"writes": {"Engine": value}})
    except Rejected as e:
        assert e.code == expected, str(e)
        assert e.status == 400 and len(str(e)) < 400
        return
    assert writes == [("Engine", next(iter(value)), expected)] and type(writes[0][2]) is type(expected)
    assert (at_ns, late) == (None, "reject")


def test_the_rest_of_a_write_is_checked() -> None:
    v, _, _ = ready()
    ok = {"writes": {"Engine": {"throttle": 1}}}
    assert v.check_write({**ok, "at_ms": 250, "late": "apply"})[1:] == (250 * MS, "apply")
    assert v.check_write({**ok, "at_ms": 10 ** 12})[1] == 10 ** 12 * MS
    for bad, code in (({}, "invalid-field"), ({"writes": []}, "invalid-field"), ({"writes": {}}, "invalid-field"),
                      ({"writes": {"Engine": 1}}, "invalid-field"), ({**ok, "at_ms": -1}, "invalid-field"),
                      ({**ok, "at_ms": 1.5}, "invalid-field"), ({**ok, "at_ms": True}, "invalid-field"),
                      ({**ok, "at_ms": 10 ** 12 + 1}, "invalid-field"), ({**ok, "late": "apply_next"}, "invalid-field"),
                      ({**ok, "late": None}, "invalid-field"), ({**ok, "at": 5}, "invalid-field"),
                      ({"writes": {"Nobody": {"x": 1}}}, "unknown-variable"),
                      ({"writes": {"Engine": {f"v{n}": 1 for n in range(257)}}}, "invalid-field")):
        with pytest.raises(Rejected) as e:
            v.check_write(bad)
        assert e.value.code == code, bad
    v.on_lost("lost", False)
    with pytest.raises(Rejected) as e:
        v.check_write(ok)
    assert (e.value.status, e.value.code) == (409, "variables-not-ready") and "lost" in str(e.value)


def test_writes_go_to_the_source_in_the_order_asked_and_fail_with_a_reason() -> None:
    async def main() -> None:
        v, source, sent = ready()
        source.write_gate = asyncio.Event()
        calls = [asyncio.create_task(v.write([("Engine", "throttle", float(n))], None, "reject")) for n in range(5)]
        await settle()
        source.write_gate.set()
        assert await asyncio.gather(*calls) == [1, 2, 3, 4, 5]
        assert [w[0][0][2] for w in source.written] == [0.0, 1.0, 2.0, 3.0, 4.0]
        # each is told as sent when registered, and is queued before write() returns
        ops = [m for m in sent if m["type"] == "operation"]
        sent_ops = [m for m in ops if m["state"] == "sent"]
        queued_ops = [m for m in ops if m["state"] == "queued"]
        assert [(m["seq"], m["done"]) for m in sent_ops] == [(n, False) for n in range(1, 6)]
        assert [(m["seq"], m["done"]) for m in queued_ops] == [(n, False) for n in range(1, 6)]
        assert sent_ops[0]["peers"] == {"Engine": {"state": "sent", "applied_at_ms": None, "late": False, "reason": None,
                                                   "applied_writes": None, "failed_write": None}}
        assert [v.operations[n]["state"] for n in range(1, 6)] == ["queued"] * 5
        for kind, status, code in (("timeout", 504, "write-timeout"), ("refused", 400, "write-refused"),
                                   ("unavailable", 502, "write-failed")):
            source.write_error = SourceError(kind, "X: why")
            with pytest.raises(Rejected) as e:
                await v.write([("Engine", "throttle", 1.0)], None, "reject")
            assert (e.value.status, e.value.code, e.value.detail) == (status, code, None)
        await v.close()
        with pytest.raises(Rejected) as e:
            await v.write([("Engine", "throttle", 1.0)], None, "reject")
        assert e.value.code == "variables-not-ready"
    asyncio.run(main())


def test_writes_do_not_pile_up_behind_a_source_that_does_not_answer() -> None:
    async def main() -> None:
        v, source, _ = make(Limits(writes_waiting=3, write_timeout_s=0.2))
        v.on_synced()
        source.write_gate = asyncio.Event()
        calls = [asyncio.create_task(v.write([("Engine", "throttle", 1.0)], None, "reject")) for _ in range(6)]
        results = await asyncio.gather(*calls[3:], return_exceptions=True)  # the ones over the limit: at once
        assert [r.code for r in results] == ["write-busy"] * 3
        results = await asyncio.gather(*calls[1:3], return_exceptions=True)  # waited for their turn in vain
        assert [(r.status, r.code) for r in results] == [(503, "write-busy")] * 2
        source.write_gate.set()
        assert await calls[0] == 1
        await v.close()
    asyncio.run(main())


# ---------------------------------------------------------------- operations


def op(seq: int, done: bool, *targets) -> OperationInfo:
    return OperationInfo(seq, done, [TargetInfo(*t) for t in targets])


def test_operations_are_told_once_per_change_and_summed_up() -> None:
    v, source, sent = ready()
    del sent[:]
    v.on_operations([op(1, False, ("Engine", "sent"))])
    v.on_operations([op(1, False, ("Engine", "sent"))])  # the same again: not told twice
    v.on_operations([op(1, False, ("Engine", "queued"), ("Body", "sent"))])
    v.on_operations([op(1, False, ("Engine", "queued"), ("Body", "disconnected"))])
    v.on_operations([op(1, True, ("Engine", "applied", 30 * MS + 5, False, "", 2), ("Body", "applied", 40 * MS, True))])
    assert [m["state"] for m in sent] == ["sent", "sent", "queued", "applied"]
    assert sent[-1] == {"type": "operation", "run_id": "7", "seq": 1, "state": "applied", "done": True, "peers": {
        "Engine": {"state": "applied", "applied_at_ms": 30, "late": False, "reason": None, "applied_writes": 2,
                   "failed_write": None},
        "Body": {"state": "applied", "applied_at_ms": 40, "late": True, "reason": None, "applied_writes": None,
                 "failed_write": None}}}
    for seq, (targets, state) in enumerate(
        (((("A", "late"),), "late"), ((("A", "rejected", None, False, "the setter failed", 1, 1),), "rejected"),
         ((("A", "applied", 0), ("B", "invalid")), "mixed"), ((("A", "busy"), ("B", "busy")), "busy"),
         ((), "unknown")),
        start=9,
    ):
        v.on_operations([op(seq, True, *targets)])
        assert sent[-1]["state"] == state
        if state == "rejected":  # the counts pass through as they came
            assert (sent[-1]["peers"]["A"]["applied_writes"], sent[-1]["peers"]["A"]["failed_write"]) == (1, 1)
    assert v.summary()["operations"] == [{k: x for k, x in m.items() if k not in ("type", "run_id")}
                                         for m in sent[3:]]


def test_operations_are_kept_up_to_a_limit_open_ones_last_and_get_their_end_with_the_run() -> None:
    async def main() -> None:
        v, source, sent = make(Limits(operations=4, operations_in_summary=2))
        v.on_synced()
        v.on_operations([op(1, False, ("Engine", "queued")), op(2, True, ("Engine", "applied", 0)),
                         op(3, True, ("Engine", "applied", 0)), op(4, False, ("Engine", "sent"), ("Body", "applied", 0)),
                         op(5, True, ("Engine", "late")), op(6, True, ("Engine", "late"))])
        assert list(v.operations) == [1, 4, 5, 6]  # the done ones went first
        # the summary: the open ones and the last two; 2 and 3 are not kept, so it is whole from 4 on
        summary = v.summary()
        assert [o["seq"] for o in summary["operations"]] == [1, 4, 5, 6]
        assert (summary["operations_from_seq"], summary["operations_open_omitted"]) == (4, 0)
        # changes were lost on the way: the open ones are read again
        source.operations[1] = op(1, True, ("Engine", "applied", 10 * MS))
        del sent[:]
        v.on_dropped()
        await settle()
        assert [(m["seq"], m["state"]) for m in sent] == [(1, "applied")]
        # the run ends: what is still open will not get a result
        del sent[:]
        await v.close()
        assert sent[0] == {"type": "operation", "run_id": "7", "seq": 4, "state": "mixed", "done": True, "peers": {
            "Engine": {"state": "unknown", "applied_at_ms": None, "late": False,
                       "reason": "the run ended before the result came", "applied_writes": None, "failed_write": None},
            "Body": {"state": "applied", "applied_at_ms": 0, "late": False, "reason": None, "applied_writes": None,
                     "failed_write": None}}}
        assert sent[1]["type"] == "variables" and len(sent) == 2
    asyncio.run(main())


def test_the_summary_has_every_open_operation_and_says_from_which_number_on_it_is_whole() -> None:
    v, source, sent = make()  # 256 kept, the last 32 in the summary
    v.on_synced()
    assert v.summary()["operations"] == [] and v.summary()["operations_from_seq"] == 0
    # a write made while it stands still stays open, and 39 more are made and applied after it
    v.on_operations([op(3, False, ("Engine", "queued"))])
    v.on_operations([op(n, True, ("Engine", "applied", 0)) for n in range(4, 43)])
    summary = v.summary()
    assert [o["seq"] for o in summary["operations"]] == [3, *range(11, 43)]
    assert (summary["operations_from_seq"], summary["operations_open_omitted"]) == (11, 0)
    # once it is done it is one of the old ones: not listed, and below the number
    v.on_operations([op(3, True, ("Engine", "applied", 0))])
    summary = v.summary()
    assert [o["seq"] for o in summary["operations"]] == list(range(11, 43))
    assert summary["operations_from_seq"] == 11
    # an open one with a low number that is first seen late is listed too
    v.on_operations([op(2, False, ("Engine", "queued")), op(43, False, ("Engine", "sent"))])
    summary = v.summary()
    assert [o["seq"] for o in summary["operations"]] == [*range(13, 43), 2, 43]
    assert (summary["operations_from_seq"], summary["operations_open_omitted"]) == (13, 0)
    # with 300 more open ones only open ones are kept, and the oldest 46 of them not any more: the summary
    # says so, and is whole only from the number after them
    v.on_operations([op(n, False, ("Engine", "queued")) for n in range(100, 400)])
    summary = v.summary()
    assert [o["seq"] for o in summary["operations"]] == list(range(144, 400))
    assert (summary["operations_from_seq"], summary["operations_open_omitted"]) == (144, 46)


def test_a_write_taken_while_the_run_ends_gets_its_end() -> None:
    async def main() -> None:
        v, source, sent = ready()
        source.write_gate = asyncio.Event()
        writing = asyncio.create_task(v.write([("Engine", "throttle", 1.0)], None, "reject"))
        await settle()
        closing = asyncio.create_task(v.close())
        await settle()
        del sent[:]
        source.write_gate.set()  # the source took it, but the run is over: no result will come
        assert await writing == 1
        await closing
        assert source.wait_queued_calls == []
        assert sent == [{"type": "operation", "run_id": "7", "seq": 1, "state": "unknown", "done": True, "peers": {
            "Engine": {"state": "unknown", "applied_at_ms": None, "late": False,
                       "reason": "the run ended before the result came", "applied_writes": None, "failed_write": None}}}]
        assert [o["seq"] for o in v.summary()["operations"]] == [1]

        # Multi-ECU write where one peer already reached FINAL before _closed flipped mid-source.write preserves FINAL.
        v2, source2, sent2 = make()
        v2.on_peers([PeerInfo("Body", "b", True, SCHEMA)], [])
        v2.on_synced()
        source2.write_gate = asyncio.Event()
        recorded: list[dict | None] = []
        writing2 = asyncio.create_task(
            v2.write(
                [("Engine", "throttle", 12.0), ("Body", "throttle", 34.0)],
                None,
                "reject",
                on_wrote=lambda seq: recorded.append(v2.operations.get(seq)),
            )
        )
        await settle()
        v2.on_operations([op(1, False, ("Engine", "applied", 10 * MS), ("Body", "sent"))])
        closing2 = asyncio.create_task(v2.close())
        await settle()
        del sent2[:]
        source2.write_gate.set()
        assert await writing2 == 1
        await closing2
        assert v2.operations[1]["state"] == "mixed" and v2.operations[1]["done"] is True
        assert v2.operations[1]["peers"]["Engine"]["state"] == "applied"
        assert v2.operations[1]["peers"]["Engine"]["applied_at_ms"] == 10
        assert v2.operations[1]["peers"]["Body"]["state"] == "unknown"
        assert recorded[0] is not None and recorded[0]["peers"]["Engine"]["state"] == "applied"
    asyncio.run(main())


def test_close_during_wait_queued_while_sent_rejects_502_and_operation_monotonicity_holds() -> None:
    async def main() -> None:
        # 1) close() cancelling in-flight wait_queued while still 'sent' & done=False MUST raise 502 write-failed
        #    (never return HTTP 200 on self._closed while unconfirmed).
        v, source, sent = make(Limits(operations=2, write_timeout_s=1.0))
        v.on_peers([PeerInfo("Body", "b", True, SCHEMA)], [])
        v.on_synced()

        gate1 = source.wait_queued_gates[1] = asyncio.Event()
        source.wait_queued_errors[1] = SourceError("unavailable", "CANCELLED: channel closed")
        close_gate = asyncio.Event()
        orig_close = source.close

        async def slow_close() -> None:
            gate1.set()
            await close_gate.wait()
            await orig_close()

        source.close = slow_close  # type: ignore[method-assign]
        in_flight = asyncio.create_task(v.write([("Engine", "throttle", 15.0)], None, "reject"))
        await settle()
        assert v.operations[1]["state"] == "sent" and v.operations[1]["done"] is False
        closing = asyncio.create_task(v.close())
        with pytest.raises(Rejected) as exc_close:
            await in_flight
        assert (exc_close.value.status, exc_close.value.code, exc_close.value.detail) == (
            502,
            "write-failed",
            {"operation": 1, "state": "sent"},
        )
        close_gate.set()
        await closing
        assert v.operations[1]["state"] == "unknown" and v.operations[1]["done"] is True

        # 2) Watch advancing operation to 'queued' (done=False) before wait_queued fails DOES confirm queue arrival.
        v2, source2, sent2 = make(Limits(operations=2, write_timeout_s=1.0))
        v2.on_peers([PeerInfo("Body", "b", True, SCHEMA)], [])
        v2.on_synced()
        gate_q = source2.wait_queued_gates[1] = asyncio.Event()
        source2.wait_queued_errors[1] = SourceError("unavailable", "UNAVAILABLE: transient drop")
        queued_race = asyncio.create_task(v2.write([("Engine", "throttle", 20.0)], None, "reject"))
        await settle()
        v2.on_operations([op(1, False, ("Engine", "queued"))])
        gate_q.set()
        assert await queued_race == 1
        assert v2.operations[1]["state"] == "queued" and v2.operations[1]["done"] is False

        # 3) Monotonicity: queued -> sent rewind is ignored; multi-peer FINAL -> queued is preserved;
        #    evicted done=True operation is never resurrected.
        del sent2[:]
        v2.on_operations([op(1, False, ("Engine", "sent"))])
        assert sent2 == [] and v2.operations[1]["state"] == "queued"

        v2.on_operations([op(2, False, ("Engine", "applied", 10 * MS), ("Body", "sent"))])
        assert v2.operations[2]["state"] == "sent" and v2.operations[2]["peers"]["Engine"]["state"] == "applied"
        v2.on_operations([op(2, False, ("Engine", "queued"), ("Body", "queued"))])
        assert v2.operations[2]["state"] == "queued"
        assert v2.operations[2]["peers"]["Engine"]["state"] == "applied"
        assert v2.operations[2]["peers"]["Body"]["state"] == "queued"
        v2.on_operations([op(2, False, ("Engine", "sent"), ("Body", "applied", 20 * MS))])
        assert v2.operations[2]["done"] is True and v2.operations[2]["state"] == "applied"

        # Evict done=True op 2 by completing ops 1, 3, 4 (Limits.operations == 2), then send stale queued for op 2.
        v2.on_operations([
            op(1, True, ("Engine", "applied", 10 * MS)),
            op(3, True, ("Engine", "applied", 30 * MS)),
            op(4, True, ("Engine", "applied", 40 * MS)),
        ])
        assert 2 not in v2.operations
        del sent2[:]
        v2.on_operations([op(2, False, ("Engine", "queued"))])
        assert 2 not in v2.operations and sent2 == []
        await v2.close()
    asyncio.run(main())


def test_wait_queued_runs_in_parallel_without_blocking_other_ecus_and_classifies_failures() -> None:
    async def main() -> None:
        v, source, sent = make(Limits(write_timeout_s=0.15))
        v.on_peers([PeerInfo("Body", "b", True, SCHEMA)], [])
        v.on_synced()

        # Operation 1 to Engine hangs in wait_queued; operation 2 to Body registers FIFO seq=2 and finishes at once.
        source.wait_queued_gates[1] = asyncio.Event()
        slow_engine = asyncio.create_task(v.write([("Engine", "throttle", 10.0)], None, "reject"))
        await settle()
        assert not slow_engine.done()
        fast_body = await v.write([("Body", "throttle", 20.0)], None, "reject")
        assert fast_body == 2
        assert v.operations[2]["state"] == "queued"

        # Slow operation 1 times out in wait_queued -> 504 write-timeout with detail {operation: 1, state: "sent"}.
        with pytest.raises(Rejected) as exc:
            await slow_engine
        assert (exc.value.status, exc.value.code, exc.value.detail) == (
            504,
            "write-timeout",
            {"operation": 1, "state": "sent"},
        )

        # Disconnect/error in wait_queued while unsettled -> 502 write-failed (never 503 write-busy).
        source.wait_queued_errors[3] = SourceError("unavailable", "UNAVAILABLE: channel closed")
        with pytest.raises(Rejected) as exc2:
            await v.write([("Engine", "throttle", 30.0)], None, "reject")
        assert (exc2.value.status, exc2.value.code, exc2.value.detail) == (
            502,
            "write-failed",
            {"operation": 3, "state": "sent"},
        )

        # Forgotten operation (None) while unsettled -> 502 write-failed with detail.
        source.wait_queued_answers[4] = None
        with pytest.raises(Rejected) as exc3:
            await v.write([("Engine", "throttle", 40.0)], None, "reject")
        assert (exc3.value.status, exc3.value.code, exc3.value.detail) == (
            502,
            "write-failed",
            {"operation": 4, "state": "sent"},
        )

        # Disconnect in wait_queued AFTER Watch already settled seq=5 as done=True -> returns seq=5 (HTTP 200).
        gate5 = source.wait_queued_gates[5] = asyncio.Event()
        source.wait_queued_errors[5] = SourceError("unavailable", "UNAVAILABLE: stream ended")
        settled_race = asyncio.create_task(v.write([("Engine", "throttle", 50.0)], None, "reject"))
        await settle()
        v.on_operations([op(5, True, ("Engine", "applied", 0))])
        gate5.set()
        assert await settled_race == 5
        assert v.operations[5]["done"] is True and v.operations[5]["state"] == "applied"

        # Monotonicity guard: stale queued/sent updates (or late callbacks after close) never rewind done=True.
        before_count = len(sent)
        v.on_operations([op(5, False, ("Engine", "queued")), op(5, False, ("Engine", "sent"))])
        assert len(sent) == before_count
        assert v.operations[5]["done"] is True and v.operations[5]["state"] == "applied"

        await v.close()
        closed_count = len(sent)
        v.on_operations([op(99, False, ("Engine", "queued"))])
        assert len(sent) == closed_count
        assert 99 not in v.operations
    asyncio.run(main())


def test_open_operations_that_had_to_be_forgotten_still_get_their_end() -> None:
    async def main() -> None:
        v, source, sent = make(Limits(operations=2))
        v.on_synced()
        v.on_operations([op(n, False, ("Engine", "queued")) for n in (1, 2, 3, 4)])  # all open: 1 and 2 go
        assert list(v.operations) == [3, 4]
        summary = v.summary()  # it says that open ones are missing, and is whole only from 3 on
        assert (summary["operations_from_seq"], summary["operations_open_omitted"]) == (3, 2)
        v.on_operations([op(1, True, ("Engine", "applied", 0))])  # one of the forgotten ends by itself
        del sent[:]
        await v.close()
        ends = [m for m in sent if m["type"] == "operation"]
        assert [(m["seq"], m["state"], m["done"]) for m in ends] == [(3, "unknown", True), (4, "unknown", True),
                                                                      (2, "unknown", True)]
        assert ends[-1]["peers"] == {}
    asyncio.run(main())


def test_ready_waits_for_expected_peers_and_flushes_on_run_active(monkeypatch: pytest.MonkeyPatch) -> None:
    import vehicle_sil_gui.variables as variables_mod

    monkeypatch.setattr(variables_mod, "TOLD_TOGETHER_S", 0.01)
    monkeypatch.setattr(variables_mod, "INITIAL_PEERS_GRACE_S", 0.03)

    async def make_empty(expected: int = 2) -> tuple[Variables, Scripted, list[dict]]:
        sent: list[dict] = []
        v = Variables(7, sent.extend)
        v.expect()
        v.set_expected_peers(expected)
        src = Scripted()
        v.connect(src)
        return v, src, sent

    async def main() -> None:
        # 1. on_synced() with empty peers stays connecting until expected peers arrive, then transitions with all peers.
        v, _source, sent = await make_empty(2)
        v.on_synced()
        assert v.state == "connecting" and sent == []
        v.on_peers([PeerInfo("Engine", "e1", True, SCHEMA)], [])
        assert v.state == "connecting"
        v.on_peers([PeerInfo("Body", "b1", True, SCHEMA[:1])], [])
        assert v.state == "ready"
        assert [p["name"] for p in sent[-1]["variables"]["peers"]] == ["Engine", "Body"]
        await v.close()

        # 2. Partial peers still transition after TOLD_TOGETHER_S; on_run_active() flushes debounced schema early.
        v2, _source2, sent2 = await make_empty(2)
        v2.on_synced()
        v2.on_peers([PeerInfo("Engine", "e1", True, SCHEMA)], [])
        await asyncio.sleep(0.03)
        assert v2.state == "ready"
        assert [p["name"] for p in sent2[-1]["variables"]["peers"]] == ["Engine"]
        v2.on_peers([PeerInfo("Body", "b1", True, SCHEMA[:1])], [])
        before = len(sent2)
        v2.on_run_active()
        assert len(sent2) == before + 1
        assert [p["name"] for p in sent2[-1]["variables"]["peers"]] == ["Engine", "Body"]
        await v2.close()

        # 3. Grace timer falls back to ready if an expected peer never connects after run becomes active.
        v3, _source3, sent3 = await make_empty(2)
        v3.on_synced()
        assert v3.state == "connecting"
        v3.on_run_active()
        await asyncio.sleep(0.06)
        assert v3.state == "ready" and sent3[-1]["variables"]["peers"] == []
        await v3.close()

    asyncio.run(main())
