// SPDX-License-Identifier: MIT
//
// For the tests of vehicle_sil::process and vehicle_sil_launch: starts a process that outlives
// its parent (a "grandchild" of the test), and tells whether a process still runs.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/types.h>
#include <unistd.h>
extern char** environ;
#endif

namespace vehicle_sil::test {

/// The path of the running executable.
inline std::string own_executable()
{
#ifdef _WIN32
    wchar_t path[32768];
    const DWORD size = GetModuleFileNameW(nullptr, path, 32768);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(size), out.data(), bytes, nullptr, nullptr);
    return out;
#else
    char path[4096];
    const ssize_t size = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    return size > 0 ? std::string(path, static_cast<std::size_t>(size)) : std::string{};
#endif
}

/// Starts `helper --pid-file <pid_file> --sleep-ms 600000` (tests/process/process_helper.cpp) and
/// does not wait for it. POSIX: it stays in the caller's process group and keeps the caller's
/// standard streams; Windows: it stays in the caller's job and inherits no handle.
inline void spawn_sleeper(const std::string& helper, const std::string& pid_file)
{
#ifdef _WIN32
    const std::string line = "\"" + helper + "\" --pid-file \"" + pid_file + "\" --sleep-ms 600000";
    const int size = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), size);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info))
    {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
#else
    std::string a0 = helper;
    std::string a1 = "--pid-file";
    std::string a2 = pid_file;
    std::string a3 = "--sleep-ms";
    std::string a4 = "600000";
    char* argv[] = {a0.data(), a1.data(), a2.data(), a3.data(), a4.data(), nullptr};
    pid_t pid = -1;
    static_cast<void>(::posix_spawn(&pid, helper.c_str(), nullptr, nullptr, argv, environ));
#endif
}

/// true while the process `pid` runs (POSIX: a zombie does not count).
inline bool process_alive(long pid)
{
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
    {
        return false;
    }
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    if (pid <= 0 || ::kill(static_cast<pid_t>(pid), 0) != 0)
    {
        return false;
    }
    // C stdio: a process that ends while its stat is read makes read() fail with ESRCH, which a
    // std::ifstream reports by throwing.
    std::string text;
    if (std::FILE* stat = std::fopen(("/proc/" + std::to_string(pid) + "/stat").c_str(), "r"))
    {
        char buffer[512];
        const std::size_t got = std::fread(buffer, 1, sizeof(buffer), stat);
        std::fclose(stat);
        text.assign(buffer, got);
    }
    if (text.empty())
    {
        return false; // gone while it was looked at
    }
    const auto close = text.rfind(')');
    return close == std::string::npos || close + 2 >= text.size() || text[close + 2] != 'Z';
#endif
}

/// The process ID in `pid_file`, waiting up to `timeout` for the file to be written; 0 if none.
inline long read_pid(const std::string& pid_file, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        std::ifstream in(pid_file);
        long pid = 0;
        if (in >> pid && pid > 0)
        {
            return pid;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/// Waits up to `timeout` for the process `pid` to be gone; true if it is.
inline bool wait_gone(long pid, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (process_alive(pid))
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

/// Ends the process `pid` alone (not its group or job), for cleaning up after a failed test and
/// for killing a launcher without its children.
inline void kill_one(long pid)
{
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (process != nullptr)
    {
        TerminateProcess(process, 137);
        CloseHandle(process);
    }
#else
    if (pid > 0)
    {
        ::kill(static_cast<pid_t>(pid), SIGKILL);
    }
#endif
}

} // namespace vehicle_sil::test
