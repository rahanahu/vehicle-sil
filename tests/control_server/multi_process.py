# SPDX-License-Identifier: MIT
"""The Control API across processes (and PCs) without vehicle_sil_launch: a SIL Kit registry,
vehicle_sil_control_server with --required (the controller with the time gate), and the two ECUs of
examples/ecu_main, whose control agents reach the server over SIL Kit; a Python client drives the
simulation.

    python multi_process.py --registry EXE --server EXE --engine EXE --dashboard EXE
        [--host 127.0.0.1] [--tls-cert C --tls-key K] [--remote-dashboard] [--timeout 600]

With --remote-dashboard the Dashboard ECU is not started here: another PC runs it
(vehicle_sil_dashboard_ecu --registry-uri silkit://<host>:<registry port>), with the ports given by
--registry-port and --control-port. The client tokens are fixed test values (see CREDENTIALS).
Exit code 0 when the scenario passed.
"""
import argparse
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

from vehicle_sil_control import Control, ms

CREDENTIALS = """\
operator tester  tester-token-0123456789
admin    admin   admin-token-0123456789
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
    p.add_argument("--engine", required=True, help="vehicle_sil_engine_ecu")
    p.add_argument("--dashboard", required=True, help="vehicle_sil_dashboard_ecu")
    p.add_argument("--host", default="127.0.0.1", help="address the other processes use to reach this PC")
    p.add_argument("--listen-host", default="127.0.0.1", help="address the registry and the server listen on")
    p.add_argument("--registry-port", type=int, default=0)
    p.add_argument("--control-port", type=int, default=0)
    p.add_argument("--tls-cert")
    p.add_argument("--tls-key")
    p.add_argument("--remote-dashboard", action="store_true")
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
        # The server and the ECUs try to connect once: only once the registry listens.
        wait_for(lambda: _listens("127.0.0.1" if args.listen_host in ("0.0.0.0", "") else args.listen_host, registry_port)
                 or procs[0].poll() is not None, 30, "the registry to listen")
        assert procs[0].poll() is None, f"the registry ended with exit code {procs[0].returncode}"
        registry_uri = f"silkit://{args.host}:{registry_port}"
        server_cmd = [args.server, "--listen", f"{args.listen_host}:{control_port}", "--registry-uri",
                      f"silkit://127.0.0.1:{registry_port}", "--required", "Engine,Dashboard", "--start-paused", "1",
                      "--credentials", str(creds), "--timeout-ms", str(int(args.timeout * 1000))]
        roots = None
        if args.tls_cert:
            server_cmd += ["--tls-cert", args.tls_cert, "--tls-key", args.tls_key]
            roots = pathlib.Path(args.tls_cert).read_bytes()
        elif args.listen_host not in ("127.0.0.1", "localhost"):
            server_cmd += ["--allow-insecure", "1"]
        procs.append(start(server_cmd, "server", logs))
        control = f"{args.host}:{control_port}"
        ecu_common = ["--registry-uri", registry_uri, "--start-timeout-ms", str(int(args.timeout * 1000))]
        procs.append(start([args.engine] + ecu_common, "engine", logs))
        if not args.remote_dashboard:
            procs.append(start([args.dashboard] + ecu_common, "dashboard", logs))
        else:
            print(f"waiting for the remote Dashboard ECU: registry port {registry_port}, control port {control_port}",
                  flush=True)

        with Control(control, token="tester-token-0123456789", root_certificates=roots) as c:
            # Paused at 0 after the window before the start: the agents said hello and receive commands.
            t = _retry(lambda: c.sim.wait_paused(args.timeout), args.timeout, "the server")
            assert t.paused and t.now == 0, t
            peers = {p.participant: p for p in c.peers()}
            print("peers: " + ", ".join(f"{name} ({'connected' if p.connected else 'not connected'})"
                                        for name, p in peers.items()), flush=True)
            assert set(peers) == {"Engine", "Dashboard"} and all(p.connected for p in peers.values()), peers
            seq = c.write({"Engine": {"engine.throttle": 80.0}}, at=ms(50))
            op = c.wait(seq, until="queued")
            assert {x.status for x in op.targets} == {"queued"}, op
            c.sim.run_until(ms(1000))
            assert c.sim.wait_paused(args.timeout).now == ms(1000)
            op = c.wait(seq)
            assert all(x.status == "applied" and x.applied_at == ms(50) for x in op.targets), op
            wait_for(lambda: (c.values("Dashboard") or _none()).time >= ms(990), 30, "Dashboard's values at 1 s")
            dashboard = c.values("Dashboard").values
            engine = c.values("Engine").values
            print(f"engine speed {engine['engine.speed']} rpm, shown by Dashboard {dashboard['dashboard.engine_speed']} rpm",
                  flush=True)
            # Towards 800 rpm + 20 rpm per percent of throttle: 2400 rpm.
            assert engine["engine.speed"] == 2400 and dashboard["dashboard.engine_speed"] == 2400, (engine, dashboard)
            with Control(control, token="admin-token-0123456789", root_certificates=roots) as admin:
                admin.sim.stop("multi-process test done")
        for proc in procs[1:]:
            proc.wait(60)
        codes = {name: proc.returncode for name, proc in zip(["server", "engine", "dashboard"], procs[1:])}
        print(f"exit codes {codes}", flush=True)
        assert all(code == 0 for code in codes.values()), codes
        server_log = (logs / "server.log").read_text()
        assert "control agents connected: Engine, Dashboard" in server_log, server_log
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


def _listens(host, port):
    try:
        with socket.create_connection((host, port), timeout=1):
            return True
    except OSError:
        return False


def _retry(f, timeout, what):
    """f() until it does not raise (the server may not listen yet)."""
    deadline = time.time() + timeout
    while True:
        try:
            return f()
        except Exception:
            if time.time() >= deadline:
                raise AssertionError(f"timed out waiting for {what}")
            time.sleep(0.1)


if __name__ == "__main__":
    sys.exit(main())
