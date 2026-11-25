# SPDX-License-Identifier: MIT
"""Windows acceptance tests for vehicle_sil_launch serve (#11):
- 10 ECU repetitions with handle count (GetProcessHandleCount), thread count and child process checks.
- Fixed-port gRPC Control Server re-listen across Runs (when built with control server).
- Console event CTRL_BREAK_EVENT while idle and while running (exit 130).
- TerminateProcess kills child ECUs via Job Object."""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import signal
import statistics
import time
import unittest

import serve_fixtures as fx
from serve_digest import digest, first_time_ms, stream_problems
from serve_support import (
    EXIT_IDLE_EOF,
    EXIT_INTERRUPTED,
    Scratch,
    Serve,
    WINDOWS,
    cold,
    env_path,
    free_port,
    runs,
    say,
    scaled,
    slope,
)

if WINDOWS:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    TH32CS_SNAPPROCESS = 0x00000002
    INVALID_HANDLE_VALUE = wt.HANDLE(-1).value

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wt.DWORD),
            ("cntUsage", wt.DWORD),
            ("th32ProcessID", wt.DWORD),
            ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
            ("th32ModuleID", wt.DWORD),
            ("cntThreads", wt.DWORD),
            ("th32ParentProcessID", wt.DWORD),
            ("pcPriClassBase", wt.LONG),
            ("dwFlags", wt.DWORD),
            ("szExeFile", wt.WCHAR * 260),
        ]

    kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    kernel32.OpenProcess.restype = wt.HANDLE
    kernel32.CloseHandle.argtypes = [wt.HANDLE]
    kernel32.CloseHandle.restype = wt.BOOL
    kernel32.GetProcessHandleCount.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
    kernel32.GetProcessHandleCount.restype = wt.BOOL
    kernel32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
    kernel32.GetExitCodeProcess.restype = wt.BOOL
    kernel32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    kernel32.Process32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    kernel32.Process32FirstW.restype = wt.BOOL
    kernel32.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    kernel32.Process32NextW.restype = wt.BOOL


def win_process_stats(pid: int):
    """Returns (handles, threads, child_pids) for `pid` on Windows."""
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    try:
        handles = wt.DWORD(0)
        ok = kernel32.GetProcessHandleCount(h, ctypes.byref(handles))
        hcount = handles.value if ok else -1
    finally:
        kernel32.CloseHandle(h)

    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    threads = -1
    children = []
    if snap != INVALID_HANDLE_VALUE:
        try:
            entry = PROCESSENTRY32W()
            entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
            if kernel32.Process32FirstW(snap, ctypes.byref(entry)):
                while True:
                    if entry.th32ProcessID == pid:
                        threads = entry.cntThreads
                    if entry.th32ParentProcessID == pid:
                        children.append(entry.th32ProcessID)
                    if not kernel32.Process32NextW(snap, ctypes.byref(entry)):
                        break
        finally:
            kernel32.CloseHandle(snap)
    return {"handles": hcount, "threads": threads, "children": children}


def win_pid_alive(pid: int) -> bool:
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return False
    try:
        code = wt.DWORD(0)
        if not kernel32.GetExitCodeProcess(h, ctypes.byref(code)):
            return False
        return code.value == 259  # STILL_ACTIVE
    finally:
        kernel32.CloseHandle(h)


@unittest.skipUnless(WINDOWS, "Windows-only acceptance test")
class ServeWindowsAcceptanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.launch = env_path("VEHICLE_SIL_LAUNCH_EXE")

    def test_11_windows_10ecu_handles_threads_and_vt0(self) -> None:
        n = runs("VEHICLE_SIL_SERVE_WIN_RUNS", 30)
        scratch = Scratch("win_10ecu")
        cfg = fx.ten_ecus(scratch)
        scen = fx.vt0_scenario(scratch)
        req = scratch.request("vt0", scenario=scen.name)
        c_ref = cold(self.launch, cfg, req)
        self.assertEqual(c_ref.returncode, 0, c_ref.stderr)
        ref_digest = digest(c_ref.result)

        handle_series = []
        thread_series = []
        hot_times = []
        with Serve(self.launch, cfg, keep_logs=False) as srv:
            srv.ready()
            for i in range(n):
                t0 = time.monotonic()
                rec = srv.run_once(req)
                hot_times.append((time.monotonic() - t0) * 1000.0)
                self.assertEqual(rec.result["result"], "passed", f"run {i + 1}")
                self.assertEqual(first_time_ms(rec.window), 0)
                self.assertEqual(digest(rec.result), ref_digest)
                # Wait briefly for any transient child/thread exit.
                stats = None
                deadline = time.monotonic() + scaled(3)
                while time.monotonic() < deadline:
                    stats = win_process_stats(srv.process.pid)
                    if stats and not stats["children"]:
                        break
                    time.sleep(0.05)
                self.assertIsNotNone(stats)
                self.assertEqual(stats["children"], [], f"child processes left at run {i + 1}: {stats}")
                handle_series.append(stats["handles"])
                thread_series.append(stats["threads"])
            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])

        tail_h = handle_series[len(handle_series) // 2:]
        tail_t = thread_series[len(thread_series) // 2:]
        h_slope = slope(tail_h)
        t_slope = slope(tail_t)
        hot_sorted = sorted(hot_times)
        p90 = hot_sorted[min(len(hot_sorted) - 1, int(len(hot_sorted) * 0.9))]
        say(f"[win-10ecu] runs={n} median={statistics.median(hot_times):.1f}ms p90={p90:.1f}ms "
            f"handles={handle_series[0]}->{handle_series[-1]} (tail slope={h_slope:.2f}/Run) "
            f"threads={thread_series[0]}->{thread_series[-1]} (tail slope={t_slope:.2f}/Run)")
        # One handle per run is a leak too: with MinGW, a thread that the C++ API of SIL Kit
        # detached left an event handle behind on every run (patch 0007, docs/dependencies.md).
        self.assertLess(abs(h_slope), 0.5, f"handle leak: {handle_series}")
        self.assertLess(abs(t_slope), 1.0, f"thread leak: {thread_series}")

    def test_11_windows_ctrl_break_and_job_object_termination(self) -> None:
        scratch = Scratch("win_ctrl")
        cfg = fx.bench(scratch, ["T1"])
        long_s = fx.long_scenario(scratch)
        req_long = scratch.request("long", scenario=long_s.name, speed=1)

        # Idle CTRL_BREAK_EVENT -> exit 130.
        with Serve(self.launch, cfg) as srv:
            srv.ready()
            os.kill(srv.process.pid, signal.CTRL_BREAK_EVENT)
            self.assertEqual(srv.wait(15), EXIT_INTERRUPTED)
            self.assertFalse(any(e.get("event") == "run_start" for e in srv.snapshot()))

        # Running CTRL_BREAK_EVENT -> interrupted, launcher_ends: true, exit 130.
        with Serve(self.launch, cfg) as srv:
            srv.ready()
            run_h = srv.start(request=req_long)
            run_h.wait_for_event("phase", phase="running")
            os.kill(srv.process.pid, signal.CTRL_BREAK_EVENT)
            res = run_h.wait()
            self.assertTrue(run_h.launcher_ends)
            self.assertEqual(res["result"], "interrupted")
            self.assertEqual(srv.wait(30), EXIT_INTERRUPTED)
            self.assertEqual(stream_problems(srv.snapshot()), [])

        # TerminateProcess kills child ECU via Job Object.
        with Serve(self.launch, cfg) as srv:
            srv.ready()
            run_h = srv.start(request=req_long)
            ev_p = run_h.wait_for_event("participant", state="started")
            child_pid = ev_p["pid"]
            srv.process.kill()
            srv.process.wait(10)
            deadline = time.monotonic() + scaled(5)
            while win_pid_alive(child_pid) and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertFalse(win_pid_alive(child_pid), f"child {child_pid} survived TerminateProcess")

    def test_11_windows_fixed_port_grpc_relisten(self) -> None:
        if os.environ.get("VEHICLE_SIL_SERVE_CONTROL_SERVER", "").upper() not in ("1", "ON", "TRUE"):
            self.skipTest("Control server not enabled in this build")
        n = runs("VEHICLE_SIL_SERVE_WIN_GRPC_RUNS", 20)
        scratch = Scratch("win_grpc_relisten")
        cfg = fx.bench(scratch, ["T1"])
        scen = fx.vt0_scenario(scratch)
        req = scratch.request("vt0", scenario=scen.name)
        port = free_port()
        address = f"127.0.0.1:{port}"
        with Serve(self.launch, cfg, arguments=["--control", address], keep_logs=False) as srv:
            srv.ready()
            for i in range(n):
                rec = srv.run_once(req)
                self.assertEqual(rec.result["result"], "passed", f"gRPC re-listen run {i + 1}")
            srv.close_input()
            self.assertEqual(srv.wait(), EXIT_IDLE_EOF)
            self.assertEqual(stream_problems(srv.snapshot()), [])
        say(f"[win-grpc] fixed-port {address} re-listen passed {n}/{n} Runs")


if __name__ == "__main__":
    unittest.main()
