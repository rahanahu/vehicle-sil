// SPDX-License-Identifier: MIT
//
// Paths as UTF-8 text and back, the same on POSIX and Windows. No process functions: code that
// only reads files can include this without child_process.hpp. Internal; not part of the
// library API.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace vehicle_sil::process {

/// `text` (UTF-8) as a path. On Windows the path holds the text as UTF-16, so a path with
/// characters outside the code page works with the wide Win32 functions; std::filesystem::path
/// made from a std::string would read the text in the code page.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);

/// `path` as UTF-8 (on Windows from its UTF-16 form; never throws for a path made with
/// path_from_utf8()).
[[nodiscard]] std::string utf8_from_path(const std::filesystem::path& path);

/// `path` as UTF-8 with '/' between its parts, also on Windows (where a path joined from parts
/// can mix '/' and '\\'): for messages, so that one path reads the same everywhere.
[[nodiscard]] std::string shown_path(const std::filesystem::path& path);

/// Resolves relative `path` against the current directory and collapses `.` / `..` segments
/// lexically with native separators (without following symbolic links).
[[nodiscard]] std::filesystem::path normalize_executable_path(const std::filesystem::path& path);

} // namespace vehicle_sil::process
