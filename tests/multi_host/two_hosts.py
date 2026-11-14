#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Two computers in one Linux: a simulation whose launcher runs on one "computer" and one of whose
ECUs runs on the other, as in docs/howto/multi-pc.md.

The computers are network namespaces a (10.9.0.1) and b (10.9.0.2) on a veth pair, made in a user
namespace (no root), each with a /tmp of its own (SIL Kit's local-domain sockets are paths there).
Where that cannot be made (not Linux; no unshare or ip; unprivileged user namespaces turned off,
as by kernel.apparmor_restrict_unprivileged_userns=1 on Ubuntu), the test is skipped (exit 77) and
says why; with VEHICLE_SIL_REQUIRE_MULTI_HOST=1 in the environment (CI), it fails instead. Any
failure after the two computers are made fails the test.

  two_hosts.py --launch <vehicle_sil_launch> --example-dir <build dir of examples/ecu_main>

What it checks: the launcher on a names a's address for other computers and the Dashboard on b
joins at it, and the run passes; the network cut while the simulation runs: the launcher says
after 10 s that virtual time does not advance, and when it does again once the link is back; the
Dashboard on b killed: the launcher aborts the run at once and names it.
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 77
REGISTRY_PORT = 18500


def skip(why: str) -> None:
    if os.environ.get("VEHICLE_SIL_REQUIRE_MULTI_HOST") == "1":
        fail(f"the two computers cannot be made, and VEHICLE_SIL_REQUIRE_MULTI_HOST=1 asks for them: {why}")
    print(f"two_hosts: skipped: {why}", flush=True)
    sys.exit(SKIP)


def fail(why: str) -> None:
    print(f"two_hosts: FAILED: {why}", flush=True)
    sys.exit(1)


def outer(args: argparse.Namespace) -> None:
    if not sys.platform.startswith("linux"):
        skip("the two computers are Linux network namespaces; this is " + sys.platform)
    for tool in ("unshare", "ip", "mount"):
        if shutil.which(tool) is None:
            skip(f"{tool} is not installed (util-linux and iproute2 make the two computers)")
    probe = subprocess.run(["unshare", "-Urnm", "true"], capture_output=True, text=True)
    if probe.returncode != 0:
        skip("unshare -Urnm fails, so unprivileged user namespaces are turned off here (for example "
             "kernel.apparmor_restrict_unprivileged_userns=1): " + (probe.stderr.strip() or f"exit {probe.returncode}"))
    run = subprocess.run(["unshare", "-Urnm", "--propagation", "private", sys.executable, os.path.abspath(__file__),
                          "--inner", "--launch", args.launch, "--example-dir", args.example_dir], timeout=600)
    sys.exit(run.returncode)


class Proc:
    """A program on one of the computers, its output read line by line."""

    def __init__(self, host: str, argv: list[str], cwd: str, stdin: bool = False):
        script = 'mount --bind /run/vsil-tmp/"$1" /tmp && cd "$2" && shift 2 && exec "$@"'
        self.lines: list[str] = []
        self.lock = threading.Condition()
        self.p = subprocess.Popen(["ip", "netns", "exec", host, "unshare", "-m", "sh", "-c", script, "sh", host, cwd, *argv],
                                  stdin=subprocess.PIPE if stdin else subprocess.DEVNULL, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, start_new_session=True)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self) -> None:
        for line in self.p.stdout:
            with self.lock:
                self.lines.append(line.rstrip("\n"))
                self.lock.notify_all()

    def text(self) -> str:
        with self.lock:
            return "\n".join(self.lines)

    def wait_for(self, part: str, timeout: float) -> str:
        """The first line with `part`; fails after `timeout` s or when the program has ended without it."""
        end = time.monotonic() + timeout
        with self.lock:
            while True:
                for line in self.lines:
                    if part in line:
                        return line
                if time.monotonic() > end or (self.p.poll() is not None and not self.reader.is_alive()):
                    fail(f"no line with {part!r} within {timeout} s; the output:\n{chr(10).join(self.lines)}")
                self.lock.wait(0.2)

    def wait(self, timeout: float) -> int:
        try:
            code = self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            fail(f"still running after {timeout} s; the output:\n{self.text()}")
        self.reader.join(10)
        return code

    def write(self, text: str) -> None:
        self.p.stdin.write(text)
        self.p.stdin.flush()

    def end(self) -> None:
        if self.p.poll() is None:
            os.killpg(self.p.pid, signal.SIGKILL)
            self.p.wait(10)


def make(*argv: str) -> None:
    """A step that makes the two computers: where it fails, they cannot be made here (skip)."""
    r = subprocess.run(argv, capture_output=True, text=True)
    if r.returncode != 0:
        skip(f"the two computers cannot be made here ({' '.join(argv)}: {r.stderr.strip()})")


def sh(*argv: str) -> None:
    """A step of the test once the two computers are made: where it fails, the test fails."""
    r = subprocess.run(argv, capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"{' '.join(argv)}: {r.stderr.strip() or f'exit {r.returncode}'}")


def config(example_dir: str) -> str:
    return ("vehicle_sil_simulation: 1\n"
            "stop_at_ms: 1000\n"
            f"registry_uri: silkit://0.0.0.0:{REGISTRY_PORT}\n"
            "networks:\n"
            "  - name: J1939_CAN\n"
            "    kind: can\n"
            "participants:\n"
            "  - name: Engine\n"
            f"    command: ['{example_dir}/engine/vehicle_sil_engine_ecu']\n"
            "  - name: Dashboard\n"
            f"    command: ['{example_dir}/dashboard/vehicle_sil_dashboard_ecu']\n"
            "    manual: true\n")


def inner(args: argparse.Namespace) -> None:
    # As root of the user namespace: the computers.
    make("mount", "-t", "tmpfs", "none", "/run")
    for d in ("/run/netns", "/run/vsil-tmp/a", "/run/vsil-tmp/b"):
        os.makedirs(d)
    make("ip", "netns", "add", "a")
    make("ip", "netns", "add", "b")
    make("ip", "link", "add", "va", "type", "veth", "peer", "name", "vb")
    make("ip", "link", "set", "va", "netns", "a")
    make("ip", "link", "set", "vb", "netns", "b")
    make("ip", "-n", "a", "addr", "add", "10.9.0.1/24", "dev", "va")
    make("ip", "-n", "b", "addr", "add", "10.9.0.2/24", "dev", "vb")
    for n, dev in (("a", "va"), ("b", "vb")):
        make("ip", "-n", n, "link", "set", "lo", "up")
        make("ip", "-n", n, "link", "set", dev, "up")
    # Not in /tmp: each computer has a /tmp of its own.
    work = tempfile.mkdtemp(prefix="two_hosts-", dir="/run")
    with open(os.path.join(work, "sim.yaml"), "w") as f:
        f.write(config(os.path.abspath(args.example_dir)))
    launch = os.path.abspath(args.launch)
    dashboard = os.path.join(os.path.abspath(args.example_dir), "dashboard", "vehicle_sil_dashboard_ecu")
    uri = f"silkit://10.9.0.1:{REGISTRY_PORT}"
    procs: list[Proc] = []
    try:
        # 1. The launcher on a tells a's address; the Dashboard on b joins there; the run passes.
        a = Proc("a", [launch, "run", "sim.yaml", "--timeout-ms", "60000"], work)
        procs.append(a)
        a.wait_for("waiting for Dashboard to join (manual)", 60)
        told = a.wait_for("from other computers, join at ", 1)
        if f"join at {uri} (this computer's address)" not in told:
            fail(f"the launcher on a names a's address {uri}:\n{told}")
        b = Proc("b", [dashboard, "--name", "Dashboard", "--registry-uri", uri, "--start-timeout-ms", "60000"], work)
        procs.append(b)
        if a.wait(60) != 0:
            fail("the run with the Dashboard on b passes:\n" + a.text())
        if b.wait(30) != 0 or "t=1000 ms engine speed" not in b.text():
            fail("the Dashboard on b got the engine speed until 1000 ms:\n" + b.text())
        print("two_hosts: joined from b, the run passed", flush=True)

        # 2. The link cut while the simulation runs (slowly, at speed 0.01): after 10 s the launcher says that
        # virtual time does not advance; once the link is back, that it does again.
        a = Proc("a", [launch, "run", "sim.yaml", "--speed", "0.01", "--stdin-commands", "--timeout-ms", "120000"], work,
                 stdin=True)
        procs.append(a)
        a.wait_for("waiting for Dashboard to join (manual)", 60)
        b = Proc("b", [dashboard, "--name", "Dashboard", "--registry-uri", uri, "--start-timeout-ms", "60000"], work)
        procs.append(b)
        a.wait_for("simulation running", 30)
        time.sleep(1)
        sh("ip", "-n", "b", "link", "set", "vb", "down")
        cut = time.monotonic()
        said = a.wait_for("virtual time has not advanced for 10 s", 40)
        if "lost its network connection" not in said:
            fail("the line names a lost connection as a cause:\n" + said)
        print(f"two_hosts: link cut, the launcher said so after {time.monotonic() - cut:.1f} s", flush=True)
        sh("ip", "-n", "b", "link", "set", "vb", "up")
        a.wait_for("virtual time advances again", 60)
        a.write('{"command":"stop"}\n')
        if a.wait(60) != 130:
            fail("a stop ends the run (130):\n" + a.text())
        if b.wait(30) != 0:
            fail("the Dashboard on b ends with the run:\n" + b.text())
        print("two_hosts: link back, it went on, stopped", flush=True)

        # 3. The Dashboard on b killed while the simulation runs: the run is aborted at once.
        a = Proc("a", [launch, "run", "sim.yaml", "--speed", "0.01", "--timeout-ms", "120000"], work)
        procs.append(a)
        a.wait_for("waiting for Dashboard to join (manual)", 60)
        b = Proc("b", [dashboard, "--name", "Dashboard", "--registry-uri", uri, "--start-timeout-ms", "60000"], work)
        procs.append(b)
        a.wait_for("simulation running", 30)
        time.sleep(1)
        b.end()
        killed = time.monotonic()
        if a.wait(30) != 1 or "participant 'Dashboard' failed" not in a.text():
            fail("the run is aborted, naming Dashboard:\n" + a.text())
        print(f"two_hosts: Dashboard on b killed, the run aborted after {time.monotonic() - killed:.1f} s", flush=True)
    finally:
        for p in procs:
            p.end()
        shutil.rmtree(work, ignore_errors=True)
    print("two_hosts: PASS", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--launch", required=True, help="vehicle_sil_launch")
    parser.add_argument("--example-dir", required=True, help="the build directory of examples/ecu_main")
    parser.add_argument("--inner", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.inner:
        inner(args)
    else:
        outer(args)


if __name__ == "__main__":
    main()
