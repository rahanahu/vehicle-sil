#!/usr/bin/env python3
"""Run the reproducible coordinated virtual-time benchmark suite and verify deterministic outputs.

Executes canonical SIL Kit coordinated multi-process cases using vehicle_sil_launch and
vehicle_sil_bench_ecu, validates deterministic state digests and timestamped CAN TX/RX causal
traces over the exact cutoff {tx_ms < W + M, rx_ms <= W + M}, extracts actual compiler and
optimization flags from build metadata, collects explicitly scoped process CPU (recording
status="unavailable" and null instead of 0 when unsupported), and writes a schema-validated
JSON report (schema/benchmark-report.schema.json).

Exit codes:
  0  all selected cases passed and verified deterministically
  1  simulation, determinism, causal trace, or invariant failure
  2  usage error (missing binaries, invalid/unaligned arguments, unsupported --control-metrics on)

Requires Python 3.11+ (standard library only).
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime
import hashlib
import json
import math
import ntpath
import os
import platform
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path, PureWindowsPath
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import busy_machine  # noqa: E402
import job_summary  # noqa: E402
import verify_common  # noqa: E402

ROOT = verify_common.ROOT
DEFAULT_OUT = ROOT / "build" / "logs" / "benchmark-vtime.json"

BENCHMARK_FORMAT_VERSION = 1
DECLARED_VARS_PER_ECU = 250
BUILTIN_PEER_VARS = 4
CAN_BASE_ID = 0x100
CAN_NETWORK_NAME = "BENCH_CAN"

ISSUE1_UNAVAILABLE_REASONS = frozenset({
    "unreadable",
    "check_failed",
    "interrupted_before_setup",
    "stopped_before_setup",
    "setup_failed",
    "internal_error",
})

INTERPRETATION_NOTES = [
    "Coordinated SIL Kit multi-process results cannot be substituted by LocalBench or in-process microbenchmarks.",
    "ecu_count, silkit_participant_count (includes SimulationController), and os_process_count are distinct quantities.",
    (
        "steady_state.wall_ms (elapsed wall time), process_cpu.* (cumulative multi-core CPU), and "
        "control_transport_metrics.counters.handler_time_ns (controller SIL Kit thread cumulative handler time across "
        "HubTransport lifetime) are orthogonal and must never be summed."
    ),
    (
        "Canonical cases use SIL Kit's built-in trivial CAN bus (can_simulation_mode: silkit_trivial, "
        "network_simulator: null); do not conflate with runs that include a NetworkSimulator participant "
        "(vehicle_sil_can_bus_simulator / sil-kit-net-sim), which adds +1 participant/process on the step barrier "
        "and alters CAN delivery timestamps."
    ),
]

TARGET_TU_SUFFIXES = {
    "vehicle_sil_launch": "tools/launch/main.cpp",
    "vehicle_sil_core": "libs/core/src/participant_runtime.cpp",
    "vehicle_sil_bench_ecu": "tools/bench_ecu/main.cpp",
}

COMPILER_WRAPPERS = frozenset({"ccache", "sccache", "distcc", "icecc", "env"})


class UsageError(Exception):
    """Invalid CLI usage or unmet preconditions (exit code 2)."""


class VerificationError(Exception):
    """Simulation, determinism, causal trace, or invariant failure (exit code 1)."""


@dataclasses.dataclass(frozen=True)
class EcuSpec:
    ecu_id: int
    name: str
    step_ms: int
    sample_ms: int = 10
    can_period_ms: int = 10
    can_offset_ms: int = 0
    can_frames: int = 1


@dataclasses.dataclass(frozen=True)
class CaseSpec:
    case_id: str
    description: str
    ecus: tuple[EcuSpec, ...]
    with_scenario: bool = False
    declared_vars: int = DECLARED_VARS_PER_ECU
    step_work: int = 0


@dataclasses.dataclass(frozen=True)
class TraceEvent:
    direction: str  # "T" or "R"
    tx_ms: int
    rx_ms: int | None
    can_id: int
    seq: int
    payload_hex: str


ALL_CASES: dict[str, CaseSpec] = {
    "case_a_1ecu_1ms": CaseSpec(
        case_id="case_a_1ecu_1ms",
        description="1 ECU (1 ms), coordinated SIL Kit, 100 Hz CAN TX (0 RX)",
        ecus=(EcuSpec(ecu_id=0, name="ecu0", step_ms=1, can_period_ms=10, can_offset_ms=0),),
    ),
    "case_b_5ecu_1ms": CaseSpec(
        case_id="case_b_5ecu_1ms",
        description="5 ECUs (1 ms x 5), coordinated SIL Kit, 100 Hz staggered CAN/ECU",
        ecus=tuple(
            EcuSpec(ecu_id=i, name=f"ecu{i}", step_ms=1, can_period_ms=10, can_offset_ms=i)
            for i in range(5)
        ),
    ),
    "case_c_5ecu_mixed": CaseSpec(
        case_id="case_c_5ecu_mixed",
        description="5 ECUs mixed rates (10 ms x 1 + 1 ms x 4), coordinated SIL Kit, 100 Hz staggered CAN/ECU",
        ecus=(
            EcuSpec(ecu_id=0, name="ecu0", step_ms=10, can_period_ms=10, can_offset_ms=0),
            *(
                EcuSpec(ecu_id=i, name=f"ecu{i}", step_ms=1, can_period_ms=10, can_offset_ms=i)
                for i in range(1, 5)
            ),
        ),
    ),
    "case_d_5ecu_1ms_scenario": CaseSpec(
        case_id="case_d_5ecu_1ms_scenario",
        description="5 ECUs (1 ms x 5) + deterministic scenario (L-03 per-step sampling), coordinated SIL Kit",
        ecus=tuple(
            EcuSpec(ecu_id=i, name=f"ecu{i}", step_ms=1, can_period_ms=10, can_offset_ms=i)
            for i in range(5)
        ),
        with_scenario=True,
    ),
}

CANONICAL_CASE_IDS = ("case_a_1ecu_1ms", "case_b_5ecu_1ms", "case_c_5ecu_mixed")


# ---------------------------------------------------------------------------
# Alignment and argument validation
# ---------------------------------------------------------------------------


def case_alignment_ms(case: CaseSpec) -> int:
    periods = [e.step_ms for e in case.ecus]
    periods.extend(e.can_period_ms for e in case.ecus if e.can_period_ms > 0)
    return math.lcm(*periods)


def case_time_step_ms(case: CaseSpec) -> int:
    return math.gcd(*(e.step_ms for e in case.ecus))


def case_tail_sim_ms(case: CaseSpec) -> int:
    return max(10, max(e.step_ms for e in case.ecus))


def validate_window_alignment(cases: list[CaseSpec], warmup_ms: int, measure_ms: int) -> None:
    if warmup_ms < 0:
        raise UsageError(f"--warmup-ms must be >= 0 (got {warmup_ms})")
    if measure_ms <= 0:
        raise UsageError(f"--measure-ms must be > 0 (got {measure_ms})")
    for case in cases:
        align_ms = case_alignment_ms(case)
        if warmup_ms % align_ms != 0:
            raise UsageError(
                f"{case.case_id}: --warmup-ms ({warmup_ms}) must be a multiple of "
                f"lcm(step_ms, can_period_ms) = {align_ms}"
            )
        if measure_ms % align_ms != 0:
            raise UsageError(
                f"{case.case_id}: --measure-ms ({measure_ms}) must be a multiple of "
                f"lcm(step_ms, can_period_ms) = {align_ms}"
            )
        tail_ms = case_tail_sim_ms(case)
        max_step = max(e.step_ms for e in case.ecus)
        if tail_ms < max_step:
            raise UsageError(f"{case.case_id}: tail_sim_ms ({tail_ms}) must be >= max(step_ms) ({max_step})")
        step_gcd = case_time_step_ms(case)
        stop_at_ms = warmup_ms + measure_ms + tail_ms
        if stop_at_ms % step_gcd != 0:
            raise UsageError(f"{case.case_id}: stop_at_ms ({stop_at_ms}) must be a multiple of time_step_ms ({step_gcd})")
        if case.with_scenario and measure_ms < 40:
            raise UsageError(f"{case.case_id}: --measure-ms must be >= 40 when running the scenario case")


def resolve_cases(selector: str) -> list[CaseSpec]:
    token = selector.strip()
    if token == "canonical":
        return [ALL_CASES[cid] for cid in CANONICAL_CASE_IDS]
    if token == "all":
        return [ALL_CASES[cid] for cid in (*CANONICAL_CASE_IDS, "case_d_5ecu_1ms_scenario")]
    chosen: list[CaseSpec] = []
    seen: set[str] = set()
    for part in token.split(","):
        cid = part.strip()
        if not cid:
            continue
        if cid not in ALL_CASES:
            valid = ", ".join(["canonical", "all", *ALL_CASES.keys()])
            raise UsageError(f"unknown case {cid!r} (valid: {valid})")
        if cid not in seen:
            seen.add(cid)
            chosen.append(ALL_CASES[cid])
    if not chosen:
        raise UsageError("--cases must select at least one case")
    return chosen


# ---------------------------------------------------------------------------
# Compiler & build provenance extraction
# ---------------------------------------------------------------------------


def _empty_target_flags() -> dict[str, Any]:
    return {
        "compiler_id": None,
        "compiler_version": None,
        "compiler_banner": None,
        "optimization_flags": None,
        "effective_opt_level": None,
        "codegen_and_defines": None,
        "sanitizers": None,
        "ndebug": None,
    }


def _strip_matching_quotes(tok: str) -> str:
    if len(tok) >= 2 and tok[0] == tok[-1] and tok[0] in ('"', "'"):
        return tok[1:-1]
    return tok


def _split_windows_cmd_tokens(cmd: str) -> list[str]:
    """Split a Windows command line / response file honoring mid-token quotes and \\\" escapes."""
    tokens: list[str] = []
    buf: list[str] = []
    in_quote = False
    has_token = False
    i = 0
    n = len(cmd)
    while i < n:
        ch = cmd[i]
        if ch.isspace() and not in_quote:
            if has_token:
                tokens.append("".join(buf))
                buf.clear()
                has_token = False
            i += 1
            continue
        has_token = True
        if ch == "\\":
            bs_start = i
            while i < n and cmd[i] == "\\":
                i += 1
            bs_count = i - bs_start
            if i < n and cmd[i] == '"':
                buf.append("\\" * (bs_count // 2))
                if bs_count % 2 == 1:
                    buf.append('"')
                else:
                    in_quote = not in_quote
                i += 1
            else:
                buf.append("\\" * bs_count)
            continue
        if ch == '"':
            in_quote = not in_quote
            i += 1
            continue
        buf.append(ch)
        i += 1
    if in_quote:
        raise ValueError("No closing quotation")
    if has_token:
        tokens.append("".join(buf))
    return tokens


def _split_cmd_tokens(cmd: str, windows: bool) -> list[str]:
    if windows:
        return _split_windows_cmd_tokens(cmd)
    return shlex.split(cmd, posix=True)


def _read_response_file_text(rsp_path: Path) -> str:
    raw = rsp_path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        text = raw.decode("utf-16")
    else:
        text = raw.decode("utf-8-sig")
    if "\x00" in text:
        raise ValueError("NUL byte in response file")
    return text


def _expand_response_files(
    tokens: list[str],
    base_dir: Path | None,
    windows: bool,
    depth: int = 0,
) -> tuple[list[str], list[str]]:
    if depth >= 4:
        unreadable = [tok for tok in tokens if tok.startswith("@") and len(tok) > 1]
        return list(tokens), unreadable
    out: list[str] = []
    unreadable: list[str] = []
    for tok in tokens:
        if tok.startswith("@") and len(tok) > 1:
            rsp_str = _strip_matching_quotes(tok[1:])
            rsp_path = Path(rsp_str)
            if not rsp_path.is_absolute() and base_dir is not None:
                rsp_path = base_dir / rsp_path
            try:
                if rsp_path.is_file():
                    rsp_text = _read_response_file_text(rsp_path)
                    rsp_tokens = _split_cmd_tokens(rsp_text, windows)
                    nested_out, nested_unreadable = _expand_response_files(
                        rsp_tokens, base_dir, windows, depth + 1
                    )
                    out.extend(nested_out)
                    unreadable.extend(nested_unreadable)
                    continue
            except (OSError, ValueError):
                pass
            unreadable.append(tok)
        out.append(tok)
    return out, unreadable


def parse_flag_tokens(tokens: list[str]) -> dict[str, Any]:
    """Extract optimization, codegen/define, sanitizer, and NDEBUG facts from compiler tokens."""
    opt_flags: list[str] = []
    codegen: list[str] = []
    active_sanitizers: list[tuple[str, str]] = []
    ndebug = False

    for tok in tokens:
        if re.fullmatch(r"(?:-O[0-3sSzZg]?|-Ofast|/O[12dsx])", tok):
            opt_flags.append(tok)
        elif tok in ("-DNDEBUG", "/DNDEBUG") or tok.startswith(("-DNDEBUG=", "/DNDEBUG=")):
            ndebug = True
            codegen.append(tok)
        elif tok in ("-UNDEBUG", "/UNDEBUG"):
            ndebug = False
            codegen.append(tok)
        elif tok == "-DDEBUG" or tok.startswith(("-flto", "-march=", "-mtune=", "-mcpu=", "-ffast-math")):
            codegen.append(tok)
        elif re.fullmatch(r"-g(?:[0-3]|gdb|dwarf(?:-[2-5])?)?", tok):
            codegen.append(tok)
        elif tok.startswith("-fsanitize=") or tok.startswith("/fsanitize="):
            prefix = tok[: len("-fsanitize=")]
            for item in tok.split("=", 1)[1].split(","):
                name = item.strip()
                if name and (prefix, name) not in active_sanitizers:
                    active_sanitizers.append((prefix, name))
        elif tok.startswith("-fno-sanitize="):
            disabled = {item.strip() for item in tok.split("=", 1)[1].split(",") if item.strip()}
            if "all" in disabled:
                active_sanitizers.clear()
            else:
                active_sanitizers = [(p, n) for p, n in active_sanitizers if n not in disabled]

    return {
        "optimization_flags": opt_flags,
        "effective_opt_level": opt_flags[-1] if opt_flags else None,
        "codegen_and_defines": codegen,
        "sanitizers": [f"{p}{n}" for p, n in active_sanitizers],
        "ndebug": ndebug,
    }


def _extract_compiler_and_arg_start(tokens: list[str]) -> tuple[str | None, int]:
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        base = tok.replace("\\", "/").rsplit("/", 1)[-1].lower()
        if base.endswith(".exe"):
            base = base[:-4]
        if base in COMPILER_WRAPPERS:
            i += 1
            while i < len(tokens) and ("=" in tokens[i] or tokens[i].startswith("-")):
                i += 1
            continue
        return tok, i + 1
    return None, len(tokens)


def _extract_compiler_path(tokens: list[str]) -> str | None:
    compiler_path, _ = _extract_compiler_and_arg_start(tokens)
    return compiler_path


def _compiler_banner(compiler_path: str | None, cache: dict[str, str | None]) -> str | None:
    if not compiler_path:
        return None
    if compiler_path in cache:
        return cache[compiler_path]
    banner: str | None = None
    try:
        proc = subprocess.run(
            [compiler_path, "--version"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdin=subprocess.DEVNULL,
            timeout=5,
        )
        if proc.returncode == 0:
            for line in proc.stdout.splitlines():
                line = line.strip()
                if line:
                    banner = line
                    break
    except (OSError, subprocess.SubprocessError):
        banner = None
    cache[compiler_path] = banner
    return banner


def _read_cmake_cxx_compiler_info(build_dir: Path) -> tuple[str | None, str | None]:
    compiler_id: str | None = None
    compiler_version: str | None = None
    cmake_files = build_dir / "CMakeFiles"
    if not cmake_files.is_dir():
        return None, None
    for path in sorted(cmake_files.glob("*/CMakeCXXCompiler.cmake")):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        m_id = re.search(r'set\s*\(\s*CMAKE_CXX_COMPILER_ID\s+"([^"]+)"\s*\)', text)
        m_ver = re.search(r'set\s*\(\s*CMAKE_CXX_COMPILER_VERSION\s+"([^"]+)"\s*\)', text)
        if m_id:
            compiler_id = m_id.group(1)
        if m_ver:
            compiler_version = m_ver.group(1)
        if compiler_id or compiler_version:
            break
    return compiler_id, compiler_version


def parse_cmake_cache(cache_path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not cache_path.is_file():
        return values
    for raw in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", "//")):
            continue
        m = re.match(r"^([A-Za-z0-9_.-]+):[^=]+=(.*)$", line)
        if m:
            values[m.group(1)] = m.group(2)
    return values


def _cache_bool(cache: dict[str, str], key: str) -> bool | None:
    if key not in cache:
        return None
    val = cache[key].strip().upper()
    if val in ("ON", "TRUE", "1", "YES"):
        return True
    if val in ("OFF", "FALSE", "0", "NO", ""):
        return False
    return None


def _is_path_within(path: Path, parent: Path) -> bool:
    try:
        return path.resolve().is_relative_to(parent.resolve())
    except OSError:
        return False


def _compute_flags_warning(
    cmake_build_type: str | None,
    flags_source: str,
    targets: dict[str, dict[str, Any]],
    bench_ecu_build_info: dict[str, Any] | None,
    *,
    build_dir: Path | None = None,
    launcher_path: Path | None = None,
    bench_ecu_path: Path | None = None,
    ambiguous_targets: list[str] | None = None,
    unreadable_rsp_targets: dict[str, list[str]] | None = None,
) -> str | None:
    warnings: list[str] = []
    ambiguous_list = list(ambiguous_targets) if ambiguous_targets else []
    ambiguous_set = set(ambiguous_list)
    if ambiguous_list:
        warnings.append(f"ambiguous compile_commands.json entries for {', '.join(ambiguous_list)}")
    unreadable_rsp_map = dict(unreadable_rsp_targets) if unreadable_rsp_targets else {}
    unreadable_rsp_set = set(unreadable_rsp_map)
    if unreadable_rsp_map:
        details = ", ".join(
            f"{name} ({', '.join(refs)})" if refs else name
            for name, refs in unreadable_rsp_map.items()
        )
        warnings.append(f"unreadable response file(s) in compile_commands.json for {details}")
    if build_dir is not None:
        if launcher_path is not None and not _is_path_within(launcher_path, build_dir):
            warnings.append(f"--launcher ({launcher_path}) resolves outside build_dir ({build_dir})")
        if bench_ecu_path is not None and not _is_path_within(bench_ecu_path, build_dir):
            warnings.append(f"--bench-ecu ({bench_ecu_path}) resolves outside build_dir ({build_dir})")
    if (
        cmake_build_type
        and cmake_build_type.lower() == "release"
        and bench_ecu_build_info is not None
        and bench_ecu_build_info.get("ndebug") is False
    ):
        warnings.append("CMAKE_BUILD_TYPE=Release but vehicle_sil_bench_ecu compiled without NDEBUG")
    if flags_source != "unavailable":
        if flags_source == "compile_commands.json":
            missing_tus = [
                name
                for name, info in targets.items()
                if info["optimization_flags"] is None
                and name not in ambiguous_set
                and name not in unreadable_rsp_set
            ]
            if missing_tus:
                warnings.append(f"missing target TU(s) in compile_commands.json: {', '.join(missing_tus)}")

        opt_levels = {
            name: info["effective_opt_level"]
            for name, info in targets.items()
            if info["optimization_flags"] is not None
        }
        if opt_levels and len(set(opt_levels.values())) > 1:
            warnings.append(f"targets compiled with differing effective optimization levels: {opt_levels}")
        if cmake_build_type and cmake_build_type.lower() == "release":
            unopt = [
                f"{name}={lvl}"
                for name, lvl in opt_levels.items()
                if lvl in (None, "-O0", "-Og", "/Od")
            ]
            if unopt:
                warnings.append(f"CMAKE_BUILD_TYPE=Release but non-optimized flags detected ({', '.join(unopt)})")
            san = [
                f"{name}={info['sanitizers']}"
                for name, info in targets.items()
                if info["sanitizers"]
            ]
            if san:
                warnings.append(f"CMAKE_BUILD_TYPE=Release but sanitizers are enabled ({', '.join(san)})")

        be_flags = targets.get("vehicle_sil_bench_ecu", {})
        if bench_ecu_build_info is not None and be_flags.get("optimization_flags") is not None:
            eff_opt = be_flags["effective_opt_level"]
            flags_optimized = eff_opt not in (None, "-O0", "/Od")
            opt_macro = bench_ecu_build_info.get("optimize_macro")
            if isinstance(opt_macro, int) and not isinstance(opt_macro, bool):
                if flags_optimized and opt_macro == 0:
                    warnings.append(
                        f"vehicle_sil_bench_ecu compile flags say {eff_opt} but --bench-build-info reports optimize_macro=0"
                    )
                elif not flags_optimized and opt_macro > 0:
                    warnings.append(
                        f"vehicle_sil_bench_ecu compile flags say {eff_opt} but --bench-build-info reports optimize_macro={opt_macro}"
                    )
            info_ndebug = bench_ecu_build_info.get("ndebug")
            if isinstance(info_ndebug, bool) and be_flags["ndebug"] is not None and info_ndebug != be_flags["ndebug"]:
                warnings.append(
                    f"vehicle_sil_bench_ecu compile flags ndebug={be_flags['ndebug']} conflicts with --bench-build-info ndebug={info_ndebug}"
                )
            info_cid = bench_ecu_build_info.get("compiler_id")
            if info_cid and be_flags["compiler_id"] and info_cid != be_flags["compiler_id"]:
                warnings.append(
                    f"vehicle_sil_bench_ecu compiler_id={be_flags['compiler_id']} conflicts with --bench-build-info compiler_id={info_cid}"
                )
            info_sans = {str(s) for s in (bench_ecu_build_info.get("sanitizers") or [])}
            flag_sans = {s.split("=", 1)[1] for s in (be_flags["sanitizers"] or []) if "=" in s}
            missing_in_flags = sorted(info_sans - flag_sans)
            missing_in_bin = sorted((flag_sans & {"address", "thread"}) - info_sans)
            if missing_in_flags or missing_in_bin:
                warnings.append(
                    f"vehicle_sil_bench_ecu sanitizers conflict: flags={sorted(flag_sans)} vs --bench-build-info={sorted(info_sans)}"
                )
    return "; ".join(warnings) if warnings else None


def extract_build_provenance(
    build_dir: Path | None,
    bench_ecu_build_info: dict[str, Any] | None,
    *,
    is_windows: bool | None = None,
    launcher_path: Path | None = None,
    bench_ecu_path: Path | None = None,
) -> tuple[dict[str, Any], dict[str, str]]:
    """Extract compiler and effective optimization flags from compile_commands.json or CMakeCache.txt."""
    windows = (os.name == "nt") if is_windows is None else is_windows
    cache_vals: dict[str, str] = {}
    cmake_build_type: str | None = None
    compiler_id_cmake: str | None = None
    compiler_ver_cmake: str | None = None
    banner_cache: dict[str, str | None] = {}

    if build_dir is not None and build_dir.is_dir():
        cache_vals = parse_cmake_cache(build_dir / "CMakeCache.txt")
        bt = cache_vals.get("CMAKE_BUILD_TYPE", "").strip()
        cmake_build_type = bt or None
        compiler_id_cmake, compiler_ver_cmake = _read_cmake_cxx_compiler_info(build_dir)

    targets: dict[str, dict[str, Any]] = {name: _empty_target_flags() for name in TARGET_TU_SUFFIXES}
    flags_source = "unavailable"
    ambiguous_targets: list[str] = []
    unreadable_rsp_targets: dict[str, list[str]] = {}

    cc_path = (build_dir / "compile_commands.json") if build_dir is not None else None
    if cc_path is not None and cc_path.is_file():
        try:
            entries = json.loads(cc_path.read_text(encoding="utf-8", errors="replace"))
        except (OSError, json.JSONDecodeError):
            entries = None
        if isinstance(entries, list):
            matched_entries: dict[str, list[tuple[str | None, dict[str, Any]]]] = {
                name: [] for name in TARGET_TU_SUFFIXES
            }
            unreadable_rsps_by_target: dict[str, list[str]] = {
                name: [] for name in TARGET_TU_SUFFIXES
            }
            for entry in entries:
                if not isinstance(entry, dict):
                    continue
                file_str = str(entry.get("file", "")).replace("\\", "/")
                for target_name, suffix in TARGET_TU_SUFFIXES.items():
                    if file_str.endswith("/" + suffix) or file_str == suffix:
                        entry_dir_raw = entry.get("directory")
                        entry_dir = Path(str(entry_dir_raw)) if entry_dir_raw else build_dir
                        raw_args = entry.get("arguments")
                        if isinstance(raw_args, list) and all(isinstance(x, str) for x in raw_args):
                            tokens = [_strip_matching_quotes(x) for x in raw_args] if windows else list(raw_args)
                        else:
                            cmd_str = str(entry.get("command", ""))
                            tokens = _split_cmd_tokens(cmd_str, windows)
                        compiler_path, arg_start = _extract_compiler_and_arg_start(tokens)
                        expanded_args, unreadable_rsps = _expand_response_files(
                            tokens[arg_start:], entry_dir, windows
                        )
                        for ref in unreadable_rsps:
                            if ref not in unreadable_rsps_by_target[target_name]:
                                unreadable_rsps_by_target[target_name].append(ref)
                        parsed = parse_flag_tokens(expanded_args)
                        entry_flags = {
                            "compiler_id": compiler_id_cmake,
                            "compiler_version": compiler_ver_cmake,
                            "compiler_banner": _compiler_banner(compiler_path, banner_cache),
                            **parsed,
                        }
                        matched_entries[target_name].append((compiler_path, entry_flags))
            unambiguous_any = False
            for target_name in TARGET_TU_SUFFIXES:
                target_matches = matched_entries[target_name]
                if not target_matches:
                    continue
                target_unreadable = unreadable_rsps_by_target[target_name]
                first = target_matches[0]
                all_equal = all(match == first for match in target_matches[1:])
                if target_unreadable:
                    unreadable_rsp_targets[target_name] = target_unreadable
                    if not all_equal:
                        ambiguous_targets.append(target_name)
                    continue
                if all_equal:
                    targets[target_name] = first[1]
                    unambiguous_any = True
                else:
                    ambiguous_targets.append(target_name)
            if unambiguous_any:
                flags_source = "compile_commands.json"

    is_multi_config = bool(cache_vals.get("CMAKE_CONFIGURATION_TYPES", "").strip()) and cmake_build_type is None
    if (
        flags_source == "unavailable"
        and not ambiguous_targets
        and not unreadable_rsp_targets
        and cache_vals
        and not is_multi_config
    ):
        flags_source = "cmake_cache"
        base_flags = cache_vals.get("CMAKE_CXX_FLAGS", "")
        type_key = f"CMAKE_CXX_FLAGS_{cmake_build_type.upper()}" if cmake_build_type else ""
        type_flags = cache_vals.get(type_key, "") if type_key else ""
        combined = f"{base_flags} {type_flags}".strip()
        tokens = _split_cmd_tokens(combined, windows) if combined else []
        san_cache = [s.strip() for s in cache_vals.get("VEHICLE_SIL_SANITIZERS", "").split(";") if s.strip()]
        for san in san_cache:
            tok = f"-fsanitize={san}"
            if tok not in tokens:
                tokens.append(tok)
        parsed = parse_flag_tokens(tokens)
        compiler_path = cache_vals.get("CMAKE_CXX_COMPILER") or None
        banner = _compiler_banner(compiler_path, banner_cache)
        for target_name in TARGET_TU_SUFFIXES:
            targets[target_name] = {
                "compiler_id": compiler_id_cmake,
                "compiler_version": compiler_ver_cmake,
                "compiler_banner": banner,
                **parsed,
            }

    build_dir_str: str | None = None
    if build_dir is not None:
        build_dir_str = verify_common.rel(build_dir.resolve(), ROOT)

    warning = _compute_flags_warning(
        cmake_build_type,
        flags_source,
        targets,
        bench_ecu_build_info,
        build_dir=build_dir,
        launcher_path=launcher_path,
        bench_ecu_path=bench_ecu_path,
        ambiguous_targets=ambiguous_targets,
        unreadable_rsp_targets=unreadable_rsp_targets,
    )
    return (
        {
            "build_dir": build_dir_str,
            "cmake_build_type": cmake_build_type,
            "flags_source": flags_source,
            "flags_warning": warning,
            "targets": targets,
            "bench_ecu_build_info": bench_ecu_build_info,
        },
        cache_vals,
    )


_GIT_DISCOVERY_ENV_VARS = frozenset({
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_COMMON_DIR",
    "GIT_INDEX_FILE",
    "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_PREFIX",
    "GIT_IMPLICIT_WORK_TREE",
    "GIT_NAMESPACE",
    "GIT_SUPER_PREFIX",
})


def _sanitized_git_env() -> dict[str, str]:
    return {k: v for k, v in os.environ.items() if k.upper() not in _GIT_DISCOVERY_ENV_VARS}


def _git_output(args: list[str], cwd: Path | None = None) -> str | None:
    try:
        proc = subprocess.run(
            ["git", *args],
            cwd=ROOT if cwd is None else cwd,
            env=_sanitized_git_env(),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdin=subprocess.DEVNULL,
            timeout=5,
        )
        if proc.returncode == 0:
            return proc.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return None


_MSYS_DRIVE_PATH_RE = re.compile(r"^/([A-Za-z])(/.*|$)")


def _msys_to_windows_path(raw: str) -> str:
    m = _MSYS_DRIVE_PATH_RE.match(raw)
    if not m:
        return raw
    drive, rest = m.group(1), m.group(2)
    return f"{drive.upper()}:{rest or '/'}"


def _cygpath_to_windows(raw: str) -> str | None:
    cygpath = shutil.which("cygpath")
    if not cygpath:
        return None
    try:
        proc = subprocess.run(
            [cygpath, "-w", raw],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdin=subprocess.DEVNULL,
            timeout=5,
        )
        if proc.returncode == 0:
            out = proc.stdout.strip()
            if out:
                return out
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def _normalize_git_path(
    raw: str,
    cwd: Path,
    *,
    is_windows: bool | None = None,
) -> Path | PureWindowsPath:
    """Normalize a path printed by git (handling MSYS2 /c/..., /tmp/..., /home/... paths on Windows)."""
    windows = (os.name == "nt") if is_windows is None else is_windows
    text = raw
    if windows and raw.startswith("/") and not raw.startswith("//"):
        converted = _cygpath_to_windows(raw)
        text = converted if converted is not None else _msys_to_windows_path(raw)
    if windows and sys.platform != "win32":
        win_path = PureWindowsPath(text)
        if not win_path.is_absolute():
            win_path = PureWindowsPath(str(cwd)) / win_path
        return PureWindowsPath(ntpath.normpath(str(win_path)))
    path = Path(text)
    if not path.is_absolute():
        path = cwd / path
    return path.resolve()


def _path_compare_key(path: Path | PureWindowsPath, *, is_windows: bool | None = None) -> str:
    windows = (os.name == "nt") if is_windows is None else is_windows
    if windows:
        if sys.platform == "win32":
            return os.path.normcase(str(Path(path).resolve()))
        return ntpath.normcase(ntpath.normpath(str(path)))
    return str(Path(path).resolve())


def _same_path(
    a: Path | PureWindowsPath | None,
    b: Path | PureWindowsPath | None,
    *,
    is_windows: bool | None = None,
) -> bool:
    if a is None or b is None:
        return False
    windows = (os.name == "nt") if is_windows is None else is_windows
    if not (windows and sys.platform != "win32"):
        try:
            if os.path.samefile(a, b):
                return True
        except OSError:
            pass
    return _path_compare_key(a, is_windows=is_windows) == _path_compare_key(b, is_windows=is_windows)


def _resolved_git_path(
    cwd: Path,
    rev_parse_flag: str,
    *,
    is_windows: bool | None = None,
) -> Path | PureWindowsPath | None:
    raw = _git_output(["rev-parse", rev_parse_flag], cwd=cwd)
    if not raw:
        return None
    try:
        return _normalize_git_path(raw, cwd, is_windows=is_windows)
    except OSError:
        return None


def _resolved_git_dir(cwd: Path, *, is_windows: bool | None = None) -> Path | PureWindowsPath | None:
    return _resolved_git_path(cwd, "--git-dir", is_windows=is_windows)


def _resolved_git_common_dir(cwd: Path, *, is_windows: bool | None = None) -> Path | PureWindowsPath | None:
    return _resolved_git_path(cwd, "--git-common-dir", is_windows=is_windows)


def _collect_repo_and_dependency_provenance(
    launcher_version_info: dict[str, Any],
    cache_vals: dict[str, str],
    *,
    root: Path | None = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    repo_root = ROOT if root is None else root
    git_commit = _git_output(["rev-parse", "HEAD"], cwd=repo_root)
    git_dirty: bool | None = None
    if git_commit is not None:
        try:
            proc = subprocess.run(
                ["git", "diff", "--quiet", "HEAD"],
                cwd=repo_root,
                env=_sanitized_git_env(),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
            git_dirty = proc.returncode != 0
        except (OSError, subprocess.SubprocessError):
            git_dirty = None

    features = sorted(str(f) for f in launcher_version_info.get("features", []))
    repo_info = {
        "git_commit": git_commit,
        "git_dirty": git_dirty,
        "vehicle_sil_version": str(launcher_version_info.get("vehicle_sil", "unknown")),
        "launcher_features": features,
    }

    silkit_dir = repo_root / "external" / "sil-kit"
    silkit_commit: str | None = None
    silkit_tag: str | None = None
    if (silkit_dir / ".git").exists():
        show_prefix = _git_output(["rev-parse", "--show-prefix"], cwd=silkit_dir)
        silkit_git_dir = _resolved_git_dir(silkit_dir)
        parent_git_dir = _resolved_git_dir(repo_root)
        silkit_common_dir = _resolved_git_common_dir(silkit_dir)
        parent_common_dir = _resolved_git_common_dir(repo_root)
        if show_prefix == "" and silkit_git_dir is not None and silkit_common_dir is not None:
            try:
                is_own_repo = (
                    not _same_path(silkit_dir, repo_root)
                    and not _same_path(silkit_git_dir, parent_git_dir)
                    and not _same_path(silkit_common_dir, parent_common_dir)
                )
            except OSError:
                is_own_repo = False
            if is_own_repo:
                silkit_commit = _git_output(["rev-parse", "HEAD"], cwd=silkit_dir)
                silkit_tag = _git_output(["describe", "--tags", "--exact-match"], cwd=silkit_dir)

    patches: list[dict[str, str]] = []
    patch_dir = repo_root / "patches" / "sil-kit"
    if patch_dir.is_dir():
        for p in sorted(patch_dir.glob("*.patch")):
            digest = hashlib.sha256(p.read_bytes()).hexdigest()
            patches.append({"name": p.name, "sha256": digest})

    deps_info = {
        "sil_kit": {
            "submodule_commit": silkit_commit,
            "tag": silkit_tag,
            "patches": patches,
            "static_linked": _cache_bool(cache_vals, "VEHICLE_SIL_STATIC"),
        },
        "control_server_built": "control-server" in features,
    }
    return repo_info, deps_info


def _collect_host_provenance() -> dict[str, Any]:
    cpu_model: str | None = None
    if sys.platform.startswith("linux"):
        cpuinfo = Path("/proc/cpuinfo")
        if cpuinfo.is_file():
            for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.lower().startswith("model name"):
                    _, _, val = line.partition(":")
                    val = val.strip()
                    if val:
                        cpu_model = val
                        break
    elif sys.platform == "darwin":
        try:
            proc = subprocess.run(
                ["sysctl", "-n", "machdep.cpu.brand_string"],
                capture_output=True,
                text=True,
                stdin=subprocess.DEVNULL,
                timeout=3,
            )
            if proc.returncode == 0 and proc.stdout.strip():
                cpu_model = proc.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    if not cpu_model:
        cpu_model = platform.processor() or None

    return {
        "os": platform.system(),
        "os_release": platform.release(),
        "arch": platform.machine(),
        "cpu_model": cpu_model,
        "cpu_logical_count": os.cpu_count(),
        "python_version": platform.python_version(),
    }


def _os_load1() -> float | None:
    if os.name == "nt" or not hasattr(os, "getloadavg"):
        return None
    try:
        return round(float(os.getloadavg()[0]), 3)
    except OSError:
        return None


# ---------------------------------------------------------------------------
# Subprocess reaping & cross-platform process CPU
# ---------------------------------------------------------------------------


def _popen_group_kwargs() -> dict[str, Any]:
    if os.name == "nt" and not hasattr(subprocess, "CREATE_NEW_PROCESS_GROUP"):
        return {}
    return verify_common.process_group_kwargs()


def run_launcher_with_rusage(
    cmd: list[str],
    cwd: Path,
    stdout_path: Path,
    stderr_path: Path,
) -> tuple[int, float, dict[str, Any]]:
    """Run vehicle_sil_launch, redirecting output to files and reaping directly with os.wait4 on POSIX.

    Returns (returncode, total_process_wall_ms, launcher_tree_total_dict).
    Never substitutes 0 when os.wait4 is unavailable (e.g. Windows): returns status="unavailable" and nulls.
    """
    with (
        open(stdout_path, "wb") as out_f,
        open(stderr_path, "wb") as err_f,
    ):
        t0 = time.monotonic_ns()
        proc = subprocess.Popen(
            cmd,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=out_f,
            stderr=err_f,
            **_popen_group_kwargs(),
        )
        try:
            if os.name != "nt" and hasattr(os, "wait4"):
                try:
                    _, status, ru = os.wait4(proc.pid, 0)
                    wall_ms = (time.monotonic_ns() - t0) / 1e6
                    returncode = os.waitstatus_to_exitcode(status)
                    proc.returncode = returncode
                    user_ms = round(max(0.0, float(ru.ru_utime) * 1000.0), 3)
                    sys_ms = round(max(0.0, float(ru.ru_stime) * 1000.0), 3)
                    total_ms = round(user_ms + sys_ms, 3)
                    raw_rss = int(ru.ru_maxrss)
                    max_rss_kb = raw_rss // 1024 if sys.platform == "darwin" else raw_rss
                    tree_cpu: dict[str, Any] = {
                        "status": "available",
                        "reason": None,
                        "interval": "full_process_lifetime",
                        "includes_non_steady_state": True,
                        "target_processes": ["vehicle_sil_launch", "<reaped_children>"],
                        "user_ms": user_ms,
                        "sys_ms": sys_ms,
                        "total_ms": total_ms,
                        "max_rss_kb": max(0, max_rss_kb),
                    }
                    return returncode, wall_ms, tree_cpu
                except OSError as exc:
                    returncode = proc.wait()
                    wall_ms = (time.monotonic_ns() - t0) / 1e6
                    tree_cpu = {
                        "status": "unavailable",
                        "reason": f"os.wait4 failed: {exc}",
                        "interval": "full_process_lifetime",
                        "includes_non_steady_state": True,
                        "target_processes": ["vehicle_sil_launch", "<reaped_children>"],
                        "user_ms": None,
                        "sys_ms": None,
                        "total_ms": None,
                        "max_rss_kb": None,
                    }
                    return returncode, wall_ms, tree_cpu
            else:
                returncode = proc.wait()
                wall_ms = (time.monotonic_ns() - t0) / 1e6
                tree_cpu = {
                    "status": "unavailable",
                    "reason": "os.wait4 not available on this platform",
                    "interval": "full_process_lifetime",
                    "includes_non_steady_state": True,
                    "target_processes": ["vehicle_sil_launch", "<reaped_children>"],
                    "user_ms": None,
                    "sys_ms": None,
                    "total_ms": None,
                    "max_rss_kb": None,
                }
                return returncode, wall_ms, tree_cpu
        except BaseException:
            verify_common.stop_group(proc)
            raise


# ---------------------------------------------------------------------------
# Trace parsing & deterministic verification
# ---------------------------------------------------------------------------


def parse_comm_tsv(path: Path) -> list[TraceEvent]:
    events: list[TraceEvent] = []
    if not path.is_file():
        raise VerificationError(f"missing communication trace file: {path}")
    for lineno, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if not raw:
            continue
        parts = raw.split("\t")
        if len(parts) != 6:
            raise VerificationError(f"{path}:{lineno}: expected 6 tab-separated fields, got {len(parts)}")
        direction, tx_s, rx_s, id_s, seq_s, payload_hex = parts
        if direction not in ("T", "R"):
            raise VerificationError(f"{path}:{lineno}: invalid direction {direction!r}")
        try:
            tx_ms = int(tx_s)
            rx_ms = None if rx_s == "-" else int(rx_s)
            can_id = int(id_s, 0)
            seq = int(seq_s)
        except ValueError as exc:
            raise VerificationError(f"{path}:{lineno}: malformed integer field: {exc}") from exc
        if direction == "T" and rx_ms is not None:
            raise VerificationError(f"{path}:{lineno}: TX event must have rx_ms == '-'")
        if direction == "R" and rx_ms is None:
            raise VerificationError(f"{path}:{lineno}: RX event missing rx_ms")
        events.append(TraceEvent(direction, tx_ms, rx_ms, can_id, seq, payload_hex.lower()))
    return events


def _require_int(obj: dict[str, Any], key: str, context: str) -> int:
    if key not in obj:
        raise VerificationError(f"{context}: missing required integer key {key!r}")
    val = obj[key]
    if isinstance(val, bool) or not isinstance(val, int):
        raise VerificationError(f"{context}: {key!r} must be an integer, got {val!r}")
    return val


def _payload_u64_le(payload_hex: str, context: str) -> int:
    try:
        raw = bytes.fromhex(payload_hex)
    except ValueError as exc:
        raise VerificationError(f"{context}: malformed hex payload {payload_hex!r}: {exc}") from exc
    return int.from_bytes(raw[:8].ljust(8, b"\x00"), "little")


def verify_iteration_traces(
    case: CaseSpec,
    warmup_ms: int,
    measure_ms: int,
    ecu_summaries: dict[str, dict[str, Any]],
    ecu_traces: dict[str, list[TraceEvent]],
) -> dict[str, Any]:
    """Verify exact window completeness, zero drops, causal CAN TX/RX delivery, and compute digests."""
    measure_end_ms = warmup_ms + measure_ms
    ecu_by_name = {e.name: e for e in case.ecus}

    if set(ecu_summaries.keys()) != set(ecu_by_name.keys()):
        raise VerificationError(
            f"{case.case_id}: ECU summary set mismatch: expected {sorted(ecu_by_name)}, got {sorted(ecu_summaries)}"
        )
    if set(ecu_traces.keys()) != set(ecu_by_name.keys()):
        raise VerificationError(
            f"{case.case_id}: ECU trace set mismatch: expected {sorted(ecu_by_name)}, got {sorted(ecu_traces)}"
        )

    # 1. Step & window completeness + zero CAN drops/overflows + zero sub-ms RX timestamps.
    for ecu in case.ecus:
        ctx = f"{case.case_id}/{ecu.name}"
        summary = ecu_summaries[ecu.name]
        if not summary.get("saw_window_start") or not summary.get("saw_window_end"):
            raise VerificationError(f"{ctx}: missing steady-state window boundary snapshot")
        if summary.get("window_start_step_ms") != warmup_ms:
            raise VerificationError(
                f"{ctx}: window_start_step_ms={summary.get('window_start_step_ms')}, expected {warmup_ms}"
            )
        if summary.get("window_end_step_ms") != measure_end_ms:
            raise VerificationError(
                f"{ctx}: window_end_step_ms={summary.get('window_end_step_ms')}, expected {measure_end_ms}"
            )
        expected_steps = measure_ms // ecu.step_ms
        if summary.get("window_steps") != expected_steps:
            raise VerificationError(
                f"{ctx}: window_steps={summary.get('window_steps')}, expected {expected_steps}"
            )
        sub_ms = _require_int(summary, "sub_ms_rx_timestamps", ctx)
        if sub_ms != 0:
            raise VerificationError(f"{ctx}: non-integer-ms CAN RX timestamps detected (sub_ms_rx_timestamps={sub_ms})")
        can_stats = summary.get("can_stats")
        if not isinstance(can_stats, dict):
            raise VerificationError(f"{ctx}: missing required can_stats object")
        for stat_key in ("receive_overflows", "sends_rejected", "frames_dropped", "port_receive_overflows"):
            val = _require_int(can_stats, stat_key, f"{ctx}.can_stats")
            if val != 0:
                raise VerificationError(f"{ctx}: non-zero CAN drop counter {stat_key}={val}")

    # 2. Sender TX schedule verification over [0, W + M).
    tx_by_sender: dict[str, list[TraceEvent]] = {}
    rx_by_receiver: dict[str, list[TraceEvent]] = {}

    for ecu in case.ecus:
        ctx = f"{case.case_id}/{ecu.name}"
        events = ecu_traces[ecu.name]
        tx_events = [ev for ev in events if ev.direction == "T"]
        rx_events = [ev for ev in events if ev.direction == "R"]
        tx_by_sender[ecu.name] = tx_events
        rx_by_receiver[ecu.name] = rx_events

        expected_tx_times: list[tuple[int, int]] = []
        if ecu.can_period_ms > 0:
            t = ecu.can_offset_ms
            while t < measure_end_ms:
                for f_idx in range(ecu.can_frames):
                    expected_tx_times.append((t, f_idx))
                t += ecu.can_period_ms

        if len(tx_events) != len(expected_tx_times):
            raise VerificationError(
                f"{ctx}: TX count {len(tx_events)} != expected {len(expected_tx_times)}"
            )

        expected_id = CAN_BASE_ID + ecu.ecu_id
        for idx, (ev, (exp_t, exp_f)) in enumerate(zip(tx_events, expected_tx_times, strict=True)):
            if ev.tx_ms >= measure_end_ms:
                raise VerificationError(f"{ctx}: TX at tx_ms={ev.tx_ms} >= W+M ({measure_end_ms})")
            if ev.tx_ms != exp_t:
                raise VerificationError(f"{ctx}: TX #{idx} at tx_ms={ev.tx_ms}, expected {exp_t}")
            if ev.can_id != expected_id:
                raise VerificationError(f"{ctx}: TX #{idx} id=0x{ev.can_id:x}, expected 0x{expected_id:x}")
            exp_seq = idx & 0xFFFF
            if ev.seq != exp_seq:
                raise VerificationError(f"{ctx}: TX #{idx} seq={ev.seq}, expected {exp_seq}")
            if len(ev.payload_hex) != 16:
                raise VerificationError(
                    f"{ctx}: TX #{idx} payload length {len(ev.payload_hex)} != 16 hex chars"
                )
            try:
                raw_bytes = bytes.fromhex(ev.payload_hex)
            except ValueError as exc:
                raise VerificationError(f"{ctx}: TX #{idx} malformed hex payload {ev.payload_hex!r}: {exc}") from exc
            if raw_bytes[0] != (ecu.ecu_id & 0xFF) or raw_bytes[1] != (exp_f & 0xFF):
                raise VerificationError(f"{ctx}: TX #{idx} payload header mismatch ({ev.payload_hex})")
            wire_seq = raw_bytes[2] | (raw_bytes[3] << 8)
            if wire_seq != exp_seq:
                raise VerificationError(f"{ctx}: TX #{idx} payload seq={wire_seq}, expected {exp_seq}")

        summary = ecu_summaries[ecu.name]
        exp_tx_in_win = sum(1 for ev in tx_events if warmup_ms <= ev.tx_ms < measure_end_ms)
        if summary.get("tx_total") != len(tx_events) or summary.get("tx_in_window") != exp_tx_in_win:
            raise VerificationError(
                f"{ctx}: summary TX counts ({summary.get('tx_total')}, {summary.get('tx_in_window')}) "
                f"!= trace counts ({len(tx_events)}, {exp_tx_in_win})"
            )
        frames_sent = _require_int(summary["can_stats"], "frames_sent", f"{ctx}.can_stats")
        if frames_sent != len(tx_events):
            raise VerificationError(
                f"{ctx}: can_stats.frames_sent={frames_sent} != tx_total={len(tx_events)}"
            )

    # 3. Causal RX verification over {tx_ms < W + M, rx_ms <= W + M} in strict canonical order.
    for receiver in case.ecus:
        ctx = f"{case.case_id}/{receiver.name}"
        actual_rx = rx_by_receiver[receiver.name]
        for ev in actual_rx:
            if ev.rx_ms is None or ev.rx_ms > measure_end_ms:
                raise VerificationError(
                    f"{ctx}: unexpected RX at rx_ms={ev.rx_ms} > W+M ({measure_end_ms})"
                )
            if ev.tx_ms >= measure_end_ms:
                raise VerificationError(
                    f"{ctx}: unexpected RX with tx_ms={ev.tx_ms} >= W+M ({measure_end_ms})"
                )

        expected_rx: list[tuple[int, int, int, int, str]] = []
        for sender in case.ecus:
            if sender.name == receiver.name:
                continue
            for tx_ev in tx_by_sender[sender.name]:
                exp_rx_ms = ((tx_ev.tx_ms // receiver.step_ms) + 1) * receiver.step_ms
                if exp_rx_ms > measure_end_ms:
                    raise VerificationError(
                        f"{ctx}: expected causal rx_ms={exp_rx_ms} exceeds W+M ({measure_end_ms})"
                    )
                expected_rx.append((exp_rx_ms, tx_ev.tx_ms, tx_ev.can_id, tx_ev.seq, tx_ev.payload_hex))

        # Match tools/bench_ecu/main.cpp canonical order: (rx_ms, tx_ms, id, payload_u64_le).
        expected_rx.sort(
            key=lambda item: (item[0], item[1], item[2], _payload_u64_le(item[4], ctx))
        )
        # Preserve actual_rx order without re-sorting so trace reordering is caught!
        actual_rx_tuples = [
            (ev.rx_ms, ev.tx_ms, ev.can_id, ev.seq, ev.payload_hex) for ev in actual_rx
        ]

        if len(actual_rx_tuples) != len(expected_rx):
            raise VerificationError(
                f"{ctx}: RX count {len(actual_rx_tuples)} != expected {len(expected_rx)}"
            )
        for idx, (act, exp) in enumerate(zip(actual_rx_tuples, expected_rx, strict=True)):
            if act != exp:
                raise VerificationError(
                    f"{ctx}: RX mismatch at index {idx}: "
                    f"got (rx_ms={act[0]}, tx_ms={act[1]}, id=0x{act[2]:x}, seq={act[3]}, payload={act[4]}), "
                    f"expected (rx_ms={exp[0]}, tx_ms={exp[1]}, id=0x{exp[2]:x}, seq={exp[3]}, payload={exp[4]})"
                )

        summary = ecu_summaries[receiver.name]
        exp_rx_in_win = sum(1 for ev in actual_rx if warmup_ms <= ev.tx_ms < measure_end_ms)
        if summary.get("rx_total") != len(actual_rx) or summary.get("rx_in_window") != exp_rx_in_win:
            raise VerificationError(
                f"{ctx}: summary RX counts ({summary.get('rx_total')}, {summary.get('rx_in_window')}) "
                f"!= trace counts ({len(actual_rx)}, {exp_rx_in_win})"
            )
        frames_received = _require_int(summary["can_stats"], "frames_received", f"{ctx}.can_stats")
        if frames_received != len(actual_rx):
            raise VerificationError(
                f"{ctx}: can_stats.frames_received={frames_received} != rx_total={len(actual_rx)}"
            )

    # 4. Canonical SHA-256 digests (per-ECU order preserved without re-sorting events).
    out_hasher = hashlib.sha256()
    for ecu in sorted(case.ecus, key=lambda e: e.name):
        s = ecu_summaries[ecu.name]
        line = f"{ecu.name}\t{s['window_steps']}\t{s['state_digest_hex']}\n"
        out_hasher.update(line.encode("utf-8"))

    trace_hasher = hashlib.sha256()
    for ecu in sorted(case.ecus, key=lambda e: e.name):
        for ev in ecu_traces[ecu.name]:
            rx_val = -1 if ev.rx_ms is None else ev.rx_ms
            line = f"{ecu.name}\t{ev.direction}\t{ev.tx_ms}\t{rx_val}\t0x{ev.can_id:x}\t{ev.seq}\t{ev.payload_hex}\n"
            trace_hasher.update(line.encode("utf-8"))

    tx_in_window = sum(int(ecu_summaries[e.name]["tx_in_window"]) for e in case.ecus)
    rx_in_window = sum(int(ecu_summaries[e.name]["rx_in_window"]) for e in case.ecus)
    tx_total = sum(int(ecu_summaries[e.name]["tx_total"]) for e in case.ecus)
    rx_total = sum(int(ecu_summaries[e.name]["rx_total"]) for e in case.ecus)

    return {
        "output_sha256": out_hasher.hexdigest(),
        "comm_trace_sha256": trace_hasher.hexdigest(),
        "comm_events_verified": tx_total + rx_total,
        "tx_in_window": tx_in_window,
        "rx_in_window": rx_in_window,
        "tx_total": tx_total,
        "rx_total": rx_total,
    }


# ---------------------------------------------------------------------------
# Issue 1 Control transport metrics integration
# ---------------------------------------------------------------------------


def parse_and_validate_control_metrics(
    path: Path,
    require_available: bool,
) -> dict[str, Any]:
    if not path.is_file():
        raise VerificationError(f"control metrics file missing: {path}")
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise VerificationError(f"failed to parse control metrics JSON {path}: {exc}") from exc
    if not isinstance(raw, dict):
        raise VerificationError(f"control metrics JSON {path} is not an object")
    if raw.get("vehicle_sil_control_metrics") != 1:
        raise VerificationError(
            f"unsupported vehicle_sil_control_metrics version: {raw.get('vehicle_sil_control_metrics')!r}"
        )

    status = raw.get("status")
    if status == "unavailable":
        reason = raw.get("unavailable_reason")
        if reason not in ISSUE1_UNAVAILABLE_REASONS:
            raise VerificationError(f"unknown control metrics unavailable_reason: {reason!r}")
        if raw.get("counters") is not None:
            raise VerificationError("unavailable control metrics must have counters == null")
        if require_available:
            raise VerificationError(f"--control-metrics on expected available metrics, got unavailable ({reason})")
        src_raw = raw.get("source")
        src_out = None
        if isinstance(src_raw, dict):
            src_out = {
                "transport": src_raw.get("transport"),
                "collector": src_raw.get("collector"),
                "scope": src_raw.get("scope"),
                "collection_point": src_raw.get("collection_point"),
            }
        return {
            "status": "unavailable",
            "unavailable_reason": reason,
            "source": src_out,
            "counters": None,
        }

    if status != "available":
        raise VerificationError(f"invalid control metrics status: {status!r}")
    if raw.get("unavailable_reason") is not None:
        raise VerificationError("available control metrics must have unavailable_reason == null")

    src = raw.get("source")
    if not isinstance(src, dict):
        raise VerificationError("available control metrics missing source object")
    if src.get("transport") != "silkit" or src.get("collector") != "HubTransport::stats":
        raise VerificationError(f"unexpected control metrics source: {src!r}")
    if src.get("scope") != "cumulative_hub_transport_lifetime":
        raise VerificationError(f"unexpected control metrics scope: {src.get('scope')!r}")

    counters = raw.get("counters")
    if not isinstance(counters, dict):
        raise VerificationError("available control metrics missing counters object")
    required_keys = (
        "messages",
        "bytes",
        "handler_calls",
        "handler_time_ns",
        "max_handler_time_ns",
        "hellos",
        "last_words",
        "late_messages",
        "undecodable",
        "unknown",
        "refused_hellos",
    )
    parsed_counters: dict[str, int] = {}
    for k in required_keys:
        v = counters.get(k)
        if isinstance(v, bool) or not isinstance(v, int) or v < 0:
            raise VerificationError(f"control metrics counter {k} must be non-negative int, got {v!r}")
        parsed_counters[k] = v

    if parsed_counters["max_handler_time_ns"] > parsed_counters["handler_time_ns"]:
        raise VerificationError(
            f"control metrics invariant violated: max_handler_time_ns ({parsed_counters['max_handler_time_ns']}) "
            f"> handler_time_ns ({parsed_counters['handler_time_ns']})"
        )
    if parsed_counters["messages"] > parsed_counters["handler_calls"]:
        raise VerificationError(
            f"control metrics invariant violated: messages ({parsed_counters['messages']}) "
            f"> handler_calls ({parsed_counters['handler_calls']})"
        )
    for zero_key in ("late_messages", "undecodable", "unknown", "refused_hellos"):
        if parsed_counters[zero_key] != 0:
            raise VerificationError(
                f"control metrics benchmark-clean invariant violated: {zero_key} == {parsed_counters[zero_key]}"
            )

    return {
        "status": "available",
        "unavailable_reason": None,
        "source": {
            "transport": "silkit",
            "collector": "HubTransport::stats",
            "scope": "cumulative_hub_transport_lifetime",
            "collection_point": src.get("collection_point"),
        },
        "counters": parsed_counters,
    }


def launcher_supports_control_metrics(version_info: dict[str, Any]) -> bool:
    features = version_info.get("features", [])
    formats = version_info.get("formats", {})
    return (
        isinstance(features, list)
        and "control-metrics" in features
        and isinstance(formats, dict)
        and formats.get("control_metrics") == 1
    )


# ---------------------------------------------------------------------------
# Simulation config & scenario generation
# ---------------------------------------------------------------------------


def write_case_files(
    work_dir: Path,
    case: CaseSpec,
    bench_ecu_path: Path,
    warmup_ms: int,
    measure_ms: int,
    tail_ms: int,
    ecu_out_dir: Path,
) -> tuple[Path, Path | None]:
    """Generate simulation.yaml (format 2) and optional deterministic scenario.yaml."""
    sim_path = work_dir / "simulation.yaml"
    participants = []
    for ecu in case.ecus:
        cmd = [
            str(bench_ecu_path),
            "--bench-id", str(ecu.ecu_id),
            "--bench-step-ms", str(ecu.step_ms),
            "--bench-vars", str(case.declared_vars),
            "--bench-sample-ms", str(ecu.sample_ms),
            "--bench-can-period-ms", str(ecu.can_period_ms),
            "--bench-can-offset-ms", str(ecu.can_offset_ms),
            "--bench-can-frames", str(ecu.can_frames),
            "--bench-step-work", str(case.step_work),
            "--bench-warmup-ms", str(warmup_ms),
            "--bench-measure-ms", str(measure_ms),
            "--bench-tail-ms", str(tail_ms),
            "--bench-out-dir", str(ecu_out_dir),
        ]
        participants.append({"name": ecu.name, "command": cmd})

    # Format 2 YAML is a superset of JSON; writing JSON syntax keeps stdlib-only YAML emission exact.
    sim_doc = {
        "vehicle_sil_simulation": 2,
        "networks": [{"name": CAN_NETWORK_NAME, "kind": "can"}],
        "participants": participants,
    }
    sim_path.write_text(json.dumps(sim_doc, indent=2) + "\n", encoding="utf-8", newline="\n")

    scenario_path: Path | None = None
    if case.with_scenario:
        scenario_path = work_dir / "scenario.yaml"
        t1 = warmup_ms + 10
        t2 = warmup_ms + (measure_ms // 2)
        t3 = warmup_ms + measure_ms - 10
        t_end = warmup_ms + measure_ms + tail_ms - case_time_step_ms(case)
        steps = [
            {
                "at_ms": t1,
                "set": {f"{e.name}.probe.cmd": float(10 + e.ecu_id) for e in case.ecus},
            },
            {
                "at_ms": t1 + 5,
                "expect": {f"{e.name}.probe.echo": float(10 + e.ecu_id) for e in case.ecus},
            },
            {
                "at_ms": t2,
                "set": {f"{e.name}.probe.cmd": float(20 + e.ecu_id) for e in case.ecus},
            },
            {
                "at_ms": t2 + 5,
                "expect": {f"{e.name}.probe.echo": float(20 + e.ecu_id) for e in case.ecus},
            },
            {
                "at_ms": t3,
                "set": {f"{e.name}.probe.cmd": float(30 + e.ecu_id) for e in case.ecus},
            },
            {
                "at_ms": t_end,
                "expect": {f"{e.name}.probe.echo": float(30 + e.ecu_id) for e in case.ecus},
            },
        ]
        scenario_doc = {
            "vehicle_sil_scenario": 1,
            "name": case.case_id,
            "steps": steps,
        }
        scenario_path.write_text(json.dumps(scenario_doc, indent=2) + "\n", encoding="utf-8", newline="\n")

    return sim_path, scenario_path


# ---------------------------------------------------------------------------
# Summary statistics helpers
# ---------------------------------------------------------------------------


def _percentile(sorted_vals: list[float], p: float) -> float:
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    k = (len(sorted_vals) - 1) * p
    lo = math.floor(k)
    hi = math.ceil(k)
    if lo == hi:
        return sorted_vals[lo]
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def summarize_triple(values: list[float], *, as_int: bool = False) -> dict[str, int | float]:
    med = statistics.median(values)
    lo = min(values)
    hi = max(values)
    if as_int:
        return {"median": int(round(med)), "min": int(lo), "max": int(hi)}
    return {"median": round(float(med), 3), "min": round(float(lo), 3), "max": round(float(hi), 3)}


def summarize_with_iqr(values: list[float]) -> dict[str, float]:
    ordered = sorted(float(v) for v in values)
    q1 = _percentile(ordered, 0.25)
    q3 = _percentile(ordered, 0.75)
    return {
        "median": round(float(statistics.median(ordered)), 3),
        "min": round(ordered[0], 3),
        "max": round(ordered[-1], 3),
        "iqr": round(max(0.0, q3 - q1), 3),
    }


def summarize_optional_cpu(records: list[dict[str, Any]]) -> dict[str, Any]:
    if all(r["status"] == "available" and r["total_ms"] is not None for r in records):
        vals = [float(r["total_ms"]) for r in records]
        return {"status": "available", **summarize_triple(vals)}
    return {"status": "unavailable", "median": None, "min": None, "max": None}


def summarize_control_metrics(records: list[dict[str, Any]]) -> dict[str, Any]:
    if all(r["status"] == "available" and r["counters"] is not None for r in records):
        return {
            "status": "available",
            "scope": "cumulative_hub_transport_lifetime",
            "handler_time_ns": summarize_triple([r["counters"]["handler_time_ns"] for r in records], as_int=True),
            "max_handler_time_ns": summarize_triple([r["counters"]["max_handler_time_ns"] for r in records], as_int=True),
            "messages": summarize_triple([r["counters"]["messages"] for r in records], as_int=True),
            "bytes": summarize_triple([r["counters"]["bytes"] for r in records], as_int=True),
            "handler_calls": summarize_triple([r["counters"]["handler_calls"] for r in records], as_int=True),
        }
    status = "not_requested" if all(r["status"] == "not_requested" for r in records) else "unavailable"
    return {
        "status": status,
        "scope": None,
        "handler_time_ns": None,
        "max_handler_time_ns": None,
        "messages": None,
        "bytes": None,
        "handler_calls": None,
    }


# ---------------------------------------------------------------------------
# Iteration execution & validation
# ---------------------------------------------------------------------------


def validate_launcher_report(
    case: CaseSpec,
    run_report: dict[str, Any],
    expected_stop_at_ms: int,
    expected_time_step_ms: int,
) -> tuple[int, int]:
    """Validate vehicle_sil_launch run --json invariants. Returns (launcher_run_wall_ms, paused_ms)."""
    if run_report.get("result") != "passed":
        raise VerificationError(
            f"{case.case_id}: launcher result={run_report.get('result')!r}, findings={run_report.get('findings')}"
        )
    findings = run_report.get("findings") or []
    bad_findings = [f for f in findings if isinstance(f, dict) and f.get("severity") in ("error", "warning")]
    if bad_findings:
        raise VerificationError(f"{case.case_id}: launcher reported error/warning findings: {bad_findings}")

    timing = run_report.get("timing")
    if not isinstance(timing, dict):
        raise VerificationError(f"{case.case_id}: launcher run --json missing timing object")
    if timing.get("sim_ms") != expected_stop_at_ms:
        raise VerificationError(
            f"{case.case_id}: timing.sim_ms={timing.get('sim_ms')}, expected {expected_stop_at_ms}"
        )
    if timing.get("time_step_ms") != expected_time_step_ms:
        raise VerificationError(
            f"{case.case_id}: timing.time_step_ms={timing.get('time_step_ms')}, expected {expected_time_step_ms}"
        )
    paused_ms = _require_int(timing, "paused_ms", f"{case.case_id}.timing")
    if paused_ms != 0:
        raise VerificationError(f"{case.case_id}: measurement boundary violated: timing.paused_ms={paused_ms} != 0")
    paced_steps = _require_int(timing, "paced_steps", f"{case.case_id}.timing")
    if paced_steps != 0:
        raise VerificationError(f"{case.case_id}: measurement boundary violated: timing.paced_steps={paced_steps} != 0")

    participants = run_report.get("participants")
    if not isinstance(participants, list):
        raise VerificationError(f"{case.case_id}: launcher run --json missing participants list")
    by_name: dict[str, dict[str, Any]] = {}
    for p in participants:
        if not isinstance(p, dict) or not isinstance(p.get("name"), str):
            raise VerificationError(f"{case.case_id}: malformed participant entry in launcher report: {p!r}")
        if p["name"] in by_name:
            raise VerificationError(f"{case.case_id}: duplicate participant {p['name']!r} in launcher report")
        by_name[p["name"]] = p
    expected_names = {ecu.name for ecu in case.ecus}
    if set(by_name.keys()) != expected_names:
        raise VerificationError(
            f"{case.case_id}: launcher participants {sorted(by_name)} != expected {sorted(expected_names)}"
        )
    for ecu in case.ecus:
        p = by_name[ecu.name]
        if p.get("exit_code") != 0:
            raise VerificationError(f"{case.case_id}/{ecu.name}: exit_code={p.get('exit_code')} != 0")
        cl = p.get("control_link")
        if not isinstance(cl, dict):
            raise VerificationError(f"{case.case_id}/{ecu.name}: missing control_link object")
        if cl.get("state") != "connected":
            raise VerificationError(f"{case.case_id}/{ecu.name}: control_link.state={cl.get('state')!r} != 'connected'")
        late_msgs = _require_int(cl, "late_messages", f"{case.case_id}/{ecu.name}.control_link")
        if late_msgs != 0:
            raise VerificationError(
                f"{case.case_id}/{ecu.name}: control_link.late_messages={late_msgs} != 0"
            )

    wall_ms = _require_int(timing, "wall_ms", f"{case.case_id}.timing")
    return max(0, wall_ms - paused_ms), paused_ms


def run_single_iteration(
    *,
    case: CaseSpec,
    phase: str,
    index: int,
    iter_dir: Path,
    launcher_path: Path,
    bench_ecu_path: Path,
    warmup_ms: int,
    measure_ms: int,
    tail_ms: int,
    control_metrics_mode: str,
    launcher_has_control_metrics: bool,
    wait_quiet: bool,
    wait_limit_s: float,
) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    iter_dir.mkdir(parents=True, exist_ok=True)
    ecu_out_dir = iter_dir / "ecu_out"
    ecu_out_dir.mkdir(parents=True, exist_ok=True)

    stop_at_ms = warmup_ms + measure_ms + tail_ms
    expected_time_step_ms = case_time_step_ms(case)
    sim_path, scenario_path = write_case_files(
        iter_dir, case, bench_ecu_path, warmup_ms, measure_ms, tail_ms, ecu_out_dir
    )

    launcher_stop_at_ms = stop_at_ms + 10 if scenario_path is not None else stop_at_ms
    cmd = [
        str(launcher_path),
        "run",
        "--json",
        "--speed", "max",
        "--stop-at-ms", str(launcher_stop_at_ms),
    ]
    if scenario_path is not None:
        cmd += ["--scenario", str(scenario_path)]

    cm_path: Path | None = None
    if control_metrics_mode == "on" or (control_metrics_mode == "auto" and launcher_has_control_metrics):
        cm_path = iter_dir / "control-metrics.json"
        cmd += ["--control-metrics", str(cm_path)]
    cmd.append(str(sim_path))

    waited_s = 0.0
    if wait_quiet:
        t_wait = time.monotonic()
        busy_machine.wait_until_quiet("benchmark-vtime", limit_s=wait_limit_s)
        waited_s = round(time.monotonic() - t_wait, 3)

    others_before = busy_machine.busy_processes()
    load1_before = _os_load1()

    stdout_path = iter_dir / "launcher.stdout.json"
    stderr_path = iter_dir / "launcher.stderr.log"
    rc, total_process_wall_ms, launcher_tree_cpu = run_launcher_with_rusage(cmd, iter_dir, stdout_path, stderr_path)

    others_after = busy_machine.busy_processes()
    load1_after = _os_load1()

    if rc != 0:
        stderr_tail = stderr_path.read_text(encoding="utf-8", errors="replace")[-1500:]
        stdout_tail = stdout_path.read_text(encoding="utf-8", errors="replace")[-1500:]
        raise VerificationError(
            f"{case.case_id} ({phase} #{index}): vehicle_sil_launch exited with {rc}\n"
            f"stdout: {stdout_tail}\nstderr: {stderr_tail}"
        )

    try:
        run_report = json.loads(stdout_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise VerificationError(f"{case.case_id} ({phase} #{index}): invalid launcher JSON output: {exc}") from exc

    launcher_run_wall_ms, paused_ms = validate_launcher_report(
        case, run_report, stop_at_ms, expected_time_step_ms
    )

    ecu_summaries: dict[str, dict[str, Any]] = {}
    ecu_traces: dict[str, list[TraceEvent]] = {}
    for ecu in case.ecus:
        sum_path = ecu_out_dir / f"ecu_{ecu.name}.summary.json"
        if not sum_path.is_file():
            raise VerificationError(f"{case.case_id}/{ecu.name}: missing summary file {sum_path}")
        try:
            parsed_summary = json.loads(sum_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise VerificationError(f"{case.case_id}/{ecu.name}: invalid summary JSON {sum_path}: {exc}") from exc
        if not isinstance(parsed_summary, dict):
            raise VerificationError(f"{case.case_id}/{ecu.name}: summary JSON {sum_path} must be an object")
        ecu_summaries[ecu.name] = parsed_summary
        ecu_traces[ecu.name] = parse_comm_tsv(ecu_out_dir / f"ecu_{ecu.name}.comm.tsv")

    trace_result = verify_iteration_traces(case, warmup_ms, measure_ms, ecu_summaries, ecu_traces)

    # Cross-ECU steady-state wall time: max(t_end_i) - min(t_start_i).
    starts = [int(ecu_summaries[e.name]["steady_start_ns"]) for e in case.ecus]
    ends = [int(ecu_summaries[e.name]["steady_end_ns"]) for e in case.ecus]
    wall_ns = max(ends) - min(starts)
    if wall_ns <= 0:
        raise VerificationError(f"{case.case_id}: non-positive steady-state wall_ns={wall_ns}")
    window_skew_ns = (max(starts) - min(starts)) + (max(ends) - min(ends))
    wall_ms = round(wall_ns / 1e6, 3)
    vt_ratio = round((measure_ms * 1e6) / wall_ns, 3)

    # ECU steady-state CPU aggregation.
    per_process_cpu: list[dict[str, Any]] = []
    for ecu in case.ecus:
        c = ecu_summaries[ecu.name].get("cpu", {})
        if c.get("status") == "available":
            per_process_cpu.append({
                "name": ecu.name,
                "status": "available",
                "reason": None,
                "user_ms": round(float(c["user_ms"]), 3),
                "sys_ms": round(float(c["sys_ms"]), 3),
                "total_ms": round(float(c["total_ms"]), 3),
            })
        else:
            per_process_cpu.append({
                "name": ecu.name,
                "status": "unavailable",
                "reason": str(c.get("reason") or "ECU CPU unavailable"),
                "user_ms": None,
                "sys_ms": None,
                "total_ms": None,
            })

    if all(p["status"] == "available" for p in per_process_cpu):
        ecu_user = round(sum(float(p["user_ms"]) for p in per_process_cpu), 3)
        ecu_sys = round(sum(float(p["sys_ms"]) for p in per_process_cpu), 3)
        ecu_total = round(ecu_user + ecu_sys, 3)
        ecu_cpu_block: dict[str, Any] = {
            "status": "available",
            "reason": None,
            "interval": "steady_state_window",
            "window_sim_ms": [warmup_ms, warmup_ms + measure_ms],
            "target_processes": [e.name for e in case.ecus],
            "excludes": ["vehicle_sil_launch"],
            "user_ms": ecu_user,
            "sys_ms": ecu_sys,
            "total_ms": ecu_total,
            "per_process": per_process_cpu,
        }
    else:
        first_reason = next(p["reason"] for p in per_process_cpu if p["status"] != "available")
        ecu_cpu_block = {
            "status": "unavailable",
            "reason": first_reason,
            "interval": "steady_state_window",
            "window_sim_ms": [warmup_ms, warmup_ms + measure_ms],
            "target_processes": [e.name for e in case.ecus],
            "excludes": ["vehicle_sil_launch"],
            "user_ms": None,
            "sys_ms": None,
            "total_ms": None,
            "per_process": per_process_cpu,
        }

    # Optional Control transport metrics.
    if control_metrics_mode == "off":
        cm_record: dict[str, Any] = {
            "status": "not_requested",
            "unavailable_reason": None,
            "source": None,
            "counters": None,
        }
    elif cm_path is None:
        cm_record = {
            "status": "unavailable",
            "unavailable_reason": "launcher_feature_unsupported",
            "source": None,
            "counters": None,
        }
    else:
        cm_record = parse_and_validate_control_metrics(
            cm_path, require_available=(control_metrics_mode == "on")
        )

    ecu_steps_by_p = {e.name: int(ecu_summaries[e.name]["window_steps"]) for e in case.ecus}
    total_wall_rounded = round(total_process_wall_ms, 3)
    launcher_wall_rounded = round(float(launcher_run_wall_ms), 3)
    setup_shutdown_ms = round(max(0.0, total_wall_rounded - launcher_wall_rounded), 3)

    record = {
        "index": index,
        "phase": phase,
        "steady_state": {
            "wall_ns": wall_ns,
            "wall_ms": wall_ms,
            "window_skew_ns": window_skew_ns,
            "virtual_to_realtime_ratio": vt_ratio,
            "completed_steps": {
                "controller_window_steps": measure_ms // expected_time_step_ms,
                "ecu_window_steps_total": sum(ecu_steps_by_p.values()),
                "ecu_window_steps_by_participant": ecu_steps_by_p,
            },
            "app_messages": {
                "tx_in_window": trace_result["tx_in_window"],
                "rx_in_window": trace_result["rx_in_window"],
                "tx_total": trace_result["tx_total"],
                "rx_total": trace_result["rx_total"],
                "tx_dropped": 0,
                "rx_dropped": 0,
            },
        },
        "non_steady_state": {
            "total_process_wall_ms": total_wall_rounded,
            "launcher_run_wall_ms": launcher_wall_rounded,
            "setup_and_shutdown_wall_ms": setup_shutdown_ms,
            "paused_ms": paused_ms,
        },
        "process_cpu": {
            "ecu_steady_state": ecu_cpu_block,
            "launcher_tree_total": launcher_tree_cpu,
        },
        "control_transport_metrics": cm_record,
        "determinism": {
            "verified": True,
            "output_sha256": trace_result["output_sha256"],
            "comm_trace_sha256": trace_result["comm_trace_sha256"],
            "comm_events_verified": trace_result["comm_events_verified"],
        },
        "environment_noise": {
            "waited_s": waited_s,
            "others_before": others_before,
            "others_after": others_after,
            "load1_before": load1_before,
            "load1_after": load1_after,
        },
    }
    return record, ecu_summaries


def build_case_report(
    case: CaseSpec,
    warmup_ms: int,
    measure_ms: int,
    tail_ms: int,
    warmup_records: list[dict[str, Any]],
    measured_records: list[dict[str, Any]],
    last_ecu_summaries: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    """Assemble one case report and assert cross-iteration bit-exact determinism."""
    all_records = [*warmup_records, *measured_records]
    if not measured_records:
        raise VerificationError(f"{case.case_id}: no measured iterations recorded")

    ref_out = all_records[0]["determinism"]["output_sha256"]
    ref_trace = all_records[0]["determinism"]["comm_trace_sha256"]
    for rec in all_records[1:]:
        if rec["determinism"]["output_sha256"] != ref_out:
            raise VerificationError(
                f"{case.case_id}: output_sha256 mismatch across iterations "
                f"({rec['phase']} #{rec['index']}: {rec['determinism']['output_sha256']} != {ref_out})"
            )
        if rec["determinism"]["comm_trace_sha256"] != ref_trace:
            raise VerificationError(
                f"{case.case_id}: comm_trace_sha256 mismatch across iterations "
                f"({rec['phase']} #{rec['index']}: {rec['determinism']['comm_trace_sha256']} != {ref_trace})"
            )

    ecus_topology = []
    control_samples_per_s = 0.0
    can_tx_per_s = 0.0
    for ecu in case.ecus:
        s = last_ecu_summaries[ecu.name]
        eff_sample_ms = int(s["effective_sample_period_ms"])
        control_samples_per_s += 1000.0 / eff_sample_ms
        if ecu.can_period_ms > 0:
            can_tx_per_s += (1000.0 / ecu.can_period_ms) * ecu.can_frames
        ecus_topology.append({
            "name": ecu.name,
            "step_ms": ecu.step_ms,
            "configured_sample_period_ms": int(s["configured_sample_period_ms"]),
            "effective_sample_period_ms": eff_sample_ms,
            "sample_every_step_active": bool(s["sample_every_step_active"]),
            "can_period_ms": ecu.can_period_ms,
            "can_offset_ms": ecu.can_offset_ms,
        })

    can_rx_per_s = can_tx_per_s * max(0, len(case.ecus) - 1)
    stop_at_ms = warmup_ms + measure_ms + tail_ms

    # Summary is computed strictly over measured_records (warmup_records excluded!).
    wall_vals = [float(r["steady_state"]["wall_ms"]) for r in measured_records]
    ratio_vals = [float(r["steady_state"]["virtual_to_realtime_ratio"]) for r in measured_records]
    ecu_cpu_recs = [r["process_cpu"]["ecu_steady_state"] for r in measured_records]
    tree_cpu_recs = [r["process_cpu"]["launcher_tree_total"] for r in measured_records]
    cm_recs = [r["control_transport_metrics"] for r in measured_records]

    return {
        "id": case.case_id,
        "description": case.description,
        "topology": {
            "ecu_count": len(case.ecus),
            "silkit_participant_count": len(case.ecus) + 1,
            "os_process_count": len(case.ecus) + 1,
            "silkit_registry_hosted_in": "vehicle_sil_launch",
            "controller_hosted_in": "vehicle_sil_launch",
            "ecus": ecus_topology,
        },
        "workload": {
            "speed": "max",
            "time_step_ms_configured": None,
            "time_step_ms_resolved": case_time_step_ms(case),
            "warmup_sim_ms": warmup_ms,
            "measure_sim_ms": measure_ms,
            "tail_sim_ms": tail_ms,
            "stop_at_ms": stop_at_ms,
            "declared_vars_per_ecu": case.declared_vars,
            "wire_vars_per_ecu": case.declared_vars + BUILTIN_PEER_VARS,
            "step_work": case.step_work,
            "scenario": "deterministic_probe" if case.with_scenario else None,
            "communication_load": {
                "can_network": CAN_NETWORK_NAME,
                "can_simulation_mode": "silkit_trivial",
                "network_simulator": None,
                "can_tx_frames_per_sim_s": round(can_tx_per_s, 3),
                "can_rx_frames_per_sim_s": round(can_rx_per_s, 3),
                "control_samples_per_sim_s": round(control_samples_per_s, 3),
            },
        },
        "measurement_boundaries": {
            "steady_state_window_sim_ms": [warmup_ms, warmup_ms + measure_ms],
            "excluded_from_steady_state": [
                "setup",
                "warmup_iterations",
                f"in_run_warmup_0_to_{warmup_ms}ms",
                f"tail_{warmup_ms + measure_ms}_to_{stop_at_ms}ms",
                "hold_paused",
                "shutdown",
            ],
        },
        "warmup_iterations": warmup_records,
        "iterations": measured_records,
        "summary": {
            "iteration_count": len(measured_records),
            "steady_state_wall_ms": summarize_with_iqr(wall_vals),
            "virtual_to_realtime_ratio": summarize_triple(ratio_vals),
            "ecu_steady_state_cpu_ms": summarize_optional_cpu(ecu_cpu_recs),
            "launcher_tree_cpu_ms": summarize_optional_cpu(tree_cpu_recs),
            "control_transport_metrics": summarize_control_metrics(cm_recs),
            "determinism": {
                "all_iterations_identical": True,
                "output_sha256": ref_out,
                "comm_trace_sha256": ref_trace,
            },
        },
    }


# ---------------------------------------------------------------------------
# Binary discovery & probing
# ---------------------------------------------------------------------------


def _exe_name(stem: str) -> str:
    return f"{stem}.exe" if os.name == "nt" else stem


def resolve_binaries(
    preset: str | None,
    build_dir_arg: str | None,
    launcher_arg: str | None,
    bench_ecu_arg: str | None,
) -> tuple[Path | None, Path, Path]:
    if preset and build_dir_arg:
        raise UsageError("give either --preset or --build-dir, not both")
    build_dir: Path | None = None
    if preset:
        build_dir = ROOT / "build" / preset
    elif build_dir_arg:
        build_dir = Path(build_dir_arg)
        if not build_dir.is_absolute():
            build_dir = (Path.cwd() / build_dir).resolve()
    elif not (launcher_arg and bench_ecu_arg):
        default_preset = "windows-mingw-release" if os.name == "nt" else "linux-gcc-release"
        build_dir = ROOT / "build" / default_preset

    launcher_path = (
        Path(launcher_arg).resolve()
        if launcher_arg
        else (build_dir / "tools" / "launch" / _exe_name("vehicle_sil_launch"))  # type: ignore[operator]
    )
    bench_ecu_path = (
        Path(bench_ecu_arg).resolve()
        if bench_ecu_arg
        else (build_dir / "tools" / "bench_ecu" / _exe_name("vehicle_sil_bench_ecu"))  # type: ignore[operator]
    )

    if not launcher_path.is_file():
        raise UsageError(
            f"vehicle_sil_launch not found at {launcher_path}; build it with "
            f"`cmake --build <build-dir> --target vehicle_sil_launch vehicle_sil_bench_ecu`"
        )
    if not bench_ecu_path.is_file():
        raise UsageError(
            f"vehicle_sil_bench_ecu not found at {bench_ecu_path}; build it with "
            f"`cmake --build <build-dir> --target vehicle_sil_launch vehicle_sil_bench_ecu` "
            f"(with VEHICLE_SIL_BUILD_BENCHMARKS=ON)"
        )
    return build_dir, launcher_path, bench_ecu_path


def probe_launcher_version(launcher_path: Path) -> dict[str, Any]:
    try:
        proc = subprocess.run(
            [str(launcher_path), "version", "--json"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdin=subprocess.DEVNULL,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise UsageError(f"failed to run {launcher_path} version --json: {exc}") from exc
    if proc.returncode != 0:
        raise UsageError(f"{launcher_path} version --json failed (exit {proc.returncode}): {proc.stderr.strip()}")
    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise UsageError(f"invalid JSON from {launcher_path} version --json: {exc}") from exc
    if not isinstance(data, dict):
        raise UsageError(f"unexpected JSON root from {launcher_path} version --json")
    return data


def probe_bench_ecu_build_info(bench_ecu_path: Path) -> dict[str, Any] | None:
    try:
        proc = subprocess.run(
            [str(bench_ecu_path), "--bench-build-info"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdin=subprocess.DEVNULL,
            timeout=15,
        )
        if proc.returncode == 0:
            data = json.loads(proc.stdout)
            if isinstance(data, dict):
                return data
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
        pass
    return None


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="scripts/benchmark-vtime.py",
        description=(
            "Run the reproducible coordinated virtual-time benchmark suite, verify deterministic "
            "state digests and timestamped CAN TX/RX causal traces, and write a JSON report."
        ),
        epilog=(
            "examples:\n"
            "  scripts/benchmark-vtime.py --preset linux-gcc-release\n"
            "  scripts/benchmark-vtime.py --preset linux-gcc-debug --smoke --no-wait\n"
            "  scripts/benchmark-vtime.py --preset linux-gcc-release --cases all --control-metrics auto\n"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--preset", help="CMake preset name (uses build/<preset>)")
    parser.add_argument("--build-dir", help="CMake build directory containing tools/ and compile_commands.json")
    parser.add_argument("--launcher", help="Explicit path to vehicle_sil_launch executable")
    parser.add_argument("--bench-ecu", help="Explicit path to vehicle_sil_bench_ecu executable")
    parser.add_argument(
        "--cases",
        default="canonical",
        help="Cases to run: canonical (A/B/C), all (A/B/C/D), or comma-separated case IDs (default: canonical)",
    )
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="CI determinism smoke profile (0 warmup, 2 iterations, warmup_ms=50, measure_ms=200)",
    )
    parser.add_argument("--iterations", type=int, default=None, help="Measured iterations per case (default: 5; smoke: 2)")
    parser.add_argument("--warmup", type=int, default=None, help="Warm-up iterations per case (default: 1; smoke: 0)")
    parser.add_argument("--warmup-ms", type=int, default=None, help="In-run warm-up virtual ms W (default: 200; smoke: 50)")
    parser.add_argument("--measure-ms", type=int, default=None, help="Steady-state virtual ms M (default: 20000; smoke: 200)")
    parser.add_argument(
        "--control-metrics",
        choices=("auto", "on", "off"),
        default="auto",
        help="Optional Issue 1 Control transport metrics collection (default: auto)",
    )
    parser.add_argument("--no-wait", action="store_true", help="Do not wait for busy_machine processes")
    parser.add_argument(
        "--wait-limit",
        type=float,
        default=busy_machine.DEFAULT_LIMIT_S,
        help="Maximum seconds to wait for busy_machine (default: 1200)",
    )
    parser.add_argument("--keep-work-dir", action="store_true", help="Keep temporary work directory for inspection")
    parser.add_argument(
        "--out",
        default=str(DEFAULT_OUT),
        help="Output JSON report path (default: build/logs/benchmark-vtime.json)",
    )
    return parser


def _print_summary(report: dict[str, Any], out_path: Path) -> None:
    build = report["provenance"]["build"]
    launch_flags = build["targets"]["vehicle_sil_launch"]
    print(
        f"benchmark-vtime: {len(report['cases'])} case(s), profile={report['run_options']['profile']}, "
        f"flags_source={build['flags_source']}, opt={launch_flags['effective_opt_level'] or 'unknown'}"
    )
    if build["flags_warning"]:
        print(f"  warning: {build['flags_warning']}")
    for case in report["cases"]:
        s = case["summary"]
        wall = s["steady_state_wall_ms"]
        ratio = s["virtual_to_realtime_ratio"]
        ecu_cpu = s["ecu_steady_state_cpu_ms"]
        ecu_cpu_str = f"{ecu_cpu['median']:.1f} ms" if ecu_cpu["status"] == "available" else "unavailable"
        cm = s["control_transport_metrics"]
        cm_str = (
            f"handler={cm['handler_time_ns']['median'] / 1e6:.2f} ms"
            if cm["status"] == "available"
            else cm["status"]
        )
        print(
            f"  {case['id']}: median={wall['median']:.3f} ms (IQR {wall['iqr']:.3f} ms, "
            f"{ratio['median']:.1f}x RT), ecu_cpu={ecu_cpu_str}, control_metrics={cm_str}, "
            f"sha256={s['determinism']['output_sha256'][:12]}.. [verified]"
        )
    print(f"  report: {verify_common.rel(out_path.resolve(), ROOT)}")


def schema_ref_for_output(out_path: Path) -> str:
    schema_target = ROOT / "schema" / "benchmark-report.schema.json"
    try:
        return Path(os.path.relpath(schema_target, out_path.parent)).as_posix()
    except ValueError:
        return schema_target.as_posix()


def main(argv: list[str] | None = None) -> int:
    verify_common.reconfigure_stdio()
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        cases = resolve_cases(args.cases)
        if args.smoke:
            warmup_iters = 0 if args.warmup is None else args.warmup
            measured_iters = 2 if args.iterations is None else args.iterations
            warmup_ms = 50 if args.warmup_ms is None else args.warmup_ms
            measure_ms = 200 if args.measure_ms is None else args.measure_ms
            profile = "smoke" if (warmup_iters, measured_iters, warmup_ms, measure_ms) == (0, 2, 50, 200) else "custom"
        else:
            warmup_iters = 1 if args.warmup is None else args.warmup
            measured_iters = 5 if args.iterations is None else args.iterations
            warmup_ms = 200 if args.warmup_ms is None else args.warmup_ms
            measure_ms = 20000 if args.measure_ms is None else args.measure_ms
            profile = "default" if (warmup_iters, measured_iters, warmup_ms, measure_ms) == (1, 5, 200, 20000) else "custom"

        if warmup_iters < 0:
            raise UsageError(f"--warmup must be >= 0 (got {warmup_iters})")
        if measured_iters < 1:
            raise UsageError(f"--iterations must be >= 1 (got {measured_iters})")
        if args.wait_limit < 0:
            raise UsageError(f"--wait-limit must be >= 0 (got {args.wait_limit})")

        validate_window_alignment(cases, warmup_ms, measure_ms)
        build_dir, launcher_path, bench_ecu_path = resolve_binaries(
            args.preset, args.build_dir, args.launcher, args.bench_ecu
        )
        version_info = probe_launcher_version(launcher_path)
        has_cm = launcher_supports_control_metrics(version_info)
        if args.control_metrics == "on" and not has_cm:
            raise UsageError(
                f"--control-metrics on requested, but {launcher_path} does not advertise "
                f"'control-metrics' feature (formats.control_metrics == 1)"
            )
    except UsageError as exc:
        print(f"benchmark-vtime: {exc}", file=sys.stderr)
        return 2

    bench_ecu_build_info = probe_bench_ecu_build_info(bench_ecu_path)
    build_prov, cache_vals = extract_build_provenance(
        build_dir,
        bench_ecu_build_info,
        launcher_path=launcher_path,
        bench_ecu_path=bench_ecu_path,
    )
    repo_prov, deps_prov = _collect_repo_and_dependency_provenance(version_info, cache_vals)
    host_prov = _collect_host_provenance()

    out_path = Path(args.out)
    if not out_path.is_absolute():
        out_path = (Path.cwd() / out_path).resolve()

    work_root = Path(tempfile.mkdtemp(prefix="vehicle-sil-bench-"))
    try:
        case_reports: list[dict[str, Any]] = []
        for case in cases:
            tail_ms = case_tail_sim_ms(case)
            warmup_records: list[dict[str, Any]] = []
            measured_records: list[dict[str, Any]] = []
            last_summaries: dict[str, dict[str, Any]] = {}

            for w_idx in range(warmup_iters):
                rec, summaries = run_single_iteration(
                    case=case,
                    phase="warmup",
                    index=w_idx,
                    iter_dir=work_root / case.case_id / f"warmup_{w_idx}",
                    launcher_path=launcher_path,
                    bench_ecu_path=bench_ecu_path,
                    warmup_ms=warmup_ms,
                    measure_ms=measure_ms,
                    tail_ms=tail_ms,
                    control_metrics_mode=args.control_metrics,
                    launcher_has_control_metrics=has_cm,
                    wait_quiet=not args.no_wait,
                    wait_limit_s=args.wait_limit,
                )
                warmup_records.append(rec)
                last_summaries = summaries

            for m_idx in range(measured_iters):
                rec, summaries = run_single_iteration(
                    case=case,
                    phase="measured",
                    index=m_idx,
                    iter_dir=work_root / case.case_id / f"iter_{m_idx}",
                    launcher_path=launcher_path,
                    bench_ecu_path=bench_ecu_path,
                    warmup_ms=warmup_ms,
                    measure_ms=measure_ms,
                    tail_ms=tail_ms,
                    control_metrics_mode=args.control_metrics,
                    launcher_has_control_metrics=has_cm,
                    wait_quiet=not args.no_wait,
                    wait_limit_s=args.wait_limit,
                )
                measured_records.append(rec)
                last_summaries = summaries

            case_reports.append(
                build_case_report(
                    case, warmup_ms, measure_ms, tail_ms, warmup_records, measured_records, last_summaries
                )
            )

        report = {
            "$schema": schema_ref_for_output(out_path),
            "vehicle_sil_benchmark": BENCHMARK_FORMAT_VERSION,
            "generated_at_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "execution_backend": "silkit_coordinated_multiprocess",
            "interpretation_contract": {
                "local_bench_substitutable": False,
                "process_count_equals_participant_count_assumed": False,
                "wall_cpu_handler_summable": False,
                "notes": list(INTERPRETATION_NOTES),
            },
            "provenance": {
                "repo": repo_prov,
                "build": build_prov,
                "dependencies": deps_prov,
                "host": host_prov,
            },
            "run_options": {
                "profile": profile,
                "warmup_iterations": warmup_iters,
                "measured_iterations": measured_iters,
                "control_metrics_mode": args.control_metrics,
                "busy_machine_wait": not args.no_wait,
            },
            "cases": case_reports,
        }

        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
        _print_summary(report, out_path)
        if args.keep_work_dir:
            print(f"  work_dir: {work_root}")
        return 0
    except VerificationError as exc:
        print(f"benchmark-vtime: FAILED: {exc}", file=sys.stderr)
        if args.keep_work_dir:
            print(f"  work_dir: {work_root}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("benchmark-vtime: interrupted", file=sys.stderr)
        return 130
    finally:
        if not args.keep_work_dir:
            shutil.rmtree(work_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(job_summary.run(main))
