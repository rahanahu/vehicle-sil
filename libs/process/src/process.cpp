// SPDX-License-Identifier: MIT
#include "vehicle_sil/process/process.hpp"

#include "spawn.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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
    Handle in;      // the child's end: the null device or the read end of the input pipe
    Handle inWrite; // ours
    if (options_.input)
    {
        Handle pipeRead;
        Handle pipeWrite;
        detail::make_pipe(pipeRead, pipeWrite);
        // The other way round than make_pipe(): the child inherits the read end.
        SetHandleInformation(pipeRead.h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        SetHandleInformation(pipeWrite.h, HANDLE_FLAG_INHERIT, 0);
        in.h = pipeRead.release();
        inWrite.h = pipeWrite.release();
    }
    else
    {
        in.h = detail::open_null_for_child();
    }
    Handle job(detail::make_kill_on_close_job());
    detail::StartFlags flags;
    flags.working_directory = options_.working_directory;
    flags.new_group = true;
    flags.only_std_handles = true;
    flags.environment = options_.environment;
    Handle process(detail::start(executable, arguments, in.h, outWrite.h, errWrite.h, job.h, flags));
    pid_ = GetProcessId(process.h);
    process_ = process.release();
    job_ = job.release();
    out_ = outRead.release();
    err_ = errRead.release();
    in_ = inWrite.release();
    try
    {
        reader_ = std::thread([this] { read_output(); });
    }
    catch (...)
    {
        // The destructor does not run: end the process and close what it would have.
        kill_tree();
        for (void* handle : {process_, job_, out_, err_, in_})
        {
            if (handle != nullptr)
            {
                CloseHandle(handle);
            }
        }
        throw;
    }
}

Process::~Process()
{
    kill_tree();
    finish_output(std::chrono::milliseconds(0));
    close_input();
    for (void* handle : {process_, job_})
    {
        if (handle != nullptr)
        {
            CloseHandle(handle);
        }
    }
}

bool Process::write_input(std::string_view bytes)
{
    if (in_ == nullptr)
    {
        return false;
    }
    while (!bytes.empty())
    {
        DWORD written = 0;
        if (!WriteFile(in_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
        {
            return false;
        }
        bytes.remove_prefix(written);
    }
    return true;
}

void Process::close_input() noexcept
{
    if (in_ != nullptr)
    {
        CloseHandle(in_);
        in_ = nullptr;
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

namespace {

/// `<system32>\conhost.exe`, or empty if Windows does not report its system directory.
std::wstring system_conhost_path()
{
    std::wstring directory(MAX_PATH, L'\0');
    for (;;)
    {
        const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        if (length == 0)
        {
            return {};
        }
        if (length < directory.size())
        {
            directory.resize(length);
            if (!directory.empty() && directory.back() != L'\\')
            {
                directory.push_back(L'\\');
            }
            directory.append(L"conhost.exe");
            return directory;
        }
        if (length > 32768 || directory.size() >= 32768)
        {
            return {};
        }
        // GetSystemDirectoryW returns the required length including the terminating null.
        directory.resize(length > directory.size() ? static_cast<std::size_t>(length) : directory.size() * 2);
    }
}

struct ConhostPaths
{
    std::wstring win32;
    std::wstring native;
};

/// Converts a Win32 drive-letter path (`C:\...`) to its NT native device path
/// (`\Device\HarddiskVolumeX\...`), or empty if QueryDosDeviceW() cannot resolve it.
std::wstring native_device_path(std::wstring_view win32Path)
{
    if (win32Path.size() < 3 || win32Path[1] != L':' || win32Path[2] != L'\\')
    {
        return {};
    }
    const wchar_t drive[3] = {win32Path[0], L':', L'\0'};
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = QueryDosDeviceW(drive, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length > 0)
        {
            const std::size_t firstEnd = buffer.find(L'\0');
            if (firstEnd == std::wstring::npos || firstEnd == 0)
            {
                return {};
            }
            std::wstring device = buffer.substr(0, firstEnd);
            if (device.back() == L'\\')
            {
                device.pop_back();
            }
            device.append(win32Path.substr(2));
            return device;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 32768)
        {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

/// `<system32>\conhost.exe` in both Win32 and NT native device forms.
ConhostPaths system_conhost_paths()
{
    ConhostPaths paths;
    paths.win32 = system_conhost_path();
    if (!paths.win32.empty())
    {
        paths.native = native_device_path(paths.win32);
    }
    return paths;
}

/// Full image path of `handle` (Win32 when `flags == 0`, NT native when `flags == PROCESS_NAME_NATIVE`),
/// or empty if it cannot be queried.
std::wstring process_image_path(HANDLE handle, DWORD flags)
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(handle, flags, path.data(), &size) != 0)
        {
            path.resize(size);
            return path;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || path.size() >= 32768)
        {
            return {};
        }
        path.resize(path.size() * 2);
    }
}

bool equal_ignore_case(std::wstring_view a, std::wstring_view b)
{
    return !a.empty() && !b.empty() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
               CSTR_EQUAL;
}

/// True if `handle` is Windows's own `<system32>\conhost.exe`. Falls back to the NT native path
/// when Win32 path lookup fails (e.g. ERROR_ACCESS_DENIED while the process is tearing down).
bool is_system_conhost(HANDLE handle, const ConhostPaths& expected)
{
    if (!expected.win32.empty())
    {
        const std::wstring win32 = process_image_path(handle, 0);
        if (!win32.empty())
        {
            return equal_ignore_case(win32, expected.win32);
        }
    }
    if (!expected.native.empty())
    {
        const std::wstring native = process_image_path(handle, PROCESS_NAME_NATIVE);
        if (!native.empty())
        {
            return equal_ignore_case(native, expected.native);
        }
    }
    return false;
}

/// True if `handle` has already exited. Combines WaitForSingleObject() (handles exit code 259 /
/// STILL_ACTIVE) with GetExitCodeProcess() (Windows updates ExitStatus before signaling EPROCESS
/// during teardown).
bool process_has_exited(HANDLE handle, bool hasSync)
{
    if (hasSync && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0)
    {
        return true;
    }
    DWORD exitCode = 0;
    return GetExitCodeProcess(handle, &exitCode) != 0 && exitCode != STILL_ACTIVE;
}

} // namespace

bool Process::others_running()
{
    if (!poll() || job_ == nullptr)
    {
        return false;
    }
    // The IDs of the processes in the job, without the first one: the count of the job may still
    // hold the first process for a moment after its end, and QueryInformationJobObject() can
    // succeed (TRUE) with NumberOfAssignedProcesses > NumberOfProcessIdsInList while a terminated
    // process is still draining from the job. Only ERROR_MORE_DATA means the buffer was too small.
    static_assert(alignof(ULONG_PTR) >= alignof(JOBOBJECT_BASIC_PROCESS_ID_LIST));
    static_assert(offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList) % sizeof(ULONG_PTR) == 0);
    static_assert(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) ==
                  offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList) + sizeof(ULONG_PTR));
    constexpr std::size_t kHeaderSlots = offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList) / sizeof(ULONG_PTR);
    constexpr std::size_t kInitialCapacity = 64;
    constexpr std::size_t kMaxCapacity = 4096;
    constexpr int kMaxRetries = 3;

    std::size_t capacity = kInitialCapacity;
    std::vector<ULONG_PTR> storage;
    JOBOBJECT_BASIC_PROCESS_ID_LIST header{};
    for (int attempt = 0;; ++attempt)
    {
        storage.assign(kHeaderSlots + capacity, 0);
        const DWORD bufferBytes = static_cast<DWORD>(storage.size() * sizeof(ULONG_PTR));
        const BOOL ok = QueryInformationJobObject(job_, JobObjectBasicProcessIdList, storage.data(), bufferBytes, nullptr);
        if (ok != 0)
        {
            std::memcpy(&header, storage.data(), sizeof(header));
            break;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_MORE_DATA)
        {
            return false;
        }
        std::memcpy(&header, storage.data(), sizeof(header));
        if (attempt >= kMaxRetries || capacity >= kMaxCapacity)
        {
            // More processes than our cap can enumerate: others are running.
            return true;
        }
        const std::size_t assigned = static_cast<std::size_t>(header.NumberOfAssignedProcesses);
        const std::size_t needed = assigned < kMaxCapacity ? assigned + 8 : kMaxCapacity;
        capacity = std::min(std::max(capacity * 2, needed), kMaxCapacity);
    }

    ConhostPaths conhostPaths;
    bool conhostPathsChecked = false;
    const ULONG_PTR* ids = storage.data() + kHeaderSlots;
    const std::size_t count = std::min<std::size_t>(header.NumberOfProcessIdsInList, capacity);
    for (std::size_t i = 0; i < count; ++i)
    {
        if (ids[i] == static_cast<ULONG_PTR>(pid_))
        {
            continue;
        }
        bool hasSync = true;
        Handle other(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(ids[i])));
        if (other.h == nullptr)
        {
            DWORD openErr = GetLastError();
            if (openErr == ERROR_ACCESS_DENIED)
            {
                hasSync = false;
                HANDLE fallback = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(ids[i]));
                if (fallback != nullptr && fallback != INVALID_HANDLE_VALUE)
                {
                    other.h = fallback;
                }
                else
                {
                    openErr = GetLastError();
                }
            }
            if (other.h == nullptr)
            {
                if (openErr == ERROR_INVALID_PARAMETER)
                {
                    // Already exited before OpenProcess().
                    continue;
                }
                // Cannot inspect (e.g. access denied): conservatively report it as still running.
                return true;
            }
        }
        // Guard against PID reuse between QueryInformationJobObject() and OpenProcess().
        BOOL inJob = FALSE;
        if (IsProcessInJob(other.h, job_, &inJob) != 0 && inJob == FALSE)
        {
            continue;
        }
        if (process_has_exited(other.h, hasSync))
        {
            continue;
        }
        if (!conhostPathsChecked)
        {
            conhostPaths = system_conhost_paths();
            conhostPathsChecked = true;
        }
        // Headless runs start console programs with CREATE_NO_WINDOW inside this job, causing
        // Windows to spawn <system32>\conhost.exe in the same job. It may outlive the participant
        // briefly during teardown; it is not a child started by the participant.
        if (is_system_conhost(other.h, conhostPaths))
        {
            continue;
        }
        // A process (including conhost.exe) that exits between the check above and
        // QueryFullProcessImageNameW() can fail image-path lookup; re-check whether it has exited
        // before reporting it as still running.
        if (process_has_exited(other.h, hasSync))
        {
            continue;
        }
        return true;
    }
    return false;
}

#else // POSIX

Process::Process(const std::filesystem::path& executable, const std::vector<std::string>& arguments, ProcessOptions options) :
  options_(std::move(options))
{
    int outWrite = -1;
    int errWrite = -1;
    const auto close_all = [&] {
        for (int* fd : {&out_, &err_, &outWrite, &errWrite, &wake_read_, &wake_write_, &in_, &pidfd_})
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
        int null = -1; // the child's standard input: the null device or the read end of the input pipe
        if (options_.input)
        {
            detail::make_pipe(null, in_);
        }
        else
        {
            null = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        }
        if (null < 0)
        {
            throw StartError(std::string("cannot open /dev/null: ") + std::strerror(errno), StartError::Cause::System);
        }
        detail::SpawnRequest request;
        request.in = null;
        request.out = outWrite;
        request.err = errWrite;
        request.working_directory = options_.working_directory;
        request.environment = options_.environment;
        request.own_group = true;
        request.end_with_caller = true;
        try
        {
            pid_ = detail::spawn(executable, arguments, request);
            pidfd_ = detail::open_pidfd(pid_);
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
    close_input();
    close_pidfd();
    for (int fd : {wake_read_, wake_write_})
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
}

void Process::close_pidfd() noexcept
{
    if (pidfd_ >= 0)
    {
        ::close(pidfd_);
        pidfd_ = -1;
    }
}

bool Process::write_input(std::string_view bytes)
{
    if (in_ < 0)
    {
        return false;
    }
    while (!bytes.empty())
    {
        const ssize_t written = ::write(in_, bytes.data(), bytes.size());
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

void Process::close_input() noexcept
{
    if (in_ >= 0)
    {
        ::close(in_);
        in_ = -1;
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
        if (status_)
        {
            close_pidfd();
        }
    }
    else if (waited < 0)
    {
        status_ = ExitStatus{-1, std::nullopt}; // not our child any more (cannot happen here)
        close_pidfd();
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
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return std::nullopt;
        }
        if (pidfd_ >= 0)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const int waitMs = static_cast<int>(
                std::clamp<std::chrono::milliseconds::rep>(left.count(), 1, std::numeric_limits<int>::max()));
            pollfd pfd{pidfd_, POLLIN, 0};
            const int rc = ::poll(&pfd, 1, waitMs);
            if (rc < 0 && errno != EINTR)
            {
                close_pidfd();
            }
            continue;
        }
        std::this_thread::sleep_for(
            std::min<std::chrono::steady_clock::duration>(std::chrono::milliseconds(5), deadline - now));
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
        close_pidfd();
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
