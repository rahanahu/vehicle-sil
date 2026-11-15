// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch connections (connections.hpp): the bridges' lists of where a network can be
// connected, read and passed on, and the outputs for people and as JSON.
#include "vehicle_sil/launch/connections.hpp"

#include "vehicle_sil/json/json_writer.hpp"
#include "vehicle_sil/launch/connect.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include "check_format.hpp"
#include "check_words.hpp"
#include "yaml_tree.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

namespace vehicle_sil::launch {

namespace {

using vehicle_sil::json::JsonWriter;

/// Reads a list of the form "vehicle_sil_targets": 1 from a parsed text.
class TargetReader
{
public:
    explicit TargetReader(const detail::YamlDocument& doc) : tree_(doc.tree()), root_(doc.root()) {}

    TargetList read() const
    {
        if (root_ == ryml::NONE || !tree_.is_map(root_))
        {
            fail("the output is not a JSON object");
        }
        const ryml::id_type version = find(root_, "vehicle_sil_targets");
        if (version == ryml::NONE)
        {
            fail("the key vehicle_sil_targets is missing");
        }
        if (const std::int64_t v = integer(version, "vehicle_sil_targets"); v != kTargetsFormat)
        {
            fail("it is a list of version " + std::to_string(v) + ", and this vehicle_sil_launch reads version " +
                 std::to_string(kTargetsFormat));
        }
        TargetList out;
        out.tool = optional_text(root_, "tool").value_or("");
        out.backend = optional_text(root_, "backend");
        out.source = optional_text(root_, "source");
        const ryml::id_type targets = find(root_, "targets");
        if (targets == ryml::NONE || !tree_.is_seq(targets))
        {
            fail("targets is missing or not an array");
        }
        std::size_t index = 0;
        for (auto item = tree_.first_child(targets); item != ryml::NONE; item = tree_.next_sibling(item), ++index)
        {
            const std::string at = "targets[" + std::to_string(index) + "]";
            if (!tree_.is_map(item))
            {
                fail(at + " is not an object");
            }
            ListedTarget target;
            const std::optional<std::string> value = optional_text(item, "value", at);
            if (!value)
            {
                fail(at + ".value is missing");
            }
            target.value = *value;
            target.label = optional_text(item, "label", at).value_or(target.value);
            target.detail = optional_text(item, "detail", at).value_or("");
            if (const ryml::id_type usable = find(item, "usable"); usable != ryml::NONE)
            {
                target.usable = boolean(usable, at + ".usable");
            }
            target.reason = optional_text(item, "reason", at).value_or("");
            out.targets.push_back(std::move(target));
        }
        if (const ryml::id_type notes = find(root_, "notes"); notes != ryml::NONE && !is_null(notes))
        {
            if (!tree_.is_seq(notes))
            {
                fail("notes is not an array");
            }
            for (auto item = tree_.first_child(notes); item != ryml::NONE; item = tree_.next_sibling(item))
            {
                out.notes.push_back(text(item, "a note"));
            }
        }
        if (const ryml::id_type error = find(root_, "error"); error != ryml::NONE && !is_null(error))
        {
            if (!tree_.is_map(error))
            {
                fail("error is not an object or null");
            }
            out.error_code = optional_text(error, "code", "error").value_or("");
            out.error_message = optional_text(error, "message", "error").value_or("");
            if (out.error_code.empty())
            {
                out.error_code = "failed";
            }
        }
        return out;
    }

private:
    [[noreturn]] static void fail(const std::string& what) { throw DescriptionError(what); }

    ryml::id_type find(ryml::id_type node, const char* key) const
    {
        return tree_.find_child(node, ryml::csubstr(key, std::char_traits<char>::length(key)));
    }

    bool is_scalar(ryml::id_type node) const { return tree_.has_val(node) && !tree_.is_container(node); }
    bool is_null(ryml::id_type node) const
    {
        return is_scalar(node) && !tree_.is_val_quoted(node) && tree_.val_is_null(node);
    }

    std::int64_t integer(ryml::id_type node, const std::string& what) const
    {
        const std::string value = is_scalar(node) && !tree_.is_val_quoted(node) ? detail::to_string(tree_.val(node)) : "";
        std::int64_t out = 0;
        const char* end = value.data() + value.size();
        const auto [ptr, ec] = std::from_chars(value.data(), end, out);
        if (value.empty() || ec != std::errc{} || ptr != end)
        {
            fail(what + " is not an integer");
        }
        return out;
    }

    std::string text(ryml::id_type node, const std::string& what) const
    {
        if (!is_scalar(node) || !tree_.is_val_quoted(node))
        {
            fail(what + " is not a string");
        }
        return detail::to_string(tree_.val(node));
    }

    bool boolean(ryml::id_type node, const std::string& what) const
    {
        const std::string value = is_scalar(node) && !tree_.is_val_quoted(node) ? detail::to_string(tree_.val(node)) : "";
        if (value != "true" && value != "false")
        {
            fail(what + " is not true or false");
        }
        return value == "true";
    }

    /// The string `key` of `node`, or empty if it is missing or null.
    std::optional<std::string> optional_text(ryml::id_type node, const char* key, const std::string& at = {}) const
    {
        const ryml::id_type child = find(node, key);
        if (child == ryml::NONE || is_null(child))
        {
            return std::nullopt;
        }
        return text(child, (at.empty() ? "" : at + ".") + key);
    }

    const ryml::Tree& tree_;
    ryml::id_type root_;
};

/// Writes the value `node` of `tree` as the member `key` (an element if "") with every key it has:
/// a quoted scalar as a string, the others as null, true, false or a number.
void write_node(JsonWriter& w, std::string_view key, const ryml::Tree& tree, ryml::id_type node)
{
    if (tree.is_map(node))
    {
        w.begin_object(key);
        for (auto child = tree.first_child(node); child != ryml::NONE; child = tree.next_sibling(child))
        {
            const std::string name = detail::to_string(tree.key(child));
            if (!name.empty()) // "" would start an element of an array
            {
                write_node(w, name, tree, child);
            }
        }
        w.end_object();
        return;
    }
    if (tree.is_seq(node))
    {
        w.begin_array(key);
        for (auto child = tree.first_child(node); child != ryml::NONE; child = tree.next_sibling(child))
        {
            write_node(w, {}, tree, child);
        }
        w.end_array();
        return;
    }
    const std::string value = tree.has_val(node) ? detail::to_string(tree.val(node)) : std::string{};
    if (tree.has_val(node) && tree.is_val_quoted(node))
    {
        w.string(key, value);
        return;
    }
    if (!tree.has_val(node) || tree.val_is_null(node))
    {
        w.null(key);
        return;
    }
    if (value == "true" || value == "false")
    {
        w.boolean(key, value == "true");
        return;
    }
    const char* end = value.data() + value.size();
    std::int64_t integer = 0;
    if (const auto [ptr, ec] = std::from_chars(value.data(), end, integer); ec == std::errc{} && ptr == end)
    {
        w.integer(key, integer);
        return;
    }
    double real = 0;
    if (const auto [ptr, ec] = std::from_chars(value.data(), end, real); ec == std::errc{} && ptr == end)
    {
        w.real(key, real);
        return;
    }
    w.string(key, value);
}

/// `text` for a line: control characters as \xNN.
std::string shown(std::string_view text)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F)
        {
            out += std::string("\\x") + kHex[byte >> 4] + kHex[byte & 0xF];
        }
        else
        {
            out += c;
        }
    }
    return out;
}

/// The line of a tool's standard error that says why (detail::LastLine), or "".
std::string last_line(const std::string& text)
{
    std::istringstream in(text);
    std::string line;
    detail::LastLine last;
    while (std::getline(in, line))
    {
        last.add(line);
    }
    return last.text();
}

/// Why --connect does not take `value` as the target of a network of `kind`, or empty.
std::optional<std::string> refused_target(NetworkKind kind, std::string_view value)
{
    if (value == kNoTarget)
    {
        return std::string("--connect takes none for SIL Kit only: give this target by another name that the bridge takes");
    }
    if (auto problem = target_problem(kind, value))
    {
        return "--connect does not take it: " + *problem;
    }
    return std::nullopt;
}

/// The lines of one network's list, for people.
std::string list_text(const NetworkConnections& network)
{
    const TargetList& list = *network.list;
    std::string out;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> refused;
    std::vector<std::string> notes;
    for (const ListedTarget& target : list.targets)
    {
        std::optional<std::string> why = target.usable ? refused_target(*network.kind, target.value) : std::nullopt;
        if (!target.usable || why)
        {
            const std::string reason = !target.usable ? (target.reason.empty() ? std::string("the bridge cannot use it") : target.reason)
                                                      : *why;
            refused.push_back("  cannot be used: " + shown(target.value) + "  (" + shown(reason) + ")");
            continue;
        }
        const std::string argument = network.name + "=" + target.value;
        std::string connect = "  --connect ";
        if (const std::optional<std::string> quoted = quoted_for_shells(argument))
        {
            connect += *quoted;
        }
        else
        {
            connect += quoted_for_sh(argument);
            notes.push_back("  note: the line of " + shown(target.value) +
                            " is quoted for sh: PowerShell and cmd read one of \" $ ` % ! \\ in it otherwise, so quote it as "
                            "your shell needs");
        }
        std::string about = target.label != target.value && !target.label.empty() ? shown(target.label) : std::string{};
        if (!target.detail.empty())
        {
            about += (about.empty() ? "" : "; ") + shown(target.detail);
        }
        rows.push_back(about.empty() ? std::vector<std::string>{connect} : std::vector<std::string>{connect, about});
    }
    out += detail::table_text(rows);
    for (const std::string& line : refused)
    {
        out += line + "\n";
    }
    for (const std::string& line : notes)
    {
        out += line + "\n";
    }
    for (const std::string& note : list.notes)
    {
        out += "  note: " + shown(note) + "\n";
    }
    if (!list.error_code.empty())
    {
        out += "  cannot list: " + shown(list.error_message) + " [" + list.error_code + "]\n";
    }
    else if (list.targets.empty())
    {
        out += "  nothing listed: " + network.tool + " found nothing to connect to on this computer\n";
    }
    return out;
}

} // namespace

std::vector<std::string> list_arguments(NetworkKind kind)
{
    switch (kind)
    {
    case NetworkKind::Ethernet:
        return {"--list-interfaces", "--json"};
    case NetworkKind::Can:
        return {"--list-channels", "--json"};
    case NetworkKind::Lin:
        break;
    }
    return {};
}

TargetList read_target_list(std::string_view json)
{
    const auto first = json.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        throw DescriptionError("the output is empty");
    }
    if (json[first] != '{')
    {
        throw DescriptionError("the output is not a JSON object");
    }
    try
    {
        const detail::YamlDocument doc(json);
        TargetList out = TargetReader(doc).read();
        out.json = std::string(json);
        return out;
    }
    catch (const detail::YamlSyntaxError& e)
    {
        throw DescriptionError("the output is not valid JSON (line " + std::to_string(e.line()) + ": " + e.message() + ")");
    }
}

TargetList failed_list(std::string_view tool, std::string_view code, std::string message)
{
    TargetList out;
    out.tool = std::string(tool);
    out.error_code = std::string(code);
    out.error_message = std::move(message);
    JsonWriter w(true);
    w.begin_object();
    w.integer("vehicle_sil_targets", kTargetsFormat);
    w.string("tool", out.tool);
    w.null("backend");
    w.null("source");
    w.begin_array("targets");
    w.end_array();
    w.begin_array("notes");
    w.end_array();
    w.begin_object("error");
    w.string("code", out.error_code);
    w.string("message", out.error_message);
    w.end_object();
    w.end_object();
    out.json = w.take();
    return out;
}

TargetList interpret_list(NetworkKind kind, std::string_view tool, const DescribeRun& run)
{
    if (run.start_error)
    {
        return failed_list(tool, "tool-cannot-start",
                           run.command_text + " could not be started (" + *run.start_error +
                               "): check that the bridge runs on this computer");
    }
    if (!run.exit_code)
    {
        return failed_list(tool, "tool-timeout",
                           run.command_text + " did not end within " + std::to_string(run.timeout.count()) +
                               " ms, so it was stopped: run that command yourself to see what it waits for");
    }
    if (const std::string dll = run.signal ? std::string{} : detail::dll_start_failure(*run.exit_code); !dll.empty())
    {
        // Windows did not start it; its standard error is empty.
        return failed_list(tool, "tool-cannot-start", run.command_text + " " + dll + ": " + detail::bridge_dll_fix(kind));
    }
    try
    {
        return read_target_list(run.standard_output);
    }
    catch (const DescriptionError& e)
    {
        const std::string line = last_line(run.standard_error);
        return failed_list(tool, "tool-bad-output",
                           run.command_text + " wrote no list that vehicle_sil_launch reads (" + e.what() + ")" +
                               (run.signal         ? ", and was ended by signal " + std::to_string(*run.signal)
                                : *run.exit_code != 0 ? ", and ended with exit code " + std::to_string(*run.exit_code)
                                                      : std::string{}) +
                               (line.empty() ? std::string{} : "; it wrote: " + line) +
                               ": use the bridge built with the vehicle-sil of this vehicle_sil_launch");
    }
}

std::optional<Finding> unknown_network(const SimulationConfig& config, std::string_view network)
{
    std::vector<std::string> names;
    for (const NetworkConfig& n : config.networks)
    {
        if (n.name == network)
        {
            return std::nullopt;
        }
        names.push_back(n.name);
    }
    std::string message = "--network names the network '" + std::string(network) + "', which the file does not have";
    if (const std::string close = detail::closest(network, names); !close.empty())
    {
        message += ": did you mean '" + close + "'? (the networks: " + detail::join(names) + ")";
    }
    else
    {
        message += names.empty() ? std::string(": the file has no networks") : ": the networks are " + detail::join(names);
    }
    return Finding{Severity::Error, "usage", std::move(message), 0, "", ""};
}

std::vector<NetworkConnections> connections_of(const SimulationConfig& config, const std::optional<std::string>& network,
                                               const ConnectTools& tools)
{
    // Each bridge once: where it is, and its list.
    std::map<std::string, std::pair<std::filesystem::path, TargetList>, std::less<>> asked;
    std::vector<NetworkConnections> out;
    for (const NetworkConfig& n : config.networks)
    {
        if (network && n.name != *network)
        {
            continue;
        }
        NetworkConnections entry;
        entry.name = n.name;
        if (!n.valid)
        {
            out.push_back(std::move(entry));
            continue;
        }
        entry.kind = n.kind;
        entry.tool = std::string(bridge_tool(n.kind));
        if (entry.tool.empty())
        {
            out.push_back(std::move(entry));
            continue;
        }
        auto it = asked.find(entry.tool);
        if (it == asked.end())
        {
            std::optional<std::filesystem::path> path = tools.find ? tools.find(entry.tool) : std::nullopt;
            TargetList list =
                !path ? failed_list(entry.tool, "tool-missing",
                                    entry.tool + " is " + tool_not_found(tools.launcher_dir) + ": " + tool_missing_fix(n.kind))
                      : interpret_list(n.kind, entry.tool, tools.describe(*path, list_arguments(n.kind)));
            it = asked.emplace(entry.tool, std::make_pair(path.value_or(std::filesystem::path{}), std::move(list))).first;
        }
        entry.tool_path = it->second.first;
        entry.list = it->second.second;
        out.push_back(std::move(entry));
    }
    return out;
}

ConnectionsReport list_connections(const std::filesystem::path& path, const std::string& shown_as,
                                   const ConnectionsOptions& options)
{
    ConnectionsReport report;
    report.file = shown_as;
    std::string text;
    if (const std::string problem = read_config_file(path, text); !problem.empty())
    {
        report.findings.push_back(Finding{Severity::Error, "file", problem, 0, "", ""});
        return report;
    }
    // No variant: every variant has the networks of the common part.
    ConfigRead read = read_config(text, {});
    if (!read.config)
    {
        report.findings = std::move(read.findings);
        return report;
    }
    report.readable = true;
    if (options.network)
    {
        if (std::optional<Finding> unknown = unknown_network(*read.config, *options.network))
        {
            report.usage = true;
            report.findings.push_back(std::move(*unknown));
            return report;
        }
    }
    ConnectTools tools;
    tools.launcher_dir = options.tool_directory;
    tools.find = [&options](std::string_view name) { return find_tool(name, options.tool_directory); };
    tools.describe = [&options](const std::filesystem::path& tool, const std::vector<std::string>& arguments) {
        return run_tool(tool, process::utf8_from_path(tool.stem()), arguments, options.list_timeout, {});
    };
    report.per_network = connections_of(*read.config, options.network, tools);
    return report;
}

bool ConnectionsReport::incomplete() const
{
    return std::any_of(per_network.begin(), per_network.end(), [](const NetworkConnections& n) {
        return !n.kind || (n.list && !n.list->error_code.empty());
    });
}

int ConnectionsReport::exit_code() const
{
    if (usage)
    {
        return 4;
    }
    if (!readable)
    {
        return 3;
    }
    return incomplete() ? 1 : 0;
}

std::optional<std::string> quoted_for_shells(std::string_view word)
{
    const auto plain = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == ':' || c == '/' || c == '=' || c == '+';
    };
    if (!word.empty() && std::all_of(word.begin(), word.end(), plain))
    {
        return std::string(word);
    }
    for (std::size_t i = 0; i < word.size(); ++i)
    {
        const auto byte = static_cast<unsigned char>(word[i]);
        const char c = word[i];
        if (byte < 0x20 || byte == 0x7F || c == '"' || c == '$' || c == '`' || c == '%' || c == '!')
        {
            return std::nullopt;
        }
        // sh: in double quotes, a backslash before these takes it away (and one at the end the closing quote).
        if (c == '\\' && (i + 1 == word.size() || word[i + 1] == '"' || word[i + 1] == '$' || word[i + 1] == '`' ||
                          word[i + 1] == '\\'))
        {
            return std::nullopt;
        }
    }
    return "\"" + std::string(word) + "\"";
}

std::string quoted_for_sh(std::string_view word)
{
    std::string out = "'";
    for (const char c : word)
    {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

std::string format_connections_text(const ConnectionsReport& report)
{
    if (!report.readable || report.usage)
    {
        return detail::findings_text(report.file, report.findings) + (report.usage ? "" : "cannot read " + report.file + "\n");
    }
    std::string out = report.file + (report.per_network.empty() ? std::string(": the file has no networks, so there is nothing to connect\n")
                                                             : std::string(": where each network can be connected on this computer\n"));
    for (const NetworkConnections& network : report.per_network)
    {
        if (!network.kind)
        {
            out += network.name + ": its kind in the file has an error, so nothing is listed: vehicle_sil_launch check " +
                   report.file + " says what is wrong\n";
            continue;
        }
        if (!network.list)
        {
            out += network.name + " (" + to_string(*network.kind) + "): SIL Kit only (there is no bridge for LIN)\n";
            continue;
        }
        const TargetList& list = *network.list;
        std::string about = network.tool;
        if (list.backend)
        {
            about += ", " + *list.backend + (list.source ? ": " + *list.source : std::string{});
        }
        out += network.name + " (" + to_string(*network.kind) + "; " + shown(about) + "):\n";
        out += list_text(network);
    }
    return out;
}

std::string format_connections_json(const ConnectionsReport& report)
{
    JsonWriter w;
    const auto optional_text = [&w](std::string_view key, const std::string& value) {
        if (value.empty())
        {
            w.null(key);
        }
        else
        {
            w.string(key, value);
        }
    };
    w.begin_object();
    w.integer("vehicle_sil_connections", kConnectionsFormat);
    w.string("vehicle_sil", version());
    optional_text("file", report.file);
    w.string("result", report.usage         ? "usage"
                       : !report.readable   ? "unreadable"
                       : report.incomplete() ? "failed"
                                             : "listed");
    w.begin_array("networks");
    for (const NetworkConnections& network : report.per_network)
    {
        w.begin_object();
        w.string("name", network.name);
        optional_text("kind", network.kind ? std::string(to_string(*network.kind)) : std::string{});
        optional_text("tool", network.tool);
        optional_text("tool_path", network.tool_path.empty() ? std::string{} : process::shown_path(network.tool_path));
        bool written = false;
        if (network.list)
        {
            try
            {
                const detail::YamlDocument doc(network.list->json);
                if (const ryml::id_type root = doc.root(); root != ryml::NONE && doc.tree().is_map(root))
                {
                    write_node(w, "list", doc.tree(), root);
                    written = true;
                }
            }
            catch (const detail::YamlSyntaxError&)
            {
                // read_target_list() and failed_list() give JSON only.
            }
        }
        if (!written)
        {
            w.null("list");
        }
        w.end_object();
    }
    w.end_array();
    detail::write_findings_json(w, report.file, report.findings);
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

} // namespace vehicle_sil::launch
