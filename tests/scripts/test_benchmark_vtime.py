# SPDX-License-Identifier: MIT
"""Unit tests for scripts/benchmark-vtime.py (Python 3.11+ standard library only).

Covers:
  - compile_commands.json flag extraction (last-flag-wins -O2 -O0, ccache wrapper, sanitizers,
    CMakeCache.txt fallback, missing metadata -> unavailable/null, Release mismatch warning)
  - up-front alignment validation for --warmup-ms / --measure-ms (exit 2 on non-multiples)
  - timestamped CAN TX/RX causal trace verification over {tx_ms < W + M, rx_ms <= W + M},
    including Case C rx_ms == W + M boundary and fault-injected failures (off-by-1 rx_ms,
    missing frame at rx_ms == W + M, TX at tx_ms >= W + M, duplicate/dropped/corrupted payload,
    wrong window_steps, non-zero CAN drop counter, cross-iteration SHA-256 mismatch)
  - steady-state boundary guards (paused_ms > 0 / paced_steps > 0 rejected, warmup_iterations
    excluded from summary medians)
  - cross-platform process CPU collection (direct POSIX os.wait4 without prior Popen.wait,
    Darwin ru_maxrss // 1024 conversion, mocked Windows os.name == "nt" -> unavailable + null)
  - optional Issue 1 --control-metrics integration (auto supported/unsupported, on/off, all
    unavailable_reason enum values including stopped_before_setup, counter invariant checks)
"""

from __future__ import annotations

import copy
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path, PureWindowsPath
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = ROOT / "scripts" / "benchmark-vtime.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("benchmark_vtime", SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["benchmark_vtime"] = mod
    spec.loader.exec_module(mod)
    return mod


BV = _load_module()


def _make_payload_hex(ecu_id: int, frame_idx: int, seq: int, word: int = 0x11223344) -> str:
    b = bytes([
        ecu_id & 0xFF,
        frame_idx & 0xFF,
        seq & 0xFF,
        (seq >> 8) & 0xFF,
        word & 0xFF,
        (word >> 8) & 0xFF,
        (word >> 16) & 0xFF,
        (word >> 24) & 0xFF,
    ])
    return b.hex()


def _synthesize_valid_traces(
    case: BV.CaseSpec,
    warmup_ms: int,
    measure_ms: int,
) -> tuple[dict[str, dict], dict[str, list]]:
    """Build exact valid ECU summaries and causal TX/RX traces for a case over {tx < W+M, rx <= W+M}."""
    measure_end_ms = warmup_ms + measure_ms
    tx_by_ecu: dict[str, list] = {}
    for ecu in case.ecus:
        tx_list = []
        seq = 0
        t = ecu.can_offset_ms
        while t < measure_end_ms:
            for f_idx in range(ecu.can_frames):
                tx_list.append(
                    BV.TraceEvent(
                        direction="T",
                        tx_ms=t,
                        rx_ms=None,
                        can_id=BV.CAN_BASE_ID + ecu.ecu_id,
                        seq=seq & 0xFFFF,
                        payload_hex=_make_payload_hex(ecu.ecu_id, f_idx, seq & 0xFFFF, word=0xABC000 + t),
                    )
                )
                seq += 1
            t += ecu.can_period_ms
        tx_by_ecu[ecu.name] = tx_list

    summaries: dict[str, dict] = {}
    traces: dict[str, list] = {}
    for ecu in case.ecus:
        rx_list = []
        for sender in case.ecus:
            if sender.name == ecu.name:
                continue
            for tx_ev in tx_by_ecu[sender.name]:
                rx_ms = ((tx_ev.tx_ms // ecu.step_ms) + 1) * ecu.step_ms
                rx_list.append(
                    BV.TraceEvent(
                        direction="R",
                        tx_ms=tx_ev.tx_ms,
                        rx_ms=rx_ms,
                        can_id=tx_ev.can_id,
                        seq=tx_ev.seq,
                        payload_hex=tx_ev.payload_hex,
                    )
                )
        rx_list.sort(
            key=lambda ev: (ev.rx_ms, ev.tx_ms, ev.can_id, BV._payload_u64_le(ev.payload_hex, ecu.name))
        )
        traces[ecu.name] = [*tx_by_ecu[ecu.name], *rx_list]
        summaries[ecu.name] = {
            "name": ecu.name,
            "configured_sample_period_ms": ecu.sample_ms,
            "effective_sample_period_ms": ecu.sample_ms,
            "sample_every_step_active": False,
            "saw_window_start": True,
            "saw_window_end": True,
            "window_start_step_ms": warmup_ms,
            "window_end_step_ms": measure_end_ms,
            "steady_start_ns": 1_000_000_000,
            "steady_end_ns": 1_025_000_000,
            "window_steps": measure_ms // ecu.step_ms,
            "tx_in_window": sum(1 for ev in tx_by_ecu[ecu.name] if warmup_ms <= ev.tx_ms < measure_end_ms),
            "rx_in_window": sum(1 for ev in rx_list if warmup_ms <= ev.tx_ms < measure_end_ms),
            "tx_total": len(tx_by_ecu[ecu.name]),
            "rx_total": len(rx_list),
            "sub_ms_rx_timestamps": 0,
            "state_digest_hex": f"{ecu.ecu_id + 1:016x}",
            "can_stats": {
                "frames_received": len(rx_list),
                "receive_overflows": 0,
                "frames_sent": len(tx_by_ecu[ecu.name]),
                "sends_rejected": 0,
                "frames_dropped": 0,
                "port_receive_overflows": 0,
            },
            "cpu": {"status": "available", "reason": None, "user_ms": 5.0, "sys_ms": 1.0, "total_ms": 6.0},
        }
    return summaries, traces


class TestBuildFlagExtraction(unittest.TestCase):
    def test_compile_commands_last_flag_wins_and_ccache_wrapper(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            cc = [
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "ccache /usr/bin/g++ -O2 -O0 -DNDEBUG -fsanitize=thread -g1 -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/libs/core/src/participant_runtime.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c libs/core/src/participant_runtime.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/bench_ecu/main.cpp",
                },
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc), encoding="utf-8")
            (bdir / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")

            with mock.patch.object(BV, "_compiler_banner", return_value="g++ (GCC) 14.2.0"):
                build_info, _ = BV.extract_build_provenance(bdir, {"ndebug": True}, is_windows=False)

            self.assertEqual(build_info["flags_source"], "compile_commands.json")
            launch = build_info["targets"]["vehicle_sil_launch"]
            self.assertEqual(launch["optimization_flags"], ["-O2", "-O0"])
            self.assertEqual(launch["effective_opt_level"], "-O0")
            self.assertEqual(launch["sanitizers"], ["-fsanitize=thread"])
            self.assertTrue(launch["ndebug"])
            self.assertIsNotNone(build_info["flags_warning"])
            self.assertIn("non-optimized", build_info["flags_warning"])
            self.assertIn("sanitizers", build_info["flags_warning"])

    def test_cmake_cache_fallback_when_compile_commands_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            (bdir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/g++\n"
                "CMAKE_CXX_FLAGS:STRING=\n"
                "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\n"
                "VEHICLE_SIL_STATIC:BOOL=ON\n",
                encoding="utf-8",
            )
            with mock.patch.object(BV, "_compiler_banner", return_value="g++ (GCC) 14.2.0"):
                build_info, cache = BV.extract_build_provenance(bdir, {"ndebug": True}, is_windows=False)

            self.assertEqual(build_info["flags_source"], "cmake_cache")
            self.assertEqual(build_info["targets"]["vehicle_sil_bench_ecu"]["effective_opt_level"], "-O3")
            self.assertIsNone(build_info["flags_warning"])
            self.assertTrue(BV._cache_bool(cache, "VEHICLE_SIL_STATIC"))

    def test_unavailable_when_no_build_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            build_info, _ = BV.extract_build_provenance(bdir, None, is_windows=False)
            self.assertEqual(build_info["flags_source"], "unavailable")
            for target in build_info["targets"].values():
                self.assertIsNone(target["optimization_flags"])
                self.assertIsNone(target["effective_opt_level"])
                self.assertIsNone(target["sanitizers"])
                self.assertIsNone(target["ndebug"])

    def test_windows_quoted_tokens_response_files_msvc_and_sanitizers(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            (bdir / "flags.rsp").write_text('"-O3" "-DNDEBUG=1" -fsanitize=address,undefined -fno-sanitize=undefined\n', encoding="utf-8")
            cc = [
                {
                    "directory": tmp,
                    "file": f"{tmp}\\tools\\launch\\main.cpp",
                    "command": '"C:\\msys64\\ucrt64\\bin\\ccache.exe" "C:\\msys64\\ucrt64\\bin\\g++.exe" @flags.rsp -c main.cpp',
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}\\libs\\core\\src\\participant_runtime.cpp",
                    "command": '"C:\\Program Files\\MSVC\\cl.exe" /Od /Oi /Ot /Oy /Og /fsanitize=address /DNDEBUG=1 -c participant_runtime.cpp',
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}\\tools\\bench_ecu\\main.cpp",
                    "command": '"C:\\msys64\\ucrt64\\bin\\g++.exe" -O3 -fsanitize=thread -fno-sanitize=all -DNDEBUG -c main.cpp',
                },
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc), encoding="utf-8")

            seen_compilers: list[str] = []

            def fake_banner(path, _cache):
                seen_compilers.append(path)
                return f"banner:{path}"

            with mock.patch.object(BV, "_compiler_banner", side_effect=fake_banner):
                build_info, _ = BV.extract_build_provenance(
                    bdir,
                    {"compiler_id": "GNU", "ndebug": True, "optimize_macro": 1, "sanitizers": []},
                    is_windows=True,
                )

            self.assertEqual(build_info["flags_source"], "compile_commands.json")
            # Quotes must be stripped and ccache.exe wrapper skipped!
            self.assertEqual(seen_compilers[0], r"C:\msys64\ucrt64\bin\g++.exe")
            launch = build_info["targets"]["vehicle_sil_launch"]
            self.assertEqual(launch["effective_opt_level"], "-O3")
            self.assertTrue(launch["ndebug"])
            self.assertEqual(launch["sanitizers"], ["-fsanitize=address"])

            # /Oi, /Ot, /Oy, /Og must NOT override primary /Od!
            core = build_info["targets"]["vehicle_sil_core"]
            self.assertEqual(core["effective_opt_level"], "/Od")
            self.assertEqual(core["sanitizers"], ["/fsanitize=address"])
            self.assertTrue(core["ndebug"])

            # -fno-sanitize=all clears thread sanitizer.
            bench = build_info["targets"]["vehicle_sil_bench_ecu"]
            self.assertEqual(bench["sanitizers"], [])

    def test_multi_config_cache_is_unavailable_and_mismatches_warn(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            # Multi-config generator without CMAKE_BUILD_TYPE must stay unavailable.
            (bdir / "CMakeCache.txt").write_text(
                "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\nCMAKE_BUILD_TYPE:STRING=\n",
                encoding="utf-8",
            )
            build_info, _ = BV.extract_build_provenance(bdir, None, is_windows=True)
            self.assertEqual(build_info["flags_source"], "unavailable")

            # Partial compile_commands.json + bench_ecu_build_info conflict + external binary override.
            cc = [
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/bench_ecu/main.cpp",
                }
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc), encoding="utf-8")
            outside = Path(tmp).parent / "outside_launcher"
            with mock.patch.object(BV, "_compiler_banner", return_value="g++"):
                info2, _ = BV.extract_build_provenance(
                    bdir,
                    {"compiler_id": "Clang", "ndebug": False, "optimize_macro": 0, "sanitizers": ["thread"]},
                    is_windows=False,
                    launcher_path=outside,
                    bench_ecu_path=bdir / "tools" / "bench_ecu" / "vehicle_sil_bench_ecu",
                )
            warn = info2["flags_warning"] or ""
            self.assertIn("missing target TU(s)", warn)
            self.assertIn("--launcher", warn)
            self.assertIn("optimize_macro=0", warn)
            self.assertIn("ndebug", warn)
            self.assertIn("sanitizers conflict", warn)

    def test_compile_commands_partial_ambiguity_leaves_null_and_warns_without_missing_tu(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            cc = [
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ -O0 -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/libs/core/src/participant_runtime.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c libs/core/src/participant_runtime.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/bench_ecu/main.cpp",
                },
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc), encoding="utf-8")
            (bdir / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")

            with mock.patch.object(BV, "_compiler_banner", return_value="g++ (GCC) 14.2.0"):
                build_info, _ = BV.extract_build_provenance(bdir, {"ndebug": True}, is_windows=False)

            self.assertEqual(build_info["flags_source"], "compile_commands.json")
            self.assertEqual(build_info["targets"]["vehicle_sil_launch"], BV._empty_target_flags())
            self.assertEqual(build_info["targets"]["vehicle_sil_core"]["effective_opt_level"], "-O3")
            self.assertEqual(build_info["targets"]["vehicle_sil_bench_ecu"]["effective_opt_level"], "-O3")
            warn = build_info["flags_warning"] or ""
            self.assertIn("ambiguous compile_commands.json entries for vehicle_sil_launch", warn)
            self.assertNotIn("missing target TU(s)", warn)

            # Ambiguous target alongside a genuinely missing target: do not duplicate ambiguous target in missing list.
            cc_partial = [e for e in cc if "participant_runtime.cpp" not in e["file"]]
            (bdir / "compile_commands.json").write_text(json.dumps(cc_partial), encoding="utf-8")
            with mock.patch.object(BV, "_compiler_banner", return_value="g++ (GCC) 14.2.0"):
                build_info2, _ = BV.extract_build_provenance(bdir, {"ndebug": True}, is_windows=False)
            warn2 = build_info2["flags_warning"] or ""
            self.assertIn("ambiguous compile_commands.json entries for vehicle_sil_launch", warn2)
            self.assertIn("missing target TU(s) in compile_commands.json: vehicle_sil_core", warn2)
            self.assertNotIn("missing target TU(s) in compile_commands.json: vehicle_sil_launch", warn2)

    def test_compile_commands_all_targets_ambiguous_is_unavailable_no_cache_fallback_and_warns(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            cc = [
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ -O0 -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/libs/core/src/participant_runtime.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c libs/core/src/participant_runtime.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/libs/core/src/participant_runtime.cpp",
                    "command": "/usr/bin/g++ -O0 -c libs/core/src/participant_runtime.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/bench_ecu/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ -O0 -c tools/bench_ecu/main.cpp",
                },
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc), encoding="utf-8")
            (bdir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/g++\n"
                "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\n",
                encoding="utf-8",
            )

            with mock.patch.object(BV, "_compiler_banner", return_value="g++ (GCC) 14.2.0"):
                build_info, _ = BV.extract_build_provenance(bdir, {"ndebug": True}, is_windows=False)

            self.assertEqual(build_info["flags_source"], "unavailable")
            for target_flags in build_info["targets"].values():
                self.assertEqual(target_flags, BV._empty_target_flags())
            warn = build_info["flags_warning"] or ""
            self.assertIn("ambiguous compile_commands.json entries for", warn)
            self.assertIn("vehicle_sil_launch", warn)
            self.assertIn("vehicle_sil_core", warn)
            self.assertIn("vehicle_sil_bench_ecu", warn)

            # Single matched target with conflicting compilers (even if banner is None) -> unavailable, no cache fallback.
            cc_compiler_conflict = [
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/clang++ -O3 -DNDEBUG -c tools/launch/main.cpp",
                },
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc_compiler_conflict), encoding="utf-8")
            with mock.patch.object(BV, "_compiler_banner", return_value=None):
                build_info2, _ = BV.extract_build_provenance(bdir, {"ndebug": True}, is_windows=False)
            self.assertEqual(build_info2["flags_source"], "unavailable")
            for target_flags in build_info2["targets"].values():
                self.assertEqual(target_flags, BV._empty_target_flags())
            self.assertIn("ambiguous compile_commands.json entries for vehicle_sil_launch", build_info2["flags_warning"] or "")

            # Even when flags_source == "unavailable" due to all-targets ambiguity, external binary path
            # warnings and Release vs bench_ecu ndebug=False warnings must still be emitted alongside ambiguity.
            outside_launcher = Path(tmp).parent / "outside_launcher"
            outside_bench = Path(tmp).parent / "outside_bench_ecu"
            with mock.patch.object(BV, "_compiler_banner", return_value=None):
                build_info3, _ = BV.extract_build_provenance(
                    bdir,
                    {"ndebug": False},
                    is_windows=False,
                    launcher_path=outside_launcher,
                    bench_ecu_path=outside_bench,
                )
            self.assertEqual(build_info3["flags_source"], "unavailable")
            warn3 = build_info3["flags_warning"] or ""
            self.assertIn("ambiguous compile_commands.json entries for vehicle_sil_launch", warn3)
            self.assertIn("--launcher", warn3)
            self.assertIn("--bench-ecu", warn3)
            self.assertIn("compiled without NDEBUG", warn3)

    def test_compile_commands_identical_duplicates_accepted_without_warning(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            cc = [
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/libs/core/src/participant_runtime.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c libs/core/src/participant_runtime.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/bench_ecu/main.cpp",
                },
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc), encoding="utf-8")
            (bdir / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")

            with mock.patch.object(BV, "_compiler_banner", return_value="g++ (GCC) 14.2.0"):
                build_info, _ = BV.extract_build_provenance(bdir, {"ndebug": True}, is_windows=False)

            self.assertEqual(build_info["flags_source"], "compile_commands.json")
            self.assertEqual(build_info["targets"]["vehicle_sil_launch"]["effective_opt_level"], "-O3")
            self.assertTrue(build_info["targets"]["vehicle_sil_launch"]["ndebug"])
            self.assertIsNone(build_info["flags_warning"])

    def test_unreadable_response_files_across_configs_are_unavailable_not_empty_flags(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            # Neither @debug.rsp nor @release.rsp exists at measurement time:
            # must NEVER parse as optimization_flags: [] or compare equal as valid flags.
            cc = [
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ @debug.rsp -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ @release.rsp -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/libs/core/src/participant_runtime.cpp",
                    "command": "/usr/bin/g++ @debug.rsp -c libs/core/src/participant_runtime.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/libs/core/src/participant_runtime.cpp",
                    "command": "/usr/bin/g++ @release.rsp -c libs/core/src/participant_runtime.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ @debug.rsp -c tools/bench_ecu/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ @release.rsp -c tools/bench_ecu/main.cpp",
                },
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc), encoding="utf-8")
            (bdir / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/g++\n"
                "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\n",
                encoding="utf-8",
            )

            with mock.patch.object(BV, "_compiler_banner", return_value="g++ (GCC) 14.2.0"):
                build_info, _ = BV.extract_build_provenance(bdir, {"ndebug": True}, is_windows=False)

            self.assertEqual(build_info["flags_source"], "unavailable")
            for target_flags in build_info["targets"].values():
                self.assertEqual(target_flags, BV._empty_target_flags())
                self.assertIsNone(target_flags["optimization_flags"])
            warn = build_info["flags_warning"] or ""
            self.assertIn("unreadable response file(s)", warn)
            self.assertIn("@debug.rsp", warn)
            self.assertIn("@release.rsp", warn)
            self.assertIn("vehicle_sil_launch", warn)
            self.assertIn("vehicle_sil_core", warn)
            self.assertIn("vehicle_sil_bench_ecu", warn)

    def test_partial_unreadable_response_file_warns_without_missing_tu_and_keeps_others(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            cc = [
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ -O3 @missing.rsp -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/libs/core/src/participant_runtime.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c libs/core/src/participant_runtime.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/bench_ecu/main.cpp",
                },
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc), encoding="utf-8")
            (bdir / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")

            with mock.patch.object(BV, "_compiler_banner", return_value="g++ (GCC) 14.2.0"):
                build_info, _ = BV.extract_build_provenance(bdir, {"ndebug": True}, is_windows=False)

            self.assertEqual(build_info["flags_source"], "compile_commands.json")
            self.assertEqual(build_info["targets"]["vehicle_sil_launch"], BV._empty_target_flags())
            self.assertEqual(build_info["targets"]["vehicle_sil_core"]["effective_opt_level"], "-O3")
            self.assertEqual(build_info["targets"]["vehicle_sil_bench_ecu"]["effective_opt_level"], "-O3")
            warn = build_info["flags_warning"] or ""
            self.assertIn("unreadable response file(s)", warn)
            self.assertIn("vehicle_sil_launch (@missing.rsp)", warn)
            self.assertNotIn("missing target TU(s)", warn)

            # Target with both conflicting entries and an unreadable response file stays null and warns.
            cc_both = [
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "/usr/bin/g++ -O0 @missing.rsp -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/libs/core/src/participant_runtime.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c libs/core/src/participant_runtime.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/bench_ecu/main.cpp",
                },
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc_both), encoding="utf-8")
            with mock.patch.object(BV, "_compiler_banner", return_value="g++ (GCC) 14.2.0"):
                build_info2, _ = BV.extract_build_provenance(bdir, {"ndebug": True}, is_windows=False)
            self.assertEqual(build_info2["targets"]["vehicle_sil_launch"], BV._empty_target_flags())
            warn2 = build_info2["flags_warning"] or ""
            self.assertIn("ambiguous compile_commands.json entries for vehicle_sil_launch", warn2)
            self.assertIn("unreadable response file(s)", warn2)
            self.assertNotIn("missing target TU(s)", warn2)

    def test_response_file_depth_limit_oserror_and_compiler_token0_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            # Cycle / excessive nesting (depth >= 4) must flag remaining @rsp as unreadable.
            (bdir / "a.rsp").write_text("@b.rsp\n", encoding="utf-8")
            (bdir / "b.rsp").write_text("@c.rsp\n", encoding="utf-8")
            (bdir / "c.rsp").write_text("@d.rsp\n", encoding="utf-8")
            (bdir / "d.rsp").write_text("@e.rsp\n", encoding="utf-8")
            (bdir / "e.rsp").write_text("-O3 -DNDEBUG\n", encoding="utf-8")

            expanded, unreadable = BV._expand_response_files(["@a.rsp", "-c", "main.cpp"], bdir, windows=False)
            self.assertEqual(unreadable, ["@e.rsp"])
            self.assertIn("@e.rsp", expanded)

            # Compiler path at tokens[0] starting with '@' must not be treated as a response file.
            cc = [
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/launch/main.cpp",
                    "command": "@tools/g++ -O3 -DNDEBUG -c tools/launch/main.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/libs/core/src/participant_runtime.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c libs/core/src/participant_runtime.cpp",
                },
                {
                    "directory": tmp,
                    "file": f"{tmp}/tools/bench_ecu/main.cpp",
                    "command": "/usr/bin/g++ -O3 -DNDEBUG -c tools/bench_ecu/main.cpp",
                },
            ]
            (bdir / "compile_commands.json").write_text(json.dumps(cc), encoding="utf-8")
            with mock.patch.object(BV, "_compiler_banner", return_value="g++"):
                build_info, _ = BV.extract_build_provenance(bdir, None, is_windows=False)
            self.assertEqual(build_info["flags_source"], "compile_commands.json")
            self.assertEqual(build_info["targets"]["vehicle_sil_launch"]["effective_opt_level"], "-O3")
            self.assertIsNone(build_info["flags_warning"])

            # OSError while reading an existing response file must mark it unreadable.
            (bdir / "unreadable.rsp").write_text("-O3\n", encoding="utf-8")
            orig_read_text = Path.read_text

            def failing_read_text(self_path, *args, **kwargs):
                if self_path.name == "unreadable.rsp":
                    raise PermissionError("permission denied")
                return orig_read_text(self_path, *args, **kwargs)

            with mock.patch.object(Path, "read_text", failing_read_text):
                _, unreadable_os = BV._expand_response_files(["@unreadable.rsp"], bdir, windows=False)
            self.assertEqual(unreadable_os, ["@unreadable.rsp"])


@unittest.skipUnless(shutil.which("git"), "git executable is required for dependency provenance tests")
class TestDependencyProvenance(unittest.TestCase):
    @staticmethod
    def _git(cwd: Path, *args: str) -> str:
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith("GIT_")}
        env.update(
            {
                "GIT_AUTHOR_NAME": "Test Author",
                "GIT_AUTHOR_EMAIL": "test@example.com",
                "GIT_COMMITTER_NAME": "Test Author",
                "GIT_COMMITTER_EMAIL": "test@example.com",
                "GIT_CONFIG_GLOBAL": os.devnull,
                "GIT_CONFIG_SYSTEM": os.devnull,
            }
        )
        proc = subprocess.run(
            ["git", *args],
            cwd=cwd,
            env=env,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            stdin=subprocess.DEVNULL,
        )
        return proc.stdout.strip()

    def _init_repo_with_commit(
        self, repo_dir: Path, filename: str, tag: str, *, separate_git_dir: Path | None = None
    ) -> str:
        repo_dir.mkdir(parents=True, exist_ok=True)
        if separate_git_dir is not None:
            separate_git_dir.parent.mkdir(parents=True, exist_ok=True)
            self._git(repo_dir, "init", f"--separate-git-dir={separate_git_dir.as_posix()}")
        else:
            self._git(repo_dir, "init")
        (repo_dir / filename).write_text("hello\n", encoding="utf-8")
        self._git(repo_dir, "add", filename)
        self._git(repo_dir, "commit", "-m", f"init {filename}")
        self._git(repo_dir, "tag", tag)
        return self._git(repo_dir, "rev-parse", "HEAD")

    def test_uninitialized_silkit_submodule_records_null_not_parent_repo(self):
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp) / "repo"
            parent_sha = self._init_repo_with_commit(parent, "README.md", "v9.9.9-parent")
            silkit_dir = parent / "external" / "sil-kit"
            silkit_dir.mkdir(parents=True)

            repo_info, deps_info = BV._collect_repo_and_dependency_provenance(
                {"vehicle_sil": "0.4.0", "features": []},
                {},
                root=parent,
            )
            self.assertEqual(repo_info["git_commit"], parent_sha)
            self.assertIsNone(deps_info["sil_kit"]["submodule_commit"])
            self.assertIsNone(deps_info["sil_kit"]["tag"])

            # Even if a bogus empty .git directory exists inside external/sil-kit (causing git to climb to parent),
            # --show-toplevel verification must prevent attributing parent's HEAD/tag to SIL Kit.
            (silkit_dir / ".git").mkdir()
            _, deps_info2 = BV._collect_repo_and_dependency_provenance(
                {"vehicle_sil": "0.4.0", "features": []},
                {},
                root=parent,
            )
            self.assertIsNone(deps_info2["sil_kit"]["submodule_commit"])
            self.assertIsNone(deps_info2["sil_kit"]["tag"])

            # Degenerate .git file pointing back to parent's .git must also be rejected via --git-dir comparison.
            (silkit_dir / ".git").rmdir()
            (silkit_dir / ".git").write_text(f"gitdir: {(parent / '.git').as_posix()}\n", encoding="utf-8")
            _, deps_info3 = BV._collect_repo_and_dependency_provenance(
                {"vehicle_sil": "0.4.0", "features": []},
                {},
                root=parent,
            )
            self.assertIsNone(deps_info3["sil_kit"]["submodule_commit"])
            self.assertIsNone(deps_info3["sil_kit"]["tag"])

    def test_git_dir_env_var_does_not_leak_parent_repo_into_silkit(self):
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp) / "repo"
            parent_sha = self._init_repo_with_commit(parent, "README.md", "v9.9.9-parent")
            silkit_dir = parent / "external" / "sil-kit"
            silkit_sha = self._init_repo_with_commit(silkit_dir, "CMakeLists.txt", "v5.0.8-silkit")
            self.assertNotEqual(parent_sha, silkit_sha)

            # Even when GIT_DIR / GIT_WORK_TREE are exported (e.g. from a git hook or worktree runner),
            # external/sil-kit must still read its own HEAD/tag and never parent's.
            with mock.patch.dict(
                os.environ,
                {"GIT_DIR": str(parent / ".git"), "GIT_WORK_TREE": str(parent)},
            ):
                repo_info, deps_info = BV._collect_repo_and_dependency_provenance(
                    {"vehicle_sil": "0.4.0", "features": []},
                    {},
                    root=parent,
                )
            self.assertEqual(repo_info["git_commit"], parent_sha)
            self.assertEqual(deps_info["sil_kit"]["submodule_commit"], silkit_sha)
            self.assertEqual(deps_info["sil_kit"]["tag"], "v5.0.8-silkit")

            # Also verify uninitialized external/sil-kit stays None under exported GIT_DIR.
            parent_uninit = Path(tmp) / "repo_uninit"
            parent_uninit_sha = self._init_repo_with_commit(parent_uninit, "README.md", "v9.9.9-uninit")
            (parent_uninit / "external" / "sil-kit").mkdir(parents=True)
            with mock.patch.dict(
                os.environ,
                {"GIT_DIR": str(parent_uninit / ".git"), "GIT_WORK_TREE": str(parent_uninit)},
            ):
                repo_info2, deps_info2 = BV._collect_repo_and_dependency_provenance(
                    {"vehicle_sil": "0.4.0", "features": []},
                    {},
                    root=parent_uninit,
                )
            self.assertEqual(repo_info2["git_commit"], parent_uninit_sha)
            self.assertIsNone(deps_info2["sil_kit"]["submodule_commit"])
            self.assertIsNone(deps_info2["sil_kit"]["tag"])

    def test_initialized_silkit_repo_records_own_commit_and_tag(self):
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp) / "repo"
            parent_sha = self._init_repo_with_commit(parent, "README.md", "v9.9.9-parent")
            silkit_dir = parent / "external" / "sil-kit"
            silkit_sha = self._init_repo_with_commit(silkit_dir, "CMakeLists.txt", "v5.0.8-silkit")

            self.assertNotEqual(parent_sha, silkit_sha)
            repo_info, deps_info = BV._collect_repo_and_dependency_provenance(
                {"vehicle_sil": "0.4.0", "features": ["control-server"]},
                {"VEHICLE_SIL_STATIC": "ON"},
                root=parent,
            )
            self.assertEqual(repo_info["git_commit"], parent_sha)
            self.assertEqual(deps_info["sil_kit"]["submodule_commit"], silkit_sha)
            self.assertEqual(deps_info["sil_kit"]["tag"], "v5.0.8-silkit")
            self.assertTrue(deps_info["sil_kit"]["static_linked"])
            self.assertTrue(deps_info["control_server_built"])

            # Also verify submodule-style .git file (gitdir pointer via --separate-git-dir).
            parent2 = Path(tmp) / "repo_gitfile"
            parent2_sha = self._init_repo_with_commit(parent2, "README.md", "v9.9.9-parent2")
            silkit_dir2 = parent2 / "external" / "sil-kit"
            gitdir2 = parent2 / ".git" / "modules" / "external" / "sil-kit"
            silkit_sha2 = self._init_repo_with_commit(
                silkit_dir2, "CMakeLists.txt", "v5.0.8-submod", separate_git_dir=gitdir2
            )
            self.assertTrue((silkit_dir2 / ".git").is_file())
            self.assertNotEqual(parent2_sha, silkit_sha2)
            repo_info2, deps_info2 = BV._collect_repo_and_dependency_provenance(
                {"vehicle_sil": "0.4.0", "features": []},
                {},
                root=parent2,
            )
            self.assertEqual(repo_info2["git_commit"], parent2_sha)
            self.assertEqual(deps_info2["sil_kit"]["submodule_commit"], silkit_sha2)
            self.assertEqual(deps_info2["sil_kit"]["tag"], "v5.0.8-submod")


class TestWindowsMsysGitPathNormalization(unittest.TestCase):
    @staticmethod
    def _to_msys_path(path: Path) -> str:
        text = path.as_posix()
        if len(text) >= 2 and text[1] == ":":
            return f"/{text[0].lower()}{text[2:]}"
        return text

    def test_msys_to_windows_path_rewrites_drive_paths_only(self):
        self.assertEqual(BV._msys_to_windows_path("/c"), "C:/")
        self.assertEqual(BV._msys_to_windows_path("/c/"), "C:/")
        self.assertEqual(BV._msys_to_windows_path("/c/Users/runneradmin/repo"), "C:/Users/runneradmin/repo")
        self.assertEqual(BV._msys_to_windows_path("/d/a/vehicle-sil/external/sil-kit"), "D:/a/vehicle-sil/external/sil-kit")
        self.assertEqual(BV._msys_to_windows_path("C:/Users/dev/repo"), "C:/Users/dev/repo")
        self.assertEqual(BV._msys_to_windows_path(".git"), ".git")
        # Non-drive POSIX paths and UNC paths must stay untouched.
        self.assertEqual(BV._msys_to_windows_path("/usr/bin/git"), "/usr/bin/git")
        self.assertEqual(BV._msys_to_windows_path("//server/share/repo"), "//server/share/repo")

    def test_normalize_git_path_converts_msys_drive_paths_when_windows(self):
        with tempfile.TemporaryDirectory() as tmp:
            if os.name == "nt":
                base = Path(tmp).resolve()
            else:
                base = Path("D:/a/vehicle-sil")
            cwd = base / "external" / "sil-kit"
            msys_cwd = self._to_msys_path(cwd)
            msys_root = f"/{base.as_posix()[0].lower()}"
            drive_root_str = f"{base.as_posix()[0].upper()}:/"
            drive_root = Path(drive_root_str) if os.name == "nt" else PureWindowsPath(drive_root_str)

            with mock.patch.object(BV.os, "name", "nt"):
                self.assertEqual(
                    BV._path_compare_key(BV._normalize_git_path(msys_cwd, cwd)),
                    BV._path_compare_key(cwd),
                )
                self.assertEqual(
                    BV._path_compare_key(BV._normalize_git_path(msys_cwd.swapcase(), cwd)),
                    BV._path_compare_key(cwd),
                )
                self.assertEqual(
                    BV._path_compare_key(BV._normalize_git_path(msys_root, cwd)),
                    BV._path_compare_key(drive_root),
                )
                self.assertEqual(
                    BV._path_compare_key(BV._normalize_git_path(f"{msys_root}/", cwd)),
                    BV._path_compare_key(drive_root),
                )
                self.assertEqual(
                    BV._path_compare_key(BV._normalize_git_path(".git", cwd)),
                    BV._path_compare_key(cwd / ".git"),
                )
                self.assertEqual(
                    BV._path_compare_key(BV._normalize_git_path("../../.git/modules/external/sil-kit", cwd)),
                    BV._path_compare_key(base / ".git" / "modules" / "external" / "sil-kit"),
                )

    def test_collect_dependency_provenance_handles_msys_git_paths_under_mocked_nt(self):
        with tempfile.TemporaryDirectory() as tmp:
            if os.name == "nt":
                parent = Path(tmp).resolve() / "repo"
                silkit = parent / "external" / "sil-kit"
                (silkit / ".git").mkdir(parents=True)
            else:
                parent = Path("C:/Users/runneradmin/msys-test/repo")
                silkit = parent / "external" / "sil-kit"

            parent_sha = "1" * 40
            silkit_sha = "2" * 40
            msys_parent_git = self._to_msys_path(parent / ".git")
            msys_silkit = self._to_msys_path(silkit)
            msys_silkit_git = self._to_msys_path(parent / ".git" / "modules" / "external" / "sil-kit")

            def fake_git_output(args, cwd=None):
                if cwd == parent:
                    if args == ["rev-parse", "HEAD"]:
                        return parent_sha
                    if args == ["rev-parse", "--git-dir"]:
                        return msys_parent_git
                if cwd == silkit:
                    if args == ["rev-parse", "--show-toplevel"]:
                        return msys_silkit
                    if args == ["rev-parse", "--git-dir"]:
                        return msys_silkit_git
                    if args == ["rev-parse", "HEAD"]:
                        return silkit_sha
                    if args == ["describe", "--tags", "--exact-match"]:
                        return "v5.0.8"
                return None

            orig_exists = Path.exists

            def fake_exists(self_path):
                if self_path == silkit / ".git":
                    return True
                return orig_exists(self_path)

            with (
                mock.patch.object(BV.os, "name", "nt"),
                mock.patch.object(BV, "_git_output", side_effect=fake_git_output),
                mock.patch.object(BV.subprocess, "run", return_value=types.SimpleNamespace(returncode=0)),
                mock.patch.object(Path, "exists", fake_exists),
            ):
                repo_info, deps_info = BV._collect_repo_and_dependency_provenance(
                    {"vehicle_sil": "0.4.0", "features": []},
                    {},
                    root=parent,
                )

            self.assertEqual(repo_info["git_commit"], parent_sha)
            self.assertEqual(deps_info["sil_kit"]["submodule_commit"], silkit_sha)
            self.assertEqual(deps_info["sil_kit"]["tag"], "v5.0.8")

            # Degenerate MSYS gitdir pointing back to parent .git (even with differing case) must still be rejected.
            def fake_git_output_parent_leak(args, cwd=None):
                if cwd == silkit and args == ["rev-parse", "--git-dir"]:
                    return msys_parent_git.swapcase()
                return fake_git_output(args, cwd=cwd)

            with (
                mock.patch.object(BV.os, "name", "nt"),
                mock.patch.object(BV, "_git_output", side_effect=fake_git_output_parent_leak),
                mock.patch.object(BV.subprocess, "run", return_value=types.SimpleNamespace(returncode=0)),
                mock.patch.object(Path, "exists", fake_exists),
            ):
                _, deps_info_leak = BV._collect_repo_and_dependency_provenance(
                    {"vehicle_sil": "0.4.0", "features": []},
                    {},
                    root=parent,
                )
            self.assertIsNone(deps_info_leak["sil_kit"]["submodule_commit"])
            self.assertIsNone(deps_info_leak["sil_kit"]["tag"])


class TestAlignmentValidation(unittest.TestCase):
    def test_canonical_aligned_passes(self):
        cases = BV.resolve_cases("canonical")
        BV.validate_window_alignment(cases, warmup_ms=200, measure_ms=20000)
        BV.validate_window_alignment(cases, warmup_ms=50, measure_ms=200)
        BV.validate_window_alignment(cases, warmup_ms=0, measure_ms=10)

    def test_unaligned_warmup_or_measure_raises_usage_error(self):
        cases = BV.resolve_cases("canonical")
        with self.assertRaises(BV.UsageError):
            BV.validate_window_alignment(cases, warmup_ms=55, measure_ms=200)
        with self.assertRaises(BV.UsageError):
            BV.validate_window_alignment(cases, warmup_ms=50, measure_ms=205)
        with self.assertRaises(BV.UsageError):
            BV.validate_window_alignment(cases, warmup_ms=-10, measure_ms=200)
        with self.assertRaises(BV.UsageError):
            BV.validate_window_alignment(cases, warmup_ms=0, measure_ms=0)

    def test_cli_exits_2_on_unaligned_args(self):
        rc = BV.main(["--warmup-ms", "55", "--measure-ms", "200"])
        self.assertEqual(rc, 2)


class TestCausalTraceVerifier(unittest.TestCase):
    def test_valid_canonical_cases_including_case_c_rx_at_w_plus_m(self):
        for cid in BV.CANONICAL_CASE_IDS:
            case = BV.ALL_CASES[cid]
            summaries, traces = _synthesize_valid_traces(case, warmup_ms=50, measure_ms=200)
            res = BV.verify_iteration_traces(case, 50, 200, summaries, traces)
            if cid == "case_a_1ecu_1ms":
                self.assertEqual(res["tx_total"], 25)
                self.assertEqual(res["rx_total"], 0)
            else:
                self.assertEqual(res["tx_total"], 125)
                self.assertEqual(res["rx_total"], 500)
                self.assertEqual(res["comm_events_verified"], 625)
            if cid == "case_c_5ecu_mixed":
                # Verify ecu0 specifically has the 4 boundary frames arriving at rx_ms == W + M == 250.
                ecu0_boundary = [
                    ev for ev in traces["ecu0"] if ev.direction == "R" and ev.rx_ms == 250
                ]
                self.assertEqual(len(ecu0_boundary), 4)

    def test_injected_faults_fail_verifier(self):
        case = BV.ALL_CASES["case_c_5ecu_mixed"]
        base_summaries, base_traces = _synthesize_valid_traces(case, warmup_ms=50, measure_ms=200)

        # 1. Off-by-1 ms rx_ms.
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        for i, ev in enumerate(t["ecu1"]):
            if ev.direction == "R":
                t["ecu1"][i] = dataclasses.replace(ev, rx_ms=ev.rx_ms + 1)
                break
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        # 2. Missing boundary frame at rx_ms == W + M (250) on ecu0.
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        t["ecu0"] = [ev for ev in t["ecu0"] if not (ev.direction == "R" and ev.rx_ms == 250)]
        s["ecu0"]["rx_total"] -= 4
        s["ecu0"]["rx_in_window"] -= 4
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        # 3. Extra TX at tx_ms == W + M (250).
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        t["ecu0"].append(
            BV.TraceEvent("T", 250, None, 0x100, 25, _make_payload_hex(0, 0, 25))
        )
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        # 4. Duplicate RX frame.
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        first_rx = next(ev for ev in t["ecu1"] if ev.direction == "R")
        t["ecu1"].append(first_rx)
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        # 5. Corrupted payload on receiver.
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        for i, ev in enumerate(t["ecu2"]):
            if ev.direction == "R":
                t["ecu2"][i] = dataclasses.replace(ev, payload_hex="deadbeef00000000")
                break
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        # 6. Wrong window_steps.
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        s["ecu0"]["window_steps"] -= 1
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        # 7. Non-zero CAN drop counter.
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        s["ecu1"]["can_stats"]["receive_overflows"] = 1
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        # 8. Missing can_stats or frames_sent / frames_received mismatch.
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        del s["ecu1"]["can_stats"]
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        s["ecu0"]["can_stats"]["frames_sent"] += 1
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        s["ecu0"]["can_stats"]["frames_received"] += 5
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        # 9. Reversed/reordered RX trace in comm.tsv must fail!
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        tx_only = [ev for ev in t["ecu0"] if ev.direction == "T"]
        rx_only = [ev for ev in t["ecu0"] if ev.direction == "R"]
        t["ecu0"] = [*tx_only, *reversed(rx_only)]
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        # 10. Non-integer-ms CAN RX timestamp detected.
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        s["ecu0"]["sub_ms_rx_timestamps"] = 1
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

        # 11. Malformed non-hex payload raises VerificationError (not ValueError).
        s, t = copy.deepcopy(base_summaries), copy.deepcopy(base_traces)
        for i, ev in enumerate(t["ecu0"]):
            if ev.direction == "T":
                t["ecu0"][i] = dataclasses.replace(ev, payload_hex="zzzzzzzzzzzzzzzz")
                break
        with self.assertRaises(BV.VerificationError):
            BV.verify_iteration_traces(case, 50, 200, s, t)

    def test_multi_frame_seq_wrap_canonical_order_passes(self):
        # Regression for asymmetric sort-key bug when can_frames > 1 crosses uint16 seq wrap (65535 -> 0).
        case = BV.CaseSpec(
            case_id="multi_frame_wrap",
            description="2 ECUs with 3 frames/slot crossing seq wrap",
            ecus=(
                BV.EcuSpec(ecu_id=0, name="ecu0", step_ms=1, can_period_ms=10, can_offset_ms=0, can_frames=3),
                BV.EcuSpec(ecu_id=1, name="ecu1", step_ms=1, can_period_ms=10, can_offset_ms=1, can_frames=3),
            ),
        )
        # Force payload_u64_le ordering to differ from hex-string / seq order by varying high bytes.
        tx0 = [
            BV.TraceEvent("T", 0, None, 0x100, 0, _make_payload_hex(0, 0, 0, word=0xFF000000)),
            BV.TraceEvent("T", 0, None, 0x100, 1, _make_payload_hex(0, 1, 1, word=0x01000000)),
            BV.TraceEvent("T", 0, None, 0x100, 2, _make_payload_hex(0, 2, 2, word=0x00000000)),
        ]
        tx1 = [
            BV.TraceEvent("T", 1, None, 0x101, 0, _make_payload_hex(1, 0, 0, word=0x22)),
            BV.TraceEvent("T", 1, None, 0x101, 1, _make_payload_hex(1, 1, 1, word=0x22)),
            BV.TraceEvent("T", 1, None, 0x101, 2, _make_payload_hex(1, 2, 2, word=0x22)),
        ]
        rx0 = sorted(
            [BV.TraceEvent("R", e.tx_ms, e.tx_ms + 1, e.can_id, e.seq, e.payload_hex) for e in tx1],
            key=lambda ev: (ev.rx_ms, ev.tx_ms, ev.can_id, BV._payload_u64_le(ev.payload_hex, "ecu0")),
        )
        rx1 = sorted(
            [BV.TraceEvent("R", e.tx_ms, e.tx_ms + 1, e.can_id, e.seq, e.payload_hex) for e in tx0],
            key=lambda ev: (ev.rx_ms, ev.tx_ms, ev.can_id, BV._payload_u64_le(ev.payload_hex, "ecu1")),
        )
        summaries, _ = _synthesize_valid_traces(case, warmup_ms=0, measure_ms=10)
        traces = {"ecu0": [*tx0, *rx0], "ecu1": [*tx1, *rx1]}
        BV.verify_iteration_traces(case, 0, 10, summaries, traces)


class TestBoundaryGuardsAndSummary(unittest.TestCase):
    def test_paused_or_paced_run_is_rejected(self):
        case = BV.ALL_CASES["case_a_1ecu_1ms"]
        good_report = {
            "result": "passed",
            "findings": [],
            "timing": {"sim_ms": 260, "time_step_ms": 1, "wall_ms": 15, "paused_ms": 0, "paced_steps": 0},
            "participants": [{"name": "ecu0", "exit_code": 0, "control_link": {"state": "connected", "late_messages": 0}}],
        }
        BV.validate_launcher_report(case, good_report, 260, 1)

        paused = copy.deepcopy(good_report)
        paused["timing"]["paused_ms"] = 10
        with self.assertRaises(BV.VerificationError):
            BV.validate_launcher_report(case, paused, 260, 1)

        paced = copy.deepcopy(good_report)
        paced["timing"]["paced_steps"] = 1
        with self.assertRaises(BV.VerificationError):
            BV.validate_launcher_report(case, paced, 260, 1)

        # Extra unexpected participant (e.g. NetworkSimulator) must be rejected.
        extra = copy.deepcopy(good_report)
        extra["participants"].append({"name": "NetworkSimulator", "exit_code": 0, "control_link": {"state": "connected", "late_messages": 0}})
        with self.assertRaises(BV.VerificationError):
            BV.validate_launcher_report(case, extra, 260, 1)

        # Missing required keys (no silent 0 default) must be rejected.
        for missing_key in ("paused_ms", "paced_steps"):
            missing = copy.deepcopy(good_report)
            del missing["timing"][missing_key]
            with self.assertRaises(BV.VerificationError):
                BV.validate_launcher_report(case, missing, 260, 1)

        missing_late = copy.deepcopy(good_report)
        del missing_late["participants"][0]["control_link"]["late_messages"]
        with self.assertRaises(BV.VerificationError):
            BV.validate_launcher_report(case, missing_late, 260, 1)

    def test_warmup_excluded_from_summary_and_sha_mismatch_rejected(self):
        case = BV.ALL_CASES["case_a_1ecu_1ms"]
        summaries, _ = _synthesize_valid_traces(case, 50, 200)

        def make_iter(phase: str, idx: int, wall_ms: float, sha: str) -> dict:
            return {
                "index": idx,
                "phase": phase,
                "steady_state": {
                    "wall_ns": int(wall_ms * 1e6),
                    "wall_ms": wall_ms,
                    "window_skew_ns": 0,
                    "virtual_to_realtime_ratio": round(200.0 / wall_ms, 3),
                    "completed_steps": {"controller_window_steps": 200, "ecu_window_steps_total": 200, "ecu_window_steps_by_participant": {"ecu0": 200}},
                    "app_messages": {"tx_in_window": 20, "rx_in_window": 0, "tx_total": 25, "rx_total": 0, "tx_dropped": 0, "rx_dropped": 0},
                },
                "non_steady_state": {"total_process_wall_ms": wall_ms + 20, "launcher_run_wall_ms": wall_ms + 2, "setup_and_shutdown_wall_ms": 18.0, "paused_ms": 0},
                "process_cpu": {
                    "ecu_steady_state": {"status": "available", "reason": None, "interval": "steady_state_window", "window_sim_ms": [50, 250], "target_processes": ["ecu0"], "excludes": ["vehicle_sil_launch"], "user_ms": 3.0, "sys_ms": 1.0, "total_ms": 4.0, "per_process": [{"name": "ecu0", "status": "available", "reason": None, "user_ms": 3.0, "sys_ms": 1.0, "total_ms": 4.0}]},
                    "launcher_tree_total": {"status": "available", "reason": None, "interval": "full_process_lifetime", "includes_non_steady_state": True, "target_processes": ["vehicle_sil_launch", "<reaped_children>"], "user_ms": 5.0, "sys_ms": 2.0, "total_ms": 7.0, "max_rss_kb": 10000},
                },
                "control_transport_metrics": {"status": "not_requested", "unavailable_reason": None, "source": None, "counters": None},
                "determinism": {"verified": True, "output_sha256": sha, "comm_trace_sha256": "b" * 64, "comm_events_verified": 25},
                "environment_noise": {"waited_s": 0.0, "others_before": [], "others_after": [], "load1_before": 0.1, "load1_after": 0.1},
            }

        sha = "a" * 64
        # Huge warm-up wall_ms (999.0) must NOT contaminate summary median!
        report = BV.build_case_report(
            case,
            50,
            200,
            10,
            warmup_records=[make_iter("warmup", 0, 999.0, sha)],
            measured_records=[make_iter("measured", 0, 10.0, sha), make_iter("measured", 1, 12.0, sha)],
            last_ecu_summaries=summaries,
        )
        self.assertEqual(report["summary"]["iteration_count"], 2)
        self.assertEqual(report["summary"]["steady_state_wall_ms"]["median"], 11.0)
        self.assertEqual(report["summary"]["steady_state_wall_ms"]["max"], 12.0)

        # Cross-iteration SHA mismatch must raise VerificationError.
        with self.assertRaises(BV.VerificationError):
            BV.build_case_report(
                case,
                50,
                200,
                10,
                warmup_records=[make_iter("warmup", 0, 10.0, sha)],
                measured_records=[make_iter("measured", 0, 10.0, "c" * 64)],
                last_ecu_summaries=summaries,
            )


class TestCrossPlatformProcessCpu(unittest.TestCase):
    def test_posix_direct_wait4_and_darwin_maxrss_division(self):
        fake_ru = types.SimpleNamespace(ru_utime=0.125, ru_stime=0.025, ru_maxrss=10 * 1024 * 1024)

        class FakePopen:
            def __init__(self, *args, **kwargs):
                self.pid = 4242
                self.returncode = None

            def wait(self):
                raise AssertionError("Popen.wait() must not be called before os.wait4()")

        with tempfile.TemporaryDirectory() as tmp:
            tdir = Path(tmp)
            with (
                mock.patch.object(BV.subprocess, "Popen", FakePopen),
                mock.patch.object(BV.os, "name", "posix"),
                mock.patch.object(BV.os, "wait4", return_value=(4242, 0, fake_ru), create=True) as mock_w4,
                mock.patch.object(BV.os, "waitstatus_to_exitcode", return_value=0, create=True),
                mock.patch.object(BV.sys, "platform", "darwin"),
            ):
                rc, _wall, cpu = BV.run_launcher_with_rusage(
                    ["fake"], tdir, tdir / "out.json", tdir / "err.log"
                )
            mock_w4.assert_called_once_with(4242, 0)
            self.assertEqual(rc, 0)
            self.assertEqual(cpu["status"], "available")
            self.assertIsNone(cpu["reason"])
            self.assertEqual(cpu["user_ms"], 125.0)
            self.assertEqual(cpu["sys_ms"], 25.0)
            self.assertEqual(cpu["total_ms"], 150.0)
            # Darwin ru_maxrss is in bytes -> divided by 1024 to KiB.
            self.assertEqual(cpu["max_rss_kb"], 10240)

    def test_windows_os_name_nt_records_unavailable_and_null_never_zero(self):
        class FakePopen:
            def __init__(self, *args, **kwargs):
                self.pid = 777
                self.returncode = None

            def wait(self):
                self.returncode = 0
                return 0

        with tempfile.TemporaryDirectory() as tmp:
            tdir = Path(tmp)
            with (
                mock.patch.object(BV.subprocess, "Popen", FakePopen),
                mock.patch.object(BV.os, "name", "nt"),
            ):
                rc, _wall, cpu = BV.run_launcher_with_rusage(
                    ["fake"], tdir, tdir / "out.json", tdir / "err.log"
                )
            self.assertEqual(rc, 0)
            self.assertEqual(cpu["status"], "unavailable")
            self.assertIsInstance(cpu["reason"], str)
            self.assertIsNone(cpu["user_ms"])
            self.assertIsNone(cpu["sys_ms"])
            self.assertIsNone(cpu["total_ms"])
            self.assertIsNone(cpu["max_rss_kb"])

    def test_keyboard_interrupt_during_wait4_cleans_up_process_group(self):
        class FakePopen:
            def __init__(self, *args, **kwargs):
                self.pid = 9999
                self.returncode = None

        with tempfile.TemporaryDirectory() as tmp:
            tdir = Path(tmp)
            with (
                mock.patch.object(BV.subprocess, "Popen", FakePopen),
                mock.patch.object(BV.os, "name", "posix"),
                mock.patch.object(BV.os, "wait4", side_effect=KeyboardInterrupt, create=True),
                mock.patch.object(BV.verify_common, "stop_group") as mock_stop,
            ):
                with self.assertRaises(KeyboardInterrupt):
                    BV.run_launcher_with_rusage(
                        ["fake"], tdir, tdir / "out.json", tdir / "err.log"
                    )
            mock_stop.assert_called_once()

    def test_schema_ref_cross_drive_value_error_falls_back(self):
        with mock.patch.object(BV.os.path, "relpath", side_effect=ValueError("different mounts")):
            ref = BV.schema_ref_for_output(Path("/other/drive/report.json"))
        self.assertTrue(ref.endswith("schema/benchmark-report.schema.json"))


class TestControlMetricsIntegration(unittest.TestCase):
    def test_feature_detection(self):
        self.assertTrue(BV.launcher_supports_control_metrics({"features": ["control-metrics"], "formats": {"control_metrics": 1}}))
        self.assertFalse(BV.launcher_supports_control_metrics({"features": ["control-server"], "formats": {"run": 2}}))

    def test_valid_available_and_all_unavailable_reasons_including_stopped_before_setup(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "cm.json"
            good = {
                "vehicle_sil_control_metrics": 1,
                "status": "available",
                "unavailable_reason": None,
                "source": {
                    "transport": "silkit",
                    "collector": "HubTransport::stats",
                    "scope": "cumulative_hub_transport_lifetime",
                    "collection_point": "before_transport_close",
                },
                "counters": {
                    "messages": 100,
                    "bytes": 12000,
                    "handler_calls": 110,
                    "handler_time_ns": 500000,
                    "max_handler_time_ns": 12000,
                    "hellos": 5,
                    "last_words": 5,
                    "late_messages": 0,
                    "undecodable": 0,
                    "unknown": 0,
                    "refused_hellos": 0,
                },
            }
            p.write_text(json.dumps(good), encoding="utf-8")
            parsed = BV.parse_and_validate_control_metrics(p, require_available=True)
            self.assertEqual(parsed["status"], "available")
            self.assertEqual(parsed["counters"]["handler_time_ns"], 500000)

            for reason in BV.ISSUE1_UNAVAILABLE_REASONS:
                unavail = {
                    "vehicle_sil_control_metrics": 1,
                    "status": "unavailable",
                    "unavailable_reason": reason,
                    "source": {
                        "transport": "silkit",
                        "collector": "HubTransport::stats",
                        "scope": "cumulative_hub_transport_lifetime",
                        "collection_point": None,
                    },
                    "counters": None,
                }
                p.write_text(json.dumps(unavail), encoding="utf-8")
                res = BV.parse_and_validate_control_metrics(p, require_available=False)
                self.assertEqual(res["status"], "unavailable")
                self.assertEqual(res["unavailable_reason"], reason)
                self.assertIsNone(res["counters"])
                with self.assertRaises(BV.VerificationError):
                    BV.parse_and_validate_control_metrics(p, require_available=True)

            # Invariant violation: max_handler_time_ns > handler_time_ns.
            bad = copy.deepcopy(good)
            bad["counters"]["max_handler_time_ns"] = 600000
            p.write_text(json.dumps(bad), encoding="utf-8")
            with self.assertRaises(BV.VerificationError):
                BV.parse_and_validate_control_metrics(p, require_available=True)

            # Invariant violation: late_messages > 0.
            bad2 = copy.deepcopy(good)
            bad2["counters"]["late_messages"] = 1
            p.write_text(json.dumps(bad2), encoding="utf-8")
            with self.assertRaises(BV.VerificationError):
                BV.parse_and_validate_control_metrics(p, require_available=True)


# Needed for dataclasses.replace in trace fault tests.
import dataclasses  # noqa: E402

if __name__ == "__main__":
    unittest.main(verbosity=2)
