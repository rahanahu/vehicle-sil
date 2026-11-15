// SPDX-License-Identifier: MIT
//
// What run reads of the bridges of --connect: the counts in their statistics lines
// (read_outside_counts()), and when the event "outside" goes out (OutsideNote). Pure: no clock, no
// thread, no lock. RunOutput calls both under its mutex.
#include "vehicle_sil/launch/run.hpp"

#include "check_words.hpp"

#include "vehicle_sil/launch/connect.hpp"

#include <algorithm>
#include <charconv>
#include <set>

namespace vehicle_sil::launch::detail {

std::optional<OutsideCounts> read_outside_counts(std::string_view tool, std::string_view line)
{
    // The keys of each bridge: the frames it sent to the bus, and those that came from a device there.
    std::string_view toKey;
    std::string_view fromKey;
    if (tool == bridge_tool(NetworkKind::Ethernet))
    {
        toKey = "silkit_to_ext";
        fromKey = "ext_from_devices";
    }
    else if (tool == bridge_tool(NetworkKind::Can))
    {
        toKey = "silkit_to_xl";
        fromKey = "xl_to_silkit";
    }
    else
    {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r')
    {
        line.remove_suffix(1);
    }
    std::string_view rest = without_tag(line);
    constexpr std::string_view kWord = "stats";
    if (rest.substr(0, kWord.size()) != kWord)
    {
        return std::nullopt;
    }
    rest.remove_prefix(kWord.size());
    std::optional<std::uint64_t> to;
    std::optional<std::uint64_t> from;
    std::set<std::string_view> seen;
    // " key=N" until the end, nothing else.
    while (!rest.empty())
    {
        if (rest.front() != ' ')
        {
            return std::nullopt;
        }
        rest.remove_prefix(1);
        const std::size_t end = std::min(rest.find(' '), rest.size());
        const std::string_view part = rest.substr(0, end);
        rest.remove_prefix(end);
        const std::size_t equals = part.find('=');
        if (equals == 0 || equals == std::string_view::npos)
        {
            return std::nullopt;
        }
        const std::string_view key = part.substr(0, equals);
        const std::string_view digits = part.substr(equals + 1);
        std::uint64_t value = 0;
        const auto [at, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (digits.empty() || error != std::errc{} || at != digits.data() + digits.size() || !seen.insert(key).second)
        {
            return std::nullopt;
        }
        if (key == toKey)
        {
            to = value;
        }
        else if (key == fromKey)
        {
            from = value;
        }
    }
    if (!to || !from)
    {
        return std::nullopt;
    }
    return OutsideCounts{*to, *from};
}

std::optional<OutsideCounts> OutsideNote::on_counts(OutsideCounts counts, std::chrono::steady_clock::time_point now)
{
    last_ = counts;
    if (written_ == counts)
    {
        return std::nullopt;
    }
    return due(now);
}

std::optional<OutsideCounts> OutsideNote::due(std::chrono::steady_clock::time_point now)
{
    if (written_ && now - written_at_ < kEvery)
    {
        return std::nullopt;
    }
    if (const auto held = flush())
    {
        written_at_ = now;
        return held;
    }
    return std::nullopt;
}

std::optional<OutsideCounts> OutsideNote::flush()
{
    if (!last_ || last_ == written_)
    {
        return std::nullopt;
    }
    written_ = last_;
    return last_;
}

} // namespace vehicle_sil::launch::detail
