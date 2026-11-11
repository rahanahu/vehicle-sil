// SPDX-License-Identifier: MIT
#include "vehicle_sil/process/child_process.hpp"

#include "vehicle_sil/process/detail/capture.hpp"

#include "spawn.hpp"

#include <algorithm>
#include <stdexcept>
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
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace vehicle_sil::process {

namespace {

/// How long run_captured() keeps reading after the process has ended: a process it started may
/// hold the streams open.
constexpr std::chrono::milliseconds kDrainAfterExit{500};

using detail::kReadsPerCall;
using detail::Sink;

} // namespace

std::filesystem::path path_from_utf8(std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string shown_path(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string utf8_from_path(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

#ifdef _WIN32

using detail::Handle;
using detail::last_error_message;
using detail::make_kill_on_close_job;
using detail::make_pipe;
using detail::start;

ChildProcess::ChildProcess(const std::filesystem::path& executable,
                           const std::vector<std::string>& arguments,
                           const std::filesystem::path& log_file)
{
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(log_file.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
    {
        throw std::runtime_error(last_error_message("CreateFileW(log)"));
    }
    try
    {
        job_ = make_kill_on_close_job();
        process_ = start(executable, arguments, GetStdHandle(STD_INPUT_HANDLE), log, log, job_);
    }
    catch (...)
    {
        CloseHandle(log);
        if (job_ != nullptr)
        {
            CloseHandle(job_);
            job_ = nullptr;
        }
        throw;
    }
    CloseHandle(log);
}

ChildProcess::~ChildProcess()
{
    kill();
    if (process_ != nullptr)
    {
        CloseHandle(process_);
    }
    if (job_ != nullptr)
    {
        CloseHandle(job_);
    }
}

std::optional<int> ChildProcess::wait_for(std::chrono::milliseconds timeout)
{
    if (exit_code_)
    {
        return exit_code_;
    }
    if (WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0)
    {
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    exit_code_ = static_cast<int>(code);
    return exit_code_;
}

void ChildProcess::kill() noexcept
{
    if (exit_code_ || process_ == nullptr)
    {
        return;
    }
    TerminateProcess(process_, 137);
    WaitForSingleObject(process_, 5000);
    DWORD code = 137;
    GetExitCodeProcess(process_, &code);
    exit_code_ = static_cast<int>(code);
}

namespace {

/// Reads what the pipe holds now, without blocking. Returns false once the pipe is closed
/// (the writers are gone) and resets it.
bool read_available(Handle& pipe, Sink& sink, bool& read_any)
{
    if (pipe.h == nullptr)
    {
        return false;
    }
    char buffer[4096];
    for (std::size_t reads = 0; reads < kReadsPerCall; ++reads)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe.h, nullptr, 0, nullptr, &available, nullptr))
        {
            pipe.reset(); // ERROR_BROKEN_PIPE: every writer has closed it
            return false;
        }
        if (available == 0)
        {
            return true;
        }
        DWORD got = 0;
        if (!ReadFile(pipe.h, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || got == 0)
        {
            pipe.reset();
            return false;
        }
        sink.append(buffer, got);
        read_any = true;
    }
    return true;
}

} // namespace

Captured run_captured(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                      const CaptureOptions& options)
{
    Handle outRead;
    Handle outWrite;
    Handle errRead;
    Handle errWrite;
    make_pipe(outRead, outWrite);
    make_pipe(errRead, errWrite);
    Handle nul(detail::open_null_for_child());
    Handle job(make_kill_on_close_job());
    detail::StartFlags flags;
    flags.working_directory = options.working_directory;
    flags.new_group = true;
    flags.only_std_handles = true;
    Handle process(start(executable, arguments, nul.h, outWrite.h, errWrite.h, job.h, flags));
    // Only the child holds the write ends now: the pipes close when it (and what it started)
    // has ended.
    outWrite.reset();
    errWrite.reset();
    nul.reset();

    Captured result;
    Sink out{result.standard_output, options.max_bytes, result.output_truncated};
    Sink err{result.standard_error, options.max_bytes, result.error_truncated, true};
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    std::optional<std::chrono::steady_clock::time_point> exitedAt;
    while (true)
    {
        bool readAny = false;
        const bool outOpen = read_available(outRead, out, readAny);
        const bool errOpen = read_available(errRead, err, readAny);
        if (!exitedAt && WaitForSingleObject(process.h, 0) == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            GetExitCodeProcess(process.h, &code);
            result.exit_code = static_cast<int>(code);
            exitedAt = std::chrono::steady_clock::now();
        }
        const auto now = std::chrono::steady_clock::now();
        if (exitedAt && ((!outOpen && !errOpen) || now - *exitedAt >= kDrainAfterExit))
        {
            break;
        }
        if (!exitedAt && now >= deadline)
        {
            TerminateJobObject(job.h, 1);
            TerminateProcess(process.h, 1);
            WaitForSingleObject(process.h, 5000);
            bool ignored = false;
            read_available(outRead, out, ignored);
            read_available(errRead, err, ignored);
            result.exit_code.reset();
            break;
        }
        if (!readAny)
        {
            WaitForSingleObject(process.h, exitedAt ? 0 : 10);
            if (exitedAt)
            {
                Sleep(10);
            }
        }
    }
    err.finish();
    return result;
}

#else // POSIX

ChildProcess::ChildProcess(const std::filesystem::path& executable,
                           const std::vector<std::string>& arguments,
                           const std::filesystem::path& log_file)
{
    const std::string program = executable.string();
    const std::string log = log_file.string();

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& argument : arguments)
    {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0)
    {
        throw StartError("cannot start " + program + ": " + std::strerror(rc), StartError::Cause::Executable);
    }
    pid_ = pid;
}

ChildProcess::~ChildProcess()
{
    kill();
}

namespace {

/// The exit code of a wait status: the code, or 128 + the signal; nullopt if it has not ended.
std::optional<int> exit_code_of(int status)
{
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        return 128 + WTERMSIG(status);
    }
    return std::nullopt;
}

} // namespace

std::optional<int> ChildProcess::wait_for(std::chrono::milliseconds timeout)
{
    if (exit_code_ || pid_ <= 0)
    {
        return exit_code_;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        int status = 0;
        const pid_t rc = ::waitpid(pid_, &status, WNOHANG);
        if (rc == pid_)
        {
            exit_code_ = exit_code_of(status);
            if (!exit_code_)
            {
                continue;
            }
            return exit_code_;
        }
        if (rc < 0 && errno != EINTR)
        {
            exit_code_ = -1;
            return exit_code_;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ChildProcess::kill() noexcept
{
    if (exit_code_ || pid_ <= 0)
    {
        return;
    }
    ::kill(pid_, SIGTERM);
    if (!wait_for(std::chrono::milliseconds(3000)))
    {
        ::kill(pid_, SIGKILL);
        (void)wait_for(std::chrono::milliseconds(3000));
    }
}

namespace {

/// A file descriptor closed when it goes out of scope.
struct Fd
{
    int fd = -1;
    Fd() = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }
    void reset() noexcept
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
    }
};

/// A pipe with both ends closed on exec and the read end non-blocking.
void make_pipe(Fd& read, Fd& write)
{
    detail::make_pipe(read.fd, write.fd);
    ::fcntl(read.fd, F_SETFL, ::fcntl(read.fd, F_GETFL) | O_NONBLOCK);
}

} // namespace

bool detail::read_available(int& fd, Sink& sink)
{
    char buffer[4096];
    for (std::size_t reads = 0; fd >= 0; ++reads)
    {
        if (reads == kReadsPerCall)
        {
            return true;
        }
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got > 0)
        {
            sink.append(buffer, static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR)
        {
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return true;
        }
        ::close(fd); // end of file, or an error
        fd = -1;
    }
    return false;
}

Captured run_captured(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                      const CaptureOptions& options)
{
    Fd outRead;
    Fd outWrite;
    Fd errRead;
    Fd errWrite;
    make_pipe(outRead, outWrite);
    make_pipe(errRead, errWrite);
    Fd null;
    null.fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null.fd < 0)
    {
        throw StartError(std::string("cannot open /dev/null: ") + std::strerror(errno), StartError::Cause::System);
    }
    detail::SpawnRequest request;
    request.in = null.fd;
    request.out = outWrite.fd;
    request.err = errWrite.fd;
    request.working_directory = options.working_directory;
    // A group of its own: the time limit ends what it started too, and the terminal's Ctrl+C
    // reaches only the caller (which then ends, and with it this child).
    request.own_group = true;
    request.end_with_caller = true;
    const pid_t pid = detail::spawn(executable, arguments, request);
    // Only the child holds the write ends now: the pipes close when it (and what it started)
    // has ended.
    outWrite.reset();
    errWrite.reset();

    Captured result;
    Sink out{result.standard_output, options.max_bytes, result.output_truncated};
    Sink err{result.standard_error, options.max_bytes, result.error_truncated, true};
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    std::optional<std::chrono::steady_clock::time_point> exitedAt;
    while (true)
    {
        const bool outOpen = detail::read_available(outRead.fd, out);
        const bool errOpen = detail::read_available(errRead.fd, err);
        if (!exitedAt)
        {
            int status = 0;
            const pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid && exit_code_of(status))
            {
                result.exit_code = exit_code_of(status);
                if (WIFSIGNALED(status))
                {
                    result.signal = WTERMSIG(status);
                }
                exitedAt = std::chrono::steady_clock::now();
            }
            else if (waited < 0 && errno != EINTR)
            {
                result.exit_code = -1;
                exitedAt = std::chrono::steady_clock::now();
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (exitedAt && ((!outOpen && !errOpen) || now - *exitedAt >= kDrainAfterExit))
        {
            break;
        }
        if (!exitedAt && now >= deadline)
        {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }
            detail::read_available(outRead.fd, out);
            detail::read_available(errRead.fd, err);
            result.exit_code.reset();
            break;
        }
        // Wait for output, or a little while for the process to end.
        pollfd fds[2];
        nfds_t count = 0;
        for (const Fd* pipe : {&outRead, &errRead})
        {
            if (pipe->fd >= 0)
            {
                fds[count++] = pollfd{pipe->fd, POLLIN, 0};
            }
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>((exitedAt ? *exitedAt + kDrainAfterExit : deadline) - now);
        const int wait = static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(left.count(), 1, 10));
        if (count > 0)
        {
            ::poll(fds, count, wait);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(wait));
        }
    }
    err.finish();
    return result;
}

#endif

} // namespace vehicle_sil::process
