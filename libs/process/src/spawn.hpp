// SPDX-License-Identifier: MIT
//
// Starting a child process, shared by ChildProcess, run_captured() and Process. Internal.
#pragma once

#include "vehicle_sil/process/child_process.hpp"
#include "vehicle_sil/process/process.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace vehicle_sil::process::detail {

#ifdef _WIN32

/// "<what> failed with Win32 error <n> (<meaning>)" of GetLastError().
std::string last_error_message(const char* what);

/// A job that kills its processes when its handle closes (also when this process dies). Throws
/// StartError (Cause::System) with the reason if it cannot be made.
HANDLE make_kill_on_close_job();

struct StartFlags
{
    /// Where the process starts; empty: the caller's directory.
    std::filesystem::path working_directory{};
    /// CREATE_NEW_PROCESS_GROUP.
    bool new_group = false;
    /// Only `in`, `out` and `err` are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST; they must be
    /// inheritable and distinct). false: every inheritable handle of the caller.
    bool only_std_handles = false;
    /// Set over the caller's environment (ProcessOptions::environment).
    std::vector<std::pair<std::string, std::string>> environment{};
};

/// Starts the process suspended with the given standard handles, puts it in `job` and resumes
/// it. Throws StartError (also when there is no job, the process cannot join it or does not
/// resume: then the process is ended before it runs). Returns the process handle.
HANDLE start(const std::filesystem::path& executable, const std::vector<std::string>& arguments, HANDLE in, HANDLE out,
             HANDLE err, HANDLE job, const StartFlags& flags = {});

/// Closes a handle when it goes out of scope.
struct Handle
{
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : h(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() noexcept
    {
        if (h != nullptr)
        {
            CloseHandle(h);
            h = nullptr;
        }
    }
    HANDLE release() noexcept
    {
        HANDLE out = h;
        h = nullptr;
        return out;
    }
};

/// A pipe whose read end stays here (not inheritable) and whose write end the child inherits.
void make_pipe(Handle& read, Handle& write);

/// The null device for reading, inheritable.
HANDLE open_null_for_child();

#else

/// What spawn() gives the child.
struct SpawnRequest
{
    /// Standard input, output and error of the child (descriptors of the caller; they need not
    /// be inheritable).
    int in = -1;
    int out = -1;
    int err = -1;
    /// Where it starts; empty: the caller's directory.
    std::filesystem::path working_directory{};
    /// A process group of its own (its ID is the child's process ID).
    bool own_group = false;
    /// Linux: SIGKILL when the calling thread ends.
    bool end_with_caller = false;
    /// Set over the caller's environment (ProcessOptions::environment).
    std::vector<std::pair<std::string, std::string>> environment{};
};

/// Starts `executable arguments` (fork and exec; no PATH search; the environment of the
/// caller). The child closes every other descriptor and sets the signals the caller ignores back
/// to their default. Throws StartError (Cause::Executable for what exec or chdir reported,
/// Cause::System for a failed fork or pipe).
pid_t spawn(const std::filesystem::path& executable, const std::vector<std::string>& arguments, const SpawnRequest& request);

/// The exit status of a wait status, or empty if it has not ended (stopped, continued).
std::optional<ExitStatus> exit_status_of(int status);

/// A pipe with both ends closed on exec, at descriptors 3 and above. Throws StartError.
void make_pipe(int& read, int& write);

#endif

} // namespace vehicle_sil::process::detail
