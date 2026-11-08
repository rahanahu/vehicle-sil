// SPDX-License-Identifier: MIT
//
// Console output and main() scaffolding shared by the vehicle-sil executables.
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

namespace vehicle_sil::cli {

/// Prints "[who] <formatted text>" as one line to stdout and flushes.
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
inline void
log(const std::string& who, const char* format, ...)
{
    std::va_list args;
    va_start(args, format);
    std::printf("[%s] ", who.c_str());
    std::vprintf(format, args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    va_end(args);
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
        std::fprintf(stderr, "error: %s\n", e.what());
        print_usage();
        return kExitUsage;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[%s] FAIL setup: %s\n", tag, e.what());
        return kExitSetupFailed;
    }
}

} // namespace vehicle_sil::cli
