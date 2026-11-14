// SPDX-License-Identifier: MIT
//
// Console output and main() scaffolding shared by the vehicle-sil executables.
//
// The executables keep their text as UTF-8 (vehicle_sil/cli/utf8.hpp). A Windows console shows
// bytes in its code page (932 on a Japanese Windows), so text for standard output and standard
// error goes through write_text(): to a console as UTF-16, whatever the code page, and to a file
// or a pipe as the UTF-8 bytes. The code page of the console is not changed (the change would
// stay after the program ends).
#pragma once

#include "vehicle_sil/cli/args.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#endif

namespace vehicle_sil::cli {

#ifdef _WIN32
/// Writes UTF-8 `text` to the console `handle` as UTF-16 (whatever its code page). false if
/// `handle` is not a console (a file, a pipe, nothing): nothing was written then.
inline bool write_console(HANDLE handle, std::string_view text)
{
    DWORD mode = 0;
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode))
    {
        return false;
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size > 0)
    {
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
        DWORD written = 0;
        WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
    }
    return true;
}
#endif

/// Writes UTF-8 `text` to `stream` (stdout or stderr, as a rule) and flushes: to a Windows console
/// as UTF-16 (readable whatever the console's code page), to anything else (a file, a pipe, every
/// stream outside Windows) as the bytes of `text`, in the stream's own mode. What the stream
/// still held is written first, so the order with printf and fputs holds. false if it could not
/// be written (a closed stream, a full disk).
inline bool write_text(std::FILE* stream, std::string_view text)
{
#ifdef _WIN32
    std::fflush(stream);
    // A stream that is not open on anything has a negative number.
    if (const int file = _fileno(stream);
        file >= 0 && write_console(reinterpret_cast<HANDLE>(_get_osfhandle(file)), text))
    {
        return true;
    }
#endif
    const bool written = std::fwrite(text.data(), 1, text.size(), stream) == text.size();
    return std::fflush(stream) == 0 && written;
}

/// The text printf would write for `format` and `args`.
[[nodiscard]] inline std::string vformat(const char* format, std::va_list args)
{
    std::va_list again;
    va_copy(again, args);
    const int size = std::vsnprintf(nullptr, 0, format, again);
    va_end(again);
    std::string text(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
    if (size > 0)
    {
        // The string has room for the 0 that ends the text after its last character.
        std::vsnprintf(text.data(), text.size() + 1, format, args);
    }
    return text;
}

/// Prints "[who] <formatted text>" as one line to stdout and flushes (through write_text()).
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
inline void
log(const std::string& who, const char* format, ...)
{
    std::va_list args;
    va_start(args, format);
    const std::string text = vformat(format, args);
    va_end(args);
    write_text(stdout, "[" + who + "] " + text + "\n");
}

/// printf to `stream` (stdout or stderr) through write_text().
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
inline void
print(std::FILE* stream, const char* format, ...)
{
    std::va_list args;
    va_start(args, format);
    const std::string text = vformat(format, args);
    va_end(args);
    write_text(stream, text);
}

/// Upper-case hex digits of up to 8 bytes ("-" for none), NUL-terminated, without allocating.
struct HexBytes
{
    std::array<char, 17> text{};
    [[nodiscard]] const char* c_str() const noexcept { return text.data(); }
};

[[nodiscard]] inline HexBytes hex(const std::uint8_t* data, std::size_t size)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    HexBytes out;
    size = std::min<std::size_t>(size, 8);
    for (std::size_t i = 0; i < size; ++i)
    {
        out.text[2 * i] = kDigits[data[i] >> 4];
        out.text[2 * i + 1] = kDigits[data[i] & 0xF];
    }
    if (size == 0)
    {
        out.text[0] = '-';
    }
    return out;
}

/// The body of main(): runs `run(args)`. A UsageError prints the message and the usage text and
/// returns kExitUsage; any other exception prints "[tag] FAIL setup: ..." and returns
/// kExitSetupFailed.
template <typename Run>
int run_main(int argc, char** argv, const char* tag, void (*print_usage)(), Run&& run)
{
    try
    {
        return run(Args(argc, argv));
    }
    catch (const UsageError& e)
    {
        print(stderr, "error: %s\n", e.what());
        print_usage();
        return kExitUsage;
    }
    catch (const std::exception& e)
    {
        print(stderr, "[%s] FAIL setup: %s\n", tag, e.what());
        return kExitSetupFailed;
    }
}

} // namespace vehicle_sil::cli
