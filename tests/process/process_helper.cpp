// SPDX-License-Identifier: MIT
//
// A child for the tests of vehicle_sil::process and vehicle_sil_launch. It does what its
// arguments say, in this order, and ignores the arguments it does not know (so that a launcher
// may add its own):
//   --out <text>         writes <text> to stdout
//   --err <text>         writes <text> to stderr
//   --out-file <path>    writes the content of <path> to stdout
//   --fill-out <n>       writes <n> bytes 'o' to stdout
//   --fill-err <n>       writes <n> bytes 'e' to stderr
//   --echo-args          writes each of its arguments on a line of its own to stdout
//   --read-stdin         reads stdin to its end and writes "stdin <bytes>" to stdout
//   --close-output       closes stdout and stderr while leaving the process alive
//   --sleep-ms <ms>      sleeps
//   --flood              writes to stdout without end
//   --abort              ends with std::abort() (POSIX: the signal SIGABRT; Linux: without a core file)
//   --pid-file <path>    writes its process ID to <path>
//   --pwd                writes its working directory (generic form, '/') to stdout
//   --env <name>         writes "<name>=<value>" of its environment to stdout, or "<name> is not set"
//   --on-stop <code>     from now on, a stop request (POSIX SIGTERM, Windows Ctrl+Break) ends it with <code>
//   --ignore-stop        from now on, ignores a stop request
//   --check-handle <n>   writes "open" if the descriptor (Windows: handle) <n> is open in it, else "closed"
//   --spawn-sleeper <path>  starts a copy of itself that writes its process ID to <path> and sleeps
//                        (10 minutes); it is not waited for
//   --run-detached <exe> <args...>  (Windows) runs the rest of the command line as a program
//                        without a console (DETACHED_PROCESS) with this helper's standard handles,
//                        waits for it and exits with its exit code; nothing after it is read
//   --exit <code>        the exit code (default 0)
// Before them: with VEHICLE_SIL_HELPER_MARK=<path> in its environment it appends a line to <path>, so
// that a test can tell whether a copy of it was started at all (also under another name).
// On Windows it reads its arguments as UTF-8 from the wide command line and writes "\n" as it is.
#include "spawn_sleeper.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

int g_stop_code = 0;

#ifdef _WIN32
BOOL WINAPI on_stop(DWORD event)
{
    if (event != CTRL_BREAK_EVENT && event != CTRL_C_EVENT)
    {
        return FALSE;
    }
    if (g_stop_code >= 0)
    {
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(g_stop_code));
    }
    return TRUE; // ignored
}
#else
extern "C" void on_stop(int)
{
    ::_exit(g_stop_code);
}
#endif

std::vector<std::string> arguments(int argc, char** argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> out;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 0; wide != nullptr && i < count; ++i)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
        if (size > 1)
        {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
        }
        out.push_back(std::move(text));
    }
    LocalFree(wide);
    return out;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

void append_helper_mark()
{
    constexpr char kLine[] = "started\n";
    constexpr std::size_t kLineSize = sizeof(kLine) - 1;
#ifdef _WIN32
    const DWORD needed = GetEnvironmentVariableW(L"VEHICLE_SIL_HELPER_MARK", nullptr, 0);
    if (needed <= 1)
    {
        return;
    }
    std::wstring path(needed, L'\0');
    const DWORD len = GetEnvironmentVariableW(L"VEHICLE_SIL_HELPER_MARK", path.data(), needed);
    if (len == 0 || len >= needed)
    {
        return;
    }
    path.resize(len);
    // FILE_APPEND_DATA without FILE_WRITE_DATA makes each WriteFile atomically append at EOF.
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        handle = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE)
        {
            break;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (handle == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0;
    static_cast<void>(WriteFile(handle, kLine, static_cast<DWORD>(kLineSize), &written, nullptr));
    CloseHandle(handle);
#else
    const char* mark = std::getenv("VEHICLE_SIL_HELPER_MARK");
    if (mark == nullptr || *mark == '\0')
    {
        return;
    }
    const int fd = ::open(mark, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        return;
    }
    const char* ptr = kLine;
    std::size_t left = kLineSize;
    while (left > 0)
    {
        const ssize_t n = ::write(fd, ptr, left);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        ptr += n;
        left -= static_cast<std::size_t>(n);
    }
    ::close(fd);
#endif
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> args = arguments(argc, argv);
    append_helper_mark();
    const std::size_t count = args.size();
    int code = 0;
    for (std::size_t i = 1; i < count; ++i)
    {
        const std::string& arg = args[i];
        const bool hasValue = i + 1 < count;
        if (arg == "--out" && hasValue)
        {
            std::fputs(args[++i].c_str(), stdout);
        }
        else if (arg == "--err" && hasValue)
        {
            std::fputs(args[++i].c_str(), stderr);
        }
        else if (arg == "--out-file" && hasValue)
        {
            std::ifstream in(args[++i], std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::fwrite(text.data(), 1, text.size(), stdout);
        }
        else if ((arg == "--fill-out" || arg == "--fill-err") && hasValue)
        {
            const std::string text(std::stoul(args[++i]), arg == "--fill-out" ? 'o' : 'e');
            std::fwrite(text.data(), 1, text.size(), arg == "--fill-out" ? stdout : stderr);
        }
        else if (arg == "--echo-args")
        {
            for (std::size_t j = 1; j < count; ++j)
            {
                std::printf("%s\n", args[j].c_str());
            }
        }
        else if (arg == "--read-stdin")
        {
            std::size_t bytes = 0;
            while (std::fgetc(stdin) != EOF)
            {
                ++bytes;
            }
            std::printf("stdin %zu\n", bytes);
        }
        else if (arg == "--close-output")
        {
            std::fclose(stdout);
            std::fclose(stderr);
        }
        else if (arg == "--sleep-ms" && hasValue)
        {
            std::fflush(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(std::stol(args[++i])));
        }
        else if (arg == "--flood")
        {
            const std::string block(65536, 'f');
            while (std::fwrite(block.data(), 1, block.size(), stdout) == block.size())
            {
            }
        }
        else if (arg == "--abort")
        {
            std::fflush(stdout);
#ifdef __linux__
            // An abort on purpose: no core file (and no crash report on the desktop).
            prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
            std::abort();
        }
        else if (arg == "--pid-file" && hasValue)
        {
            std::ofstream out(args[++i], std::ios::binary);
#ifdef _WIN32
            out << GetCurrentProcessId();
#else
            out << ::getpid();
#endif
        }
        else if (arg == "--env" && hasValue)
        {
            const std::string name = args[++i];
            const char* value = std::getenv(name.c_str());
            if (value != nullptr)
            {
                std::printf("%s=%s\n", name.c_str(), value);
            }
            else
            {
                std::printf("%s is not set\n", name.c_str());
            }
        }
        else if (arg == "--pwd")
        {
            std::printf("%s\n", std::filesystem::current_path().generic_string().c_str());
        }
        else if ((arg == "--on-stop" && hasValue) || arg == "--ignore-stop")
        {
            g_stop_code = arg == "--on-stop" ? std::stoi(args[++i]) : -1;
#ifdef _WIN32
            SetConsoleCtrlHandler(on_stop, TRUE);
#else
            std::signal(SIGTERM, g_stop_code >= 0 ? on_stop : SIG_IGN);
#endif
        }
        else if (arg == "--check-handle" && hasValue)
        {
#ifdef _WIN32
            DWORD flags = 0;
            const bool open = GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(args[++i]))), &flags) != 0;
#else
            const bool open = ::fcntl(std::stoi(args[++i]), F_GETFD) != -1;
#endif
            std::printf("%s\n", open ? "open" : "closed");
        }
        else if (arg == "--spawn-sleeper" && hasValue)
        {
            vehicle_sil::test::spawn_sleeper(vehicle_sil::test::own_executable(), args[++i]);
        }
        else if (arg == "--run-detached" && hasValue)
        {
#ifdef _WIN32
            std::string line;
            for (std::size_t j = i + 1; j < count; ++j)
            {
                line += (line.empty() ? "\"" : " \"") + args[j] + "\"";
            }
            const int size = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), size);
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            for (HANDLE h : {startup.hStdInput, startup.hStdOutput, startup.hStdError})
            {
                SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            }
            PROCESS_INFORMATION info{};
            if (!CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, DETACHED_PROCESS, nullptr, nullptr, &startup, &info))
            {
                std::fprintf(stderr, "helper: cannot run %s (Win32 error %lu)\n", line.c_str(), GetLastError());
                return 2;
            }
            WaitForSingleObject(info.hProcess, INFINITE);
            DWORD exitCode = 0;
            GetExitCodeProcess(info.hProcess, &exitCode);
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
            std::fflush(stdout);
            return static_cast<int>(exitCode);
#else
            std::fputs("helper: --run-detached is for Windows\n", stderr);
            return 2;
#endif
        }
        else if (arg == "--exit" && hasValue)
        {
            code = std::stoi(args[++i]);
        }
    }
    std::fflush(stdout);
    return code;
}
