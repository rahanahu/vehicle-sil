# SPDX-License-Identifier: MIT
"""Loads the generated gRPC stubs of the Control API v1.

The stubs are generated from proto/vehicle_sil/control/v1/control.proto with grpcio-tools into
a directory that is put on sys.path: VEHICLE_SIL_CONTROL_STUBS if set, else the package's
_generated directory (python -m vehicle_sil_control.generate makes it).
"""
import os
import pathlib
import sys

_STUBS = pathlib.Path(os.environ.get("VEHICLE_SIL_CONTROL_STUBS", pathlib.Path(__file__).parent / "_generated"))
if not (_STUBS / "vehicle_sil" / "control" / "v1" / "control_pb2.py").exists():
    raise ImportError(
        f"the Control API stubs are missing in {_STUBS}: run `python -m vehicle_sil_control.generate` "
        "(needs grpcio-tools) or set VEHICLE_SIL_CONTROL_STUBS")
if str(_STUBS) not in sys.path:
    sys.path.insert(0, str(_STUBS))

from vehicle_sil.control.v1 import control_pb2 as pb  # noqa: E402
from vehicle_sil.control.v1 import control_pb2_grpc as rpc  # noqa: E402

__all__ = ["pb", "rpc"]
