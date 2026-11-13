// SPDX-License-Identifier: MIT
//
// A child process that a launcher runs and watches (vehicle_sil_launch run): its output line by
// line while it runs, a stop request and the end of everything it started. Internal; not part of
// the library API.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil::process {

/// How a process ended.
struct ExitStatus
{
    /// The exit code (POSIX: 128 + the signal if a signal ended it; Windows: the exit code as an
    /// int, so NTSTATUS values such as 0xC000013A are negative).
    int code = 0;
    /// POSIX: the signal that ended it; empty otherwise.
    std::optional<int> signal{};

    friend bool operator==(const ExitStatus&, const ExitStatus&) = default;
};

/// The name of a POSIX signal ("SIGTERM"), or "signal <n>" for one without a name here.
[[nodiscard]] std::string signal_name(int signal);

/// Which stream a line came from.
enum class Stream
{
    Output, ///< standard output
    Error,  ///< standard error
};

struct ProcessOptions
{
    /// The directory the process starts in; empty: the caller's.
    std::filesystem::path working_directory{};
    /// Called with each line the process writes to its standard output or standard error, without
    /// its line end ("\n" or "\r\n"), on the process's reader thread (one call at a time per
    /// process; the calls of different processes may overlap). A line longer than `max_line`
    /// bytes is passed in pieces of at most `max_line` bytes, cut between UTF-8 characters; a last line without a line end is passed
    /// when the stream closes. Must not throw.
    std::function<void(Stream, std::string_view line)> on_line{};
    std::size_t max_line = 4096;
    /// true: its standard input is a pipe that write_input() writes to and close_input() closes
    /// (else the null device).
    bool input = false;
    /// Variables (name, value) set in its environment, over the caller's; the rest of its
    /// environment is the caller's. A name must not be empty or have '='.
    std::vector<std::pair<std::string, std::string>> environment{};
};

/// A process with its standard input from the null device (or a pipe: ProcessOptions::input) and its standard output and standard
/// error read line by line (ProcessOptions::on_line). It inherits no other handle or descriptor of
/// the caller. It and every process it starts belong together, so that they can be asked to stop
/// and ended as one:
///
/// - POSIX: a process group of its own, whose ID is the process ID. The terminal's Ctrl+C does
///   not reach it (the caller decides when it stops). On Linux it receives SIGKILL when the
///   thread that started it ends (also when the caller dies): start it from a thread that lives
///   as long as the process may (the main thread). A process it started that leaves the group
///   (setsid(), setpgid()) is not ended with it; one that outlives it is not ended when the
///   caller dies.
/// - Windows: a new process group (CTRL_BREAK_EVENT reaches it, the console's Ctrl+C does not)
///   and a job object that ends every process in it when its last handle closes, also when the
///   caller dies.
///
/// The executable is a path (PATH is not searched). Not thread safe: one thread uses it.
class Process
{
public:
    /// Starts `executable arguments`. Throws StartError if it cannot be started.
    Process(const std::filesystem::path& executable, const std::vector<std::string>& arguments, ProcessOptions options);
    /// kill_tree() if it runs, then stops reading (finish_output() without waiting).
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) = delete;
    Process& operator=(Process&&) = delete;

    /// How it ended, or empty while it runs. Does not wait.
    std::optional<ExitStatus> poll();
    /// Waits up to `timeout` for its end (in steps of a few milliseconds).
    std::optional<ExitStatus> wait_for(std::chrono::milliseconds timeout);

    /// Asks it and the processes it started to stop: POSIX SIGTERM to its process group,
    /// Windows CTRL_BREAK_EVENT to its process group (only if the caller has a console the
    /// process shares). false if the request could not be sent (it has ended, or no console).
    bool request_stop() noexcept;

    /// Ends it and every process of its group (POSIX: SIGKILL to the process group) or job
    /// (Windows), and waits for its end. Also when it has ended already (for what it left).
    void kill_tree() noexcept;

    /// After its end: true if processes it started still run in its group or job.
    [[nodiscard]] bool others_running();

    /// Waits up to `timeout` until both output streams have closed (every process that holds
    /// them has ended) and every line has been passed on; then stops reading either way. true if
    /// they closed. Idempotent.
    bool finish_output(std::chrono::milliseconds timeout);

    /// The process ID.
    [[nodiscard]] long id() const noexcept { return static_cast<long>(pid_); }
    /// ProcessOptions::input: writes `bytes` to its standard input (blocks while the pipe is full).
    /// false if it cannot (no input, closed, the process has closed its end). POSIX: the caller
    /// ignores SIGPIPE, or a write after the process has closed its end ends the caller.
    bool write_input(std::string_view bytes);
    /// Closes its standard input (the process reads its end). Idempotent.
    void close_input() noexcept;

private:
    void read_output();

    ProcessOptions options_;
    std::optional<ExitStatus> status_;
    std::mutex mutex_;
    std::condition_variable closed_;
    bool output_closed_ = false; ///< guarded by mutex_
    std::thread reader_;
#ifdef _WIN32
    void* process_ = nullptr; // HANDLE
    void* job_ = nullptr;     // HANDLE
    void* out_ = nullptr;     // HANDLE, the read end of standard output
    void* err_ = nullptr;     // HANDLE, the read end of standard error
    void* in_ = nullptr;      // HANDLE, the write end of standard input (ProcessOptions::input)
    bool stop_reading_ = false; ///< guarded by mutex_
    unsigned long pid_ = 0;
#else
    int pid_ = -1;
    bool group_gone_ = false; ///< the process group was seen empty: its ID is not ours any more
    int out_ = -1;
    int err_ = -1;
    int in_ = -1; ///< the write end of standard input (ProcessOptions::input)
    int wake_read_ = -1;  ///< the reader waits on it too: a byte ends the reading
    int wake_write_ = -1;
#endif
};

} // namespace vehicle_sil::process
