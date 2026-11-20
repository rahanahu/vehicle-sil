# SPDX-License-Identifier: MIT
"""Python client of the vehicle-sil Control API (gRPC). See docs/control-api.md.

The names are loaded on first use, so `python -m vehicle_sil_control.generate` works before the
gRPC stubs exist, and Launch, LaunchError and control_address (the launcher as a child process,
standard library only) without gRPC.
"""
# The vehicle-sil release of this client. A literal, not the package metadata (the tools archive puts the
# client in place without installing it); scripts/package.py version keeps it equal to CMakeLists.txt.
__version__ = "0.2.0"

_LAUNCHER = ["Launch", "LaunchError", "control_address"]
__all__ = ["Control", "ControlError", "ControlWarning", "EventInfo", "Operation", "Peer", "SimulationState", "Snapshot", "Target",
           "TimeState", "Values", "Variable",
           "ms", "s"] + _LAUNCHER


def __getattr__(name):
    if name in _LAUNCHER:
        from . import launcher

        return getattr(launcher, name)
    if name in __all__:
        from . import client

        return getattr(client, name)
    raise AttributeError(name)
