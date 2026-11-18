// SPDX-License-Identifier: MIT
//
// What run reads of the bridges of --connect: the counts in their statistics lines
// (read_outside_counts(), read_outside_stats()), their link lines (read_outside_link_line()), what
// those add up to (OutsideState), and when the event "outside" goes out (OutsideNote). Pure: no
// clock, no thread, no lock. RunOutput calls them under its mutex.
#include "vehicle_sil/launch/run.hpp"

#include "check_words.hpp"

#include "vehicle_sil/launch/connect.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::launch::detail {

bool is_link_line(std::string_view line)
{
    constexpr std::string_view kWord = "link ";
    return without_tag(line).substr(0, kWord.size()) == kWord;
}

std::optional<std::map<std::string, std::uint64_t>> read_outside_stats(std::string_view tool, std::string_view line)
{
    if (tool != bridge_tool(NetworkKind::Ethernet) && tool != bridge_tool(NetworkKind::Can))
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
    std::map<std::string, std::uint64_t> all;
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
        const std::string_view digits = part.substr(equals + 1);
        std::uint64_t value = 0;
        const auto [at, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        // At most the largest signed 64-bit integer: the JSON forms write the counts as such.
        if (digits.empty() || error != std::errc{} || at != digits.data() + digits.size() ||
            value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            !all.emplace(std::string(part.substr(0, equals)), value).second)
        {
            return std::nullopt;
        }
    }
    return all;
}

std::optional<OutsideCounts> read_outside_counts(std::string_view tool, std::string_view line)
{
    // The keys of each bridge: the frames it sent to the bus, and those that came from a device there.
    const bool ethernet = tool == bridge_tool(NetworkKind::Ethernet);
    const std::string toKey = ethernet ? "silkit_to_ext" : "silkit_to_xl";
    const std::string fromKey = ethernet ? "ext_from_devices" : "xl_to_silkit";
    const auto all = read_outside_stats(tool, line);
    if (!all)
    {
        return std::nullopt;
    }
    const auto to = all->find(toKey);
    const auto from = all->find(fromKey);
    if (to == all->end() || from == all->end())
    {
        return std::nullopt;
    }
    return OutsideCounts{to->second, from->second};
}

std::optional<OutsideLinkLine> read_outside_link_line(std::string_view line)
{
    if (!is_link_line(line))
    {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r')
    {
        line.remove_suffix(1);
    }
    std::string_view rest = without_tag(line);
    rest.remove_prefix(std::string_view("link").size());
    OutsideLinkLine out;
    while (!rest.empty())
    {
        if (rest.front() == ' ')
        {
            rest.remove_prefix(1);
            continue;
        }
        const std::size_t end = std::min(rest.find(' '), rest.size());
        const std::string_view part = rest.substr(0, end);
        rest.remove_prefix(end);
        const std::size_t equals = part.find('=');
        if (equals == 0 || equals == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = part.substr(0, equals);
        const std::string_view value = part.substr(equals + 1);
        if (key == "link" && !out.link)
        {
            if (value == "up") out.link = OutsideLinkValue::Up;
            else if (value == "down") out.link = OutsideLinkValue::Down;
            else if (value == "unknown") out.link = OutsideLinkValue::Unknown;
        }
        else if (key == "bus" && !out.bus)
        {
            if (value == "active") out.bus = OutsideBusValue::Active;
            else if (value == "passive") out.bus = OutsideBusValue::Passive;
            else if (value == "bus-off") out.bus = OutsideBusValue::BusOff;
            else if (value == "unknown") out.bus = OutsideBusValue::Unknown;
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::uint64_t>> dropped_counts(const std::map<std::string, std::uint64_t>& counts)
{
    std::vector<std::pair<std::string, std::uint64_t>> out;
    for (const std::string_view key : kOutsideDroppedKeys)
    {
        const auto at = counts.find(std::string(key));
        if (at != counts.end() && at->second != 0)
        {
            out.emplace_back(at->first, at->second);
        }
    }
    return out;
}

void OutsideState::Span::begin(std::chrono::steady_clock::time_point now)
{
    if (!since)
    {
        since = now;
        ++count;
    }
}

void OutsideState::Span::end(std::chrono::steady_clock::time_point now)
{
    if (since)
    {
        total += now - *since;
        since.reset();
    }
}

std::chrono::steady_clock::duration OutsideState::Span::total_at(std::chrono::steady_clock::time_point now) const
{
    return since ? total + (now - *since) : total;
}

bool OutsideState::on_link(const OutsideLinkLine& line, std::chrono::steady_clock::time_point now)
{
    bool changed = false;
    if (line.link && line.link != link_)
    {
        link_ = line.link;
        changed = true;
        if (*link_ == OutsideLinkValue::Down)
        {
            down_.begin(now);
        }
        else
        {
            down_.end(now);
        }
    }
    if (line.bus && line.bus != bus_)
    {
        bus_ = line.bus;
        changed = true;
        if (*bus_ == OutsideBusValue::BusOff)
        {
            bus_off_.begin(now);
        }
        else
        {
            bus_off_.end(now);
        }
        if (*bus_ == OutsideBusValue::Passive)
        {
            passive_.begin(now);
        }
        else
        {
            passive_.end(now);
        }
    }
    return changed;
}

std::string OutsideState::link_text() const
{
    if (!link_)
    {
        return {};
    }
    return *link_ == OutsideLinkValue::Up ? "up" : *link_ == OutsideLinkValue::Down ? "down" : "unknown";
}

std::string OutsideState::bus_text() const
{
    if (!bus_)
    {
        return {};
    }
    switch (*bus_)
    {
    case OutsideBusValue::Active: return "active";
    case OutsideBusValue::Passive: return "passive";
    case OutsideBusValue::BusOff: return "bus-off";
    case OutsideBusValue::Unknown: break;
    }
    return "unknown";
}

std::string OutsideState::lost(std::chrono::steady_clock::time_point now, std::chrono::milliseconds limit) const
{
    const auto over = [&](const Span& span) { return span.since && now - *span.since >= limit; };
    const std::string seconds = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(limit).count());
    if (over(down_))
    {
        return "link down for " + seconds + " s";
    }
    if (over(bus_off_))
    {
        return "bus off for " + seconds + " s";
    }
    return {};
}

OutsideLink OutsideState::summary(std::chrono::steady_clock::time_point now) const
{
    const auto ms = [](std::chrono::steady_clock::duration d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d);
    };
    OutsideLink out;
    out.counts = counts_;
    out.down_count = down_.count;
    out.down_total = ms(down_.total_at(now));
    out.bus_off_count = bus_off_.count;
    out.bus_off_total = ms(bus_off_.total_at(now));
    out.passive_total = ms(passive_.total_at(now));
    return out;
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
