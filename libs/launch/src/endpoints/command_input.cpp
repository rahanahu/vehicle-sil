// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/command_input.hpp"

#include "vehicle_sil/config/detail/yaml_tree.hpp"

#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/runner/control_rules.hpp"

#include <charconv>
#include <string>

namespace vehicle_sil::launch {

std::optional<Command> parse_command(std::string_view line)
{
    if (!line.empty() && line.back() == '\r')
    {
        line.remove_suffix(1);
    }
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos)
    {
        return std::nullopt;
    }
    Command out;
    const std::string kExpected = "write one JSON object per line, such as {\"command\":\"stop\"}";
    if (line[first] != '{')
    {
        out.message = "the line is not a JSON object: " + kExpected;
        out.reason = reason::kInvalidCommand;
        return out;
    }
    try
    {
        const config::detail::YamlDocument doc(line);
        const ryml::Tree& tree = doc.tree();
        const auto root = doc.root();
        if (root == ryml::NONE || !tree.is_map(root))
        {
            out.message = "the line is not a JSON object: " + kExpected;
            out.reason = reason::kInvalidCommand;
            return out;
        }
        const auto node = tree.find_child(root, ryml::csubstr("command", 7));
        if (node == ryml::NONE || !tree.has_val(node) || tree.is_container(node) || !tree.is_val_quoted(node))
        {
            out.message = std::string(node == ryml::NONE ? "the object has no \"command\"" : "\"command\" is not a string") +
                          ": " + kExpected;
            out.reason = reason::kInvalidCommand;
            return out;
        }
        out.name = config::detail::to_string(tree.val(node));
        const auto scalar = [&](const char* key) -> std::optional<std::pair<std::string, bool>> {
            const auto n = tree.find_child(root, ryml::to_csubstr(key));
            if (n == ryml::NONE || !tree.has_val(n) || tree.is_container(n))
            {
                return std::nullopt;
            }
            return std::pair{config::detail::to_string(tree.val(n)), tree.is_val_quoted(n)};
        };
        // The sender's number, before the arguments: a refusal for them says it too.
        if (tree.find_child(root, ryml::csubstr("id", 2)) != ryml::NONE)
        {
            const auto id = scalar("id");
            std::int64_t value = 0;
            const char* end = id ? id->first.data() + id->first.size() : nullptr;
            if (!id || id->second || id->first.empty() || std::from_chars(id->first.data(), end, value).ptr != end ||
                value < 0 || value > kMaxCommandId)
            {
                out.message = "\"id\" must be a whole number from 0 to " + std::to_string(kMaxCommandId) +
                              " (the sender's number for the command, written back in its command event), such as "
                              "{\"command\":\"pause\",\"id\":1}; leave it out if no answer is looked for";
                out.reason = reason::kInvalidArgument;
                return out;
            }
            out.id = value;
        }
        if (*out.name == "step")
        {
            const auto ms = scalar("ms");
            std::int64_t value = 0;
            const char* end = ms ? ms->first.data() + ms->first.size() : nullptr;
            if (!ms || ms->second || ms->first.empty() || std::from_chars(ms->first.data(), end, value).ptr != end ||
                value < 1 || value > config::kMaxMillis)
            {
                out.message = "step needs \"ms\", a whole number of milliseconds of virtual time from 1 to " +
                              std::to_string(config::kMaxMillis) + ", such as {\"command\":\"step\",\"ms\":100}";
                out.reason = reason::kInvalidArgument;
                return out;
            }
            out.ms = value;
        }
        else if (*out.name == "run_until")
        {
            const auto at = scalar("at_ms");
            std::int64_t value = 0;
            const char* end = at ? at->first.data() + at->first.size() : nullptr;
            if (!at || at->second || at->first.empty() || std::from_chars(at->first.data(), end, value).ptr != end ||
                value < 0 || value > config::kMaxMillis)
            {
                out.message = "run_until needs \"at_ms\", the virtual time to stand still at, a whole number of "
                              "milliseconds from 0 to " +
                              std::to_string(config::kMaxMillis) + ", such as {\"command\":\"run_until\",\"at_ms\":5000}";
                out.reason = reason::kInvalidArgument;
                return out;
            }
            out.at_ms = value;
        }
        else if (*out.name == "speed")
        {
            const auto value = scalar("value");
            if (!value || value->second != (value->first == "max") || !config::parse_speed(value->first, out.speed))
            {
                out.message = "speed needs \"value\", a number of simulation seconds per wall-clock second from 0.000001 "
                              "to 1000000 or \"max\", such as {\"command\":\"speed\",\"value\":1}";
                out.reason = reason::kInvalidArgument;
                return out;
            }
        }
    }
    catch (const config::detail::YamlSyntaxError&)
    {
        out.message = "the line is not valid JSON: " + kExpected;
        out.reason = reason::kInvalidCommand;
        return out;
    }
    static constexpr std::string_view kNames[] = {"stop", "pause", "resume", "step", "run_until", "speed"};
    if (std::find(std::begin(kNames), std::end(kNames), *out.name) == std::end(kNames))
    {
        // The name as far as it is worth showing.
        const std::string shown = out.name->size() > 64 ? out.name->substr(0, 64) + "..." : *out.name;
        out.message = "unknown command '" + shown + "': the commands are stop, pause, resume, step, run_until and speed";
        out.reason = reason::kUnknownCommand;
        return out;
    }
    out.accepted = true;
    return out;
}

void CommandInput::append(std::string_view bytes)
{
    while (!bytes.empty())
    {
        const auto end = bytes.find('\n');
        const std::string_view part = bytes.substr(0, end);
        if (!skipping_)
        {
            pending_.append(part);
            if (pending_.size() > kMaxCommandLine)
            {
                Command tooLong;
                tooLong.message = "the line is longer than " + std::to_string(kMaxCommandLine) + " bytes";
                tooLong.reason = reason::kInvalidCommand;
                on_command_(tooLong);
                pending_.clear();
                skipping_ = true;
            }
        }
        if (end == std::string_view::npos)
        {
            return;
        }
        if (!skipping_)
        {
            line(pending_);
        }
        pending_.clear();
        skipping_ = false;
        bytes.remove_prefix(end + 1);
    }
}

void CommandInput::finish()
{
    if (!skipping_ && !pending_.empty())
    {
        line(pending_);
    }
    pending_.clear();
    skipping_ = false;
}

void CommandInput::line(std::string_view text)
{
    if (auto command = parse_command(text))
    {
        on_command_(*command);
    }
}

} // namespace vehicle_sil::launch
