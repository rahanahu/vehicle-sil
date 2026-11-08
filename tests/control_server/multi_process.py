# SPDX-License-Identifier: MIT
"""The Control API across processes (and PCs): a SIL Kit registry, the control server with the
time gate, and vehicle_sil_control_peer processes; a Python client drives the simulation.

    python multi_process.py --registry EXE --server EXE --peer EXE
        [--host 127.0.0.1] [--tls-cert C --tls-key K] [--remote-body] [--timeout 600]

With --remote-body the Body peer is not started here: another PC runs it (vehicle_sil_control_peer
--role body --participant Body --registry-uri silkit://<host>:<registry port> --control-server
<host>:<control port> --token <body token>), with the ports given by --registry-port and
--control-port. The tokens are fixed test values (see CREDENTIALS).
Exit code 0 when the scenario passed.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time

from vehicle_sil_control import Control, ms

CREDENTIALS = """\
operator tester  tester-token-0123456789
admin    admin   admin-token-0123456789
agent    engine  engine-token-0123456789  Engine
agent    body    body-token-0123456789    Body
"""


def start(cmd, name, logs):
    log = open(logs / f"{name}.log", "w")
    return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)


def wait_for(pred, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {what}")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--registry", required=True)
    p.add_argument("--server", required=True)
    p.add_argument("--peer", required=True)
    p.add_argument("--host", default="127.0.0.1", help="address the other processes use to reach this PC")
    p.add_argument("--listen-host", default="127.0.0.1", help="address the registry and the server listen on")
    p.add_argument("--registry-port", type=int, default=0)
    p.add_argument("--control-port", type=int, default=0)
    p.add_argument("--tls-cert")
    p.add_argument("--tls-key")
    p.add_argument("--remote-body", action="store_true")
    p.add_argument("--timeout", type=float, default=120)
    p.add_argument("--logs", default=None)
    args = p.parse_args()

    logs = pathlib.Path(args.logs or tempfile.mkdtemp(prefix="vsil-multi-"))
    logs.mkdir(parents=True, exist_ok=True)
    creds = logs / "credentials"
    creds.write_text(CREDENTIALS)
    procs = []
    try:
        registry_port = args.registry_port or 18500 + os.getpid() % 1000
        control_port = args.control_port or 19500 + os.getpid() % 1000
        procs.append(start([args.registry, "--listen-uri", f"silkit://{args.listen_host}:{registry_port}"], "registry", logs))
        time.sleep(0.5)
        registry_uri = f"silkit://{args.host}:{registry_port}"
        server_cmd = [args.server, "--listen", f"{args.listen_host}:{control_port}", "--registry-uri",
                      f"silkit://127.0.0.1:{registry_port}", "--required", "Engine,Body", "--start-paused", "1",
                      "--credentials", str(creds), "--timeout-ms", str(int(args.timeout * 1000))]
        roots = None
        if args.tls_cert:
            server_cmd += ["--tls-cert", args.tls_cert, "--tls-key", args.tls_key]
            roots = pathlib.Path(args.tls_cert).read_bytes()
        elif args.listen_host not in ("127.0.0.1", "localhost"):
            server_cmd += ["--allow-insecure", "1"]
        procs.append(start(server_cmd, "server", logs))
        control = f"{args.host}:{control_port}"
        peer_common = ["--registry-uri", registry_uri, "--control-server", control]
        if args.tls_cert:
            peer_common += ["--root-cert", args.tls_cert]
        procs.append(start([args.peer, "--role", "engine", "--participant", "Engine", "--token", "engine-token-0123456789"]
                           + peer_common, "engine", logs))
        if not args.remote_body:
            procs.append(start([args.peer, "--role", "body", "--participant", "Body", "--token", "body-token-0123456789"]
                               + peer_common, "body", logs))
        else:
            print(f"waiting for the remote Body peer: registry port {registry_port}, control port {control_port}", flush=True)

        with Control(control, token="tester-token-0123456789", root_certificates=roots) as c:
            wait_for(lambda: _try(lambda: sum(p.connected for p in c.peers()) == 2), args.timeout, "two peers")
            print("peers: " + ", ".join(p.participant for p in c.peers()), flush=True)
            t = c.sim.wait_paused(args.timeout)
            assert t.paused and t.now == 0, t
            seq = c.write({"Engine": {"engine.throttle": 80.0}, "Body": {"cabin.lamp": 1}}, at=ms(50))
            op = c.wait(seq, until="queued")
            assert {x.status for x in op.targets} == {"queued"}, op
            c.sim.run_until(ms(1000))
            assert c.sim.wait_paused(args.timeout).now == ms(1000)
            op = c.wait(seq)
            assert all(x.status == "applied" and x.applied_at == ms(50) for x in op.targets), op
            wait_for(lambda: (c.values("Body") or _none()).time >= ms(990), 30, "Body's values at 1 s")
            body = c.values("Body").values
            engine = c.values("Engine").values
            print(f"engine speed {engine['engine.speed']:.0f} rpm, received by Body {body['body.engine_speed']:.0f} rpm",
                  flush=True)
            assert engine["engine.speed"] > 3000 and body["body.engine_speed"] > 3000
            assert body["cabin.lamp"] == 1
            with Control(control, token="admin-token-0123456789", root_certificates=roots) as admin:
                admin.sim.stop("multi-process test done")
        for proc in procs[1:]:
            proc.wait(60)
        codes = {name: proc.returncode for name, proc in zip(["server", "engine", "body"], procs[1:])}
        print(f"exit codes {codes}", flush=True)
        assert all(code == 0 for code in codes.values()), codes
        print("PASS", flush=True)
        return 0
    except Exception as e:  # report and fail
        print(f"FAIL: {e!r} (logs in {logs})", flush=True)
        return 1
    finally:
        for proc in procs:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    proc.kill()


class _None:
    time = -1


def _none():
    return _None()


def _try(f):
    try:
        return f()
    except Exception:
        return False


if __name__ == "__main__":
    sys.exit(main())
