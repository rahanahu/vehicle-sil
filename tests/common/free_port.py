# SPDX-License-Identifier: MIT
"""Pick a free TCP port on 127.0.0.1 below the OS ephemeral range.

Standard library only. Kept under tests/common/ alongside timeout_scale.py so Python
tests under tests/ can share it without adding tests/ itself to sys.path.
"""

from __future__ import annotations

import pathlib
import random
import socket


def free_port() -> int:
    """Return a TCP port on 127.0.0.1 that nothing listens on now, for a test that binds it later.

    Chosen at random below the OS ephemeral port range (/proc/sys/net/ipv4/ip_local_port_range on
    Linux, 49152 elsewhere) so parallel tests binding port 0 or opening outgoing connections cannot
    steal the port before the test binds it. Falls back to port 0 if none of the candidates are free.
    """
    below = 49152
    try:
        below = int(pathlib.Path("/proc/sys/net/ipv4/ip_local_port_range").read_text().split()[0])
    except (OSError, ValueError, IndexError):
        pass
    candidates = random.sample(range(20000, below), 100) if below - 20000 >= 1000 else []
    for port in [*candidates, 0]:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(("127.0.0.1", port))
            except OSError:
                continue
            return int(s.getsockname()[1])
    raise OSError("no free port on 127.0.0.1")
