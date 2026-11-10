# SPDX-License-Identifier: MIT
"""Python client of the vehicle-sil Control API (gRPC). See docs/control-api.md.

The names are loaded on first use, so `python -m vehicle_sil_control.generate` works before the
gRPC stubs exist.
"""
__all__ = ["Control", "ControlError", "ControlWarning", "EventInfo", "Operation", "Peer", "SimulationState", "Target",
           "TimeState", "Values", "Variable",
           "ms", "s"]


def __getattr__(name):
    if name in __all__:
        from . import client

        return getattr(client, name)
    raise AttributeError(name)
