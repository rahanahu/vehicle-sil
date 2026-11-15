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
#include <cstdio>
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
            if (const ryml::id_type ethernet = find(item, "ethernet"); ethernet != ryml::NONE && tree_.is_map(ethernet))
            {
                target.pcap_name = optional_text(ethernet, "pcap_name", at + ".ethernet").value_or("");
            }
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

/// Writes the list `node` (its object) as the member `key`, every key as write_node() does, but for
/// the targets that the launcher made unusable (ListedTarget::refused): "usable": false and their
/// "reason" (added if the bridge wrote none).
void write_list(JsonWriter& w, std::string_view key, const ryml::Tree& tree, ryml::id_type node, const TargetList& list)
{
    w.begin_object(key);
    for (auto child = tree.first_child(node); child != ryml::NONE; child = tree.next_sibling(child))
    {
        const std::string name = detail::to_string(tree.key(child));
        if (name.empty())
        {
            continue;
        }
        if (name != "targets" || !tree.is_seq(child))
        {
            write_node(w, name, tree, child);
            continue;
        }
        w.begin_array(name);
        std::size_t index = 0;
        for (auto item = tree.first_child(child); item != ryml::NONE; item = tree.next_sibling(item), ++index)
        {
            const ListedTarget* target = index < list.targets.size() && list.targets[index].refused ? &list.targets[index] : nullptr;
            if (target == nullptr || !tree.is_map(item))
            {
                write_node(w, {}, tree, item);
                continue;
            }
            w.begin_object();
            bool usable = false;
            bool reason = false;
            for (auto member = tree.first_child(item); member != ryml::NONE; member = tree.next_sibling(member))
            {
                const std::string m = detail::to_string(tree.key(member));
                if (m == "usable")
                {
                    w.boolean(m, false);
                    usable = true;
                }
                else if (m == "reason")
                {
                    w.string(m, target->reason);
                    reason = true;
                }
                else if (!m.empty())
                {
                    write_node(w, m, tree, member);
                }
            }
            if (!usable)
            {
                w.boolean("usable", false);
            }
            if (!reason)
            {
                w.string("reason", target->reason);
            }
            w.end_object();
        }
        w.end_array();
    }
    w.end_object();
}

/// `text` of a bridge for a line, harmless on a terminal: C0 controls, DEL, and every byte above
/// 0x7F of a text that is not UTF-8, as \xNN; the controls of detail::unicode_control_at() (C1,
/// the direction of text) as \u{NNNN}.
std::string shown(std::string_view text)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    const bool utf8 = vehicle_sil::detail::is_utf8(text);
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (const std::size_t control = utf8 ? detail::unicode_control_at(text.substr(i)) : 0; control != 0)
        {
            // U+0080..U+009F: C2 xx; U+202A..U+202E, U+2066..U+2069: E2 80|81 xx.
            const unsigned code = control == 2 ? static_cast<unsigned char>(text[i + 1])
                                               : 0x2000u + ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) +
                                                     (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            char u[16];
            std::snprintf(u, sizeof(u), "\\u{%04X}", code);
            out += u;
            i += control - 1;
        }
        else if (byte < 0x20 || byte == 0x7F || (!utf8 && byte >= 0x80))
        {
            out += std::string("\\x") + kHex[byte >> 4] + kHex[byte & 0xF];
        }
        else
        {
            out += text[i];
        }
    }
    return out;
}

/// The depth of the deepest object or array in `tree` under `node` (1 for one that holds scalars).
std::size_t depth_of(const ryml::Tree& tree, ryml::id_type node)
{
    std::size_t deepest = 0;
    std::vector<std::pair<ryml::id_type, std::size_t>> open{{node, 1}};
    while (!open.empty())
    {
        const auto [at, depth] = open.back();
        open.pop_back();
        if (!tree.is_container(at))
        {
            continue;
        }
        deepest = std::max(deepest, depth);
        for (auto child = tree.first_child(at); child != ryml::NONE; child = tree.next_sibling(child))
        {
            open.emplace_back(child, depth + 1);
        }
    }
    return deepest;
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

/// Why --connect cannot name the network `name`, or empty: it takes the name up to the first '=',
/// and the command line takes an argument that starts with "--" for an option.
std::optional<std::string> network_problem(std::string_view name)
{
    if (name.find('=') != std::string_view::npos)
    {
        return std::string("the network's name has '=', and --connect takes the network's name up to the first '=': "
                           "rename the network (a name without '=')");
    }
    if (name.rfind("--", 0) == 0)
    {
        return std::string("the network's name starts with '--', which the command line takes for an option: rename the "
                           "network");
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
    // A name that --connect cannot give: one line for the network, not one per target.
    const std::optional<std::string> unnamed = network_problem(network.name);
    if (unnamed && !list.targets.empty())
    {
        refused.push_back("  cannot be connected with --connect: " + *unnamed);
    }
    for (const ListedTarget& target : list.targets)
    {
        if (unnamed)
        {
            continue;
        }
        if (!target.usable)
        {
            refused.push_back("  cannot be used: " + shown(target.value) + "  (" +
                              shown(target.reason.empty() ? std::string("the bridge cannot use it") : target.reason) + ")");
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
                            " is quoted for sh: PowerShell and cmd read one of \" $ ` % ! \\ \xE2\x80\x9C \xE2\x80\x9D \xE2\x80\x9E in it "
                            "otherwise, so quote it as your shell needs");
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
        out += "  cannot list: " + shown(list.error_message) + " [" + shown(list.error_code) + "]\n";
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
        if (doc.root() != ryml::NONE && depth_of(doc.tree(), doc.root()) > kMaxListDepth)
        {
            throw DescriptionError("it is nested deeper than " + std::to_string(kMaxListDepth) + " levels");
        }
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

std::optional<std::string> refused_target(std::string_view network, NetworkKind kind, const ListedTarget& target)
{
    if (std::optional<std::string> problem = network_problem(network))
    {
        return problem;
    }
    if (target.value == kNoTarget)
    {
        return target.pcap_name.empty() || target.pcap_name == kNoTarget
                   ? std::string("--connect takes none for SIL Kit only, so an interface of this name cannot be connected: "
                                 "rename the interface")
                   : "--connect takes none for SIL Kit only: give this adapter by its pcap name, " + target.pcap_name;
    }
    if (std::optional<std::string> problem = target_problem(kind, target.value))
    {
        return "--connect does not take it: " + *problem;
    }
    return std::nullopt;
}

void refuse_targets(std::string_view network, NetworkKind kind, TargetList& list)
{
    for (ListedTarget& target : list.targets)
    {
        if (!target.usable)
        {
            continue;
        }
        if (std::optional<std::string> why = refused_target(network, kind, target))
        {
            target.usable = false;
            target.reason = std::move(*why);
            target.refused = true;
        }
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
    std::map<std::string, std::pair<std::filesystem::path, std::optional<TargetList>>, std::less<>> asked;
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
        const std::optional<std::string> none = no_bridge_here(n.kind, tools.can_bridge);
        const std::string tool(bridge_tool(n.kind));
        auto it = asked.find(tool);
        if (!tool.empty() && it == asked.end())
        {
            std::optional<std::filesystem::path> path = tools.find ? tools.find(tool) : std::nullopt;
            std::optional<TargetList> list;
            if (path)
            {
                list = interpret_list(n.kind, tool, tools.describe(*path, list_arguments(n.kind)));
            }
            else if (!none)
            {
                list = failed_list(tool, "tool-missing", tool + " is " + tool_not_found(tools.launcher_dir) + ": " +
                                                            tool_missing_fix(n.kind));
            }
            it = asked.emplace(tool, std::make_pair(path.value_or(std::filesystem::path{}), std::move(list))).first;
        }
        // No bridge for the kind here (LIN; CAN but on Windows): SIL Kit only, unless one was found
        // anyway (a build for tests).
        if (it == asked.end() || !it->second.second)
        {
            entry.no_bridge = none.value_or("");
            out.push_back(std::move(entry));
            continue;
        }
        entry.tool = tool;
        entry.tool_path = it->second.first;
        entry.list = it->second.second;
        refuse_targets(n.name, n.kind, *entry.list);
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
        return run_tool(tool, process::utf8_from_path(tool.stem()), arguments, options.list_timeout,
                        bridge_working_directory(tool));
    };
    report.per_network = connections_of(*read.config, options.network, tools);
    // The file's errors, as check reports them: a network with an error may be missing above.
    for (Finding& finding : read.findings)
    {
        if (finding.severity == Severity::Error)
        {
            report.findings.push_back(std::move(finding));
        }
    }
    return report;
}

bool ConnectionsReport::incomplete() const
{
    // No bridge on this computer (tool-missing) is part of the answer, not a failure.
    return std::any_of(per_network.begin(), per_network.end(),
                       [](const NetworkConnections& n) {
                           return !n.kind ||
                                  (n.list && !n.list->error_code.empty() && n.list->error_code != "tool-missing");
                       }) ||
           std::any_of(findings.begin(), findings.end(), [](const Finding& f) { return f.severity == Severity::Error; });
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
        // PowerShell takes the quotation marks U+201C, U+201D and U+201E for '"' (E2 80 9C..9E).
        if (byte == 0xE2 && i + 2 < word.size() && static_cast<unsigned char>(word[i + 1]) == 0x80 &&
            static_cast<unsigned char>(word[i + 2]) >= 0x9C && static_cast<unsigned char>(word[i + 2]) <= 0x9E)
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
            out += network.name + " (" + to_string(*network.kind) + "): SIL Kit only (" + network.no_bridge + ")\n";
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
    if (!report.findings.empty())
    {
        // The file's errors: what check says of it too.
        out += "\n" + detail::findings_text(report.file, report.findings) +
               "the file has errors: the list above has the networks that could be read\n";
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
                    write_list(w, "list", doc.tree(), root, *network.list);
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
