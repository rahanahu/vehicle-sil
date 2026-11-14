#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""vehicle_sil_ethernet_bridge on real Linux interfaces, without root: a TAP device and a veth pair
made in a network namespace of a user namespace (as tests/multi_host/two_hosts.py makes its two
computers), with a SIL Kit registry and a SIL Kit peer (eth_bridge_it_peer --mode answer) inside.
The "device outside" is this script, with packet sockets of its own.

  bridge_netns.py --scenario <tap|packet> --bridge <vehicle_sil_ethernet_bridge>
                  --peer <eth_bridge_it_peer> --registry <sil-kit-registry>

tap     the bridge on a TAP device made before it starts: frames from a packet socket on the
        device reach the SIL Kit peer unchanged and its answers come back; a frame of another
        EtherType is filtered; what the bridge writes to the device does not come back to it
        (loop_suspected stays 0). A second bridge on the same device is refused (busy).
        --tap-create: the device is made down (the bridge warns and tells when it is up) and is
        gone when the bridge stops. The messages for a device that is missing, not a TAP, there
        already, and (with the capabilities dropped) not allowed to be made. The device removed
        while the bridge runs: the bridge fails with exit code 1.
packet  the bridge on one end of a veth pair, this script on the other: frames with no VLAN tag,
        one tag and two tags pass both ways with their tags; a frame another socket sends on the
        bridge's own interface reaches the SIL Kit peer; what the bridge sends does not come back
        to it. --promiscuous sets the interface's promiscuity while the bridge runs only. The
        messages for an interface that is missing, not Ethernet, and (with the capability
        dropped) not allowed to be opened. The link down and up: the monitor lines. The interface
        removed: exit code 1.

Exit code 77 (skipped), with the reason, where this cannot be made:
 - no user namespaces (not Linux; no unshare or ip; unprivileged user namespaces turned off): as
   two_hosts.py, a failure instead with VEHICLE_SIL_REQUIRE_MULTI_HOST=1;
 - the namespace is there, but a TAP device, a veth pair or a packet socket cannot be made in it:
   a failure instead with VEHICLE_SIL_REQUIRE_ETHERNET_NETNS=1.
Any failure after that fails the test.

EtherType 0x88B5 is known to be free for local experiments (not checked against the standard).
"""
import argparse
import os
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

SKIP = 77
REGISTRY_PORT = 18500
REGISTRY_URI = f"silkit://127.0.0.1:{REGISTRY_PORT}"
NETWORK = "ETH_NETNS"

PEER_MAC = bytes.fromhex("020000000001")  # the SIL Kit side
DEVICE_MAC = bytes.fromhex("020000000002")  # this script, outside
OTHER_MAC = bytes.fromhex("020000000003")  # another program on the bridge's own interface
BROADCAST = b"\xff" * 6
EXPERIMENTAL = 0x88B5
OTHER_ETHERTYPE = 0x88B6

# linux/if_packet.h
ETH_P_ALL = 0x0003
SOL_PACKET = 263
PACKET_AUXDATA = 8
PACKET_OUTGOING = 4
TP_STATUS_VLAN_VALID = 1 << 4
TP_STATUS_VLAN_TPID_VALID = 1 << 6

SCALE = float(os.environ.get("VEHICLE_SIL_TEST_TIMEOUT_SCALE") or 1)
WAIT = 30 * SCALE

STATISTICS = ("silkit_to_ext", "ext_to_silkit", "own_tx_dropped", "loop_suspected", "mac_unguarded", "filtered_to_ext",
              "filtered_from_ext", "oversize", "malformed", "ext_overruns", "ext_queue_full", "ext_tx_failures",
              "silkit_tx_failures")


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


def skip_no_interface(why: str) -> None:
    if os.environ.get("VEHICLE_SIL_REQUIRE_ETHERNET_NETNS") == "1":
        fail(f"the interfaces cannot be made, and VEHICLE_SIL_REQUIRE_ETHERNET_NETNS=1 asks for them: {why}")
    say(f"skipped: the user namespace is there, but the interfaces of the test cannot be made in it: {why}")
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
                          "--scenario", args.scenario, "--bridge", args.bridge, "--peer", args.peer, "--registry", args.registry],
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
        for line in self.p.stdout:
            with self.lock:
                self.lines.append(line.rstrip("\n"))
                self.lock.notify_all()

    def text(self) -> str:
        with self.lock:
            return "\n".join(self.lines)

    def wait_for(self, part: str, timeout: float = WAIT) -> str:
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
    """A step that makes the interfaces: where it fails, the test cannot run here (skip)."""
    r = subprocess.run(argv, capture_output=True, text=True)
    if r.returncode != 0:
        skip_no_interface(f"{' '.join(argv)}: {r.stderr.strip() or f'exit {r.returncode}'}")


def sh(*argv: str) -> str:
    """A step of the test once the interfaces are made: where it fails, the test fails."""
    r = subprocess.run(argv, capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"{' '.join(argv)}: {r.stderr.strip() or f'exit {r.returncode}'}")
    return r.stdout


def frame(destination: bytes, source: bytes, ether_type: int, payload_bytes: int, first: int, tags: bytes = b"") -> bytes:
    payload = bytes((first + i) & 0xFF for i in range(payload_bytes))
    return destination + source + tags + struct.pack("!H", ether_type) + payload


def tag(tpid: int, tci: int) -> bytes:
    return struct.pack("!HH", tpid, tci)


def answer_to(request: bytes) -> bytes:
    """What the SIL Kit peer sends back: to the source, from its own address, the rest unchanged."""
    return request[6:12] + PEER_MAC + request[12:]


class Wire:
    """A packet socket on one interface: a device (or another program) there."""

    def __init__(self, interface: str):
        try:
            self.s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        except OSError as e:
            skip_no_interface(f"a packet socket: {e}")
        self.s.bind((interface, 0))
        self.s.setsockopt(SOL_PACKET, PACKET_AUXDATA, 1)
        self.seen: list[tuple[bytes, int]] = []  # every frame of the test's EtherTypes: (bytes, packet type)

    def send(self, data: bytes) -> None:
        self.s.send(data)

    def _receive(self, timeout: float) -> tuple[bytes, int] | None:
        self.s.settimeout(timeout)
        try:
            data, ancillary, _, address = self.s.recvmsg(65536, socket.CMSG_SPACE(20))
        except (socket.timeout, TimeoutError):
            return None
        # The kernel takes the outer VLAN tag out of a received frame and tells it beside the frame.
        for level, kind, value in ancillary:
            if level == SOL_PACKET and kind == PACKET_AUXDATA and len(value) >= 20:
                status, _, _, _, _, tci, tpid = struct.unpack("IIIHHHH", value[:20])
                if tci or status & TP_STATUS_VLAN_VALID:
                    data = data[:12] + tag(tpid if status & TP_STATUS_VLAN_TPID_VALID else 0x8100, tci) + data[12:]
        return data, address[2]

    def expect(self, wanted: bytes, what: str, outgoing: bool = False) -> None:
        """Waits for the frame `wanted` (as received, or as sent by another socket on this interface)."""
        end = time.monotonic() + WAIT
        while time.monotonic() < end:
            got = self._receive(0.5)
            if got is None:
                continue
            data, kind = got
            if EXPERIMENTAL.to_bytes(2, "big") not in data[12:22] and OTHER_ETHERTYPE.to_bytes(2, "big") not in data[12:22]:
                continue  # the kernel's own frames (IPv6 neighbour discovery)
            self.seen.append((data, kind))
            if data == wanted and (kind == PACKET_OUTGOING) == outgoing:
                return
        fail(f"{what}: the frame {wanted.hex()} did not arrive; the frames of the test seen here: "
             + ", ".join(f"{d.hex()} (packet type {k})" for d, k in self.seen))


def statistics(text: str) -> dict[str, int]:
    lines = [line for line in text.splitlines() if "] stats " in line]
    check(len(lines) == 1, f"the bridge prints one statistics line:\n{text}")
    found = dict((k, int(v)) for k, v in re.findall(r"\b([a-z_]+)=([0-9]+)", lines[0]))
    check(tuple(found) == STATISTICS, f"the statistics line has the documented keys in order: {lines[0]}")
    return found


class World:
    """The registry, the SIL Kit peer, and the bridges of one scenario."""

    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.work = tempfile.mkdtemp(prefix="bridge_netns-")
        self.procs: list[Proc] = []
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
        self.peer: Proc | None = None

    def run(self, argv: list[str]) -> Proc:
        p = Proc(argv, self.work)
        self.procs.append(p)
        return p

    def bridge_argv(self, backend: str, interface: str, more: list[str], participant: str) -> list[str]:
        return [self.bridge_path, "--registry-uri", REGISTRY_URI, "--participant", participant, "--network", NETWORK,
                "--backend", backend, "--interface", interface, *more]

    def bridge(self, backend: str, interface: str, more: list[str], participant: str = "EthernetBridge",
               before: list[str] | None = None) -> Proc:
        return self.run((before or []) + self.bridge_argv(backend, interface, more, participant))

    def start_peer(self, bridge_participant: str) -> None:
        self.peer = self.run([self.args.peer, "--registry-uri", REGISTRY_URI, "--network", NETWORK, "--bridge", bridge_participant,
                              "--mode", "answer", "--mac", "02:00:00:00:00:01", "--timeout-ms", str(int(WAIT * 1000))])
        self.peer.wait_for("[peer] ready")

    def refused(self, backend: str, interface: str, more: list[str], message: str, participant: str,
                before: list[str] | None = None) -> None:
        """The bridge ends with exit code 3 and exactly `message` as the reason."""
        b = self.bridge(backend, interface, more, participant, before)
        code = b.wait()
        line = f"[eth-bridge] FAIL setup: {message}"
        check(code == 3 and line in b.text().splitlines(), f"the bridge is refused (exit code 3) with\n  {line}\nbut ended with "
                                                           f"{code} and:\n{b.text()}")

    def end(self) -> None:
        for p in self.procs:
            p.end()
        shutil.rmtree(self.work, ignore_errors=True)


def round_trips(world: World, wire: Wire, requests: list[bytes], what: str) -> None:
    """Each request sent on `wire` reaches the SIL Kit peer unchanged, and its answer comes back."""
    for request in requests:
        wire.send(request)
        wire.expect(answer_to(request), what)
        world.peer.wait_for(f"[peer] rx {request.hex()}")
    # Our own frames do not come back to the socket that sent them.
    own = [d.hex() for d, _ in wire.seen if d in requests]
    check(not own, f"{what}: the packet socket got frames it sent itself: {own}")


def no_capability(*names: str) -> list[str] | None:
    """A prefix that runs a program without the capabilities `names`, or None without setpriv."""
    if shutil.which("setpriv") is None:
        return None
    return ["setpriv", "--bounding-set", ",".join("-" + n for n in names), "--inh-caps", "-all"]


def interfaces_here() -> str:
    return ", ".join(name for _, name in socket.if_nameindex())


def tap(world: World) -> None:
    make("ip", "tuntap", "add", "dev", "vsil0", "mode", "tap")
    make("ip", "link", "set", "vsil0", "up")
    wire = Wire("vsil0")

    bridge = world.bridge("tap", "vsil0", ["--ethertype", "0x88b5"])
    started = bridge.wait_for("Ctrl+C to stop")
    check(f"(participant 'EthernetBridge', {REGISTRY_URI}) <-> tap 'vsil0' (TAP device in this computer): EtherType 0x88b5 only, "
          "both ways; MAC guard on; Ctrl+C to stop" in started, "the start line names the TAP device:\n" + started)
    world.start_peer("EthernetBridge")

    requests = [frame(PEER_MAC, DEVICE_MAC, EXPERIMENTAL, 46, 1), frame(BROADCAST, DEVICE_MAC, EXPERIMENTAL, 100, 2),
                frame(PEER_MAC, DEVICE_MAC, EXPERIMENTAL, 1500, 3)]
    round_trips(world, wire, requests, "tap")
    wire.send(frame(PEER_MAC, DEVICE_MAC, OTHER_ETHERTYPE, 46, 4))  # not let in
    # One more round trip behind it, so that the bridge has seen the frame above when it stops.
    last = frame(PEER_MAC, DEVICE_MAC, EXPERIMENTAL, 46, 5)
    round_trips(world, wire, [last], "tap")
    requests.append(last)
    say("tap: frames from the device reached the SIL Kit peer and its answers came back")

    # A TAP device serves one program.
    world.refused("tap", "vsil0", [], "cannot attach to TAP 'vsil0': Device or resource busy. Another program has it open, and a "
                  "TAP device serves one program: stop that program, or use another TAP device.", "SecondBridge")

    check(bridge.stop() == 0, "the bridge stops with exit code 0:\n" + bridge.text())
    stats = statistics(bridge.text())
    check(stats["ext_to_silkit"] == len(requests) and stats["silkit_to_ext"] == len(requests),
          f"the bridge forwarded {len(requests)} frames each way: {stats}")
    check(stats["filtered_from_ext"] >= 1, f"the frame of another EtherType was filtered: {stats}")
    # The answers the bridge wrote to the device have the SIL Kit peer's source address: had one
    # come back to the bridge, it would have counted it (the link says it does not echo).
    check(stats["loop_suspected"] == 0 and stats["own_tx_dropped"] == 0 and "warning:" not in bridge.text(),
          f"nothing the bridge wrote to the TAP device came back to it:\n{bridge.text()}")
    say("tap: what the bridge wrote did not come back to it (loop_suspected=0, own_tx_dropped=0, no warning)")

    # --tap-create: made down, gone with the bridge.
    made = world.bridge("tap", "vsil1", ["--tap-create", "--ethertype", "0x88b5"], "CreatingBridge")
    made.wait_for("<-> tap 'vsil1' (TAP device in this computer, made by the bridge)")
    made.wait_for("[eth-bridge] warning: interface 'vsil1' is down: no frame passes until it is up. Bring it up with 'sudo ip "
                  "link set vsil1 up'.")
    sh("ip", "link", "set", "vsil1", "up")
    made.wait_for("[eth-bridge] interface vsil1: link up")
    wire1 = Wire("vsil1")
    round_trips(world, wire1, [frame(PEER_MAC, DEVICE_MAC, EXPERIMENTAL, 46, 6)], "tap made by the bridge")
    check(made.stop() == 0, "the bridge that made its TAP device stops with exit code 0:\n" + made.text())
    check(subprocess.run(["ip", "link", "show", "vsil1"], capture_output=True).returncode != 0,
          "the TAP device the bridge made is gone when the bridge has stopped")
    say("tap: --tap-create made the device, warned that it was down, told when it was up, and removed it")

    # What is refused, and how the message tells to fix it.
    world.refused("tap", "nope0", [],
                  "no TAP device 'nope0'. Make it first: 'sudo ip tuntap add dev nope0 mode tap user $USER' and 'sudo ip link set "
                  "nope0 up'; or let the bridge make it while it runs with --tap-create (needs CAP_NET_ADMIN). Interfaces here: "
                  f"{interfaces_here()}.", "Refused1")
    world.refused("tap", "lo", [], "interface 'lo' is not a TAP device. For a network interface use --backend packet; a TAP device "
                  "is made with 'sudo ip tuntap add dev <name> mode tap user $USER'.", "Refused2")
    world.refused("tap", "vsil0", ["--tap-create"], "cannot create TAP 'vsil0': an interface with that name exists. Leave out "
                  "--tap-create to attach to it, or choose another name.", "Refused3")
    world.refused("tap", "an-interface-name-too-long", [],
                  "interface name 'an-interface-name-too-long' cannot be used: Linux interface names have 1 to 15 characters. "
                  "Give the name as --list-interfaces prints it.", "Refused4")
    without = no_capability("net_admin")
    if without is None:
        say("tap: NOT CHECKED (no setpriv): the message when the TAP device may not be made")
    else:
        world.refused("tap", "vsil2", ["--tap-create"],
                      "cannot create TAP 'vsil2': Operation not permitted. Creating a TAP device needs CAP_NET_ADMIN: make it "
                      "first with 'sudo ip tuntap add dev vsil2 mode tap user $USER' and 'sudo ip link set vsil2 up' and leave "
                      "out --tap-create, or run as root.", "Refused5", before=without)
    say("tap: the messages for a device that is missing, not a TAP, there already, badly named, not allowed")

    # The device removed while the bridge runs.
    last_bridge = world.bridge("tap", "vsil0", [], "LastBridge")
    last_bridge.wait_for("Ctrl+C to stop")
    sh("ip", "link", "del", "vsil0")
    code = last_bridge.wait()
    check(code == 1 and "[eth-bridge] FAIL: interface 'vsil0' is gone (removed or renamed while the bridge ran)" in last_bridge.text()
          and "] stats silkit_to_ext=" in last_bridge.text(),
          f"the bridge fails (exit code 1, not {code}) when its TAP device is removed, with its statistics:\n{last_bridge.text()}")
    say("tap: the device removed while the bridge ran: FAIL and exit code 1")


def packet(world: World) -> None:
    make("ip", "link", "add", "va", "type", "veth", "peer", "name", "vb")
    make("ip", "link", "set", "va", "up")
    make("ip", "link", "set", "vb", "up")
    far = Wire("vb")  # the device at the other end of the cable
    near = Wire("va")  # another program on the bridge's own interface

    check("promiscuity 0" in sh("ip", "-d", "link", "show", "va"), "va is not promiscuous before the bridge starts")
    bridge = world.bridge("packet", "va", ["--ethertype", "0x88b5", "--promiscuous"])
    started = bridge.wait_for("Ctrl+C to stop")
    check(f"(participant 'EthernetBridge', {REGISTRY_URI}) <-> packet 'va' (network interface): EtherType 0x88b5 only, both ways; "
          "promiscuous on; MAC guard on; Ctrl+C to stop" in started, "the start line names the interface:\n" + started)
    check("promiscuity 1" in sh("ip", "-d", "link", "show", "va"), "--promiscuous makes va promiscuous while the bridge runs")
    world.start_peer("EthernetBridge")

    one_tag = tag(0x8100, 0x6005)  # priority 3, VLAN 5
    requests = [frame(PEER_MAC, DEVICE_MAC, EXPERIMENTAL, 46, 1),
                frame(PEER_MAC, DEVICE_MAC, EXPERIMENTAL, 46, 2, one_tag),
                frame(BROADCAST, DEVICE_MAC, EXPERIMENTAL, 60, 3, tag(0x8100, 0x0001)),
                frame(PEER_MAC, DEVICE_MAC, EXPERIMENTAL, 46, 4, tag(0x88A8, 0x0064) + tag(0x8100, 0x0005)),
                frame(PEER_MAC, DEVICE_MAC, EXPERIMENTAL, 1500, 5),
                frame(PEER_MAC, DEVICE_MAC, EXPERIMENTAL, 1500, 6, one_tag)]
    round_trips(world, far, requests, "packet")
    say("packet: frames with no tag, one VLAN tag and two tags passed both ways with their tags")

    # Another program on this computer sends on the bridge's own interface: the bridge sees it,
    # and that program's socket sees the bridge's answer go out.
    local = frame(PEER_MAC, OTHER_MAC, EXPERIMENTAL, 46, 7, one_tag)
    near.send(local)
    world.peer.wait_for(f"[peer] rx {local.hex()}")
    near.expect(answer_to(local), "another socket on the bridge's interface", outgoing=True)
    far.expect(answer_to(local), "the far end, for the frame of another socket")
    requests.append(local)
    say("packet: a frame another socket sent on the bridge's interface reached the SIL Kit peer")

    far.send(frame(PEER_MAC, DEVICE_MAC, OTHER_ETHERTYPE, 46, 8))  # not let in
    last = frame(PEER_MAC, DEVICE_MAC, EXPERIMENTAL, 46, 9)
    round_trips(world, far, [last], "packet")
    requests.append(last)

    # The link down and up again: the monitor lines.
    sh("ip", "link", "set", "vb", "down")
    bridge.wait_for("[eth-bridge] interface va: link down")
    sh("ip", "link", "set", "vb", "up")
    bridge.wait_for("[eth-bridge] interface va: link up")
    say("packet: the link down and up again: the bridge said so")

    check(bridge.stop() == 0, "the bridge stops with exit code 0:\n" + bridge.text())
    stats = statistics(bridge.text())
    check(stats["ext_to_silkit"] == len(requests) and stats["silkit_to_ext"] == len(requests),
          f"the bridge forwarded {len(requests)} frames each way: {stats}")
    check(stats["filtered_from_ext"] >= 1, f"the frame of another EtherType was filtered: {stats}")
    check(stats["loop_suspected"] == 0 and stats["own_tx_dropped"] == 0 and "warning:" not in bridge.text(),
          f"nothing the bridge sent on its socket came back to it:\n{bridge.text()}")
    say("packet: what the bridge sent did not come back to it (loop_suspected=0, own_tx_dropped=0, no warning)")
    check("promiscuity 0" in sh("ip", "-d", "link", "show", "va"), "va is not promiscuous any more once the bridge has stopped")

    world.refused("packet", "nope0", ["--all-frames"],
                  f"no interface 'nope0' for --backend packet. Interfaces here: {interfaces_here()} (run --list-interfaces).",
                  "Refused1")
    world.refused("packet", "lo", ["--all-frames"],
                  "interface 'lo' is not Ethernet (link type 772): --backend packet carries Ethernet frames only. Choose an "
                  "Ethernet interface (run --list-interfaces).", "Refused2")
    without = no_capability("net_raw")
    if without is None:
        say("packet: NOT CHECKED (no setpriv): the message when the packet socket may not be opened")
    else:
        world.refused("packet", "va", ["--all-frames"],
                      "cannot open a packet socket on 'va': Operation not permitted. A network interface needs CAP_NET_RAW: run "
                      f"'sudo setcap cap_net_raw+ep {world.bridge_path}' once, or run as root.", "Refused3", before=without)
    say("packet: the messages for an interface that is missing, not Ethernet, not allowed")

    last_bridge = world.bridge("packet", "va", ["--all-frames"], "LastBridge")
    last_bridge.wait_for("Ctrl+C to stop")
    sh("ip", "link", "del", "va")
    code = last_bridge.wait()
    check(code == 1 and "[eth-bridge] FAIL: interface 'va' is gone (removed or renamed while the bridge ran)" in last_bridge.text()
          and "] stats silkit_to_ext=" in last_bridge.text(),
          f"the bridge fails (exit code 1, not {code}) when its interface is removed, with its statistics:\n{last_bridge.text()}")
    say("packet: the interface removed while the bridge ran: FAIL and exit code 1")


def inner(args: argparse.Namespace) -> None:
    # As root of the user namespace. A /tmp of its own: SIL Kit's local-domain sockets are paths there.
    r = subprocess.run(["mount", "-t", "tmpfs", "none", "/tmp"], capture_output=True, text=True)
    if r.returncode != 0:
        skip_no_namespace("mount -t tmpfs none /tmp: " + r.stderr.strip())
    make("ip", "link", "set", "lo", "up")
    world = World(args)
    try:
        if args.scenario == "tap":
            tap(world)
        else:
            packet(world)
    finally:
        world.end()
    say("PASS")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scenario", required=True, choices=("tap", "packet"))
    parser.add_argument("--bridge", required=True, help="vehicle_sil_ethernet_bridge")
    parser.add_argument("--peer", required=True, help="eth_bridge_it_peer")
    parser.add_argument("--registry", required=True, help="sil-kit-registry")
    parser.add_argument("--inner", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.bridge, args.peer, args.registry = (os.path.abspath(p) for p in (args.bridge, args.peer, args.registry))
    if args.inner:
        inner(args)
    else:
        outer(args)


if __name__ == "__main__":
    main()
