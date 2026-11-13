// SPDX-License-Identifier: MIT
#include "run_output.hpp"

#include "run_words.hpp"

#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
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

RunOutput::RunOutput(const RunContext& ctx) : ctx_(ctx) { logs_.resize(ctx.config.participants.size()); }

void RunOutput::write(const std::string& line) const
{
    if (ctx_.options.write_line)
    {
        ctx_.options.write_line(line);
    }
}

void RunOutput::say_held(const std::string& text) const
{
    if (ctx_.options.events != nullptr)
    {
        ctx_.options.events->log(std::string(kLauncherName), text);
    }
    else
    {
        write(std::string(kLauncherTag) + text);
    }
}

bool RunOutput::open_logs(RunOutcome& outcome)
{
    if (ctx_.options.log_dir.empty())
    {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(ctx_.options.log_dir, ec);
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
        const auto file = ctx_.options.log_dir / process::path_from_utf8(name + ".log");
        outcome.participants[i].log = process::shown_path(file);
        logs_[i] = std::make_unique<std::ofstream>(file, std::ios::binary | std::ios::trunc);
        if (!*logs_[i])
        {
            outcome.setup_error = "cannot write the log " + process::shown_path(file) +
                                   ": check that --log-dir names a directory you can write to";
            return false;
        }
    }
    return true;
}

void RunOutput::on_line(std::size_t index, std::string_view line)
{
    const std::lock_guard lock(mutex_);
    if (ctx_.options.events != nullptr)
    {
        ctx_.options.events->log(ctx_.config.participants[index].name, printable(line));
    }
    else
    {
        write("[" + ctx_.config.participants[index].name + "] " + printable(line));
    }
    if (logs_[index])
    {
        *logs_[index] << line << '\n' << std::flush;
    }
}

} // namespace detail

} // namespace vehicle_sil::launch
