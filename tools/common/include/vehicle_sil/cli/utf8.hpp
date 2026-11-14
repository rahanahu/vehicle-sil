// SPDX-License-Identifier: MIT
//
// The command line and file names as UTF-8, shared by the vehicle-sil executables. On Windows
// main()'s argv is in the code page of the process: a name or a path in another script arrives
// broken, and SIL Kit and the configuration files expect UTF-8.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace vehicle_sil::cli {

#ifdef _WIN32
/// UTF-16 `text` as UTF-8 (unpaired surrogates become U+FFFD).
[[nodiscard]] inline std::string utf8_from_wide(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), length, out.data(), size, nullptr, nullptr);
    }
    return out;
}
#endif

/// The arguments after the program name, as UTF-8. On Windows they are taken from the wide
/// command line of the process, not from `argv` (which is in the code page); elsewhere `argv` is
/// returned as it is. Call it with main()'s argc and argv.
[[nodiscard]] inline std::vector<std::string> utf8_arguments(int argc, char** argv)
{
#ifdef _WIN32
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr)
    {
        // Out of memory, in practice: better the code page's text than no arguments.
        return std::vector<std::string>(argv + (argc > 0 ? 1 : 0), argv + argc);
    }
    std::vector<std::string> out;
    for (int i = 1; i < count; ++i)
    {
        out.push_back(utf8_from_wide(wide[i]));
    }
    LocalFree(wide);
    return out;
#else
    return std::vector<std::string>(argv + (argc > 0 ? 1 : 0), argv + argc);
#endif
}

/// The path that the UTF-8 `text` names. Open files through it: on Windows a std::string given to
/// a stream is read in the code page.
[[nodiscard]] inline std::filesystem::path utf8_path(std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

} // namespace vehicle_sil::cli
