"""The availability of the variables (whether an ECU uses each now) as variables.Variables keeps and tells it, with a
source that the test scripts (no gRPC, no process): kept per incarnation apart from the schema, told after the
summary that names the variables, a few times a second at most, and never in the way of a write or a watch."""

from __future__ import annotations

import asyncio
import json

from vehicle_sil_gui.hub import encode
from vehicle_sil_gui.source import ListSample, PeerInfo, SnapshotInfo, TimeInfo
from vehicle_sil_gui.variables import TOLD_TOGETHER_S, Limits, Variables, Watch

MS = 1_000_000


def variable(name: str, role: str = "input", availability: str | None = None) -> dict:
    return {"name": name, "type": "integer", "role": role, "unit": "", "min": None, "max": None, "step": None,
            "description": "", "enum": [], "availability": availability}


# Three variables that report (unknown at first), two that do not.
ENGINE = [variable("ecu.connected"), variable("io.lamp", availability="unknown"),
          variable("io.fog", availability="unknown"), variable("io.out", "output", "unknown"),
          variable("rpm", "output")]
DECIDED = ["active", "active", "inactive", "inactive", "active"]
PLAIN = [variable("level"), variable("ticks", "output")]


class Clock(asyncio.SelectorEventLoop):
    """An event loop whose clock the test moves: what is called later is due by it, not by the wall clock."""
    now = 0.0

    def time(self) -> float:
        return self.now


def run(main) -> None:
    loop = Clock()
    try:
        loop.run_until_complete(main(loop))
    finally:
        loop.close()


async def tick(loop: Clock, seconds: float) -> None:
    loop.now += seconds
    for _ in range(5):
        await asyncio.sleep(0)


class Source:
    def __init__(self):
        self.sink = None
        self.written: list = []
        self.values: dict[str, ListSample] = {}

    def start(self, sink) -> None:
        self.sink = sink

    async def read_snapshot(self, peers: list[str], timeout_s: float) -> SnapshotInfo:
        return SnapshotInfo(TimeInfo(30 * MS, True, True, False), {p: self.values.get(p) for p in peers}, True)

    async def write(self, writes, at_ns, late, timeout_s: float) -> int:
        self.written.append(writes)
        return len(self.written)

    async def close(self) -> None:
        pass


def make(*peers: PeerInfo, limits: Limits | None = None) -> tuple[Variables, Source, list[dict]]:
    """Variables that are ready, with these peers (default: Engine of incarnation a), and what they sent."""
    sent: list[dict] = []
    v = Variables(7, sent.extend, limits=limits)
    v.expect()
    source = Source()
    v.connect(source)
    v.on_peers(list(peers) or [PeerInfo("Engine", "a", True, ENGINE)], [])
    v.on_synced()
    return v, source, sent


def shown(v: Variables, peer: str = "Engine") -> dict[str, str]:
    """The availability of each variable of the peer in the summary."""
    return {x["name"]: x["availability"] for p in v.summary()["peers"] if p["name"] == peer for x in p["variables"]}


def told(sent: list[dict]) -> list[dict]:
    return [m for m in sent if m["type"] == "availability"]


def test_the_summary_has_each_variables_state_and_a_change_is_told_once() -> None:
    async def main(loop: Clock) -> None:
        v, _, sent = make()
        assert sent[-1]["type"] == "variables"
        assert shown(v) == {"ecu.connected": "active", "io.lamp": "unknown", "io.fog": "unknown", "io.out": "unknown",
                            "rpm": "active"}
        v.on_availability("Engine", "a", DECIDED)
        assert shown(v) == {"ecu.connected": "active", "io.lamp": "active", "io.fog": "inactive", "io.out": "inactive",
                            "rpm": "active"}
        assert told(sent) == []  # not at once: the changes of a short time go together
        await tick(loop, TOLD_TOGETHER_S)
        # Of the peer, every variable that reports, by name.
        assert told(sent) == [{"type": "availability", "session": 7,
                               "peers": {"Engine": {"io.lamp": "active", "io.fog": "inactive", "io.out": "inactive"}}}]
        # The control server sends it again now and then: nothing changed, nothing is told.
        v.on_availability("Engine", "a", DECIDED)
        await tick(loop, 1)
        assert len(told(sent)) == 1
        # A state this server does not know is shown as in use; a variable that does not report is always in use.
        v.on_availability("Engine", "a", ["inactive", "later", "inactive", "inactive", "inactive"])
        await tick(loop, 1)
        assert told(sent)[-1]["peers"] == {"Engine": {"io.lamp": "active", "io.fog": "inactive", "io.out": "inactive"}}
        assert len(told(sent)) == 1
    run(main)


def test_a_peer_told_again_keeps_its_availability() -> None:
    async def main(loop: Clock) -> None:
        v, _, sent = make()
        v.on_availability("Engine", "a", DECIDED)
        await tick(loop, 1)
        expected = shown(v)
        # The ECU leaves the simulation and comes back under the same incarnation, its SIL Kit state changes: the
        # control server tells the peer again, and the states stay what the ECU said last.
        v.on_peers([PeerInfo("Engine", "a", False, ENGINE)], [])
        await tick(loop, 1)
        assert shown(v) == expected
        v.on_peers([PeerInfo("Engine", "a", True, ENGINE)], [])
        await tick(loop, 1)
        assert shown(v) == expected
        # The connection to the control server is lost and made again (it sends the peer again): the same.
        v.on_lost("the control server ended the connection", False)
        v.on_peers([PeerInfo("Engine", "a", True, ENGINE)], [])
        v.on_synced()
        assert sent[-1]["type"] == "variables" and sent[-1]["variables"]["state"] == "ready"
        assert shown(v) == expected
        assert len(told(sent)) == 1
    run(main)


def test_the_availability_belongs_to_one_incarnation() -> None:
    async def main(loop: Clock) -> None:
        v, _, sent = make()
        # Of another incarnation than the peer's now (the process before, or one not told yet): dropped.
        v.on_availability("Engine", "b", DECIDED)
        await tick(loop, 1)
        assert shown(v)["io.fog"] == "unknown" and told(sent) == []
        v.on_availability("Engine", "a", DECIDED)
        await tick(loop, 1)
        assert len(told(sent)) == 1
        # Another process under the name starts from the states it declares, and its summary comes before any word
        # of its states (the summary that comes has them), also when a change of the one before is still to be told.
        v.on_availability("Engine", "a", ["active"] * 5)
        await tick(loop, TOLD_TOGETHER_S / 2)
        before = len(sent)
        v.on_peers([PeerInfo("Engine", "b", True, ENGINE)], [])
        assert shown(v)["io.fog"] == "unknown"
        v.on_availability("Engine", "a", DECIDED)  # the process before, late: dropped
        v.on_availability("Engine", "b", ["active", "inactive", "active", "active", "active"])
        await tick(loop, TOLD_TOGETHER_S / 2)  # the change of the one before is due
        assert [m["type"] for m in sent[before:]] == ["variables"]
        await tick(loop, 1)
        assert [m["type"] for m in sent[before:]] == ["variables"]
        assert shown(v) == {"ecu.connected": "active", "io.lamp": "inactive", "io.fog": "active", "io.out": "active",
                            "rpm": "active"}
        # A list that does not fit the schema: dropped.
        v.on_availability("Engine", "b", ["inactive"] * 4)
        await tick(loop, 1)
        assert shown(v)["io.fog"] == "active" and len(sent) == before + 1
        # A new incarnation whose variables do not report: what the one before said is gone, and nothing is told.
        v.on_peers([PeerInfo("Engine", "c", True, [{**x, "availability": None} for x in ENGINE])], [])
        v.on_availability("Engine", "c", DECIDED)
        await tick(loop, 1)
        assert set(shown(v).values()) == {"active"}
        assert [m["type"] for m in sent[before + 1:]] == ["variables"]
    run(main)


def test_a_peer_that_reports_nothing_and_what_the_limits_leave_out_are_not_told() -> None:
    async def main(loop: Clock) -> None:
        # One peer only, and three variables: Body, and Engine's io.out and rpm, are left out of the summary.
        v, _, sent = make(PeerInfo("Engine", "a", True, ENGINE), PeerInfo("Body", "b", True, PLAIN),
                          limits=Limits(peers=1, variables=3))
        assert [p["name"] for p in v.summary()["peers"]] == ["Engine"]
        v.on_availability("Body", "b", ["inactive", "inactive"])
        v.on_availability("Engine", "a", DECIDED)
        await tick(loop, 1)
        assert told(sent)[-1]["peers"] == {"Engine": {"io.lamp": "active", "io.fog": "inactive"}}
        # A peer none of whose variables report: never told.
        w, _, sent = make(PeerInfo("Body", "b", True, PLAIN))
        w.on_availability("Body", "b", ["inactive", "inactive"])
        await tick(loop, 1)
        assert told(sent) == [] and set(shown(w, "Body").values()) == {"active"}
    run(main)


def test_before_the_variables_are_ready_and_at_the_end_the_summary_says_it() -> None:
    async def main(loop: Clock) -> None:
        sent: list[dict] = []
        v = Variables(7, sent.extend)
        v.expect()
        v.connect(Source())
        v.on_peers([PeerInfo("Engine", "a", True, ENGINE)], [])
        v.on_availability("Engine", "a", DECIDED)  # with the first peers, before the summary that names them
        await tick(loop, 1)
        assert told(sent) == []
        v.on_synced()
        assert sent[-1]["type"] == "variables" and shown(v)["io.fog"] == "inactive"
        # A change not told yet when the run ends is in the last summary.
        v.on_availability("Engine", "a", ["active"] * 5)
        await v.close()
        await tick(loop, 1)
        assert told(sent) == [] and sent[-1]["type"] == "variables" and sent[-1]["variables"]["state"] == "ended"
        assert shown(v)["io.fog"] == "active"
    run(main)


def test_many_changes_are_told_once_for_each_gathering_time() -> None:
    async def main(loop: Clock) -> None:
        v, _, sent = make()
        changes, every = 1000, 0.001  # an ECU that switches a variable at every step of 1 ms
        for i in range(changes):
            v.on_availability("Engine", "a", ["active", "active", "inactive" if i % 2 else "active", "active",
                                              "active"])
            await tick(loop, every)
        await tick(loop, 1)
        gatherings = changes * every / TOLD_TOGETHER_S
        assert 1 <= len(told(sent)) <= gatherings + 1
        assert told(sent)[-1]["peers"]["Engine"]["io.fog"] == "inactive"
    run(main)


def test_the_size_of_a_peer_of_300_variables() -> None:
    async def main(loop: Clock) -> None:
        schema = [variable(f"io.input_{i:03}", availability="unknown") for i in range(300)]
        v, _, sent = make(PeerInfo("Nvm", "a", True, schema))
        v.on_availability("Nvm", "a", ["inactive"] * 300)
        await tick(loop, 1)
        names = sum(len(x["name"]) for x in schema)
        # One message for the peer: each variable's name and state, and little else.
        assert len(encode(told(sent)[0])) <= names + 14 * 300 + 100
        # The summary: one field more for each variable.
        summary = v.message()
        without = json.loads(json.dumps(summary))
        for p in without["variables"]["peers"]:
            for x in p["variables"]:
                del x["availability"]
        assert len(encode(summary)) - len(encode(without)) <= len(',"availability":"inactive"') * 300
    run(main)


def test_a_variable_not_in_use_is_written_and_watched_as_any_other() -> None:
    async def main(loop: Clock) -> None:
        v, source, _ = make()
        v.on_availability("Engine", "a", DECIDED)
        await tick(loop, 1)
        writes, at_ns, late = v.check_write({"session": 7, "writes": {"Engine": {"io.fog": 3}}})
        await v.write(writes, at_ns, late)
        assert source.written == [[("Engine", "io.fog", 3)]]
        source.values["Engine"] = ListSample("a", 30 * MS, 3, [1, 1, 3, 6, 900])
        await v.refresh()
        values = v.values_message(Watch(1, None, 0.02, 0.0, lambda w: None))
        assert values["peers"]["Engine"]["values"] == {"ecu.connected": 1, "io.lamp": 1, "io.fog": 3, "io.out": 6,
                                                       "rpm": 900}
    run(main)
