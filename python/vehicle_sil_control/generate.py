# SPDX-License-Identifier: MIT
"""Generates the gRPC stubs: python -m vehicle_sil_control.generate [--proto-root DIR] [--out DIR]"""
import argparse
import pathlib
import sys


def generate(proto_root: pathlib.Path, out: pathlib.Path) -> None:
    from grpc_tools import protoc  # grpcio-tools

    out.mkdir(parents=True, exist_ok=True)
    proto = proto_root / "vehicle_sil" / "control" / "v1" / "control.proto"
    code = protoc.main(["protoc", f"-I{proto_root}", f"--python_out={out}", f"--grpc_python_out={out}", str(proto)])
    if code != 0:
        raise RuntimeError(f"protoc failed ({code})")


def main(argv=None) -> int:
    here = pathlib.Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proto-root", type=pathlib.Path, default=here.parents[1] / "proto")
    parser.add_argument("--out", type=pathlib.Path, default=here / "_generated")
    args = parser.parse_args(argv)
    generate(args.proto_root, args.out)
    print(f"generated into {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
