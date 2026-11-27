# SPDX-License-Identifier: MIT
"""Python client of the vehicle-sil Control API (gRPC). See docs/control-api.md.

The names are loaded on first use, so `python -m vehicle_sil_control.generate` works before the
gRPC stubs exist, and Launch, LaunchError, control_address, Serve, RunHandle and ServeError
(the launcher as a child process, standard library only) without gRPC.
"""
# The vehicle-sil release of this client. A literal, not the package metadata (the tools archive puts the
# client in place without installing it); scripts/package.py version keeps it equal to CMakeLists.txt.
__version__ = "0.5.0"

_LAUNCHER = ["Launch", "LaunchError", "control_address"]
_SERVE = ["RunHandle", "Serve", "ServeError"]
__all__ = ["Control", "ControlError", "ControlWarning", "EventInfo", "Operation", "Peer", "SimulationState", "Snapshot", "Target",
           "TimeState", "Values", "Variable",
           "ms", "s"] + _LAUNCHER + _SERVE


def __getattr__(name):
    if name in _LAUNCHER:
        from . import launcher

        return getattr(launcher, name)
    if name in _SERVE:
        from . import serve

        return getattr(serve, name)
    if name in __all__:
        from . import client

        return getattr(client, name)
    raise AttributeError(name)
