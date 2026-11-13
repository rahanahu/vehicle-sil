// SPDX-License-Identifier: MIT
#include "spawn.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string_view>
#include <system_error>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

extern char** environ;
#endif

namespace vehicle_sil::process {

std::string signal_name(int signal)
{
#ifndef _WIN32
    switch (signal)
    {
    case SIGHUP: return "SIGHUP";
    case SIGINT: return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGILL: return "SIGILL";
    case SIGTRAP: return "SIGTRAP";
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGKILL: return "SIGKILL";
    case SIGUSR1: return "SIGUSR1";
    case SIGSEGV: return "SIGSEGV";
    case SIGUSR2: return "SIGUSR2";
    case SIGPIPE: return "SIGPIPE";
    case SIGALRM: return "SIGALRM";
    case SIGTERM: return "SIGTERM";
    default: break;
    }
#endif
    return "signal " + std::to_string(signal);
}

} // namespace vehicle_sil::process

namespace vehicle_sil::process::detail {

#ifdef _WIN32

namespace {

// Quotes one argument following the rules of CommandLineToArgvW / the MSVC and MinGW CRTs.
std::wstring quote_argument(const std::wstring& argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        return argument;
    }
    std::wstring quoted = L"\"";
    for (auto it = argument.begin();; ++it)
    {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\')
        {
            ++it;
            ++backslashes;
        }
        if (it == argument.end())
        {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
        }
        else
        {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(*it);
    }
    quoted.push_back(L'"');
    return quoted;
}

std::wstring widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring command_line(const std::filesystem::path& executable, const std::vector<std::string>& arguments)
{
    std::wstring line = quote_argument(executable.wstring());
    for (const auto& argument : arguments)
    {
        line += L' ';
        line += quote_argument(widen(argument));
    }
    return line;
}

} // namespace

std::string last_error_message(const char* what)
{
    const DWORD code = GetLastError();
    std::string reason;
    switch (code)
    {
    case ERROR_FILE_NOT_FOUND:
        reason = " (the file does not exist)";
        break;
    case ERROR_PATH_NOT_FOUND:
        reason = " (the directory does not exist)";
        break;
    case ERROR_ACCESS_DENIED:
        reason = " (access denied)";
        break;
    case ERROR_BAD_EXE_FORMAT:
        reason = " (not a Windows executable)";
        break;
    case ERROR_DIRECTORY:
        reason = " (the working directory is not a directory)";
        break;
    default:
        break;
    }
    return std::string(what) + " failed with Win32 error " + std::to_string(code) + reason;
}

namespace {
const char* const kJobHint = " (vehicle-sil runs each child in a job object, to stop it with what it starts; "
                             "this Windows does not allow that here)";
} // namespace

HANDLE make_kill_on_close_job()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr)
    {
        throw StartError("cannot make the job object for a child: " + last_error_message("CreateJobObjectW") + kJobHint,
                         StartError::Cause::System);
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        // The message before CloseHandle() changes the last error.
        const std::string error = last_error_message("SetInformationJobObject");
        CloseHandle(job);
        throw StartError("cannot make the job object for a child: " + error + kJobHint, StartError::Cause::System);
    }
    return job;
}

/// The caller's environment block with `variables` set over it (names compared without case, as
/// Windows does), for CREATE_UNICODE_ENVIRONMENT.
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& variables)
{
    std::vector<std::wstring> names;
    for (const auto& variable : variables)
    {
        names.push_back(widen(variable.first));
    }
    const auto is_set_here = [&names](std::wstring_view entry) {
        // "=C:=C:\..." and the like: a name that starts with '='.
        const auto equals = entry.find(L'=', 1);
        const std::wstring_view name = entry.substr(0, equals);
        return std::any_of(names.begin(), names.end(), [name](const std::wstring& set) {
            return CompareStringOrdinal(set.data(), static_cast<int>(set.size()), name.data(), static_cast<int>(name.size()),
                                        TRUE) == CSTR_EQUAL;
        });
    };
    std::wstring block;
    if (wchar_t* strings = GetEnvironmentStringsW(); strings != nullptr)
    {
        for (const wchar_t* entry = strings; *entry != L'\0'; entry += std::wcslen(entry) + 1)
        {
            if (!is_set_here(entry))
            {
                block.append(entry);
                block.push_back(L'\0');
            }
        }
        FreeEnvironmentStringsW(strings);
    }
    for (std::size_t i = 0; i < variables.size(); ++i)
    {
        block += names[i] + L"=" + widen(variables[i].second);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

HANDLE start(const std::filesystem::path& executable, const std::vector<std::string>& arguments, HANDLE in, HANDLE out,
             HANDLE err, HANDLE job, const StartFlags& flags)
{
    std::wstring line = command_line(executable, arguments);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = in;
    startup.StartupInfo.hStdOutput = out;
    startup.StartupInfo.hStdError = err;
    PROCESS_INFORMATION info{};
    const std::string what = "cannot start " + shown_path(executable) + ": ";
    const std::string jobHint = kJobHint;
    if (job == nullptr)
    {
        throw StartError(what + "no job object" + jobHint, StartError::Cause::System);
    }
    DWORD creation = CREATE_SUSPENDED | (flags.new_group ? CREATE_NEW_PROCESS_GROUP : 0) |
                     // Without a console of this process (a service, some CI), a console
                     // program would open a window of its own: every child of every caller.
                     (GetConsoleCP() == 0 ? CREATE_NO_WINDOW : 0);
    // Only the three standard handles are inherited: another child started at the same time
    // must not keep this child's pipes open.
    std::vector<std::uint64_t> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
    HANDLE inherited[3] = {in, out, err};
    if (flags.only_std_handles)
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage.resize(size / sizeof(std::uint64_t) + 1);
        attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
        {
            throw StartError(what + last_error_message("InitializeProcThreadAttributeList"), StartError::Cause::System);
        }
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                       nullptr, nullptr))
        {
            const std::string error = last_error_message("UpdateProcThreadAttribute");
            DeleteProcThreadAttributeList(attributes);
            throw StartError(what + error, StartError::Cause::System);
        }
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        creation |= EXTENDED_STARTUPINFO_PRESENT;
    }
    const std::wstring directory = flags.working_directory.wstring();
    // A relative path is from the caller's directory, also when the child starts in another one.
    std::error_code ec;
    const std::filesystem::path program = executable.is_relative() ? std::filesystem::absolute(executable, ec) : executable;
    std::wstring environment;
    if (!flags.environment.empty())
    {
        environment = environment_block(flags.environment);
        creation |= CREATE_UNICODE_ENVIRONMENT;
    }
    const BOOL created = CreateProcessW(program.wstring().c_str(), line.data(), nullptr, nullptr, TRUE, creation,
                                        environment.empty() ? nullptr : environment.data(),
                                        directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &info);
    const std::string createError = created ? std::string{} : last_error_message("CreateProcessW");
    if (attributes != nullptr)
    {
        DeleteProcThreadAttributeList(attributes);
    }
    if (!created)
    {
        throw StartError(what + createError, StartError::Cause::Executable);
    }
    const auto end_suspended = [&info] {
        TerminateProcess(info.hProcess, 1);
        WaitForSingleObject(info.hProcess, 5000);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    };
    // Still suspended: a process outside the job could not be stopped with the processes it
    // starts, so it does not run at all.
    if (!AssignProcessToJobObject(job, info.hProcess))
    {
        const std::string error = last_error_message("AssignProcessToJobObject");
        end_suspended();
        throw StartError(what + error + jobHint, StartError::Cause::System);
    }
    // A thread that does not resume would leave the process waiting for ever.
    if (ResumeThread(info.hThread) == static_cast<DWORD>(-1))
    {
        const std::string error = last_error_message("ResumeThread");
        end_suspended();
        throw StartError(what + error, StartError::Cause::System);
    }
    CloseHandle(info.hThread);
    return info.hProcess;
}

void make_pipe(Handle& read, Handle& write)
{
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE r = nullptr;
    HANDLE w = nullptr;
    if (!CreatePipe(&r, &w, &inherit, 0))
    {
        throw StartError(last_error_message("CreatePipe"), StartError::Cause::System);
    }
    read.h = r;
    write.h = w;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
}

HANDLE open_null_for_child()
{
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE)
    {
        throw StartError(last_error_message("opening NUL"), StartError::Cause::System);
    }
    return nul;
}

#else // POSIX

namespace {

/// What the child reports through the error pipe before it execs.
struct ChildError
{
    int stage = 0; ///< 1: chdir, 2: exec, 3: the descriptors
    int error = 0; ///< errno
};

/// A descriptor at 3 or above (the child's 0 to 2 are taken), closed on exec.
int above_std(int fd)
{
    if (fd >= 3)
    {
        return fd;
    }
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    ::close(fd);
    return moved;
}

[[noreturn]] void child_fail(int fd, int stage)
{
    const ChildError report{stage, errno};
    ssize_t ignored = ::write(fd, &report, sizeof(report));
    static_cast<void>(ignored);
    ::_exit(127);
}

} // namespace

void make_pipe(int& read, int& write)
{
    int fds[2] = {-1, -1};
#ifdef __linux__
    const int rc = ::pipe2(fds, O_CLOEXEC);
#else
    const int rc = ::pipe(fds);
    if (rc == 0)
    {
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }
#endif
    if (rc != 0)
    {
        throw StartError(std::string("pipe() failed: ") + std::strerror(errno), StartError::Cause::System);
    }
    read = above_std(fds[0]);
    write = above_std(fds[1]);
    if (read < 0 || write < 0)
    {
        throw StartError(std::string("fcntl(F_DUPFD_CLOEXEC) failed: ") + std::strerror(errno), StartError::Cause::System);
    }
}

std::optional<ExitStatus> exit_status_of(int status)
{
    if (WIFEXITED(status))
    {
        return ExitStatus{WEXITSTATUS(status), std::nullopt};
    }
    if (WIFSIGNALED(status))
    {
        return ExitStatus{128 + WTERMSIG(status), WTERMSIG(status)};
    }
    return std::nullopt;
}

pid_t spawn(const std::filesystem::path& executable, const std::vector<std::string>& arguments, const SpawnRequest& request)
{
    // Everything the child needs is made here: between fork() and exec() a child of a process
    // with threads may only make async-signal-safe calls. A relative path is from the caller's
    // directory, also when the child starts in another one.
    std::error_code ec;
    const std::string program = (executable.is_relative() ? std::filesystem::absolute(executable, ec) : executable).string();
    const std::string directory = request.working_directory.string();
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& argument : arguments)
    {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    // The environment: the caller's, with request.environment set over it.
    std::vector<std::string> variables;
    std::vector<char*> envp;
    if (!request.environment.empty())
    {
        for (char** entry = environ; *entry != nullptr; ++entry)
        {
            const std::string_view text(*entry);
            const std::string_view name = text.substr(0, text.find('='));
            if (std::none_of(request.environment.begin(), request.environment.end(),
                             [name](const auto& variable) { return variable.first == name; }))
            {
                envp.push_back(*entry);
            }
        }
        for (const auto& [name, value] : request.environment)
        {
            variables.push_back(name + "=" + value);
        }
        for (auto& variable : variables)
        {
            envp.push_back(variable.data());
        }
        envp.push_back(nullptr);
    }
    char** const childEnvironment = envp.empty() ? environ : envp.data();
    std::vector<int> ignored;
    for (int sig = 1; sig < NSIG; ++sig)
    {
        struct sigaction current{};
        if (sig != SIGKILL && sig != SIGSTOP && ::sigaction(sig, nullptr, &current) == 0 && current.sa_handler == SIG_IGN)
        {
            ignored.push_back(sig);
        }
    }
    rlimit files{};
    const int maxFd = ::getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur != RLIM_INFINITY && files.rlim_cur < 65536
                          ? static_cast<int>(files.rlim_cur)
                          : 65536;
    int errorRead = -1;
    int errorWrite = -1;
    make_pipe(errorRead, errorWrite);
    const pid_t parent = ::getpid();

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        const int error = errno;
        ::close(errorRead);
        ::close(errorWrite);
        throw StartError("cannot start " + program + ": fork() failed: " + std::strerror(error), StartError::Cause::System);
    }
    if (pid == 0)
    {
        if (request.own_group)
        {
            ::setpgid(0, 0);
        }
#ifdef __linux__
        if (request.end_with_caller)
        {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() != parent)
            {
                ::_exit(127); // the caller is gone already
            }
        }
#else
        static_cast<void>(parent);
#endif
        struct sigaction byDefault{};
        byDefault.sa_handler = SIG_DFL;
        for (const int sig : ignored)
        {
            ::sigaction(sig, &byDefault, nullptr);
        }
        sigset_t none;
        ::sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        // The standard descriptors: first copies above 2, so that none is overwritten before it
        // is copied.
        const int in = ::fcntl(request.in, F_DUPFD, 3);
        const int out = ::fcntl(request.out, F_DUPFD, 3);
        const int err = ::fcntl(request.err, F_DUPFD, 3);
        if (in < 0 || out < 0 || err < 0 || ::dup2(in, 0) < 0 || ::dup2(out, 1) < 0 || ::dup2(err, 2) < 0)
        {
            child_fail(errorWrite, 3);
        }
        // The error pipe at 3 (closed on exec), everything above it closed.
        if (errorWrite != 3)
        {
            if (::dup2(errorWrite, 3) < 0)
            {
                child_fail(errorWrite, 3);
            }
            errorWrite = 3;
        }
        ::fcntl(3, F_SETFD, FD_CLOEXEC);
#if defined(__linux__) && defined(SYS_close_range)
        if (::syscall(SYS_close_range, 4U, ~0U, 0U) != 0)
#endif
        {
            for (int fd = 4; fd < maxFd; ++fd)
            {
                ::close(fd);
            }
        }
        if (!directory.empty() && ::chdir(directory.c_str()) != 0)
        {
            child_fail(errorWrite, 1);
        }
        ::execve(program.c_str(), argv.data(), childEnvironment);
        child_fail(errorWrite, 2);
    }
    if (request.own_group)
    {
        ::setpgid(pid, pid); // also here, so that the group exists when spawn() returns
    }
    ::close(errorWrite);
    ChildError report{};
    std::size_t got = 0;
    while (got < sizeof(report))
    {
        const ssize_t n = ::read(errorRead, reinterpret_cast<char*>(&report) + got, sizeof(report) - got);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    ::close(errorRead);
    if (got == 0)
    {
        return pid; // exec closed the pipe
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    const std::string reason = got < sizeof(report) ? std::string("the child ended before it could start the program")
                               : report.stage == 1  ? "cannot change to the directory " + directory + ": " + std::strerror(report.error)
                               : report.stage == 2  ? std::string(std::strerror(report.error))
                                                    : std::string("cannot set up its standard streams: ") + std::strerror(report.error);
    throw StartError("cannot start " + program + ": " + reason,
                     report.stage == 3 ? StartError::Cause::System : StartError::Cause::Executable);
}

#endif

} // namespace vehicle_sil::process::detail
