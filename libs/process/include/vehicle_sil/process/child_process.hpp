// SPDX-License-Identifier: MIT
//
// Child processes for the vehicle-sil tools and the process-level tests (POSIX posix_spawn /
// Win32 CreateProcessW): one that writes to a log file and runs until it is waited for or
// killed, and a run to the end with its standard output and standard error captured and a
// time limit. Internal; not part of the library API.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::process {

/// `text` (UTF-8) as a path. On Windows the path holds the text as UTF-16, so a path with
/// characters outside the code page works with the wide Win32 functions; std::filesystem::path
/// made from a std::string would read the text in the code page.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);

/// `path` as UTF-8 (on Windows from its UTF-16 form; never throws for a path made with
/// path_from_utf8()).
[[nodiscard]] std::string utf8_from_path(const std::filesystem::path& path);

/// `path` as UTF-8 with '/' between its parts, also on Windows (where a path joined from parts
/// can mix '/' and '\\'): for messages, so that one path reads the same everywhere.
[[nodiscard]] std::string shown_path(const std::filesystem::path& path);

/// A process could not be started: because of the executable (missing, not executable, not a
/// program for this system), or because the system refused (pipes, the Windows job object).
class StartError : public std::runtime_error
{
public:
    enum class Cause
    {
        Executable,
        System,
    };
    StartError(const std::string& message, Cause cause) : std::runtime_error(message), cause_(cause) {}
    [[nodiscard]] Cause cause() const noexcept { return cause_; }

private:
    Cause cause_;
};

/// A process that runs `executable arguments` with its standard output and standard error in a
/// log file and its standard input inherited. The arguments are UTF-8 (on Windows they are
/// quoted as the C runtimes read them back).
class ChildProcess
{
public:
    /// Starts `executable` with `arguments`, redirecting stdout and stderr to `log_file`.
    /// Throws StartError if the process cannot be started.
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

/// How run_captured() runs a process.
struct CaptureOptions
{
    /// How long the process may run (wall clock). After that it is killed (on Windows with the
    /// processes it started).
    std::chrono::milliseconds timeout{10000};
    /// The most bytes kept of each of standard output (its beginning) and standard error (its
    /// end); the rest is read and dropped (Captured::output_truncated, error_truncated).
    std::size_t max_bytes = std::size_t{1} << 20;
};

/// What a process run by run_captured() did.
struct Captured
{
    /// The exit code (128 + signal number for a POSIX signal; on Windows the process's exit
    /// code as an int), or nullopt if it did not end within CaptureOptions::timeout and was
    /// killed.
    std::optional<int> exit_code;
    /// POSIX: the signal that ended the process (exit_code is then 128 + it); empty otherwise.
    std::optional<int> signal{};
    std::string standard_output; ///< as written (bytes; "\r\n" is not changed)
    std::string standard_error;
    bool output_truncated = false; ///< standard output was longer than CaptureOptions::max_bytes
    bool error_truncated = false;  ///< standard error was longer than CaptureOptions::max_bytes
};

/// Runs `executable arguments` to its end, with standard input from the null device and
/// standard output and standard error each captured, and returns what it wrote and its exit
/// code. The time limit holds also for a process that writes without a pause. The caller's file
/// descriptors 0 to 2 are expected open (as in any program started from a shell or a launcher). Waits for the streams to close, at most a short while after the process has ended (a
/// process it started may keep them open). The process does not search PATH: `executable` is
/// a path (on Windows with its extension, such as ".exe"). Throws StartError, naming the
/// executable and the reason, if the process cannot be started.
[[nodiscard]] Captured run_captured(const std::filesystem::path& executable,
                                    const std::vector<std::string>& arguments,
                                    const CaptureOptions& options = {});

} // namespace vehicle_sil::process
