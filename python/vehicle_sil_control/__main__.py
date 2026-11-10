# SPDX-License-Identifier: MIT
"""vsil: command line of the vehicle-sil Control API.

    python -m vehicle_sil_control [--server HOST:PORT] <command> ...

Times are milliseconds of simulation time.
"""
import argparse
import os
import sys

from .client import Control, ControlError, EventInfo, Operation, Peer, Values, ms


def _parse_value(text: str):
    lowered = text.lower()
    if lowered in ("true", "false"):
        return lowered == "true"
    try:
        return int(text, 0)
    except ValueError:
        return float(text)


def _fmt_ms(ns_value) -> str:
    return "-" if ns_value is None else f"{ns_value / 1e6:g} ms"


def _print_operation(op: Operation) -> None:
    for t in op.targets:
        extra = f" at {_fmt_ms(t.applied_at)}" if t.applied_at is not None else ""
        reason = f" ({t.reason})" if t.reason else ""
        print(f"seq {op.seq} {t.peer}: {t.status}{extra}{reason}")


def _print_time(t) -> None:
    if not t.available:
        print("this server does not control the time")
        return
    state = "paused" if t.paused else ("running" if t.running else ("ended" if t.ended else "not started"))
    speed = "max" if t.speed is None else f"{t.speed:g}x"
    print(f"{state} at {_fmt_ms(t.now)}, speed {speed}" + (f", pause at {_fmt_ms(t.pause_target)}" if t.pause_target is not None else ""))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="vsil", description="vehicle-sil Control API command line")
    parser.add_argument("--server", default=os.environ.get("VEHICLE_SIL_CONTROL_SERVER", "127.0.0.1:8510"))
    parser.add_argument("--token", default=os.environ.get("VEHICLE_SIL_CONTROL_TOKEN"),
                        help="bearer token (default: $VEHICLE_SIL_CONTROL_TOKEN)")
    parser.add_argument("--root-cert", default=os.environ.get("VEHICLE_SIL_CONTROL_ROOT_CERT"),
                        help="PEM file of the server's root certificate: use TLS (default: $VEHICLE_SIL_CONTROL_ROOT_CERT)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("peers", help="list the peers")
    p = sub.add_parser("show", help="variables of a peer with their latest values")
    p.add_argument("peer")
    p = sub.add_parser("set", help="write variables: set PEER NAME=VALUE...")
    p.add_argument("peer")
    p.add_argument("assignments", nargs="+")
    p.add_argument("--at-ms", type=float, help="simulation time to take effect (default: the next step)")
    p.add_argument("--apply-late", action="store_true", help="apply in the next step if the time has passed")
    p.add_argument("--wait", choices=["none", "queued", "done"], default="queued",
                   help="wait until every peer queued it (default) or until it took effect (done)")
    sub.add_parser("time", help="where the simulation time stands")
    p = sub.add_parser("pause", help="stand still (at the next gate step, or --at-ms)")
    p.add_argument("--at-ms", type=float)
    p.add_argument("--wait", action="store_true")
    sub.add_parser("resume")
    p = sub.add_parser("step", help="run for MS and stand still")
    p.add_argument("ms", type=float)
    p = sub.add_parser("run-until", help="run until MS and stand still")
    p.add_argument("ms", type=float)
    p = sub.add_parser("speed", help="simulation seconds per wall-clock second, or 'max'")
    p.add_argument("factor")
    p = sub.add_parser("forget", help="remove a disconnected peer from the server")
    p.add_argument("peer")
    p = sub.add_parser("stop", help="stop the simulation")
    p.add_argument("--reason", default="stopped from vsil")
    p = sub.add_parser("watch", help="print updates until Ctrl+C or --count")
    p.add_argument("--peer", action="append", default=[])
    p.add_argument("--no-values", action="store_true")
    p.add_argument("--count", type=int, default=0)
    args = parser.parse_args(argv)

    try:
        roots = None
        if args.root_cert:
            with open(args.root_cert, "rb") as f:
                roots = f.read()
        with Control(args.server, token=args.token, root_certificates=roots) as c:
            if args.command == "peers":
                for peer in c.peers():
                    state = ("connected" if peer.connected else "disconnected") if peer.has_agent else "no agent"
                    failed = f", FAILED: {peer.error}" if peer.failed else ""
                    sim = f"\tSIL Kit: {peer.simulation_state}" if peer.simulation_state else ""
                    print(f"{peer.participant}\t{state}{failed}\t{len(peer.schema)} variables{sim}")
            elif args.command == "show":
                peer = c.peer(args.peer)
                values = c.values(args.peer)
                for v in peer.schema:
                    value = "" if values is None else values.values.get(v.name)
                    limits = ""
                    if v.min is not None or v.max is not None:
                        limits = f" [{'' if v.min is None else f'{v.min:g}'}..{'' if v.max is None else f'{v.max:g}'}]"
                    items = f" {{{', '.join(f'{k}={n}' for k, n in v.items.items())}}}" if v.items else ""
                    print(f"{v.name}\t{value}\t{v.unit}\t{v.causality} {v.type}{limits}{items}")
            elif args.command == "set":
                variables = {}
                for a in args.assignments:
                    name, _, text = a.partition("=")
                    if not text:
                        parser.error(f"expected NAME=VALUE, got {a}")
                    variables[name] = _parse_value(text)
                at = None if args.at_ms is None else ms(args.at_ms)
                seq = c.write({args.peer: variables}, at=at, late="apply_next" if args.apply_late else "reject")
                print(f"seq {seq}")
                if args.wait != "none":
                    _print_operation(c.wait(seq, until=args.wait))
            elif args.command == "time":
                _print_time(c.sim.time())
            elif args.command == "pause":
                c.sim.pause(None if args.at_ms is None else ms(args.at_ms))
                _print_time(c.sim.wait_paused() if args.wait else c.sim.time())
            elif args.command == "resume":
                _print_time(c.sim.resume())
            elif args.command == "step":
                c.sim.step(ms(args.ms))
                _print_time(c.sim.wait_paused())
            elif args.command == "run-until":
                c.sim.run_until(ms(args.ms))
                _print_time(c.sim.wait_paused())
            elif args.command == "speed":
                _print_time(c.sim.set_speed(None if args.factor == "max" else float(args.factor)))
            elif args.command == "forget":
                c.forget_peer(args.peer)
                print(f"{args.peer} forgotten")
            elif args.command == "stop":
                _print_time(c.sim.stop(args.reason))
            elif args.command == "watch":
                n = 0
                for u in c.watch(peers=args.peer, values=not args.no_values):
                    if isinstance(u, Peer):
                        print(f"peer {u.participant} {'connected' if u.connected else 'disconnected'}"
                              + (f" FAILED {u.error}" if u.failed else ""))
                    elif isinstance(u, Values):
                        print(f"values {u.peer} {_fmt_ms(u.time)} " + " ".join(f"{k}={v}" for k, v in u.values.items()))
                    elif isinstance(u, EventInfo):
                        print(f"event {u.peer} {_fmt_ms(u.time)} {u.level}: {u.text}")
                    elif isinstance(u, Operation):
                        _print_operation(u)
                    elif u[0] == "initial_done":
                        continue
                    else:
                        print(f"peer {u[1]} removed")
                    sys.stdout.flush()
                    n += 1
                    if args.count and n >= args.count:
                        break
    except ControlError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
