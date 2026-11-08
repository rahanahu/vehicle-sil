// SPDX-License-Identifier: MIT
//
// Minimal cross-platform child process handling for the integration test launcher
// (POSIX posix_spawn / Win32 CreateProcess). stdout and stderr of the child go to a log file.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::test {

class ChildProcess
{
public:
    /// Starts `executable` with `arguments`, redirecting stdout and stderr to `log_file`.
    /// Throws std::runtime_error if the process cannot be started.
    ChildProcess(const std::filesystem::path& executable,
                 const std::vector<std::string>& arguments,
                 const std::filesystem::path& log_file);

    /// Kills the process if it is still running.
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    /// Waits up to `timeout` for the process to exit.
    /// @returns The exit code (128 + signal number for a POSIX signal), or nullopt on timeout.
    std::optional<int> wait_for(std::chrono::milliseconds timeout);

    /// Polls once without blocking.
    std::optional<int> poll() { return wait_for(std::chrono::milliseconds(0)); }

    /// Terminates the process forcefully and reaps it. No-op if it already exited.
    void kill() noexcept;

private:
    std::optional<int> exit_code_;
#ifdef _WIN32
    void* process_ = nullptr; // HANDLE
    void* job_ = nullptr;     // HANDLE, kills the child if the launcher dies
#else
    int pid_ = -1;
#endif
};

} // namespace vehicle_sil::test
