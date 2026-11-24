#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""vehicle_sil_socketcan_bridge on a virtual CAN interface (vcan), without root: the interface is made in a
network namespace of a user namespace (as tests/ethernet_bridge/bridge_netns.py makes its interfaces), with a SIL Kit
registry and the SIL Kit peer of the Vector XL bridge's tests inside. The "device outside" is this script, with raw
CAN sockets of its own.

  bridge_netns.py --scenario <roundtrip|link|refused|shared|launch> --bridge <vehicle_sil_socketcan_bridge>
                  --peer <vehicle_sil_vector_xl_it_peer> --registry <sil-kit-registry>
                  [--launch <vehicle_sil_launch> --ecu <vehicle_sil_engine_ecu>]   (launch)

roundtrip  every frame of the peer's matrix (11 and 29 bit identifiers, DLC 0..8, remote frames, DLC 15) reaches a
           device on vcan0 unchanged, which answers each (inverted bytes, or 0,1,2,... for a remote frame), and the
           answers reach the peer. The bridge's own frames come back to its socket (CAN_RAW_RECV_OWN_MSGS) and are
           counted as own_tx_dropped, not as from a device. A frame another socket of this namespace sends on vcan0
           (candump, cangen and the like are such programs) is from a device, and the bridge forwards it
           (ext_from_devices, ext_to_silkit). --bitrate on vcan is noted and ignored.
link       the interface set down and up again: the link lines (link=down, link=up) and the warnings, and the bridge
           receives again afterwards. The interface removed: FAIL and exit code 1.
refused    exit code 3 with the reason and the way to fix it: no such interface, an interface that is down, no
           registry. --list-channels (text and JSON); a bridge that stops on SIGTERM ends with exit code 0.
shared     a second bridge on the same interface: both warn, the link lines say shared and the peers, the list
           says the interface is in use; when the second one ends, shared is 0 again.
launch     vehicle_sil_launch run --connect J1939_CAN=vcan0 starts the bridge itself (found on PATH; the target has no
           prefix and is not an XL channel, so the launcher takes the socketcan backend) beside the engine ECU of
           examples/ecu_main. The ECU's frames reach a device on vcan0, a frame of the device reaches the run (the bridge's
           statistics line counts both); the launcher stopped the bridge with exit code 0 at the end.

Exit code 77 (skipped), with the reason, where this cannot be made:
 - no user namespaces (not Linux; no unshare or ip; unprivileged user namespaces turned off): a failure instead
   with VEHICLE_SIL_REQUIRE_MULTI_HOST=1;
 - the namespace is there, but vcan0 or a CAN socket cannot be made in it (the vcan module is not loaded: sudo modprobe
   vcan): a failure instead with VEHICLE_SIL_REQUIRE_SOCKETCAN=1.
Any failure after that fails the test.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

_COMMON_DIR = str(pathlib.Path(__file__).resolve().parents[1] / "common")
if _COMMON_DIR not in sys.path:
    sys.path.insert(0, _COMMON_DIR)
from timeout_scale import timeout_scale  # noqa: E402

SKIP = 77
REGISTRY_PORT = 18510
REGISTRY_URI = f"silkit://127.0.0.1:{REGISTRY_PORT}"
NETWORK = "CAN_NETNS"
PARTICIPANT = "SocketCanBridge"

CAN_EFF_FLAG = 0x80000000
CAN_RTR_FLAG = 0x40000000
CAN_ERR_FLAG = 0x20000000
FRAME = struct.Struct("=IB3x8s")

try:
    SCALE = timeout_scale()
except ValueError as e:
    raise SystemExit(str(e)) from None
WAIT = 30 * SCALE

STATISTICS = ("silkit_to_ext", "ext_to_silkit", "ext_from_devices", "own_tx_dropped", "error_frames", "overruns",
              "silkit_rejected", "ext_rejected", "ext_queue_full", "ext_tx_failures", "silkit_tx_failures",
              "ext_tx_dropped", "ext_tx_unsent")
MATRIX_FRAMES = 58  # frame_matrix() of tests/can_bridge


def say(text: str) -> None:
    print(f"bridge_netns: {text}", flush=True)


def fail(why: str) -> None:
    say(f"FAILED: {why}")
    sys.exit(1)


def skip_no_namespace(why: str) -> None:
    if os.environ.get("VEHICLE_SIL_REQUIRE_MULTI_HOST") == "1":
        fail(f"the namespace cannot be made, and VEHICLE_SIL_REQUIRE_MULTI_HOST=1 asks for it: {why}")
    say(f"skipped: no user namespace with a network of its own: {why}")
    sys.exit(SKIP)


def skip_no_vcan(why: str) -> None:
    if os.environ.get("VEHICLE_SIL_REQUIRE_SOCKETCAN") == "1":
        fail(f"vcan0 or a CAN socket cannot be made, and VEHICLE_SIL_REQUIRE_SOCKETCAN=1 asks for them: {why}")
    say(f"skipped: the user namespace is there, but a virtual CAN interface cannot be made in it "
        f"(sudo modprobe vcan, and the kernel's can_raw): {why}")
    sys.exit(SKIP)


def check(condition: bool, what: str) -> None:
    if not condition:
        fail(what)


def outer(args: argparse.Namespace) -> None:
    if not sys.platform.startswith("linux"):
        skip_no_namespace("this is " + sys.platform)
    for tool in ("unshare", "ip", "mount"):
        if shutil.which(tool) is None:
            skip_no_namespace(f"{tool} is not installed (util-linux and iproute2)")
    probe = subprocess.run(["unshare", "-Urnm", "true"], capture_output=True, text=True)
    if probe.returncode != 0:
        skip_no_namespace("unshare -Urnm fails, so unprivileged user namespaces are turned off here (for example "
                          "kernel.apparmor_restrict_unprivileged_userns=1): " + (probe.stderr.strip() or f"exit {probe.returncode}"))
    run = subprocess.run(["unshare", "-Urnm", "--propagation", "private", sys.executable, os.path.abspath(__file__), "--inner",
                          "--scenario", args.scenario, "--bridge", args.bridge, "--peer", args.peer, "--registry", args.registry,
                          *(["--launch", args.launch, "--ecu", args.ecu] if args.scenario == "launch" else [])],
                         timeout=600 * SCALE)
    sys.exit(run.returncode)


class Proc:
    """A program in the namespace, its output read line by line."""

    def __init__(self, argv: list[str], cwd: str):
        self.lines: list[str] = []
        self.lock = threading.Condition()
        self.p = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, start_new_session=True)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self) -> None:
        try:
            for line in self.p.stdout:
                with self.lock:
                    self.lines.append(line.rstrip("\n"))
                    self.lock.notify_all()
        finally:
            with self.lock:
                self.lock.notify_all()

    def text(self) -> str:
        with self.lock:
            return "\n".join(self.lines)

    def wait_until(self, predicate, describe, timeout: float = WAIT) -> None:
        """Waits until `predicate(lines)` holds, woken on every output line; fails after `timeout` s or on EOF."""
        end = time.monotonic() + timeout
        with self.lock:
            while True:
                if predicate(self.lines):
                    return
                remaining = end - time.monotonic()
                if remaining <= 0 or (self.p.poll() is not None and not self.reader.is_alive()):
                    fail(f"{describe(self.lines)}; the output:\n{chr(10).join(self.lines)}")
                self.lock.wait(min(0.2, remaining))

    def wait_for(self, part: str, timeout: float = WAIT, after: int = 0) -> str:
        """The first line with `part` (from line number `after` on); fails after `timeout` s or when the program has
        ended without it."""
        end = time.monotonic() + timeout
        with self.lock:
            while True:
                for line in self.lines[after:]:
                    if part in line:
                        return line
                remaining = end - time.monotonic()
                if remaining <= 0 or (self.p.poll() is not None and not self.reader.is_alive()):
                    fail(f"no line with {part!r} within {timeout} s; the output:\n{chr(10).join(self.lines)}")
                self.lock.wait(min(0.2, remaining))

    def count(self) -> int:
        with self.lock:
            return len(self.lines)

    def wait(self, timeout: float = WAIT) -> int:
        try:
            code = self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            fail(f"still running after {timeout} s; the output:\n{self.text()}")
        self.reader.join(10)
        return code

    def stop(self) -> int:
        """SIGTERM, as a user's stop; the exit code."""
        self.p.send_signal(signal.SIGTERM)
        return self.wait()

    def end(self) -> None:
        if self.p.poll() is None:
            os.killpg(self.p.pid, signal.SIGKILL)
            self.p.wait(10)


def make(*argv: str) -> None:
    """A step that makes the interface: where it fails, the test cannot run here (skip)."""
    r = subprocess.run(argv, capture_output=True, text=True)
    if r.returncode != 0:
        skip_no_vcan(f"{' '.join(argv)}: {r.stderr.strip() or f'exit {r.returncode}'}")


def sh(*argv: str) -> str:
    """A step of the test once the interface is made: where it fails, the test fails."""
    r = subprocess.run(argv, capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"{' '.join(argv)}: {r.stderr.strip() or f'exit {r.returncode}'}")
    return r.stdout


class Device:
    """A raw CAN socket on vcan0: a device (or another program on this computer) there. With `echo`, it answers
    every frame as the Vector XL tests' fake device does: a data frame comes back with its bytes inverted, a remote
    frame as a data frame of 0, 1, 2, ..."""

    def __init__(self, interface: str, echo: bool = False):
        try:
            self.s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
            self.s.bind((interface,))
        except OSError as e:
            skip_no_vcan(f"a CAN socket on {interface}: {e}")
        self.s.settimeout(0.2)
        self.seen: list[tuple[int, bytes]] = []
        self.done = threading.Event()
        self.thread = threading.Thread(target=self._echo, daemon=True) if echo else None
        if self.thread:
            self.thread.start()

    def send(self, can_id: int, data: bytes, extended: bool = False) -> None:
        self.s.send(FRAME.pack(can_id | (CAN_EFF_FLAG if extended else 0), len(data), data.ljust(8, b"\0")))

    def _echo(self) -> None:
        while not self.done.is_set():
            try:
                raw = self.s.recv(FRAME.size)
            except (socket.timeout, TimeoutError):
                continue
            except OSError:
                return
            can_id, length, data = FRAME.unpack(raw)
            self.seen.append((can_id, data[:length]))
            answer = bytes(range(length)) if can_id & CAN_RTR_FLAG else bytes(b ^ 0xFF for b in data[:length])
            self.s.send(FRAME.pack(can_id & ~CAN_RTR_FLAG, length, answer.ljust(8, b"\0")))

    def silence(self) -> None:
        """Stops answering, once an answer in progress is sent."""
        self.done.set()
        if self.thread:
            self.thread.join(5)

    def end(self) -> None:
        self.silence()
        self.s.close()


class World:
    """The registry and the programs of one scenario."""

    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.work = tempfile.mkdtemp(prefix="bridge_netns-")
        self.procs: list[Proc] = []
        self.devices: list[Device] = []
        self.bridge_path = os.path.realpath(args.bridge)
        registry = self.run([args.registry, "--listen-uri", REGISTRY_URI])
        end = time.monotonic() + WAIT
        while True:
            try:
                socket.create_connection(("127.0.0.1", REGISTRY_PORT), 0.5).close()
                break
            except OSError:
                if time.monotonic() > end or registry.p.poll() is not None:
                    fail("the registry does not listen:\n" + registry.text())
                time.sleep(0.05)

    def run(self, argv: list[str]) -> Proc:
        p = Proc(argv, self.work)
        self.procs.append(p)
        return p

    def device(self, interface: str, echo: bool = False) -> Device:
        d = Device(interface, echo)
        self.devices.append(d)
        return d

    def bridge_argv(self, interface: str, more: list[str], participant: str = PARTICIPANT, registry_uri: str = REGISTRY_URI) -> list[str]:
        return [self.bridge_path, "--registry-uri", registry_uri, "--participant", participant, "--network", NETWORK,
                "--interface", interface, *more]

    def bridge(self, interface: str, more: list[str] | None = None, participant: str = PARTICIPANT) -> Proc:
        return self.run(self.bridge_argv(interface, ["--stats-every-ms", "100", *(more or [])], participant))

    def refused(self, argv: list[str], message: str) -> None:
        """The bridge ends with exit code 3 and `message` (a part of it) as the reason."""
        b = self.run(argv)
        code = b.wait()
        fails = [line for line in b.text().splitlines() if line.startswith("[bridge] FAIL setup: ")]
        check(code == 3 and len(fails) == 1 and message in fails[0],
              f"the bridge is refused (exit code 3) with a reason that has\n  {message}\nbut ended with {code} and:\n{b.text()}")

    def end(self) -> None:
        for d in self.devices:
            d.end()
        for p in self.procs:
            p.end()
        shutil.rmtree(self.work, ignore_errors=True)


def statistics(text: str, last: bool = False) -> dict[str, int]:
    lines = [line for line in text.splitlines() if "] stats " in line]
    check(lines != [], f"the bridge printed a statistics line:\n{text}")
    line = lines[-1]
    found = dict((k, int(v)) for k, v in re.findall(r"\b([a-z_]+)=([0-9]+)", line))
    check(tuple(found) == STATISTICS, f"the statistics line has the documented keys in order: {line}")
    return found


def final_statistics(bridge: Proc) -> dict[str, int]:
    """Stops the bridge as a user does (exit code 0) and reads its last statistics line."""
    code = bridge.stop()
    check(code == 0, f"the bridge stops with exit code 0, not {code}:\n{bridge.text()}")
    return statistics(bridge.text())


def wait_for_statistics(bridge: Proc, **expected: int) -> None:
    """Waits until the latest statistics line has at least the counts in `expected`."""
    def enough(lines: list[str]) -> bool:
        stats = [line for line in lines if "] stats " in line]
        if not stats:
            return False
        found = dict((k, int(v)) for k, v in re.findall(r"\b([a-z_]+)=([0-9]+)", stats[-1]))
        return all(found.get(k, 0) >= v for k, v in expected.items())

    bridge.wait_until(enough, lambda _lines: f"the statistics never reached {expected}")


def link_lines(bridge: Proc) -> list[str]:
    return [line.split("] link ", 1)[1] for line in bridge.text().splitlines() if line.startswith("[bridge] link ")]


def wait_for_link(bridge: Proc, value: str) -> None:
    def reached(lines: list[str]) -> bool:
        links = [line.split("] link ", 1)[1] for line in lines if line.startswith("[bridge] link ")]
        return bool(links) and f"link={value}" in links[-1]

    bridge.wait_until(reached, lambda _lines: f"the last link line does not say link={value}")


def start_peer(world: World, *more: str) -> Proc:
    peer = world.run([world.args.peer, "--registry-uri", REGISTRY_URI, "--network", NETWORK, "--bridge", PARTICIPANT,
                      "--timeout-ms", str(int(WAIT * 1000)), *more])
    return peer


def roundtrip(world: World) -> None:
    make("ip", "link", "add", "vcan0", "type", "vcan")
    make("ip", "link", "set", "vcan0", "up")
    echo = world.device("vcan0", echo=True)
    other = world.device("vcan0")
    # --tx-timeout-ms: the bridge sent nothing for longer than the timeout when the peer starts, and a healthy bus drops nothing.
    bridge = world.bridge("vcan0", ["--bitrate", "250000", "--tx-timeout-ms", "100"])
    started = bridge.wait_for("Ctrl+C to stop")
    check(f"(participant '{PARTICIPANT}', {REGISTRY_URI}) <-> CAN interface 'vcan0'; Ctrl+C to stop" in started,
          "the start line names the interface:\n" + started)
    check("[bridge] warning: --bitrate 250000 is ignored: vcan0 is a virtual CAN interface, which has no bit rate" in bridge.text(),
          "a bit rate on vcan is noted and ignored:\n" + bridge.text())
    first = bridge.wait_for("] link ")
    check(re.search(r"\] link bus=unknown link=up device=[0-9a-f]{16} marked=1 shared=0$", first) is not None,
          f"the first link line says the bus is not monitored (vcan), the link is up and the mark is placed:\n{first}")

    peer = start_peer(world)
    code = peer.wait(2 * WAIT)
    check(code == 0 and "[peer] PASS" in peer.text(), f"the peer's {MATRIX_FRAMES} frames came back from the device, each once "
                                                       f"(exit code {code}):\n{peer.text()}\n{bridge.text()}")
    check(len(echo.seen) == MATRIX_FRAMES, f"the device saw each frame of the matrix once, not {len(echo.seen)} (the bridge's own "
                                          f"frames do not come back to its socket as new ones):\n{bridge.text()}")
    say(f"roundtrip: {MATRIX_FRAMES} frames went to the device on vcan0 and its answers came back")

    # Another program of this computer sends on the interface: the bridge forwards what it hears.
    echo.silence()  # the answering device is silent from now on
    other.send(0x321, bytes([1, 2, 3]))
    wait_for_statistics(bridge, ext_to_silkit=MATRIX_FRAMES + 1)
    stats = final_statistics(bridge)
    check(stats["silkit_to_ext"] == MATRIX_FRAMES, f"every frame of the peer went out: {stats}")
    check(stats["own_tx_dropped"] == MATRIX_FRAMES,
          f"each of the bridge's own frames came back as a receipt and was dropped (own_tx_dropped): {stats}")
    check(stats["ext_from_devices"] == MATRIX_FRAMES + 1 and stats["ext_to_silkit"] == MATRIX_FRAMES + 1,
          f"the answers and the frame of the other program are from devices, the bridge's own frames are not: {stats}")
    check(all(stats[k] == 0 for k in ("error_frames", "ext_queue_full", "ext_tx_failures", "silkit_tx_failures", "ext_tx_dropped",
                                      "ext_rejected", "silkit_rejected")), f"nothing went wrong: {stats}")
    check("stats " in bridge.text() and "FAIL" not in bridge.text(), f"the bridge ended without a failure:\n{bridge.text()}")
    say("roundtrip: the bridge's own frames are dropped as receipts; the frame of another program is forwarded")


def link(world: World) -> None:
    make("ip", "link", "add", "vcan0", "type", "vcan")
    make("ip", "link", "set", "vcan0", "up")
    device = world.device("vcan0")
    bridge = world.bridge("vcan0", ["--tx-timeout-ms", "100"])
    bridge.wait_for("Ctrl+C to stop")
    wait_for_link(bridge, "up")

    sh("ip", "link", "set", "vcan0", "down")
    wait_for_link(bridge, "down")
    bridge.wait_for("warning: interface vcan0 is down: frames are not sent or received until it is up again (sudo ip link set vcan0 up)")
    sh("ip", "link", "set", "vcan0", "up")
    wait_for_link(bridge, "up")
    bridge.wait_for("warning: interface vcan0 is up again")
    values = [re.search(r"link=(\w+)", line).group(1) for line in link_lines(bridge)]
    check(values == ["up", "down", "up"], f"the link lines say up, down, up, each once: {values}\n{bridge.text()}")

    # After down and up the bridge hears the bus again.
    device.s.close()
    device.s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    device.s.bind(("vcan0",))
    device.send(0x100, b"\x01")
    wait_for_statistics(bridge, ext_from_devices=1)
    # ... and it sends again: the frames of the peer (no device answers) all go out, none is dropped or fails.
    peer = start_peer(world, "--answers", "none")
    code = peer.wait(2 * WAIT)
    check(code == 0, f"the peer ends well (exit code {code}):\n{peer.text()}")
    wait_for_statistics(bridge, silkit_to_ext=MATRIX_FRAMES)
    stats = statistics(bridge.text())
    check(stats["silkit_to_ext"] == MATRIX_FRAMES and stats["ext_tx_failures"] == 0 and stats["ext_tx_dropped"] == 0,
          f"after down and up the bridge sends every frame: {stats}\n{bridge.text()}")
    say("link: the link lines said up, down, up; the bridge heard the bus again and sent again")

    sh("ip", "link", "del", "vcan0")
    code = bridge.wait()
    lines = bridge.text().splitlines()
    check(code == 1 and any(line.startswith("[bridge] FAIL: the interface is gone") for line in lines),
          f"the interface removed: FAIL and exit code 1, not {code}:\n{bridge.text()}")
    say("link: with the interface removed the bridge failed with exit code 1")


def refused(world: World) -> None:
    make("ip", "link", "add", "vcan0", "type", "vcan")
    make("ip", "link", "set", "vcan0", "up")
    make("ip", "link", "add", "vcan1", "type", "vcan")  # stays down
    world.refused(world.bridge_argv("vcan7", []), "there is no CAN interface 'vcan7' on this computer; there are: vcan0 vcan1 (--list-channels tells more)")
    world.refused(world.bridge_argv("vcan1", []), "interface vcan1 is down. Bring it up, then start again: sudo ip link set vcan1 up")
    nowhere = f"silkit://127.0.0.1:{REGISTRY_PORT + 1}"
    b = world.run(world.bridge_argv("vcan0", [], registry_uri=nowhere))
    code = b.wait()
    fails = [line for line in b.text().splitlines() if "FAIL setup: " in line]
    check(code == 3 and len(fails) == 1 and f"cannot connect to the SIL Kit registry at {nowhere} (" in fails[0]
          and fails[0].endswith(f"Start the registry first ('sil-kit-registry --listen-uri {nowhere}'), or give the address it listens at "
                                "with --registry-uri."), f"no registry: exit code 3, where the bridge looked and what to do:\n{b.text()}")
    # The usage error is exit code 4.
    u = world.run([world.bridge_path, "--registry-uri", REGISTRY_URI])
    check(u.wait() == 4 and "error: missing --interface" in u.text(), f"a missing --interface is a usage error:\n{u.text()}")
    n = world.run([world.bridge_path, "--registry-uri", REGISTRY_URI, "--interface", "vcan0"])
    check(n.wait() == 4 and "error: missing --network: name the SIL Kit CAN network to bridge" in n.text(),
          f"a missing --network is a usage error, the bridge joins no network by default:\n{n.text()}")
    say("refused: no interface, an interface down and no registry end with exit code 3 and the way to fix it")

    text = subprocess.run([world.bridge_path, "--list-channels"], capture_output=True, text=True)
    check(text.returncode == 0 and text.stdout == "CAN interfaces (--interface):\n  vcan0  virtual CAN, up\n  vcan1  virtual CAN, down\n",
          f"--list-channels:\n{text.stdout}{text.stderr}")
    listed = json.loads(subprocess.run([world.bridge_path, "--list-channels", "--json"], capture_output=True, text=True, check=True).stdout)
    targets = {t["value"]: t for t in listed["targets"]}
    check(listed["vehicle_sil_targets"] == 1 and listed["tool"] == "vehicle_sil_socketcan_bridge" and listed["error"] is None
          and list(targets) == ["vcan0", "vcan1"], f"--list-channels --json names the interfaces:\n{listed}")
    check(targets["vcan0"]["can"] == {"kind": "vcan", "up": True, "bitrate": None, "state": None, "restart_ms": None}
          and targets["vcan1"]["can"]["up"] is False and targets["vcan1"]["usable"] is True
          and "down" in targets["vcan1"]["detail"] and targets["vcan0"]["in_use"] is False
          and targets["vcan0"]["in_use_by"] == [] and targets["vcan0"]["in_use_count"] == 0,
          f"--list-channels --json: kind, up, bit rate, state; an interface that is down is usable:\n{targets}")
    say("refused: --list-channels says the interfaces, also one that is down")

    b = world.bridge("vcan0")
    b.wait_for("Ctrl+C to stop")
    check(b.stop() == 0, f"SIGTERM stops the bridge with exit code 0:\n{b.text()}")


def shared(world: World) -> None:
    make("ip", "link", "add", "vcan0", "type", "vcan")
    make("ip", "link", "set", "vcan0", "up")
    first = world.bridge("vcan0", participant="First")
    first.wait_for("Ctrl+C to stop")
    check("shared=0" in first.wait_for("] link "), f"the first bridge is alone:\n{first.text()}")
    second = world.bridge("vcan0", participant="Second")
    second.wait_for("Ctrl+C to stop")
    first_pid, second_pid = first.p.pid, second.p.pid
    second.wait_for(f"warning: CAN interface 'vcan0' is also used by another vehicle-sil bridge (process {first_pid})")
    first.wait_for(f"warning: CAN interface 'vcan0' is also used by another vehicle-sil bridge (process {second_pid})")
    first.wait_for(f"shared=1 peers={second_pid}")
    listed = json.loads(subprocess.run([world.bridge_path, "--list-channels", "--json"], capture_output=True, text=True, check=True).stdout)
    target = listed["targets"][0]
    check(target["in_use"] is True and sorted(target["in_use_by"]) == sorted([first_pid, second_pid]) and target["in_use_count"] == 2
          and target["usable"] is True, f"--list-channels says the interface is in use by both bridges:\n{target}")
    say("shared: both bridges warned, the link lines name the other, the list says in use")

    before = first.count()
    check(second.stop() == 0, "the second bridge stops")
    first.wait_for("shared=0", after=before)
    check(first.stop() == 0, "the first bridge stops")
    say("shared: when the second bridge ended, the first one's shared went back to 0")


def launch(world: World) -> None:
    make("ip", "link", "add", "vcan0", "type", "vcan")
    make("ip", "link", "set", "vcan0", "up")
    device = world.device("vcan0")
    # The device reads what the simulation sends, and sends a frame of its own now and then (a 29-bit one, as a J1939
    # device would).
    from_simulation: list[int] = []
    stop = threading.Event()

    def listen() -> None:
        while not stop.is_set():
            try:
                raw = device.s.recv(FRAME.size)
            except (socket.timeout, TimeoutError):
                continue
            except OSError:
                return
            can_id, length, _ = FRAME.unpack(raw)
            if can_id & CAN_EFF_FLAG:
                from_simulation.append(can_id & 0x1FFFFFFF)

    def talk() -> None:
        while not stop.wait(0.2):
            try:
                device.send(0x18FEF1F9 & 0x1FFFFFFF, b"\x01\x02\x03\x04", extended=True)
            except OSError:
                return

    threads = [threading.Thread(target=listen, daemon=True), threading.Thread(target=talk, daemon=True)]
    for t in threads:
        t.start()
    with open(os.path.join(world.work, "sim.yaml"), "w", encoding="utf-8") as f:
        f.write("vehicle_sil_simulation: 2\n"
                "registry_uri: silkit://127.0.0.1:0\n"
                "networks:\n"
                "  - name: J1939_CAN\n"
                "    kind: can\n"
                "participants:\n"
                "  - name: Engine\n"
                f"    command: [{json.dumps(world.args.ecu)}]\n"
                "    step_ms: 10\n")
    # The launcher finds the bridge on PATH (it is not next to vehicle_sil_launch in the build).
    env = dict(os.environ, PATH=os.path.dirname(world.bridge_path) + os.pathsep + os.environ.get("PATH", ""))
    run = subprocess.Popen([world.args.launch, "run", "--events", "--stop-at-ms", "3000", "--speed", "1", "--log-dir", "logs", "--connect", "J1939_CAN=vcan0", "sim.yaml"],
                           cwd=world.work, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, start_new_session=True)
    try:
        text, _ = run.communicate(timeout=4 * WAIT)
    except subprocess.TimeoutExpired:
        os.killpg(run.pid, signal.SIGKILL)
        fail("the run did not end:\n" + (run.communicate()[0] or ""))
    finally:
        stop.set()
        for t in threads:
            t.join(5)
    events = [json.loads(line) for line in text.splitlines() if line.startswith("{\"event\":")]
    results = [e["result"] for e in events if e["event"] == "result"]
    check(run.returncode == 0 and len(results) == 1, f"the run ends with exit code 0 (not {run.returncode}):\n{text}")
    result = results[0]
    outside = result["outside"]
    check(len(outside) == 1 and {k: outside[0][k] for k in ("network", "target", "participant")}
          == {"network": "J1939_CAN", "target": "vcan0", "participant": "bridge.J1939_CAN"},
          f"the result says where J1939_CAN was connected:\n{outside}")
    check(isinstance(outside[0]["to_outside"], int) and outside[0]["to_outside"] >= 1
          and isinstance(outside[0]["from_outside"], int) and outside[0]["from_outside"] >= 1,
          f"the result counts the frames to the device and from it:\n{outside}")
    check(len(from_simulation) >= 1, f"the frames of the ECU reached the device on vcan0 ({len(from_simulation)} seen):\n{outside}")
    # The bridge's last statistics line (in its log): the frames of the device came in, and the ECU's frames went out.
    with open(os.path.join(world.work, "logs", "bridge.J1939_CAN.log"), encoding="utf-8") as f:
        logged = [line for line in f.read().splitlines() if "] stats " in line]
    check(logged != [], f"the bridge's log has its statistics line:\n{text}")
    stats = dict(pair.split("=") for pair in logged[-1].split("] stats ", 1)[1].split())
    check(int(stats["silkit_to_ext"]) >= 1 and int(stats["ext_from_devices"]) >= 1,
          f"the bridge forwarded the ECU's frames to the device and the device's frames to the run: {stats}")
    bridge = [p for p in result["participants"] if p["name"] == "bridge.J1939_CAN"]
    check(len(bridge) == 1 and bridge[0]["for_network"] == "J1939_CAN" and bridge[0]["ended"] == "stopped"
          and bridge[0]["exit_code"] == 0, f"the launcher stopped the bridge at the end, with exit code 0:\n{bridge}")
    started = [e for e in events if e["event"] == "participant" and e.get("name") == "bridge.J1939_CAN" and e["state"] == "started"]
    check(len(started) == 1 and started[0]["command"][0] == world.bridge_path
          and started[0]["command"][1:5] == ["--network", "J1939_CAN", "--interface", "vcan0"],
          f"the launcher started the SocketCAN bridge of PATH on vcan0:\n{started}")
    say(f"launch: run --connect J1939_CAN=vcan0 started the SocketCAN bridge; {len(from_simulation)} frames reached the device "
        f"and {stats['ext_from_devices']} of its frames reached the run")


def inner(args: argparse.Namespace) -> None:
    # As root of the user namespace. A /tmp of its own: SIL Kit's local-domain sockets are paths there.
    r = subprocess.run(["mount", "-t", "tmpfs", "none", "/tmp"], capture_output=True, text=True)
    if r.returncode != 0:
        skip_no_namespace("mount -t tmpfs none /tmp: " + r.stderr.strip())
    r = subprocess.run(["ip", "link", "set", "lo", "up"], capture_output=True, text=True)
    if r.returncode != 0:
        skip_no_namespace("ip link set lo up: " + (r.stderr.strip() or f"exit {r.returncode}"))
    world = World(args)
    try:
        {"roundtrip": roundtrip, "link": link, "refused": refused, "shared": shared, "launch": launch}[args.scenario](world)
    finally:
        world.end()
    say("PASS")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scenario", required=True, choices=("roundtrip", "link", "refused", "shared", "launch"))
    parser.add_argument("--bridge", required=True, help="vehicle_sil_socketcan_bridge")
    parser.add_argument("--peer", required=True, help="vehicle_sil_vector_xl_it_peer")
    parser.add_argument("--registry", required=True, help="sil-kit-registry")
    parser.add_argument("--launch", help="vehicle_sil_launch (launch)")
    parser.add_argument("--ecu", help="vehicle_sil_engine_ecu (launch)")
    parser.add_argument("--inner", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.scenario == "launch" and not (args.launch and args.ecu):
        parser.error("--scenario launch needs --launch and --ecu")
    args.bridge, args.peer, args.registry = (os.path.abspath(p) for p in (args.bridge, args.peer, args.registry))
    args.launch, args.ecu = (os.path.abspath(p) if p else p for p in (args.launch, args.ecu))
    if args.inner:
        inner(args)
    else:
        outer(args)


if __name__ == "__main__":
    main()
