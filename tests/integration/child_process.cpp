// SPDX-License-Identifier: MIT
#include "child_process.hpp"

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
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace vehicle_sil::test {

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

std::string last_error_message(const char* what)
{
    return std::string(what) + " failed with Win32 error " + std::to_string(GetLastError());
}

} // namespace

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

    std::wstring commandLine = quote_argument(executable.wstring());
    for (const auto& argument : arguments)
    {
        commandLine += L' ';
        commandLine += quote_argument(widen(argument));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log;
    startup.hStdError = log;

    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_ != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED, nullptr, nullptr, &startup, &info);
    const std::string error = created ? std::string{} : last_error_message("CreateProcessW");
    CloseHandle(log);
    if (!created)
    {
        if (job_ != nullptr)
        {
            CloseHandle(job_);
            job_ = nullptr;
        }
        throw std::runtime_error(error + " for " + executable.string());
    }
    if (job_ != nullptr)
    {
        AssignProcessToJobObject(job_, info.hProcess);
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    process_ = info.hProcess;
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
        throw std::runtime_error("posix_spawn(" + program + ") failed: " + std::strerror(rc));
    }
    pid_ = pid;
}

ChildProcess::~ChildProcess()
{
    kill();
}

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
            if (WIFEXITED(status))
            {
                exit_code_ = WEXITSTATUS(status);
            }
            else if (WIFSIGNALED(status))
            {
                exit_code_ = 128 + WTERMSIG(status);
            }
            else
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

#endif

} // namespace vehicle_sil::test
