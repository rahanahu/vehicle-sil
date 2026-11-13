"""variables.Variables and the hub's deferred messages, with a source that the test scripts (no gRPC, no process)."""

from __future__ import annotations

import asyncio
import json
import math

import pytest
from vehicle_sil_gui.hub import Client, Hub, Outgoing
from vehicle_sil_gui.source import ListSample, OperationInfo, PeerInfo, SourceError, TargetInfo, TimeInfo
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


class Scripted:
    """A source whose answers the test sets: `time` and `values` are what a read gives. With `gate`, each read
    waits until the test answers it (release), with what `time` and `values` are at that moment: the answer may
    then be older than what the source tells meanwhile, as an answer on its way is."""

    def __init__(self):
        self.sink = None
        self.time = running(0)
        self.values: dict[str, ListSample] = {}
        self.operations: dict[int, OperationInfo] = {}
        self.peers: list[PeerInfo] = []
        self.written: list = []
        self.write_error: SourceError | None = None
        self.write_gate: asyncio.Event | None = None
        self.gate: asyncio.Queue | None = None
        self.reads: list[str] = []
        self.closed = False

    def start(self, sink) -> None:
        self.sink = sink

    async def _answer(self, what: str) -> tuple:
        self.reads.append(what)
        if self.gate is not None:
            return await self.gate.get()
        return self.time, self.values

    def release(self, n: int = 1) -> None:
        for _ in range(n):
            self.gate.put_nowait((self.time, dict(self.values)))

    async def read_time(self, timeout_s: float) -> TimeInfo:
        return (await self._answer("time"))[0]

    async def read_values(self, peer: str, timeout_s: float):
        return (await self._answer(f"values {peer}"))[1].get(peer)

    async def read_operation(self, seq: int, timeout_s: float):
        return self.operations.get(seq)

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


# ---------------------------------------------------------------- paused, sim_ms and the values belong together


def test_paused_is_true_only_after_the_values_were_read_while_it_stood_still() -> None:
    async def main() -> None:
        v, source, _ = make()
        source.gate = asyncio.Queue()
        v.on_samples({"Engine": sample(80, 8, rpm=80)})
        v.on_time(running(90))
        v.on_synced()
        assert message(v)["paused"] is False and message(v)["sim_ms"] == 90
        # The source stands still at 100, and its last sample (the step at 90) has not come by itself yet.
        source.time = standing(100)
        source.values["Engine"] = sample(90, 9, rpm=90)
        v.on_time(standing(100))
        await settle()
        m = message(v)  # not known to be complete yet: not called paused
        assert (m["paused"], m["sim_ms"], m["peers"]["Engine"]["sample_ms"]) == (False, 100, 80)
        source.release(3)  # the time, the values, the time again
        await settle()
        m = message(v)
        assert (m["paused"], m["sim_ms"]) == (True, 100)
        assert m["peers"]["Engine"] == {"sample_ms": 90, "values": {"throttle": 0.0, "rpm": 90, "gear": 0,
                                                                    "lamp": False, "count": 0, "free": 0.0}}
        assert source.reads == ["time", "values Engine", "time"]
        # The step at 100 itself comes while it stands still: still paused at 100.
        v.on_samples({"Engine": sample(100, 10, rpm=100)})
        assert message(v)["paused"] is True and message(v)["peers"]["Engine"]["sample_ms"] == 100
        # An older sample that comes late by itself does not replace a newer one.
        v.on_samples({"Engine": sample(90, 9, rpm=90)})
        assert message(v)["peers"]["Engine"]["sample_ms"] == 100
    asyncio.run(main())


def test_a_sample_of_a_later_step_ends_paused_before_the_time_says_so() -> None:
    async def main() -> None:
        v, source, _ = make()
        source.time = standing(100)
        v.on_time(standing(100))
        v.on_synced()
        await settle()
        assert message(v)["paused"] is True
        # It runs again; the values come before the news of the time. The source still says 100 when asked.
        source.gate = asyncio.Queue()
        v.on_samples({"Engine": sample(110, 11)})
        m = message(v)
        assert m["paused"] is False and m["peers"]["Engine"]["sample_ms"] == 110
        source.time = running(120)
        source.release(1)
        await settle()
        assert message(v) == {**message(v), "paused": False, "sim_ms": 120}
    asyncio.run(main())


def test_the_time_moving_while_the_values_are_read_is_not_called_paused() -> None:
    async def main() -> None:
        v, source, _ = make()
        source.gate = asyncio.Queue()
        source.time = standing(100)
        v.on_time(standing(100))
        v.on_synced()
        await settle()
        source.release(2)  # the time (100, standing) and the values
        await settle()
        source.time = standing(200)  # stepped on meanwhile: the second read of the time differs
        source.values["Engine"] = sample(190, 19)
        source.release(1)
        await settle()
        assert message(v)["paused"] is False
        source.release(3)  # read again, now it holds
        await settle()
        m = message(v)
        assert (m["paused"], m["sim_ms"], m["peers"]["Engine"]["sample_ms"]) == (True, 200, 190)
    asyncio.run(main())


def test_news_of_another_time_while_reading_is_not_overwritten_by_the_older_read() -> None:
    async def main() -> None:
        v, source, _ = make()
        source.gate = asyncio.Queue()
        source.time = standing(100)
        v.on_time(standing(100))
        v.on_synced()
        await settle()
        source.release(3)  # all three reads are answered with "standing at 100" ...
        v.on_time(standing(300))  # ... but before the loop gets to them, the source tells of 300
        source.time = standing(300)
        await settle()
        assert message(v)["paused"] is False and message(v)["sim_ms"] == 300
        source.release(3)
        await settle()
        assert (message(v)["paused"], message(v)["sim_ms"]) == (True, 300)
    asyncio.run(main())


def test_a_read_that_fails_is_tried_again_and_a_lost_connection_ends_it() -> None:
    async def main() -> None:
        v, source, sent = make()

        async def failing(timeout_s):
            raise SourceError("timeout", "DEADLINE_EXCEEDED: no answer")
        good = source.read_time
        source.read_time = failing
        source.time = standing(100)
        v.on_time(standing(100))
        v.on_synced()
        await asyncio.sleep(0.05)
        assert message(v)["paused"] is False
        source.read_time = good
        await asyncio.sleep(0.4)
        assert message(v)["paused"] is True
        v.on_lost("the control server at 127.0.0.1:1 cannot be reached", False)
        assert v.state == "connecting" and not v.has_values and v.settled_ns is None
        assert sent[-1]["variables"]["reason"].endswith("connecting again")
        assert sent[-1]["variables"]["peers"][0]["connected"] is False
        v.on_peers([PeerInfo("Engine", "a", True, SCHEMA)], [])
        v.on_time(standing(100))
        v.on_synced()
        await settle()
        assert v.state == "ready" and message(v)["paused"] is True
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
        await asyncio.sleep(0.1)
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


def test_standing_still_is_confirmed_in_the_end_however_often_the_reads_were_overtaken() -> None:
    async def main() -> None:
        v, source, _ = make()
        source.gate = asyncio.Queue()
        source.time = standing(100)
        v.on_time(standing(100))
        v.on_synced()
        await settle()
        # Steps in quick succession: each try reads a standstill that ends before its second look at the time.
        for n in range(1, 6):
            source.release(2)
            source.time = standing(100 + 10 * n)
            source.values["Engine"] = sample(100 + 10 * n, 10 + n)
            source.release(1)
            await asyncio.sleep(0.25)  # also past the pause between the later tries
        assert message(v)["paused"] is False and message(v)["sim_ms"] == 150
        # The source tells of the time it stands at, which is what the server already knows from its own reads.
        v.on_time(standing(150))
        source.release(3)
        await asyncio.sleep(0.25)
        m = message(v)
        assert (m["paused"], m["sim_ms"], m["peers"]["Engine"]["sample_ms"]) == (True, 150, 150)
        await v.close()
    asyncio.run(main())


def test_a_sample_after_the_standstill_keeps_it_unconfirmed_until_the_time_fits() -> None:
    async def main() -> None:
        v, source, _ = make()
        source.time = standing(100)
        source.values["Engine"] = sample(110, 11)  # a source that breaks its word: a step after the standstill
        v.on_time(standing(100))
        v.on_synced()
        await asyncio.sleep(0.5)
        assert message(v)["paused"] is False
        source.time = standing(110)  # it was only the news of the time that was behind
        await asyncio.sleep(0.5)
        assert (message(v)["paused"], message(v)["sim_ms"]) == (True, 110)
        await v.close()
    asyncio.run(main())


def test_a_command_passed_on_ends_paused_until_it_is_read_again() -> None:
    async def main() -> None:
        v, source, _ = make()
        source.time = standing(100)
        v.on_time(standing(100))
        v.on_synced()
        await settle()
        assert message(v)["paused"] is True
        # A resume was written to the launcher, which has not taken it yet: the source still says standing.
        del source.reads[:]
        v.unsettle()
        assert message(v)["paused"] is False
        await asyncio.sleep(0.05)
        assert source.reads == [] and message(v)["paused"] is False  # not read again at once
        v.on_time(running(110))  # the news of the time: it runs
        await settle()
        assert message(v)["paused"] is False
        # A command the launcher refused: it still stands, no news comes, and that is found again by itself.
        source.time = standing(200)
        v.on_time(standing(200))
        await settle()
        assert message(v)["paused"] is True
        v.unsettle()
        assert message(v)["paused"] is False
        await asyncio.sleep(0.5)
        assert (message(v)["paused"], message(v)["sim_ms"]) == (True, 200)
        await v.close()
    asyncio.run(main())


def test_a_read_that_raises_something_else_does_not_end_the_trying() -> None:
    async def main() -> None:
        v, source, _ = make()
        good = source.read_time

        async def broken(timeout_s):
            raise RuntimeError("a bug in a source")
        source.read_time = broken
        source.time = standing(100)
        v.on_time(standing(100))
        v.on_synced()
        await asyncio.sleep(0.05)
        source.read_time = good
        await asyncio.sleep(0.4)
        assert message(v)["paused"] is True
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


def test_a_peer_that_is_another_process_now_loses_its_values() -> None:
    async def main() -> None:
        v, source, sent = make()
        v.on_synced()
        v.on_samples({"Engine": sample(10, 5)})
        v.on_peers([PeerInfo("Engine", "b", True, SCHEMA)], [])
        assert message(v)["peers"] == {}
        v.on_samples({"Engine": ListSample("b", 0, 1, [1.0, 1, 0, True, 0, 0.0])})
        assert message(v)["peers"]["Engine"]["values"]["lamp"] is True
        # the changed peer is told once, a little later (peers come one by one)
        v.on_peers([PeerInfo("Engine", "b", False, SCHEMA)], [])
        assert [m["type"] for m in sent] == ["variables"]
        await asyncio.sleep(0.1)
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

        class Item:
            size = 0

            def render(self):
                return None if state["n"] < 0 else json.dumps({"n": state["n"]})

        async def send(text: str) -> None:
            texts.append(text)
        hub = Hub(lambda session, from_seq: [{"type": "hello"}], limit=10)
        client = hub.client(send)
        item = Item()
        for n in range(1000):  # a screen that does not read: the values change a thousand times
            state["n"] = n
            client.put_deferred(item)
        assert len(client.items) == 1
        task = asyncio.create_task(hub.serve(client, None, None))
        await asyncio.sleep(0.01)
        assert texts == ['{"type":"hello"}', '{"n": 999}'] and not client.deferred
        # nothing for an item that has nothing to say any more
        state["n"] = -1
        client.put_deferred(item)
        await asyncio.sleep(0.01)
        assert len(texts) == 2
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
        # each is told as an operation at once (the source's own news of it may come later, or earlier)
        ops = [m for m in sent if m["type"] == "operation"]
        assert [(m["seq"], m["state"], m["done"]) for m in ops] == [(n, "sent", False) for n in range(1, 6)]
        assert ops[0]["peers"] == {"Engine": {"state": "sent", "applied_at_ms": None, "late": False, "reason": None}}
        for kind, status, code in (("timeout", 504, "write-timeout"), ("refused", 400, "write-refused"),
                                   ("unavailable", 502, "write-failed")):
            source.write_error = SourceError(kind, "X: why")
            with pytest.raises(Rejected) as e:
                await v.write([("Engine", "throttle", 1.0)], None, "reject")
            assert (e.value.status, e.value.code) == (status, code)
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
    v.on_operations([op(1, True, ("Engine", "applied", 30 * MS + 5), ("Body", "applied", 40 * MS, True))])
    assert [m["state"] for m in sent] == ["sent", "sent", "queued", "applied"]
    assert sent[-1] == {"type": "operation", "session": 7, "seq": 1, "state": "applied", "done": True, "peers": {
        "Engine": {"state": "applied", "applied_at_ms": 30, "late": False, "reason": None},
        "Body": {"state": "applied", "applied_at_ms": 40, "late": True, "reason": None}}}
    for targets, state in (((("A", "late"),), "late"), ((("A", "rejected", None, False, "the setter failed"),), "rejected"),
                           ((("A", "applied", 0), ("B", "invalid")), "mixed"), ((("A", "busy"), ("B", "busy")), "busy"),
                           ((), "unknown")):
        v.on_operations([op(9, True, *targets)])
        assert sent[-1]["state"] == state
    assert v.summary()["operations"] == [{k: x for k, x in m.items() if k not in ("type", "session")}
                                         for m in (sent[3], sent[-1])]


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
        assert sent[0] == {"type": "operation", "session": 7, "seq": 4, "state": "mixed", "done": True, "peers": {
            "Engine": {"state": "unknown", "applied_at_ms": None, "late": False,
                       "reason": "the run ended before the result came"},
            "Body": {"state": "applied", "applied_at_ms": 0, "late": False, "reason": None}}}
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
        assert sent == [{"type": "operation", "session": 7, "seq": 1, "state": "unknown", "done": True, "peers": {
            "Engine": {"state": "unknown", "applied_at_ms": None, "late": False,
                       "reason": "the run ended before the result came"}}}]
        assert [o["seq"] for o in v.summary()["operations"]] == [1]
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
