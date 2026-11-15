// SPDX-License-Identifier: MIT
#include "ecu_main_describe_stdout.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#include <stdlib.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace vehicle_sil::ecu_main_detail {

namespace {

#ifdef _WIN32
void ignore_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}

/// While it lives, a call of the C runtime on this thread with a bad argument (e.g. _dup() or
/// _get_osfhandle() of a descriptor that is not open) returns its error instead of calling the
/// invalid parameter handler, which by default ends the process under the UCRT. The function
/// is looked up in the UCRT at run time (the MinGW-w64 headers do not declare it); with another
/// C runtime (msvcrt.dll, which returns the error anyway) nothing changes.
class QuietCrt
{
public:
    QuietCrt()
    {
        if (const HMODULE crt = GetModuleHandleW(L"ucrtbase.dll"))
        {
            // Through void (*)(): a generic function pointer type for -Wcast-function-type.
            set_ = reinterpret_cast<Set>(reinterpret_cast<void (*)()>(
                GetProcAddress(crt, "_set_thread_local_invalid_parameter_handler")));
        }
        if (set_ != nullptr)
        {
            old_ = set_(&ignore_invalid_parameter);
        }
    }
    ~QuietCrt()
    {
        if (set_ != nullptr)
        {
            set_(old_);
        }
    }
    QuietCrt(const QuietCrt&) = delete;
    QuietCrt& operator=(const QuietCrt&) = delete;

private:
    using Set = _invalid_parameter_handler(__cdecl*)(_invalid_parameter_handler);
    Set set_ = nullptr;
    _invalid_parameter_handler old_ = nullptr;
};

bool fd_is_open(int fd)
{
    // -2: a standard descriptor without an OS handle (a program without a console).
    const intptr_t handle = _get_osfhandle(fd);
    return handle != -1 && handle != -2;
}
int dup_fd(int fd) { return _dup(fd); }
int dup2_fd(int from, int to) { return _dup2(from, to); }
void close_fd(int fd) { static_cast<void>(_close(fd)); }
int open_null() { return _open("NUL", _O_WRONLY | _O_BINARY); }
#else
/// Nothing to quiet: POSIX calls return their errors. (Not trivial, so that an unused one is
/// not a warning.)
struct QuietCrt
{
    QuietCrt() noexcept {}
    ~QuietCrt() {}
    QuietCrt(const QuietCrt&) = delete;
    QuietCrt& operator=(const QuietCrt&) = delete;
};
bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }
int dup_fd(int fd) { return ::dup(fd); }
int dup2_fd(int from, int to) { return ::dup2(from, to); }
void close_fd(int fd) { static_cast<void>(::close(fd)); }
int open_null() { return ::open("/dev/null", O_WRONLY); }
#endif

} // namespace

#ifdef _WIN32
long write_fd(int fd, const char* data, std::size_t size)
{
    return _write(fd, data, static_cast<unsigned int>(size > 0x40000000u ? 0x40000000u : size));
}
#else
long write_fd(int fd, const char* data, std::size_t size) { return static_cast<long>(::write(fd, data, size)); }
#endif

DescribeStdout::DescribeStdout()
{
    std::cout.flush();
    std::cerr.flush();
    static_cast<void>(std::fflush(stdout));
    static_cast<void>(std::fflush(stderr));
    const QuietCrt quiet;
    if (!fd_is_open(kStderrFd))
    {
        const int null = open_null();
        if (null != -1 && null != kStderrFd)
        {
            static_cast<void>(dup2_fd(null, kStderrFd));
            close_fd(null);
        }
    }
    if (fd_is_open(kStdoutFd))
    {
        saved_ = dup_fd(kStdoutFd);
    }
    if (fd_is_open(kStderrFd))
    {
        static_cast<void>(dup2_fd(kStderrFd, kStdoutFd));
    }
#ifdef _WIN32
    if (saved_ != -1)
    {
        static_cast<void>(_setmode(saved_, _O_BINARY)); // "\n" stays "\n"
    }
#endif
}

DescribeStdout::~DescribeStdout()
{
    if (saved_ != -1)
    {
        const QuietCrt quiet;
        close_fd(saved_);
    }
}

bool DescribeStdout::write(const std::string& text)
{
    if (saved_ == -1)
    {
        return false;
    }
    const QuietCrt quiet;
    std::size_t done = 0;
    while (done < text.size())
    {
        const long n = write_fd(saved_, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool write_stdout(const std::string& text)
{
    std::cout << std::flush;
#ifdef _WIN32
    std::fflush(stdout);
    const int old = _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::cout << text << std::flush;
    const bool ok = static_cast<bool>(std::cout);
#ifdef _WIN32
    if (old != -1)
    {
        static_cast<void>(_setmode(_fileno(stdout), old));
    }
#endif
    return ok;
}

} // namespace vehicle_sil::ecu_main_detail
