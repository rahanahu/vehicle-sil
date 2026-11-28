// SPDX-License-Identifier: MIT
#include "vehicle_sil/process/paths.hpp"

#include <system_error>

namespace vehicle_sil::process {

std::filesystem::path path_from_utf8(std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string shown_path(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string utf8_from_path(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path normalize_executable_path(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {};
    }
    std::error_code ec;
    std::filesystem::path resolved = path.is_relative() ? std::filesystem::absolute(path, ec) : path;
    if (ec)
    {
        resolved = path;
    }
    resolved = resolved.lexically_normal();
    resolved.make_preferred();
    return resolved;
}

} // namespace vehicle_sil::process
