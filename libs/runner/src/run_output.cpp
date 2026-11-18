// SPDX-License-Identifier: MIT
#include "run_output.hpp"

#include "vehicle_sil/runner/detail/run_words.hpp"

#include "vehicle_sil/process/paths.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>

namespace vehicle_sil::launch {

std::string detail::log_name(const std::string& name)
{
    std::string out;
    for (const char c : name)
    {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                           c == '-' || c == '_';
        out += plain ? c : '_';
    }
    // Windows reads these names (also before an extension) as devices: the same rule everywhere,
    // so that one configuration gives the same files.
    static constexpr std::string_view kDevices[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                                    "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                                    "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    std::string stem = out.substr(0, out.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (std::find(std::begin(kDevices), std::end(kDevices), stem) != std::end(kDevices))
    {
        out.insert(stem.size(), "_");
    }
    return out;
}

// A child's output must not move the cursor or end the line early on the launcher's terminal.
std::string detail::printable(std::string_view line)
{
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size();)
    {
        const auto byte = static_cast<unsigned char>(line[i]);
        if (byte < 0x80)
        {
            out += (byte < 0x20 && byte != '\t') || byte == 0x7F ? '?' : static_cast<char>(byte);
            ++i;
            continue;
        }
        // A UTF-8 sequence: its length, the smallest value of that length, the value.
        const std::size_t length = byte >= 0xF5 ? 0 : byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC2 ? 2 : 0;
        std::uint32_t value = length == 4 ? byte & 0x07u : length == 3 ? byte & 0x0Fu : byte & 0x1Fu;
        bool valid = length != 0 && i + length <= line.size();
        for (std::size_t k = 1; valid && k < length; ++k)
        {
            const auto next = static_cast<unsigned char>(line[i + k]);
            valid = (next & 0xC0) == 0x80;
            value = (value << 6) | (next & 0x3Fu);
        }
        const std::uint32_t smallest = length == 4 ? 0x10000 : length == 3 ? 0x800 : 0x80;
        valid = valid && value >= smallest && value <= 0x10FFFF && (value < 0xD800 || value > 0xDFFF);
        if (!valid)
        {
            out += '?'; // not UTF-8: one '?' per byte that starts no character
            ++i;
            continue;
        }
        // The C1 control characters (U+0080 to U+009F) steer a terminal too.
        if (value <= 0x9F)
        {
            out += '?';
        }
        else
        {
            out.append(line.substr(i, length));
        }
        i += length;
    }
    return out;
}

namespace detail {

RunOutput::RunOutput(const RunContext& ctx) : ctx_(ctx)
{
    logs_.resize(ctx.config.participants.size());
    last_lines_.resize(ctx.config.participants.size());
    outside_.resize(ctx.config.participants.size());
    states_.resize(ctx.config.participants.size());
    last_statistics_.resize(ctx.config.participants.size());
}

const NetworkOutside* RunOutput::outside_of(std::size_t index) const
{
    const std::optional<NetworkOutside>& outside = ctx_.plan.members[index].outside;
    return outside ? &*outside : nullptr;
}

void RunOutput::write_outside(std::size_t index, const OutsideCounts& counts) const
{
    event([&](RunEvents& e) { e.outside(outside_of(index)->network, counts.to_outside, counts.from_outside); });
}

void RunOutput::write(const std::string& line) const
{
    if (ctx_.observers.write_line)
    {
        ctx_.observers.write_line(line);
    }
}

void RunOutput::say_held(const std::string& text) const
{
    if (ctx_.events != nullptr)
    {
        ctx_.events->log(std::string(kLauncherName), text);
    }
    else
    {
        write(std::string(kLauncherTag) + text);
    }
}

bool RunOutput::open_logs(RunOutcome& outcome)
{
    if (ctx_.plan.log_dir.empty())
    {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(ctx_.plan.log_dir, ec);
    // Compared without case: on Windows and macOS "Engine.log" and "engine.log" are one file.
    std::set<std::string> used;
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    for (std::size_t i = 0; i < ctx_.config.participants.size(); ++i)
    {
        if (!ctx_.config.participants[i].enabled)
        {
            continue; // not started: no log
        }
        std::string name = log_name(ctx_.config.participants[i].name);
        for (int n = 2; !used.insert(lower(name)).second; ++n)
        {
            name = log_name(ctx_.config.participants[i].name) + "-" + std::to_string(n);
        }
        const auto file = ctx_.plan.log_dir / process::path_from_utf8(name + ".log");
        outcome.participants[i].log = process::shown_path(file);
        logs_[i] = std::make_unique<std::ofstream>(file, std::ios::binary | std::ios::trunc);
        if (!*logs_[i])
        {
            outcome.setup.error = "cannot write the log " + process::shown_path(file) +
                                   ": check that --log-dir names a directory you can write to";
            return false;
        }
    }
    return true;
}

void RunOutput::on_line(std::size_t index, std::string_view line)
{
    const std::lock_guard lock(mutex_);
    if (logs_[index])
    {
        *logs_[index] << line << '\n' << std::flush;
    }
    if (const NetworkOutside* outside = outside_of(index); outside != nullptr)
    {
        last_lines_[index].add(line);
        // The launcher's to read while the bridge runs: the event "outside" and the result show its
        // counts; the last such line is shown once at the end (take_counts()).
        if (const auto all = read_outside_stats(outside->tool, line))
        {
            states_[index].on_counts(*all, std::chrono::steady_clock::now());
            tell_outside(std::chrono::steady_clock::now());
        }
        if (const auto counts = read_outside_counts(outside->tool, line))
        {
            if (const auto now = outside_[index].on_counts(*counts, std::chrono::steady_clock::now()))
            {
                write_outside(index, *now);
            }
            last_statistics_[index] = std::string(line);
            return;
        }
        // The bridge's warning that its device is shared: the launcher tells it, with the bridges of
        // this run taken out (outside_shared()); the log keeps it.
        if (is_shared_warning(line))
        {
            return;
        }
        // The state of the bridge's link, for the launcher; the log keeps it.
        if (is_link_line(line))
        {
            if (const auto link = read_outside_link_line(line))
            {
                const auto now = std::chrono::steady_clock::now();
                const std::string before_link = states_[index].link_text();
                const std::string before_bus = states_[index].bus_text();
                if (states_[index].on_link(*link, now))
                {
                    // The first line says "up" or "active" when all is well: nothing to tell.
                    const auto change = [&](const std::string& was, const std::string& is, const char* what) {
                        if (is != was && !(was.empty() && (is == "up" || is == "active")))
                        {
                            say_link_change(index, std::string(what) + " " + is, now);
                        }
                    };
                    change(before_link, states_[index].link_text(), "link");
                    change(before_bus, states_[index].bus_text(), "bus");
                }
                tell_outside(now);
            }
            return;
        }
    }
    show(index, line);
}

void RunOutput::tell_outside(std::chrono::steady_clock::time_point now)
{
    std::vector<RunState::Outside> all;
    for (std::size_t i = 0; i < states_.size(); ++i)
    {
        const NetworkOutside* outside = outside_of(i);
        if (outside == nullptr)
        {
            continue;
        }
        RunState::Outside one = states_[i].facts(now);
        one.network = outside->network;
        one.target = outside->target;
        one.participant = outside->participant;
        all.push_back(std::move(one));
    }
    if (all == told_outside_)
    {
        return;
    }
    told_outside_ = all;
    // Under `mutex_`, then TimeCommands' mutex: the order of the locks (see the class).
    ctx_.controls.set_outside(std::move(all));
}

void RunOutput::say_link_change(std::size_t index, const std::string& what, std::chrono::steady_clock::time_point now)
{
    const NetworkOutside& outside = *outside_of(index);
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%.2f", std::chrono::duration<double>(now - began_).count());
    say_held("outside: " + outside.network + " -> " + outside.target + ": " + what + " at " + seconds + " s");
}

void RunOutput::say_outside_shared(std::size_t index, const OutsideForeign& foreign, std::chrono::steady_clock::time_point now)
{
    say_link_change(index, "also used by " + shared_text(foreign), now);
}

void RunOutput::show(std::size_t index, std::string_view line) const
{
    if (ctx_.events != nullptr)
    {
        ctx_.events->log(ctx_.config.participants[index].name, printable(line));
    }
    else
    {
        write("[" + ctx_.config.participants[index].name + "] " + printable(line));
    }
}

std::string RunOutput::last_line(std::size_t index)
{
    const std::lock_guard lock(mutex_);
    return last_lines_[index].failure();
}

void RunOutput::outside_due(std::chrono::steady_clock::time_point now)
{
    const std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < outside_.size(); ++i)
    {
        if (const auto counts = outside_[i].due(now))
        {
            write_outside(i, *counts);
        }
    }
    // The 5 s and 10 s of the condition run out here.
    tell_outside(now);
}

std::optional<std::pair<std::size_t, std::string>> RunOutput::outside_lost(std::chrono::steady_clock::time_point now)
{
    const std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < states_.size(); ++i)
    {
        if (std::string why = states_[i].lost(now, ctx_.plan.outside_lost_after); !why.empty())
        {
            return std::make_pair(i, std::move(why));
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> RunOutput::outside_shared(std::chrono::steady_clock::time_point now, const std::vector<OutsideOwn>& own,
                                                      std::optional<std::chrono::milliseconds> stop_after)
{
    const std::lock_guard lock(mutex_);
    std::vector<OutsideSharing> all;
    for (const OutsideState& state : states_)
    {
        all.push_back(state.sharing());
    }
    const auto since_start = std::chrono::duration_cast<std::chrono::milliseconds>(now - began_);
    std::optional<std::size_t> stop;
    for (std::size_t i = 0; i < states_.size(); ++i)
    {
        if (outside_of(i) == nullptr)
        {
            continue; // not a bridge of --connect
        }
        const OutsideForeign foreign = foreign_sharing(i, all, own);
        if (states_[i].on_foreign(foreign, now, since_start))
        {
            say_outside_shared(i, foreign, now);
        }
        if (stop_after && !stop && states_[i].shared_for(now, *stop_after))
        {
            states_[i].stop_shared(stop_after->count() == 0 ? OutsideLink::SharedStop::AtStart : OutsideLink::SharedStop::AfterDelay);
            stop = i;
        }
    }
    tell_outside(now);
    return stop;
}

bool RunOutput::link_lines_read(const std::vector<std::size_t>& bridges)
{
    const std::lock_guard lock(mutex_);
    return std::all_of(bridges.begin(), bridges.end(), [this](std::size_t i) { return states_[i].link_line_seen(); });
}

OutsideLink RunOutput::take_link(std::size_t index, std::chrono::steady_clock::time_point now)
{
    const std::lock_guard lock(mutex_);
    return states_[index].summary(now);
}

std::optional<OutsideCounts> RunOutput::take_counts(std::size_t index)
{
    const std::lock_guard lock(mutex_);
    if (const auto counts = outside_[index].flush())
    {
        write_outside(index, *counts);
    }
    // The bridge's report for people: its last statistics line, with every count (its line at the
    // stop, or the last while it ran if it was ended without one), once.
    if (!last_statistics_[index].empty())
    {
        show(index, last_statistics_[index]);
        last_statistics_[index].clear();
    }
    return outside_[index].last();
}

} // namespace detail

} // namespace vehicle_sil::launch
