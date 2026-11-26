// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/probe.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace vehicle_sil::launch {

std::string write_control_metrics_file(const std::filesystem::path& path, std::string_view text)
{
    if (path.empty())
    {
        return "the path is empty";
    }
    const std::filesystem::path filename = path.filename();
    if (filename.empty() || filename == "." || filename == "..")
    {
        return "the path names a directory, not a file";
    }
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
    {
        return "the path is an existing directory, not a file";
    }
    if (path.has_parent_path())
    {
        ec.clear();
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            return "its parent directory cannot be created (" + ec.message() + ")";
        }
    }
    std::random_device rd;
    const auto now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto tid = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::uint64_t nonce = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
    nonce ^= now * 0x9e3779b97f4a7c15ULL;
    nonce ^= tid + 0xbf58476d1ce4e5b9ULL;
    std::filesystem::path beside = path;
    beside += ".writing-" + std::to_string(nonce);
    {
        std::ofstream out(beside, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return "it cannot be created (does its directory exist, and may it be written?)";
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (out.fail())
        {
            std::error_code ignored;
            std::filesystem::remove(beside, ignored);
            return "it cannot be written (is the disk full?)";
        }
    }
    ec.clear();
    std::filesystem::rename(beside, path, ec);
    if (ec)
    {
        std::error_code ignored;
        std::filesystem::remove(beside, ignored);
        return "the file written beside it cannot replace it (" + ec.message() +
               "; on Windows, another program may have it open)";
    }
    return {};
}

} // namespace vehicle_sil::launch
