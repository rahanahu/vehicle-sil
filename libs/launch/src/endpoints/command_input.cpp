// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/command_input.hpp"

#include "vehicle_sil/config/detail/yaml_tree.hpp"

#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/runner/control_rules.hpp"

#include <charconv>
#include <utility>
#include <string>
#include <system_error>

namespace vehicle_sil::launch {

namespace {

bool whole_number(const std::string& text, std::int64_t& value)
{
    const char* const end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

/// parse_command(), and with `serve` parse_serve_command() (which it fills but for `command`).
std::optional<runner::Command> parse(std::string_view line, ServeCommand* serve)
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
    runner::Command out;
    const std::string kExpected = "write one JSON object per line, such as {\"command\":\"stop\"}";
    if (line[first] != '{')
    {
        out.message = "the line is not a JSON object: " + kExpected;
        out.reason = runner::reason::kInvalidCommand;
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
            out.reason = runner::reason::kInvalidCommand;
            return out;
        }
        const auto node = tree.find_child(root, ryml::csubstr("command", 7));
        if (node == ryml::NONE || !tree.has_val(node) || tree.is_container(node) || !tree.is_val_quoted(node))
        {
            out.message = std::string(node == ryml::NONE ? "the object has no \"command\"" : "\"command\" is not a string") +
                          ": " + kExpected;
            out.reason = runner::reason::kInvalidCommand;
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
        // serve: a string that is not empty, if the key is there; false: it is there, and not that.
        const auto text = [&](const char* key, std::string& value, bool& given) {
            given = tree.find_child(root, ryml::to_csubstr(key)) != ryml::NONE;
            if (!given)
            {
                return true;
            }
            const auto got = scalar(key);
            if (!got || !got->second || got->first.empty())
            {
                return false;
            }
            value = got->first;
            return true;
        };
        if (serve != nullptr)
        {
            // The run it is for, before anything else: a refusal of it says the run too.
            std::string run;
            bool given = false;
            if (!text("run", run, given))
            {
                out.message = "\"run\" must be the ID of the run the command is for, as a string, as the event run_start "
                              "gave it, such as {\"command\":\"stop\",\"run\":\"<run ID>\"}";
                out.reason = runner::reason::kInvalidArgument;
                return out;
            }
            if (given)
            {
                serve->run = std::move(run);
            }
        }
        // The sender's number, before the arguments: a refusal for them says it too.
        if (tree.find_child(root, ryml::csubstr("id", 2)) != ryml::NONE)
        {
            const auto id = scalar("id");
            std::int64_t value = 0;
            if (!id || id->second || id->first.empty() || !whole_number(id->first, value) ||
                value < 0 || value > runner::kMaxCommandId)
            {
                out.message = "\"id\" must be a whole number from 0 to " + std::to_string(runner::kMaxCommandId) +
                              " (the sender's number for the command, written back in its command event), such as "
                              "{\"command\":\"pause\",\"id\":1}; leave it out if no answer is looked for";
                out.reason = runner::reason::kInvalidArgument;
                return out;
            }
            out.id = value;
        }
        if (serve != nullptr && *out.name == "run")
        {
            bool given = false;
            for (const auto& [key, value] : {std::pair{"request", &serve->request}, std::pair{"timeline", &serve->timeline}})
            {
                if (!text(key, *value, given))
                {
                    out.message = std::string("run: \"") + key + "\" must be the path of a file as a string, such as " +
                                  "{\"command\":\"run\",\"request\":\"bench.json\",\"timeline\":\"timeline.json\"}; leave it out " +
                                  "for none";
                    out.reason = runner::reason::kInvalidArgument;
                    return out;
                }
            }
        }
        else if (*out.name == "step")
        {
            const auto ms = scalar("ms");
            std::int64_t value = 0;
            if (!ms || ms->second || ms->first.empty() || !whole_number(ms->first, value) ||
                value < 1 || value > config::kMaxMillis)
            {
                out.message = "step needs \"ms\", a whole number of milliseconds of virtual time from 1 to " +
                              std::to_string(config::kMaxMillis) + ", such as {\"command\":\"step\",\"ms\":100}";
                out.reason = runner::reason::kInvalidArgument;
                return out;
            }
            out.ms = value;
        }
        else if (*out.name == "run_until")
        {
            const auto at = scalar("at_ms");
            std::int64_t value = 0;
            if (!at || at->second || at->first.empty() || !whole_number(at->first, value) ||
                value < 0 || value > config::kMaxMillis)
            {
                out.message = "run_until needs \"at_ms\", the virtual time to stand still at, a whole number of "
                              "milliseconds from 0 to " +
                              std::to_string(config::kMaxMillis) + ", such as {\"command\":\"run_until\",\"at_ms\":5000}";
                out.reason = runner::reason::kInvalidArgument;
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
                out.reason = runner::reason::kInvalidArgument;
                return out;
            }
        }
    }
    catch (const config::detail::YamlSyntaxError&)
    {
        out.message = "the line is not valid JSON: " + kExpected;
        out.reason = runner::reason::kInvalidCommand;
        return out;
    }
    static constexpr std::string_view kNames[] = {"stop", "pause", "resume", "step", "run_until", "speed"};
    if (std::find(std::begin(kNames), std::end(kNames), *out.name) == std::end(kNames) &&
        (serve == nullptr || *out.name != "run"))
    {
        // The name as far as it is worth showing.
        const std::string shown = out.name->size() > 64 ? out.name->substr(0, 64) + "..." : *out.name;
        out.message = "unknown command '" + shown + "': the commands are " +
                      (serve != nullptr ? "run, " : "") + "stop, pause, resume, step, run_until and speed";
        out.reason = runner::reason::kUnknownCommand;
        return out;
    }
    out.accepted = true;
    return out;
}

} // namespace

std::optional<runner::Command> parse_command(std::string_view line) { return parse(line, nullptr); }

std::optional<ServeCommand> parse_serve_command(std::string_view line)
{
    ServeCommand out;
    std::optional<runner::Command> command = parse(line, &out);
    if (!command)
    {
        return std::nullopt;
    }
    out.command = std::move(*command);
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
                runner::Command tooLong;
                tooLong.message = "the line is longer than " + std::to_string(kMaxCommandLine) + " bytes";
                tooLong.reason = runner::reason::kInvalidCommand;
                too_long(tooLong);
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

void CommandInput::too_long(const runner::Command& command)
{
    if (on_serve_)
    {
        ServeCommand serve;
        serve.command = command;
        on_serve_(serve);
    }
    else
    {
        on_command_(command);
    }
}

void CommandInput::line(std::string_view text)
{
    if (on_serve_)
    {
        if (auto command = parse_serve_command(text))
        {
            on_serve_(*command);
        }
    }
    else if (auto command = parse_command(text))
    {
        on_command_(*command);
    }
}

} // namespace vehicle_sil::launch
