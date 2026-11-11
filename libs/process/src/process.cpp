// SPDX-License-Identifier: MIT
#include "vehicle_sil/process/process.hpp"

#include "spawn.hpp"

#include <cstring>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace vehicle_sil::process {

namespace {

/// Cuts a stream into lines: "\n" ends a line, a "\r" before it is dropped, a line longer than
/// `max` goes out in pieces.
class LineSplitter
{
public:
    LineSplitter(Stream stream, const ProcessOptions& options) : stream_(stream), options_(options) {}

    void append(const char* data, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i)
        {
            if (data[i] == '\n')
            {
                if (!line_.empty() && line_.back() == '\r')
                {
                    line_.pop_back();
                }
                emit();
                continue;
            }
            line_.push_back(data[i]);
            // A "\r" right after the limit waits for the next byte: with "\n" it is the line end
            // and leaves no empty piece behind.
            while (line_.size() > max() && !(line_.size() == max() + 1 && line_.back() == '\r'))
            {
                // Not in the middle of a UTF-8 character (unless a whole piece is one).
                std::size_t cut = max();
                while (cut > 0 && (static_cast<unsigned char>(line_[cut]) & 0xC0) == 0x80)
                {
                    --cut;
                }
                if (cut == 0)
                {
                    cut = max();
                }
                std::string rest = line_.substr(cut);
                line_.resize(cut);
                emit();
                line_ = std::move(rest);
            }
        }
    }

    /// The stream closed: a last line without its end.
    void finish()
    {
        if (!line_.empty())
        {
            emit();
        }
    }

private:
    [[nodiscard]] std::size_t max() const { return options_.max_line > 0 ? options_.max_line : 1; }
    void emit()
    {
        if (options_.on_line)
        {
            options_.on_line(stream_, line_);
        }
        line_.clear();
    }

    Stream stream_;
    const ProcessOptions& options_;
    std::string line_;
};

} // namespace

#ifdef _WIN32

using detail::Handle;

Process::Process(const std::filesystem::path& executable, const std::vector<std::string>& arguments, ProcessOptions options) :
  options_(std::move(options))
{
    Handle outRead;
    Handle outWrite;
    Handle errRead;
    Handle errWrite;
    detail::make_pipe(outRead, outWrite);
    detail::make_pipe(errRead, errWrite);
    Handle nul(detail::open_null_for_child());
    Handle job(detail::make_kill_on_close_job());
    detail::StartFlags flags;
    flags.working_directory = options_.working_directory;
    flags.new_group = true;
    flags.only_std_handles = true;
    Handle process(detail::start(executable, arguments, nul.h, outWrite.h, errWrite.h, job.h, flags));
    pid_ = GetProcessId(process.h);
    process_ = process.release();
    job_ = job.release();
    out_ = outRead.release();
    err_ = errRead.release();
    try
    {
        reader_ = std::thread([this] { read_output(); });
    }
    catch (...)
    {
        // The destructor does not run: end the process and close what it would have.
        kill_tree();
        for (void* handle : {process_, job_, out_, err_})
        {
            CloseHandle(handle);
        }
        throw;
    }
}

Process::~Process()
{
    kill_tree();
    finish_output(std::chrono::milliseconds(0));
    for (void* handle : {process_, job_})
    {
        if (handle != nullptr)
        {
            CloseHandle(handle);
        }
    }
}

void Process::read_output()
{
    LineSplitter out(Stream::Output, options_);
    LineSplitter err(Stream::Error, options_);
    HANDLE pipes[2] = {out_, err_};
    LineSplitter* splitters[2] = {&out, &err};
    char buffer[4096];
    while (pipes[0] != nullptr || pipes[1] != nullptr)
    {
        {
            const std::lock_guard lock(mutex_);
            if (stop_reading_)
            {
                break;
            }
        }
        bool readAny = false;
        for (int i = 0; i < 2; ++i)
        {
            if (pipes[i] == nullptr)
            {
                continue;
            }
            DWORD available = 0;
            if (!PeekNamedPipe(pipes[i], nullptr, 0, nullptr, &available, nullptr))
            {
                splitters[i]->finish(); // every writer has closed it
                CloseHandle(pipes[i]);
                pipes[i] = nullptr;
                continue;
            }
            if (available == 0)
            {
                continue;
            }
            DWORD got = 0;
            if (!ReadFile(pipes[i], buffer, available < sizeof(buffer) ? available : sizeof(buffer), &got, nullptr) || got == 0)
            {
                splitters[i]->finish();
                CloseHandle(pipes[i]);
                pipes[i] = nullptr;
                continue;
            }
            splitters[i]->append(buffer, got);
            readAny = true;
        }
        if (!readAny)
        {
            std::unique_lock lock(mutex_);
            closed_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stop_reading_; });
        }
    }
    for (int i = 0; i < 2; ++i)
    {
        if (pipes[i] != nullptr)
        {
            splitters[i]->finish();
            CloseHandle(pipes[i]);
        }
    }
    const std::lock_guard lock(mutex_);
    output_closed_ = true;
    closed_.notify_all();
}

bool Process::finish_output(std::chrono::milliseconds timeout)
{
    bool closed = false;
    {
        std::unique_lock lock(mutex_);
        closed = closed_.wait_for(lock, timeout, [this] { return output_closed_; });
        stop_reading_ = true;
        closed_.notify_all();
    }
    if (reader_.joinable())
    {
        reader_.join();
    }
    return closed;
}

std::optional<ExitStatus> Process::poll() { return wait_for(std::chrono::milliseconds(0)); }

std::optional<ExitStatus> Process::wait_for(std::chrono::milliseconds timeout)
{
    if (status_ || process_ == nullptr)
    {
        return status_;
    }
    if (WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0)
    {
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    status_ = ExitStatus{static_cast<int>(code), std::nullopt};
    return status_;
}

bool Process::request_stop() noexcept
{
    if (poll())
    {
        return false;
    }
    return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid_) != 0;
}

void Process::kill_tree() noexcept
{
    if (job_ != nullptr)
    {
        TerminateJobObject(job_, 1);
    }
    if (!poll() && process_ != nullptr)
    {
        TerminateProcess(process_, 1);
        static_cast<void>(wait_for(std::chrono::milliseconds(5000)));
    }
}

bool Process::others_running()
{
    if (!poll() || job_ == nullptr)
    {
        return false;
    }
    // The IDs of the processes in the job, without the first one: the count of the job may still
    // hold the first process for a moment after its end.
    struct
    {
        JOBOBJECT_BASIC_PROCESS_ID_LIST list;
        ULONG_PTR more[63];
    } ids{};
    if (!QueryInformationJobObject(job_, JobObjectBasicProcessIdList, &ids, sizeof(ids), nullptr) &&
        GetLastError() != ERROR_MORE_DATA)
    {
        return false;
    }
    const ULONG_PTR* id = ids.list.ProcessIdList;
    for (DWORD i = 0; i < ids.list.NumberOfProcessIdsInList; ++i)
    {
        if (id[i] != static_cast<ULONG_PTR>(pid_))
        {
            return true;
        }
    }
    // More than fit here: some are others.
    return ids.list.NumberOfAssignedProcesses > ids.list.NumberOfProcessIdsInList;
}

#else // POSIX

Process::Process(const std::filesystem::path& executable, const std::vector<std::string>& arguments, ProcessOptions options) :
  options_(std::move(options))
{
    int outWrite = -1;
    int errWrite = -1;
    const auto close_all = [&] {
        for (int* fd : {&out_, &err_, &outWrite, &errWrite, &wake_read_, &wake_write_})
        {
            if (*fd >= 0)
            {
                ::close(*fd);
                *fd = -1;
            }
        }
    };
    try
    {
        detail::make_pipe(out_, outWrite);
        detail::make_pipe(err_, errWrite);
        detail::make_pipe(wake_read_, wake_write_);
        const int null = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null < 0)
        {
            throw StartError(std::string("cannot open /dev/null: ") + std::strerror(errno), StartError::Cause::System);
        }
        detail::SpawnRequest request;
        request.in = null;
        request.out = outWrite;
        request.err = errWrite;
        request.working_directory = options_.working_directory;
        request.own_group = true;
        request.end_with_caller = true;
        try
        {
            pid_ = detail::spawn(executable, arguments, request);
        }
        catch (...)
        {
            ::close(null);
            throw;
        }
        ::close(null);
    }
    catch (...)
    {
        close_all();
        throw;
    }
    // Only the child holds the write ends now: the pipes close when it (and what it started)
    // has ended.
    ::close(outWrite);
    ::close(errWrite);
    try
    {
        reader_ = std::thread([this] { read_output(); });
    }
    catch (...)
    {
        // The destructor does not run: end the process and close what it would have.
        kill_tree();
        outWrite = -1;
        errWrite = -1;
        close_all();
        throw;
    }
}

Process::~Process()
{
    kill_tree();
    finish_output(std::chrono::milliseconds(0));
    for (int fd : {wake_read_, wake_write_})
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
}

void Process::read_output()
{
    LineSplitter out(Stream::Output, options_);
    LineSplitter err(Stream::Error, options_);
    int fds[2] = {out_, err_};
    LineSplitter* splitters[2] = {&out, &err};
    char buffer[4096];
    bool woken = false;
    while (!woken && (fds[0] >= 0 || fds[1] >= 0))
    {
        pollfd waits[3] = {{fds[0], POLLIN, 0}, {fds[1], POLLIN, 0}, {wake_read_, POLLIN, 0}};
        if (::poll(waits, 3, -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        for (int i = 0; i < 2; ++i)
        {
            if (fds[i] < 0 || waits[i].revents == 0)
            {
                continue;
            }
            const ssize_t got = ::read(fds[i], buffer, sizeof(buffer));
            if (got > 0)
            {
                splitters[i]->append(buffer, static_cast<std::size_t>(got));
            }
            else if (got == 0 || (errno != EINTR && errno != EAGAIN))
            {
                splitters[i]->finish();
                ::close(fds[i]);
                fds[i] = -1;
            }
        }
        woken = waits[2].revents != 0;
    }
    for (int i = 0; i < 2; ++i)
    {
        if (fds[i] >= 0)
        {
            splitters[i]->finish();
            ::close(fds[i]);
        }
    }
    const std::lock_guard lock(mutex_);
    output_closed_ = true;
    closed_.notify_all();
}

bool Process::finish_output(std::chrono::milliseconds timeout)
{
    bool closed = false;
    {
        std::unique_lock lock(mutex_);
        closed = closed_.wait_for(lock, timeout, [this] { return output_closed_; });
    }
    if (reader_.joinable())
    {
        if (!closed)
        {
            const char wake = 1;
            ssize_t ignored = ::write(wake_write_, &wake, 1);
            static_cast<void>(ignored);
        }
        reader_.join();
    }
    return closed;
}

std::optional<ExitStatus> Process::poll()
{
    if (status_ || pid_ <= 0)
    {
        return status_;
    }
    int status = 0;
    pid_t waited = 0;
    while ((waited = ::waitpid(pid_, &status, WNOHANG)) < 0 && errno == EINTR)
    {
    }
    if (waited == pid_)
    {
        status_ = detail::exit_status_of(status);
    }
    else if (waited < 0)
    {
        status_ = ExitStatus{-1, std::nullopt}; // not our child any more (cannot happen here)
    }
    return status_;
}

std::optional<ExitStatus> Process::wait_for(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        if (auto status = poll())
        {
            return status;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool Process::request_stop() noexcept
{
    if (poll())
    {
        return false;
    }
    return ::kill(-pid_, SIGTERM) == 0;
}

void Process::kill_tree() noexcept
{
    if (pid_ <= 0)
    {
        return;
    }
    // The group outlives its first process while others are in it (and its ID is not reused
    // while it is in use). Once it was seen empty, its ID may belong to another group. Limit: if it
    // emptied after the first process was reaped and its number was reused before this call, the
    // signal reaches that other group (not guarded: it needs the whole range of process IDs to
    // wrap around in between).
    if (!group_gone_ && ::kill(-pid_, SIGKILL) != 0 && errno == ESRCH)
    {
        group_gone_ = true;
    }
    if (!poll())
    {
        ::kill(pid_, SIGKILL);
        int status = 0;
        pid_t waited = 0;
        while ((waited = ::waitpid(pid_, &status, 0)) < 0 && errno == EINTR)
        {
        }
        status_ = waited == pid_ ? detail::exit_status_of(status) : std::optional<ExitStatus>(ExitStatus{-1, std::nullopt});
    }
}

bool Process::others_running()
{
    // Before the first process is reaped, it is a member of its group itself.
    if (!poll() || group_gone_)
    {
        return false;
    }
    if (::kill(-pid_, 0) == 0)
    {
        return true;
    }
    group_gone_ = errno == ESRCH;
    return false;
}

#endif

} // namespace vehicle_sil::process
