# SPDX-License-Identifier: MIT
"""Shared gRPC Control API helpers (stub resolution, metadata constants, token redaction,
trailing reason extraction, and Protobuf conversions) for `vehicle_sil_control` and `vehicle_sil_gui`.

Top-level imports use only the Python standard library so importing this module is `python -S` safe.
"""
from __future__ import annotations

import importlib
import importlib.util
import logging
import math
import os
import re
import secrets
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any

STUBS_VARIABLE: str = "VEHICLE_SIL_CONTROL_STUBS"
RUN_KEY: str = "vehicle-sil-run"
RUN_METADATA: str = RUN_KEY
REASON_KEY: str = "vehicle-sil-reason"
REASON_METADATA: str = REASON_KEY
REASON_RUN_CHANGED: str = "run-changed"
SCENARIO_CLIENT: str = "scenario"
PROTO: Path = Path("vehicle_sil") / "control" / "v1" / "control.proto"
ROLE_LINE: str = "operator vehicle-sil-gui {token}\n"
ADDRESS = re.compile(r"127\.0\.0\.1:(\d{1,5})")

log = logging.getLogger("vehicle_sil_gui")


def repository(anchor: Path | None = None) -> Path | None:
    """The repository root containing `proto/vehicle_sil/control/v1/control.proto`, or None."""
    start = (anchor or Path(__file__)).resolve()
    for parent in start.parents:
        if (parent / "proto" / PROTO).is_file():
            return parent
    return None


def generate_stubs(root: Path, out: Path) -> None:
    """Generate gRPC stubs from `root / "proto"` into `out` using `vehicle_sil_control.generate`."""
    path = root / "python" / "vehicle_sil_control" / "generate.py"
    spec = importlib.util.spec_from_file_location("_vehicle_sil_control_generate", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.generate(root / "proto", out)


_UNSET = object()


def resolve_stubs_directory(
    *,
    root: Path | None | object = _UNSET,
    error_cls: type[Exception] = ImportError,
    allow_package_generated: bool = True,
) -> tuple[Path, bool]:
    """Locate or generate the Control API gRPC stubs directory.
    Returns `(stubs_dir, made_temporary)`."""
    explicit = os.environ.get(STUBS_VARIABLE)
    if explicit:
        explicit_path = Path(explicit)
        if not (explicit_path / PROTO.with_name("control_pb2.py")).is_file():
            raise error_cls(
                f"{STUBS_VARIABLE}={explicit} has no {PROTO.with_name('control_pb2.py')}: make the "
                "stubs with `python -m vehicle_sil_control.generate --out <directory>`"
            )
        return explicit_path, False
    repo = repository() if root is _UNSET else root
    assert repo is None or isinstance(repo, Path)
    generated = repo / "python" / "vehicle_sil_control" / "_generated" if repo is not None else None
    if repo is not None:
        out = Path(tempfile.mkdtemp(prefix="vehicle-sil-gui-stubs-"))
        try:
            generate_stubs(repo, out)
            return out, True
        except ImportError:
            shutil.rmtree(out, ignore_errors=True)
        except Exception as e:
            shutil.rmtree(out, ignore_errors=True)
            log.warning("making the Control API stubs failed (%s)", e)
    if generated is not None and (generated / PROTO.with_name("control_pb2.py")).is_file():
        return generated, False
    if allow_package_generated:
        pkg_generated = Path(__file__).resolve().parent / "_generated"
        if (pkg_generated / PROTO.with_name("control_pb2.py")).is_file():
            return pkg_generated, False
    where = (
        "this GUI server does not run from the vehicle-sil repository, which has the proto file they are made from"
        if repo is None
        else "grpcio-tools is not installed (`uv sync` installs it)"
    )
    raise error_cls(
        f"the Control API stubs are missing ({where}): run the GUI server from the repository after "
        "`uv sync`, or make the stubs there with `python -m vehicle_sil_control.generate --out "
        f"<directory>` and set {STUBS_VARIABLE} to that directory"
    )


def import_stubs(
    stubs: Path,
    made: bool = False,
    *,
    error_cls: type[Exception] = ImportError,
) -> tuple[Any, Any]:
    """Import `(control_pb2, control_pb2_grpc)` from `stubs`, removing temporary directories if `made`."""
    if str(stubs) not in sys.path:
        sys.path.insert(0, str(stubs))
    try:
        pb = importlib.import_module("vehicle_sil.control.v1.control_pb2")
        rpc = importlib.import_module("vehicle_sil.control.v1.control_pb2_grpc")
    except Exception as e:
        raise error_cls(
            f"the Control API stubs in {stubs} cannot be loaded ({type(e).__name__}: {e}): make them "
            "again with `python -m vehicle_sil_control.generate` (docs/control-api.md)"
        ) from None
    finally:
        if made:
            if str(stubs) in sys.path:
                sys.path.remove(str(stubs))
            shutil.rmtree(stubs, ignore_errors=True)
    return pb, rpc


def credentials_file(*, role_line: str = ROLE_LINE, prefix: str = "vehicle-sil-gui-control-") -> tuple[Path, str]:
    """Create a user-only temporary credentials file and return `(path, token)`."""
    token = secrets.token_urlsafe(32)
    fd, name = tempfile.mkstemp(prefix=prefix, suffix=".txt")
    try:
        with os.fdopen(fd, "w", encoding="ascii", newline="\n") as f:
            f.write(role_line.format(token=token))
    except BaseException:
        remove_file(Path(name))
        raise
    return Path(name), token


def remove_file(path: Path | None) -> bool:
    """Remove `path`; returns True if gone or None, False if still held open."""
    if path is None:
        return True
    try:
        path.unlink()
    except FileNotFoundError:
        return True
    except OSError:
        return False
    return True


def check_loopback_address(address: object) -> str:
    """Validate that `address` is `127.0.0.1:<1..65535>`."""
    m = ADDRESS.fullmatch(address) if isinstance(address, str) else None
    if m is None or not 0 < int(m.group(1)) <= 65535:
        raise ValueError("the launcher's control_server event names an address that is not 127.0.0.1:<port>")
    return address


def redact_token(text: str, token: str | None) -> str:
    """Replace `token` in `text` with `<token>`."""
    return text.replace(token, "<token>") if token else text


def trailing_reason(err: object) -> str | None:
    """Extract `vehicle-sil-reason` from a gRPC RpcError's trailing metadata."""
    metadata = getattr(err, "trailing_metadata", None)
    if not callable(metadata):
        return None
    try:
        entries = metadata() or ()
    except Exception:
        return None
    for entry in entries:
        key, val = entry[0], entry[1]
        if isinstance(key, bytes):
            key = key.decode("utf-8", errors="ignore")
        if key.lower() == REASON_KEY:
            return val.decode("utf-8", errors="ignore") if isinstance(val, bytes) else str(val)
    return None


def enum_name(enum_wrapper: Any, value: int, prefix: str, default: str = "unknown") -> str:
    """Convert a protobuf enum integer to its lowercase suffix without `prefix`."""
    try:
        return enum_wrapper.Name(value)[len(prefix):].lower()
    except ValueError:
        return default


def to_proto_value(x: bool | int | float, pb: Any) -> Any:
    """Convert a Python bool/int/float into a `pb.Value`."""
    if isinstance(x, bool):
        return pb.Value(b=x)
    if isinstance(x, int):
        return pb.Value(i=x)
    return pb.Value(d=float(x))


def from_proto_value(v: Any, *, non_finite_none: bool = False) -> bool | int | float | None:
    """Extract a Python bool/int/float from a `pb.Value`."""
    kind = v.WhichOneof("kind")
    if not kind:
        return None
    val = getattr(v, kind)
    if non_finite_none and isinstance(val, float) and not math.isfinite(val):
        return None
    return val


def decode_proto_snapshot_values(values: Any, types: list[str]) -> list[ Any ]:
    """Decode a sequence of `pb.Value` according to `types` (`real`, `bool`, `integer`/`enum`)."""
    out = []
    for value, type_ in zip(values, types):
        if type_ == "real":
            x = value.d
            out.append(x if math.isfinite(x) else None)
        elif type_ == "bool":
            out.append(value.b)
        elif type_:
            out.append(value.i)
        else:
            out.append(from_proto_value(value, non_finite_none=True))
    return out


def proto_target_fields(
    t: Any,
    pb: Any,
    *,
    unspecified_as_unknown: bool = False,
) -> tuple[str, str, int | None, bool, str, int | None, int | None]:
    """Extract `(peer, status, applied_at_ns, late, reason, applied_writes, failed_write)` from a `TargetResult`."""
    status = enum_name(pb.TargetStatus, t.status, "TARGET_STATUS_")
    if unspecified_as_unknown and status == "unspecified":
        status = "unknown"
    applied_at = t.applied_at_ns if t.HasField("applied_at_ns") else None
    applied_writes = t.applied_writes if t.HasField("applied_writes") else None
    failed_write = t.failed_write if t.HasField("failed_write") else None
    return (t.peer, status, applied_at, t.late, t.reason, applied_writes, failed_write)
