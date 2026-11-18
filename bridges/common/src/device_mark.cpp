// SPDX-License-Identifier: MIT
//
// The parts of the device mark that do not touch the OS: names, the hash, reading /proc/net/unix,
// the link line's keys and the texts. The OS parts are in device_mark_os.cpp.
#include "vehicle_sil/bridge_runtime/device_mark.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <utility>

namespace vehicle_sil::bridge_runtime {
namespace detail {
namespace {

constexpr std::string_view kLinuxRoot = "vehicle-sil/device/";
/// The bytes of an abstract name: sun_path (108) without its first NUL.
constexpr std::size_t kAbstractNameBytes = 107;
/// The longest "<pid>/<random>" after the prefix: 10 digits, '/', 16 hex digits.
constexpr std::size_t kLinuxTailBytes = 10 + 1 + 16;

bool plain(std::string_view identity) noexcept
{
    if (identity.empty())
    {
        return false;
    }
    return std::all_of(identity.begin(), identity.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
               c == '@' || c == '+' || c == '-';
    });
}

std::string hex16(std::uint64_t value)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

bool all_hex(std::string_view text) noexcept
{
    return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

/// The process id of a mark's name after the prefix ("<pid>/<16 hex digits>"), or nullopt for
/// anything else (the name of a device whose identity only begins like this one's).
std::optional<std::uint32_t> mark_pid(std::string_view tail)
{
    const auto slash = tail.find('/');
    if (slash == std::string_view::npos || slash == 0 || tail.size() - slash - 1 != 16 || !all_hex(tail.substr(slash + 1)))
    {
        return std::nullopt;
    }
    std::uint32_t pid = 0;
    const auto [end, error] = std::from_chars(tail.data(), tail.data() + slash, pid);
    if (error != std::errc() || end != tail.data() + slash)
    {
        return std::nullopt;
    }
    return pid;
}

std::string pid_list(const std::vector<std::uint32_t>& pids, std::string_view separator)
{
    std::string text;
    for (const auto pid : pids)
    {
        if (!text.empty())
        {
            text += separator;
        }
        text += std::to_string(pid);
    }
    return text;
}

/// " (process 4321)", " (processes 4321, 4400)", or empty without process ids.
std::string processes(const std::vector<std::uint32_t>& pids)
{
    if (pids.empty())
    {
        return {};
    }
    return (pids.size() == 1 ? " (process " : " (processes ") + pid_list(pids, ", ") + ")";
}

} // namespace

std::string device_hash(std::string_view kind, std::string_view identity)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto add = [&hash](std::string_view part) {
        for (const char c : part)
        {
            hash ^= static_cast<unsigned char>(c);
            hash *= 0x100000001b3ULL;
        }
    };
    add(kind);
    add("/");
    add(identity);
    return hex16(hash);
}

std::string linux_mark_prefix(std::string_view kind, std::string_view identity)
{
    std::string prefix = std::string(kLinuxRoot) + std::string(kind) + "/";
    if (plain(identity) && prefix.size() + identity.size() + 1 + kLinuxTailBytes <= kAbstractNameBytes)
    {
        prefix += identity;
    }
    else
    {
        prefix += device_hash(kind, identity);
    }
    prefix += '/';
    return prefix;
}

std::string linux_mark_name(std::string_view prefix, std::uint32_t pid, std::uint64_t random)
{
    return std::string(prefix) + std::to_string(pid) + "/" + hex16(random);
}

DevicePeers read_proc_net_unix(std::string_view text, std::string_view prefix, std::string_view own)
{
    DevicePeers peers;
    std::size_t at = 0;
    while (at < text.size())
    {
        auto end = text.find('\n', at);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        const std::string_view line = text.substr(at, end - at);
        at = end + 1;
        // "Num: RefCount Protocol Flags Type St Inode Path": the path is the last field, where
        // there is one; an abstract name shows as "@<name>".
        const auto mark = line.rfind(" @");
        if (mark == std::string_view::npos)
        {
            continue;
        }
        std::string_view name = line.substr(mark + 2);
        while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
        {
            name.remove_suffix(1);
        }
        if (!name.starts_with(prefix) || (!own.empty() && name == own))
        {
            continue;
        }
        if (const auto pid = mark_pid(name.substr(prefix.size())))
        {
            ++peers.count;
            peers.pids.push_back(*pid);
        }
    }
    std::sort(peers.pids.begin(), peers.pids.end());
    peers.pids.erase(std::unique(peers.pids.begin(), peers.pids.end()), peers.pids.end());
    return peers;
}

std::string windows_mark_name(std::string_view space, std::string_view hash, unsigned number)
{
    return std::string(space) + "vehicle-sil-device-" + std::string(hash) + "-" + std::to_string(number);
}

std::string format_device_keys(std::string_view device, bool marked, const DevicePeers& peers)
{
    std::string keys = " device=" + std::string(device) + " marked=" + (marked ? "1" : "0") +
                       " shared=" + std::to_string(peers.count);
    if (!peers.pids.empty())
    {
        keys += " peers=" + pid_list(peers.pids, ",");
    }
    return keys;
}

std::optional<std::string> shared_warning(std::string_view device_text, const DevicePeers& before, const DevicePeers& now)
{
    std::vector<std::uint32_t> added;
    std::set_difference(now.pids.begin(), now.pids.end(), before.pids.begin(), before.pids.end(), std::back_inserter(added));
    // A new process id means a new bridge even when the count stayed (one left, another came); without ids (Windows) only a
    // larger count tells.
    if (now.count <= before.count && added.empty())
    {
        return std::nullopt;
    }
    const std::size_t grown = now.count > before.count ? now.count - before.count : 0;
    const std::size_t more = std::max<std::size_t>(grown, added.size());
    // "both" is right for 2 bridges on the device; with more, all of them mix.
    const char* const mixing = more == 1 ? "both" : "all of them";
    std::string text = std::string(device_text) + " is also used by " +
                       (more == 1 ? std::string("another vehicle-sil bridge") : std::to_string(more) + " more vehicle-sil bridges") +
                       processes(added) + ": the frames of " + mixing +
                       " mix there: if that is not meant, connect them to different devices, or "
                       "stop a bridge left over from an earlier run";
    return text;
}

} // namespace detail

std::string in_use_text(const DevicePeers& peers)
{
    if (peers.count == 0)
    {
        return {};
    }
    return (peers.count == 1 ? std::string("in use by a vehicle-sil bridge")
                             : "in use by " + std::to_string(peers.count) + " vehicle-sil bridges") +
           detail::processes(peers.pids);
}

DeviceShareWatch::DeviceShareWatch(DeviceMark mark, std::string device_text) :
  mark_(std::move(mark)),
  device_text_(std::move(device_text))
{
    if (!mark_.marked())
    {
        warnings_.push_back("the other vehicle-sil bridges on " + device_text_ + " cannot find this one: " + mark_.why_not());
    }
    count();
}

void DeviceShareWatch::count()
{
    DevicePeers now = mark_.others();
    if (auto warning = detail::shared_warning(device_text_, peers_, now))
    {
        warnings_.push_back(std::move(*warning));
    }
    peers_ = std::move(now);
}

std::string DeviceShareWatch::link_keys() const
{
    return detail::format_device_keys(mark_.device(), mark_.marked(), peers_);
}

std::vector<std::string> DeviceShareWatch::take_warnings()
{
    return std::exchange(warnings_, {});
}

} // namespace vehicle_sil::bridge_runtime
