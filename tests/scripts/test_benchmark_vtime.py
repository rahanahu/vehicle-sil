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
import sys
import tempfile
import types
import unittest
from pathlib import Path
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
