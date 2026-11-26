// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/probe.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace vehicle_sil::launch {
namespace {

std::filesystem::path normalize_for_compare(const std::filesystem::path& path)
{
    const auto canonicalize_step = [](const std::filesystem::path& input) {
        std::error_code ec;
        // Always anchor to an absolute path before weakly_canonical(): on libstdc++, weakly_canonical()
        // leaves a bare non-existent filename like "out.json" relative while expanding "./out.json" to
        // "<cwd>/out.json".
        std::filesystem::path abs = std::filesystem::absolute(input, ec);
        if (ec)
        {
            abs = input;
            ec.clear();
        }
        std::filesystem::path out = std::filesystem::weakly_canonical(abs, ec);
        if (ec)
        {
            out = abs;
        }
        out = out.lexically_normal();
        while (out.has_relative_path() && out.filename().empty())
        {
            out = out.parent_path();
        }
        return out;
    };

    std::filesystem::path out = canonicalize_step(path);
    // weakly_canonical() stops before a dangling leaf symlink because status() reports not_found.
    // Follow dangling leaf symlinks in a bounded loop (SYMLOOP_MAX = 40) so a symlink pointing to
    // a not-yet-existing output file matches its target without looping on cyclic symlinks.
    for (int hop = 0; hop < 40; ++hop)
    {
        std::error_code ec;
        if (!std::filesystem::is_symlink(out, ec) || ec)
        {
            break;
        }
        const std::filesystem::path target = std::filesystem::read_symlink(out, ec);
        if (ec || target.empty())
        {
            break;
        }
        out = canonicalize_step(target.is_absolute() ? target : (out.parent_path() / target));
    }
    return out;
}

} // namespace

bool same_file_path(const std::filesystem::path& a, const std::filesystem::path& b)
{
    if (a.empty() || b.empty())
    {
        return false;
    }
    std::error_code ec;
    const bool eq = std::filesystem::equivalent(a, b, ec);
    if (!ec)
    {
        return eq;
    }
    std::filesystem::path na = normalize_for_compare(a);
    std::filesystem::path nb = normalize_for_compare(b);
#ifdef _WIN32
    na.make_preferred();
    nb.make_preferred();
    std::wstring wa = na.wstring();
    std::wstring wb = nb.wstring();
    if (wa.size() != wb.size())
    {
        return false;
    }
    return std::equal(wa.begin(), wa.end(), wb.begin(),
                      [](wchar_t ca, wchar_t cb) { return std::towlower(ca) == std::towlower(cb); });
#else
    return na == nb;
#endif
}

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
