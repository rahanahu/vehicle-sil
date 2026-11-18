// SPDX-License-Identifier: MIT
//
// What run reads of the bridges of --connect: the counts in their statistics lines
// (read_outside_counts(), read_outside_stats()), their link lines (read_outside_link_line()), what
// those add up to (OutsideState, with the bridges outside the run that share a device,
// foreign_sharing()), and when the event "outside" goes out (OutsideNote). Pure: no
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

bool is_shared_warning(std::string_view line)
{
    constexpr std::string_view kWord = "warning: ";
    const std::string_view rest = without_tag(line);
    return rest.substr(0, kWord.size()) == kWord && rest.find(" is also used by ") != std::string_view::npos &&
           rest.find(" vehicle-sil bridge") != std::string_view::npos;
}

std::optional<std::map<std::string, std::uint64_t>> read_outside_stats(std::string_view tool, std::string_view line)
{
    if (!is_bridge_tool(tool))
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
    // Both bridges use the same names.
    const std::string toKey = "silkit_to_ext";
    const std::string fromKey = "ext_from_devices";
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
        else if (key == "device" && !out.device && !value.empty())
        {
            out.device = std::string(value);
        }
        else if (key == "marked" && !out.marked && (value == "0" || value == "1"))
        {
            out.marked = value == "1";
        }
        else if (key == "shared" && !out.shared)
        {
            std::uint32_t n = 0;
            const auto [at, error] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (!value.empty() && error == std::errc{} && at == value.data() + value.size())
            {
                out.shared = n;
            }
        }
        else if (key == "peers" && out.peers.empty())
        {
            // pid,pid,...: one that is not a number leaves the key out.
            std::vector<long> pids;
            std::string_view list = value;
            bool ok = !list.empty();
            while (ok && !list.empty())
            {
                const std::size_t comma = std::min(list.find(','), list.size());
                long pid = 0;
                const std::string_view digits = list.substr(0, comma);
                const auto [at, error] = std::from_chars(digits.data(), digits.data() + digits.size(), pid);
                ok = !digits.empty() && error == std::errc{} && at == digits.data() + digits.size() && pid > 0;
                pids.push_back(pid);
                list.remove_prefix(std::min(comma + 1, list.size()));
            }
            if (ok)
            {
                out.peers = std::move(pids);
            }
        }
    }
    return out;
}

OutsideForeign foreign_sharing(std::size_t index, const std::vector<OutsideSharing>& all, const std::vector<OutsideOwn>& own)
{
    const OutsideSharing& me = all[index];
    if (!me.seen || me.shared == 0)
    {
        return {};
    }
    // With the ids (Linux): those of the bridges this run started are not foreign. The bridge's own
    // is not in its list. One that has ended may still be in a report that has not been renewed.
    const bool known = !me.peers.empty() && std::none_of(own.begin(), own.end(), [](const OutsideOwn& o) { return o.alive && o.pid == 0; });
    if (known)
    {
        OutsideForeign out;
        for (const long pid : me.peers)
        {
            if (std::none_of(own.begin(), own.end(), [pid](const OutsideOwn& o) { return o.pid == pid; }))
            {
                out.pids.push_back(pid);
            }
        }
        out.count = static_cast<std::uint32_t>(out.pids.size());
        return out;
    }
    // Without: the run's other bridges that run, have placed a mark and sit on the same device.
    std::uint32_t mine = 0;
    for (std::size_t j = 0; j < all.size() && j < own.size(); ++j)
    {
        if (j == index || !own[j].alive)
        {
            continue;
        }
        if (!all[j].seen)
        {
            return {}; // it has not said where it is: the count cannot be told yet
        }
        if (all[j].marked && !me.device.empty() && all[j].device == me.device)
        {
            ++mine;
        }
    }
    return OutsideForeign{me.shared > mine ? me.shared - mine : 0, {}};
}

std::string shared_text(const OutsideForeign& foreign)
{
    std::string ids;
    for (const long pid : foreign.pids)
    {
        ids += (ids.empty() ? "" : ", ") + std::to_string(pid);
    }
    const bool one = foreign.count == 1;
    std::string text = one ? std::string("another vehicle-sil bridge") : std::to_string(foreign.count) + " other vehicle-sil bridges";
    if (!ids.empty())
    {
        text += std::string(" (") + (foreign.pids.size() == 1 ? "process " : "processes ") + ids + ")";
    }
    return text;
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
    sharing_.seen = true;
    if (line.device)
    {
        sharing_.device = *line.device;
    }
    if (line.marked)
    {
        sharing_.marked = *line.marked;
    }
    if (line.shared)
    {
        // The line says the whole of it: no peers key is no ids (none, or a system that has none).
        sharing_.shared = *line.shared;
        sharing_.peers = line.peers;
    }
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
    out.device = sharing_.device;
    out.shared_most = foreign_most_;
    out.shared_peers = foreign_pids_;
    out.shared_first = foreign_first_;
    out.shared_stop = shared_stop_;
    return out;
}

bool OutsideState::on_foreign(const OutsideForeign& foreign, std::chrono::steady_clock::time_point now,
                              std::chrono::milliseconds since_start)
{
    if (foreign.count == 0)
    {
        foreign_.end(now);
        return false;
    }
    foreign_.begin(now);
    if (!foreign_first_)
    {
        foreign_first_ = since_start;
    }
    for (const long pid : foreign.pids)
    {
        if (std::find(foreign_pids_.begin(), foreign_pids_.end(), pid) == foreign_pids_.end())
        {
            foreign_pids_.push_back(pid);
        }
    }
    const bool more = foreign.count > foreign_most_;
    foreign_most_ = std::max(foreign_most_, foreign.count);
    return more;
}

bool OutsideState::shared_for(std::chrono::steady_clock::time_point now, std::chrono::milliseconds limit) const
{
    return foreign_.since && now - *foreign_.since >= limit;
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
