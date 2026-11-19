#!/usr/bin/env python3
"""Summarise a profiling trace of one run: the critical path of each virtual-time step across the processes, and where
the time of each scenario write goes.

  scripts/trace-report.py DIR             DIR holds the trace-*.json files of one run (VEHICLE_SIL_TRACE=DIR)
  scripts/trace-report.py DIR --merged M  also writes M, all processes in one file, for ui.perfetto.dev

How the trace is taken and what each record means: docs/internals/trace.md. The files of one host share one clock
(steady_clock), so they are read together as they are; files from several hosts are read the same way, and the
summary gives an estimate of each ECU's clock offset from the launcher's (it is not applied).

Steps: SIL Kit starts step t+1 of a participant when every participant has completed step t. A step is completed at
the end of its handler, or, when the launcher held it (a scenario), when it was released (step.release). For each step
the summary names the participant that completed last (the critical one), how long its step took, and the time from
that completion to the first start of the next step (synchronisation: SIL Kit's messages and thread wake-ups).
Launcher hold: the launcher's step, the scenario thread's wake-up, the scenario's writes, the rest of the tick, the
gate thread's wake-up. Write round trip (writes to one ECU): the hub up to the send, the send, the way to the ECU's
handler, the handler, the way back, the hub's handler, the scenario thread's wake-up.

Numbers are us: mean, p50, p90, p99 over the steps after --skip-ms of virtual time (the start-up), without the last
five. The summary goes to the output; the details (as JSON) go to build/logs/trace-report.json (--json). Exit codes:
0 summarised, 1 nothing to summarise (no trace files, or too few steps; the message says why), 2 usage error.
Requires Python 3.11+ (standard library only). The trace's format is internal and may change with any release.
"""

from __future__ import annotations

import argparse
import bisect
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_JSON = ROOT / "build" / "logs" / "trace-report.json"


class Trace:
    """The events of all processes, by process and name. Times in ns of the host's steady clock."""

    def __init__(self, files: list[Path]):
        self.processes: dict[int, dict] = {}
        self.events: list[dict] = []
        for f in files:
            obj = json.loads(f.read_text(encoding="utf-8"))
            meta = obj.get("otherData", {})
            pid = meta["pid"]
            self.processes[pid] = {"name": meta["process"], "file": f.name, "dropped": meta.get("dropped", 0)}
            self.events += obj.get("traceEvents", [])
        names = [p["name"] for p in self.processes.values()]
        for pid, p in self.processes.items():
            p["label"] = p["name"] if names.count(p["name"]) == 1 else f"{p['name']}-{pid}"
        self.spans: dict[tuple[int, str], list[tuple[int, int, int, int, int]]] = defaultdict(list)
        self.instants: dict[tuple[int, str], list[tuple[int, int, int]]] = defaultdict(list)
        for e in self.events:
            ph = e.get("ph")
            if ph == "X":
                start = ns(e["ts"])
                args = e.get("args", {})
                self.spans[(e["pid"], e["name"])].append((start, start + ns(e["dur"]), args.get("vt", -1),
                                                          args.get("arg", 0), e["tid"]))
            elif ph == "i":
                args = e.get("args", {})
                self.instants[(e["pid"], e["name"])].append((ns(e["ts"]), args.get("vt", -1), args.get("arg", 0)))
        for v in self.spans.values():
            v.sort()
        for v in self.instants.values():
            v.sort()

    def label(self, pid: int) -> str:
        return self.processes[pid]["label"]


def ns(us: float) -> int:
    return round(us * 1000)


def quantile(xs: list[int], p: float) -> int:
    k = min(len(xs) - 1, max(0, round(p * (len(xs) - 1))))
    return xs[k]


def stat(values) -> dict:
    xs = sorted(v for v in values if v is not None)
    if not xs:
        return {"n": 0}
    return {"n": len(xs), "mean_us": round(statistics.fmean(xs) / 1000, 2), "p50_us": round(quantile(xs, .5) / 1000, 2),
            "p90_us": round(quantile(xs, .9) / 1000, 2), "p99_us": round(quantile(xs, .99) / 1000, 2)}


def analyse(t: Trace, skip_ms: float) -> dict:
    """The details; raises ValueError with what is missing."""
    parts = [pid for pid in t.processes if t.spans.get((pid, "step"))]
    if not parts:
        raise ValueError("no process recorded a step (span 'step'): were the files written by a run of virtual time?")
    launcher = next((pid for pid in t.processes if t.spans.get((pid, "scenario.tick")) or t.spans.get((pid, "hub.send"))
                     or t.spans.get((pid, "hub.receive_step"))), None)
    ecus = [pid for pid in parts if pid != launcher]

    # Each participant's start, length and completion of every step.
    released = {vt: time for time, vt, _ in t.instants.get((launcher, "step.release"), [])}
    start: dict[int, dict[int, int]] = {}
    length: dict[int, dict[int, int]] = {}
    complete: dict[int, dict[int, int]] = {}
    held: set[int] = set()
    for pid in parts:
        st, ln, co = {}, {}, {}
        for s, e, vt, arg, _ in t.spans[(pid, "step")]:
            st[vt], ln[vt] = s, e - s
            if pid == launcher and arg == 1 and vt in released:
                held.add(vt)
                co[vt] = released[vt]
            else:
                co[vt] = e
        start[pid], length[pid], complete[pid] = st, ln, co

    every_vt = sorted(set().union(*(start[p].keys() for p in parts)))
    if len(every_vt) < 3:
        raise ValueError(f"only {len(every_vt)} steps were recorded: run longer")
    step_ns = min(b - a for a, b in zip(every_vt, every_vt[1:]))
    # A participant with a larger step (the launcher without a scenario) takes part in the steps it has; the grid
    # is the finest step.
    every = [p for p in parts if len(start[p]) * 2 > len(every_vt)] or parts
    vts = sorted(set.intersection(*(set(start[p]) for p in every)))
    first = vts[0] + skip_ms * 1e6
    vts = [v for v in vts if v >= first and all(v + step_ns in start[p] for p in every)][:-5]
    if len(vts) < 3:
        raise ValueError(f"fewer than 3 steps after the first {skip_ms:g} ms that every participant recorded: run "
                         "longer, lower --skip-ms, or raise VEHICLE_SIL_TRACE_EVENTS if records were dropped")

    rows = []
    last_count: dict[str, int] = defaultdict(int)
    for vt in vts:
        nxt = vt + step_ns
        here = [p for p in parts if vt in complete[p] and nxt in start[p]]
        last = max(here, key=lambda p: complete[p][vt])
        last_count[t.label(last)] += 1
        ref = launcher if launcher in here else here[0]
        rows.append({"vt": vt, "period": start[ref][nxt] - start[ref][vt],
                     "critical": complete[last][vt] - start[last][vt],
                     "sync": min(start[p][nxt] for p in here) - complete[last][vt],
                     "slowest_ecu_step": max((length[p][vt] for p in ecus if p in here), default=None)})
    out: dict = {
        "processes": {t.label(p): {"file": v["file"], "dropped": v["dropped"]} for p, v in t.processes.items()},
        "launcher": t.label(launcher) if launcher is not None else None,
        "ecus": [t.label(p) for p in ecus],
        "step_ms": step_ns / 1e6,
        "steps": len(rows), "from_ms": vts[0] / 1e6, "to_ms": vts[-1] / 1e6,
        "last_to_complete": dict(sorted(last_count.items(), key=lambda kv: -kv[1])),
        "period": stat(r["period"] for r in rows),
        "critical_step": stat(r["critical"] for r in rows),
        "sync": stat(r["sync"] for r in rows),
        "slowest_ecu_step": stat(r["slowest_ecu_step"] for r in rows),
    }
    analysed = set(vts)
    out["ecu_steps"] = {}
    for p in ecus:
        def lengths(name, pick=lambda arg: True):
            return stat(e - s for s, e, vt, arg, _ in t.spans.get((p, name), []) if vt in analysed and pick(arg))
        out["ecu_steps"][t.label(p)] = {
            "step": lengths("step"), "handler": lengths("step.handler"),
            "agent.step_begin": lengths("agent.step_begin"), "agent.step_end": lengths("agent.step_end"),
            "agent.publish with a sample": lengths("agent.publish", lambda a: a == 1),
            "agent.publish without": lengths("agent.publish", lambda a: a == 0)}
    if launcher is not None:
        out.update(analyse_hold(t, launcher, vts, start, length, released, held, {r["vt"]: r["period"] for r in rows}))
        out.update(analyse_writes(t, launcher, ecus, (start[every[0]][vts[0]], start[every[0]][vts[-1]])))
    return out


def analyse_hold(t: Trace, launcher: int, vts, start, length, released, held, period_of) -> dict:
    ticks = {vt: (s, e) for s, e, vt, _, _ in t.spans.get((launcher, "scenario.tick"), [])}
    woken = {vt: time for time, vt, _ in t.instants.get((launcher, "scenario.woken"), [])}
    gate = {vt: s for s, e, vt, _, _ in t.spans.get((launcher, "gate.release"), [])}
    writes = t.spans.get((launcher, "scenario.write"), [])
    write_starts = [w[0] for w in writes]
    rows = []
    for vt in vts:
        if vt not in held or vt not in ticks or vt not in woken or vt not in gate:
            continue
        s, e = ticks[vt]
        i = bisect.bisect_left(write_starts, s)
        inside = []
        while i < len(writes) and writes[i][0] < e:
            inside.append(writes[i][1] - writes[i][0])
            i += 1
        step_end = start[launcher][vt] + length[launcher][vt]
        rows.append({"vt": vt, "n_writes": len(inside),
                     "launcher_step": length[launcher][vt],
                     "scenario_wake": woken[vt] - step_end,
                     "to_tick": s - woken[vt],
                     "writes": sum(inside),
                     "rest_of_tick": (e - s) - sum(inside),
                     "gate_wake": gate[vt] - e,
                     "release": released[vt] - gate[vt],
                     "hold": released[vt] - start[launcher][vt]})
    out = {}
    for label, pick in (("hold_with_writes", lambda r: r["n_writes"] > 0), ("hold_without_writes", lambda r: r["n_writes"] == 0)):
        rs = [r for r in rows if pick(r)]
        if rs:
            out[label] = {k: stat(r[k] for r in rs) for k in rs[0] if k not in ("vt", "n_writes")}
            out[label]["writes_per_step"] = round(statistics.fmean(r["n_writes"] for r in rs), 2)
            out[label]["period"] = stat(period_of[r["vt"]] for r in rs)
    return out


def analyse_writes(t: Trace, launcher: int, ecus: list[int], window: tuple[int, int]) -> dict:
    by_seq: dict[int, dict] = defaultdict(lambda: defaultdict(list))
    for name in ("scenario.write", "hub.write", "hub.send", "hub.receive_answer"):
        for s, e, _, seq, _ in t.spans.get((launcher, name), []):
            by_seq[seq][name].append((s, e))
    ecu_steps = {}
    for p in ecus:
        for s, e, _, seq, _ in t.spans.get((p, "agent.receive_command"), []):
            by_seq[seq]["agent.receive_command"].append((s, e, p))
        ecu_steps[p] = [(s, e) for s, e, _, _, _ in t.spans[(p, "step")]]

    def in_step(p, a, b):
        steps = ecu_steps[p]
        i = bisect.bisect_left(steps, (a, a)) - 1
        return any(s < b and e > a for s, e in steps[max(i, 0):i + 3])

    parts = defaultdict(list)
    offsets = defaultdict(list)
    n = more_targets = during_step = 0
    for seq, d in by_seq.items():
        if not d.get("scenario.write") or not window[0] <= d["scenario.write"][0][0] <= window[1]:
            continue
        if not all(len(d.get(k, [])) == 1 for k in ("hub.send", "hub.receive_answer", "agent.receive_command")):
            more_targets += 1
            continue
        (ws, we), = d["scenario.write"]
        (ss, se), = d["hub.send"]
        (cs, ce, p), = d["agent.receive_command"]
        (bs, be), = d["hub.receive_answer"]
        n += 1
        during_step += in_step(p, se, cs)
        for k, v in (("total", we - ws), ("hub to send", ss - ws), ("send", se - ss), ("to the ECU", cs - se),
                     ("ECU handler", ce - cs), ("back", bs - ce), ("hub handler", be - bs), ("scenario wake", we - be)):
            parts[k].append(v)
        offsets[p].append(((cs - se) + (ce - bs)) / 2)
    out = {"write_round_trip": {"n": n, "with_more_targets": more_targets,
                                "share_arriving_during_an_ecu_step": round(during_step / n, 3) if n else None,
                                **{k: stat(v) for k, v in parts.items()}}}
    out["clock_offset_from_launcher_us"] = {t.label(p): round(statistics.median(v) / 1000, 2) for p, v in offsets.items()}
    return out


def numbers(s: dict) -> str:
    if not s or not s.get("n"):
        return "       -"
    return "".join(f"{s[k]:9.1f}" for k in ("mean_us", "p50_us", "p90_us", "p99_us"))


def summary(d: dict) -> list[str]:
    dropped = {k: v["dropped"] for k, v in d["processes"].items() if v["dropped"]}
    lines = [f"trace-report: {len(d['processes'])} processes (launcher: {d['launcher'] or 'none'}; ECUs: "
             f"{', '.join(d['ecus']) or 'none'}), step {d['step_ms']:g} ms, {d['steps']} steps from {d['from_ms']:g} to "
             f"{d['to_ms']:g} ms"]
    if dropped:
        lines.append("  records dropped (the rings kept the newest; raise VEHICLE_SIL_TRACE_EVENTS for more): "
                     + ", ".join(f"{k} {v}" for k, v in dropped.items()))
    lines.append(f"  {'(us)':38s}{'mean':>9s}{'p50':>9s}{'p90':>9s}{'p99':>9s}")

    def row(label, s, indent=2):
        lines.append(f"{' ' * indent}{label:{40 - indent}s}{numbers(s)}")

    row("step period", d["period"])
    total = sum(d["last_to_complete"].values())
    lines.append("    last to complete: " + ", ".join(f"{k} {100 * v / total:.0f}%" for k, v in d["last_to_complete"].items()))
    row("its step (with the hold)", d["critical_step"], 4)
    row("sync (its completion to next start)", d["sync"], 4)
    if d["ecus"]:
        row("slowest ECU step", d["slowest_ecu_step"], 4)
    for key, title in (("hold_with_writes", "launcher hold, steps with writes"),
                       ("hold_without_writes", "launcher hold, steps without writes")):
        if key in d:
            h = d[key]
            lines.append(f"  {title} ({h['hold']['n']} steps, {h['writes_per_step']:g} writes per step)")
            row("step period", h["period"], 4)
            for k, label in (("hold", "hold (launcher step to release)"), ("launcher_step", "launcher step"),
                             ("scenario_wake", "scenario thread wake-up"), ("writes", "writes"),
                             ("rest_of_tick", "rest of the tick"), ("gate_wake", "gate thread wake-up")):
                row(label, h[k], 4)
    w = d.get("write_round_trip")
    if w and w["n"]:
        lines.append(f"  write round trip ({w['n']} writes to one ECU, {w['with_more_targets']} to more left out; "
                     f"{100 * w['share_arriving_during_an_ecu_step']:.1f}% arrived during the ECU's step)")
        for k in ("total", "hub to send", "send", "to the ECU", "ECU handler", "back", "hub handler", "scenario wake"):
            row(k, w[k], 4)
        lines.append("  clock offset from the launcher (us, estimate): "
                     + ", ".join(f"{k} {v:g}" for k, v in d["clock_offset_from_launcher_us"].items()))
    return lines


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="scripts/trace-report.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("dir", type=Path, help="the directory with the trace-*.json files of one run")
    p.add_argument("--skip-ms", type=float, default=200, help="virtual ms left out at the start (default: 200)")
    p.add_argument("--json", type=Path, default=DEFAULT_JSON, help="where the details go (default: build/logs/trace-report.json)")
    p.add_argument("--merged", type=Path, help="also write all processes' events to this file (ui.perfetto.dev opens it)")
    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as e:
        return 0 if e.code == 0 else 2
    if args.skip_ms < 0:
        print("trace-report: --skip-ms must not be negative", file=sys.stderr)
        return 2
    files = sorted(args.dir.glob("trace-*.json"))
    if not files:
        print(f"trace-report: no trace-*.json in {args.dir}: run with the environment variable VEHICLE_SIL_TRACE={args.dir} "
              "(docs/internals/trace.md), and let the run end normally", file=sys.stderr)
        return 1
    try:
        t = Trace(files)
    except (ValueError, KeyError) as e:
        print(f"trace-report: a file in {args.dir} is not a whole trace ({e}); a process that was killed writes none, "
              "and one that is still writing leaves it incomplete", file=sys.stderr)
        return 1
    if args.merged:
        args.merged.parent.mkdir(parents=True, exist_ok=True)
        args.merged.write_text(json.dumps({"displayTimeUnit": "ns", "traceEvents": t.events}), encoding="utf-8")
    try:
        details = analyse(t, args.skip_ms)
    except ValueError as e:
        print(f"trace-report: {e}", file=sys.stderr)
        return 1
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(details, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print("\n".join(summary(details)))
    print(f"  details: {args.json}" + (f"; merged trace: {args.merged}" if args.merged else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
