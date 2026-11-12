# SPDX-License-Identifier: MIT
"""Python client of the vehicle-sil Control API (gRPC). See docs/control-api.md.

The names are loaded on first use, so `python -m vehicle_sil_control.generate` works before the
gRPC stubs exist, and Launch, LaunchError and control_address (the launcher as a child process,
standard library only) without gRPC.
"""
_LAUNCHER = ["Launch", "LaunchError", "control_address"]
__all__ = ["Control", "ControlError", "ControlWarning", "EventInfo", "Operation", "Peer", "SimulationState", "Target",
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
