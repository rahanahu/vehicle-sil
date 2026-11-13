// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/run.hpp"

#include "check_format.hpp"

#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/ecu/detail/json_writer.hpp"
#include "vehicle_sil/process/child_process.hpp"
#include "yaml_tree.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace vehicle_sil::launch {

namespace {

// --- reading --describe ---------------------------------------------------------------------

/// Reads the JSON of --describe; every error names the key.
class DescriptionReader
{
public:
    explicit DescriptionReader(const detail::YamlDocument& doc) : tree_(doc.tree()), root_(doc.root()) {}

    ReportedEcu read()
    {
        if (root_ == ryml::NONE || !tree_.is_map(root_))
        {
            fail("it is not a JSON object");
        }
        const auto format = integer(member(root_, "vehicle_sil_ecu", ""), "vehicle_sil_ecu");
        if (format != kEcuDescriptionFormat)
        {
            throw DescriptionError("format " + std::to_string(format));
        }
        ReportedEcu out;
        if (const auto node = find(root_, "vehicle_sil"); node != ryml::NONE)
        {
            out.vehicle_sil_version = text(node, "vehicle_sil");
        }
        out.description.name = text(member(root_, "name", ""), "name");
        const auto step = member(root_, "step_ns", "");
        if (!is_null(step))
        {
            const auto ns = integer(step, "step_ns");
            if (ns <= 0)
            {
                fail("step_ns is " + std::to_string(ns) + ", not a step");
            }
            out.description.step = SimDuration{ns};
        }
        const auto networks = member(root_, "networks", "");
        if (!tree_.is_seq(networks))
        {
            fail("networks is not an array");
        }
        std::size_t index = 0;
        for (auto item = tree_.first_child(networks); item != ryml::NONE; item = tree_.next_sibling(item), ++index)
        {
            out.description.networks.push_back(network(item, "networks[" + std::to_string(index) + "]"));
        }
        // An executable of a vehicle-sil without variables does not write the key: unknown, not none.
        if (const auto variables = find(root_, "variables"); variables != ryml::NONE)
        {
            if (!tree_.is_seq(variables))
            {
                fail("variables is not an array");
            }
            index = 0;
            for (auto item = tree_.first_child(variables); item != ryml::NONE; item = tree_.next_sibling(item), ++index)
            {
                out.description.variables.push_back(variable(item, "variables[" + std::to_string(index) + "]"));
            }
            out.variables_reported = true;
        }
        // Absent (an executable of an earlier vehicle-sil) or null: it does not offer its variables.
        if (const auto control = find(root_, "control"); control != ryml::NONE && !is_null(control))
        {
            if (!tree_.is_map(control))
            {
                fail("control is not an object");
            }
            ControlOffer offer;
            offer.transport = text(member(control, "transport", "control"), "control.transport");
            const auto version = integer(member(control, "wire_version", "control"), "control.wire_version");
            if (version < 0 || version > 65535)
            {
                fail("control.wire_version is " + std::to_string(version) + ", not a version");
            }
            offer.wire_version = static_cast<int>(version);
            out.description.control = std::move(offer);
        }
        return out;
    }

private:
    [[noreturn]] static void fail(const std::string& what) { throw DescriptionError(what); }

    ryml::id_type find(ryml::id_type node, const char* key) const
    {
        return tree_.find_child(node, ryml::csubstr(key, std::char_traits<char>::length(key)));
    }

    ryml::id_type member(ryml::id_type node, const char* key, const std::string& at) const
    {
        const auto child = find(node, key);
        if (child == ryml::NONE)
        {
            fail((at.empty() ? "" : at + ": ") + "the key " + key + " is missing");
        }
        return child;
    }

    bool is_scalar(ryml::id_type node) const { return tree_.has_val(node) && !tree_.is_container(node); }
    bool is_null(ryml::id_type node) const { return is_scalar(node) && tree_.val_is_null(node); }

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

    /// A number or null (min, max, step).
    std::optional<double> real(ryml::id_type node, const std::string& what) const
    {
        if (is_null(node))
        {
            return std::nullopt;
        }
        const std::string value = is_scalar(node) && !tree_.is_val_quoted(node) ? detail::to_string(tree_.val(node)) : "";
        double out = 0;
        const char* end = value.data() + value.size();
        const auto [ptr, ec] = std::from_chars(value.data(), end, out);
        if (value.empty() || ec != std::errc{} || ptr != end)
        {
            fail(what + " is not a number or null");
        }
        return out;
    }

    /// The value of `names` (the enumerators' names, in the order of the enumeration) that `node` is.
    template <class Enum, std::size_t N>
    Enum name_of(ryml::id_type node, const std::string& what, const std::array<Enum, N>& values,
                 const char* (*name)(Enum)) const
    {
        const std::string given = text(node, what);
        std::string known;
        for (const Enum value : values)
        {
            if (given == name(value))
            {
                return value;
            }
            known += std::string(known.empty() ? "" : ", ") + name(value);
        }
        fail(what + " is \"" + given + "\", not one of " + known);
    }

    control::VariableInfo variable(ryml::id_type node, const std::string& at) const
    {
        if (!tree_.is_map(node))
        {
            fail(at + " is not an object");
        }
        control::VariableInfo out;
        out.name = text(member(node, "name", at), at + ".name");
        out.type = name_of(member(node, "type", at), at + ".type",
                           std::array{control::ValueType::Bool, control::ValueType::Integer, control::ValueType::Real,
                                      control::ValueType::Enum},
                           &vehicle_sil::detail::value_type_name);
        out.causality = name_of(member(node, "causality", at), at + ".causality",
                                std::array{control::Causality::Input, control::Causality::Output, control::Causality::Parameter},
                                &vehicle_sil::detail::causality_name);
        out.unit = text(member(node, "unit", at), at + ".unit");
        out.min = real(member(node, "min", at), at + ".min");
        out.max = real(member(node, "max", at), at + ".max");
        out.step = real(member(node, "step", at), at + ".step");
        out.description = text(member(node, "description", at), at + ".description");
        const auto items = member(node, "items", at);
        if (!tree_.is_seq(items))
        {
            fail(at + ".items is not an array");
        }
        std::size_t index = 0;
        for (auto item = tree_.first_child(items); item != ryml::NONE; item = tree_.next_sibling(item), ++index)
        {
            const std::string itemAt = at + ".items[" + std::to_string(index) + "]";
            if (!tree_.is_map(item))
            {
                fail(itemAt + " is not an object");
            }
            out.items.push_back(control::EnumItem{text(member(item, "name", itemAt), itemAt + ".name"),
                                                  integer(member(item, "value", itemAt), itemAt + ".value")});
        }
        return out;
    }

    NetworkDescription network(ryml::id_type node, const std::string& at) const
    {
        if (!tree_.is_map(node))
        {
            fail(at + " is not an object");
        }
        NetworkDescription out;
        out.name = text(member(node, "name", at), at + ".name");
        const std::string use = text(member(node, "use", at), at + ".use");
        static const std::pair<const char*, NetworkUse> kUses[] = {{"raw", NetworkUse::Raw},
                                                                   {"j1939", NetworkUse::J1939},
                                                                   {"lin_master", NetworkUse::LinMaster},
                                                                   {"lin_slave", NetworkUse::LinSlave}};
        const auto it = std::find_if(std::begin(kUses), std::end(kUses), [&](const auto& u) { return use == u.first; });
        if (it == std::end(kUses))
        {
            fail(at + ".use is \"" + use + "\", not raw, j1939, lin_master or lin_slave");
        }
        out.use = it->second;
        const std::string kind = text(member(node, "kind", at), at + ".kind");
        if (kind != to_string(out.kind()))
        {
            fail(at + ".kind is \"" + kind + "\", but use \"" + use + "\" is on " + to_string(out.kind()));
        }
        const auto nodes = member(node, "nodes", at);
        if (!tree_.is_seq(nodes))
        {
            fail(at + ".nodes is not an array");
        }
        std::size_t index = 0;
        for (auto item = tree_.first_child(nodes); item != ryml::NONE; item = tree_.next_sibling(item), ++index)
        {
            const std::string nodeAt = at + ".nodes[" + std::to_string(index) + "]";
            if (!tree_.is_map(item))
            {
                fail(nodeAt + " is not an object");
            }
            J1939NodeDescription j1939;
            const auto address = integer(member(item, "address", nodeAt), nodeAt + ".address");
            if (address < 0 || address > j1939::kNullAddress)
            {
                fail(nodeAt + ".address is " + std::to_string(address) + ", not 0 to 254");
            }
            j1939.address = static_cast<j1939::Address>(address);
            const auto name = member(item, "j1939_name", nodeAt);
            if (!is_null(name))
            {
                const std::string hex = text(name, nodeAt + ".j1939_name");
                std::uint64_t value = 0;
                const char* end = hex.data() + hex.size();
                const bool wellFormed = hex.size() == 18 && hex.rfind("0x", 0) == 0 &&
                                        std::from_chars(hex.data() + 2, end, value, 16).ptr == end;
                if (!wellFormed)
                {
                    fail(nodeAt + ".j1939_name is \"" + hex + "\", not \"0x\" and 16 hexadecimal digits");
                }
                j1939.name = j1939::Name{value};
            }
            const bool capable =
                boolean(member(item, "arbitrary_address_capable", nodeAt), nodeAt + ".arbitrary_address_capable");
            if (capable != j1939.arbitrary_address_capable())
            {
                fail(nodeAt + ".arbitrary_address_capable is " + (capable ? "true" : "false") +
                     ", but the bit of its j1939_name says the other");
            }
            out.nodes.push_back(j1939);
        }
        if (out.use != NetworkUse::J1939 && !out.nodes.empty())
        {
            fail(at + " has nodes, but only a j1939 network has them");
        }
        return out;
    }

    const ryml::Tree& tree_;
    ryml::id_type root_;
};

// --- text helpers -----------------------------------------------------------------------------

std::string participant_prefix(const std::string& name) { return "participant '" + name + "': "; }

/// "10 ms", "500 us" or "250 ns".
std::string step_text(SimDuration step)
{
    const auto ns = step.count();
    if (ns % 1'000'000 == 0)
    {
        return std::to_string(ns / 1'000'000) + " ms";
    }
    if (ns % 1'000 == 0)
    {
        return std::to_string(ns / 1'000) + " us";
    }
    return std::to_string(ns) + " ns";
}

std::string hex2(unsigned value)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "0x%02X", value & 0xFFu);
    return buffer;
}

/// "0x80 (128)".
std::string address_text(j1939::Address address) { return hex2(address) + " (" + std::to_string(address) + ")"; }

/// "A and B", "A, B and C".
std::string and_list(const std::vector<std::string>& items)
{
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        out += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
    }
    return out;
}

/// `line` cut to at most 300 bytes, not inside a UTF-8 sequence, with "..." when cut.
std::string shortened(std::string line)
{
    if (line.size() <= 300)
    {
        return line;
    }
    std::size_t cut = 300;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
    {
        --cut;
    }
    return line.substr(0, cut) + "...";
}

/// The last `lines` lines of `text` (without "\r", each at most 300 bytes).
std::string last_lines(const std::string& text, std::size_t lines)
{
    std::vector<std::string> all;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        std::erase(line, '\r');
        all.push_back(shortened(std::move(line)));
    }
    while (!all.empty() && all.back().empty())
    {
        all.pop_back();
    }
    const std::size_t from = all.size() > lines ? all.size() - lines : 0;
    std::string out;
    for (std::size_t i = from; i < all.size(); ++i)
    {
        out += (out.empty() ? "" : "\n") + all[i];
    }
    return out;
}

/// The first `lines` lines of `text` (without "\r", each at most 300 bytes).
std::string first_lines(const std::string& text, std::size_t lines)
{
    std::string out;
    std::istringstream in(text.substr(0, 4096));
    std::string line;
    for (std::size_t n = 0; n < lines && std::getline(in, line); ++n)
    {
        std::erase(line, '\r');
        out += (n == 0 ? "" : "\n") + shortened(std::move(line));
    }
    return out;
}

std::string stderr_detail(const std::string& standard_error, bool truncated)
{
    const std::string tail = last_lines(standard_error, 10);
    return tail.empty()  ? "its standard error is empty"
           : truncated ? "the end of its standard error (longer than 1 MiB; only its end was kept):\n" + tail
                       : "the end of its standard error:\n" + tail;
}

/// The number of code points of UTF-8 text (its width in a table).
std::size_t width(std::string_view text)
{
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

/// Rows of columns, each column as wide as its widest cell, two spaces between; no trailing
/// spaces.
std::string table(const std::vector<std::vector<std::string>>& rows)
{
    std::vector<std::size_t> widths;
    for (const auto& row : rows)
    {
        widths.resize(std::max(widths.size(), row.size()));
        for (std::size_t i = 0; i < row.size(); ++i)
        {
            widths[i] = std::max(widths[i], width(row[i]));
        }
    }
    std::string out;
    for (const auto& row : rows)
    {
        std::string line;
        for (std::size_t i = 0; i < row.size(); ++i)
        {
            line += row[i];
            if (i + 1 < row.size())
            {
                line.append(widths[i] - width(row[i]) + 2, ' ');
            }
        }
        out += line + "\n";
    }
    return out;
}

std::string count_text(std::size_t n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

/// A network of an ECU as a table cell: "J1939_CAN (j1939 0x00 0x80)", "Body (lin_master)".
std::string network_cell(const NetworkDescription& network)
{
    std::string out = network.name + " (" + to_string(network.use);
    if (network.use == NetworkUse::J1939)
    {
        if (network.nodes.empty())
        {
            out += ", no node";
        }
        for (const auto& node : network.nodes)
        {
            out += " " + (node.address == j1939::kNullAddress ? std::string("any") : hex2(node.address));
        }
    }
    return out + ")";
}

/// true if the participant is enabled and described itself (its networks count).
bool is_on_any(const ParticipantConfig& participant, const ParticipantResult& result)
{
    return participant.enabled && result.status == ParticipantStatus::Described && result.ecu;
}

/// true if the participant is enabled and described itself on `network` with its kind (as the rules
/// count; any kind for a network whose kind could not be read).
bool is_on(const ParticipantConfig& participant, const ParticipantResult& result, const NetworkConfig& network)
{
    return participant.enabled && result.status == ParticipantStatus::Described && result.ecu &&
           std::any_of(result.ecu->description.networks.begin(), result.ecu->description.networks.end(),
                       [&](const NetworkDescription& n) {
                           return n.name == network.name && (!network.valid || n.kind() == network.kind);
                       });
}

const char* severity_name(Severity severity)
{
    return severity == Severity::Error ? "error" : severity == Severity::Warning ? "warning" : "note";
}

const char* status_name(ParticipantStatus status)
{
    switch (status)
    {
    case ParticipantStatus::Described:
        return "described";
    case ParticipantStatus::NotChecked:
        return "not_checked";
    case ParticipantStatus::Failed:
        return "failed";
    case ParticipantStatus::Skipped:
        return "skipped";
    case ParticipantStatus::Disabled:
        return "disabled";
    }
    return "skipped";
}

/// The command as shown in messages, on one line: an argument that is empty or has white space,
/// quotes or control characters in quotes, the control characters as \n, \t, \r or \xNN.
std::string command_text(const std::vector<std::string>& words)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const auto& word : words)
    {
        bool quote = word.empty();
        std::string shown;
        for (const char c : word)
        {
            const auto byte = static_cast<unsigned char>(c);
            quote = quote || byte <= ' ' || c == '"' || c == '\'';
            if (c == '\n')
            {
                shown += "\\n";
            }
            else if (c == '\t')
            {
                shown += "\\t";
            }
            else if (c == '\r')
            {
                shown += "\\r";
            }
            else if (byte < ' ' || byte == 0x7F)
            {
                shown += std::string("\\x") + kHex[byte >> 4] + kHex[byte & 0xF];
            }
            else
            {
                shown += c;
            }
        }
        out += (out.empty() ? "" : " ") + (quote ? "'" + shown + "'" : shown);
    }
    return out;
}

// --- the comparison -----------------------------------------------------------------------------

/// One J1939 node on a network: who has it, for messages.
struct NodeOnNetwork
{
    std::string label; ///< "Engine", or "Engine (node 2)" if Engine has more than one there
    J1939NodeDescription node;
};

} // namespace

ReportedEcu read_description(std::string_view json)
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
        return DescriptionReader(doc).read();
    }
    catch (const detail::YamlSyntaxError& e)
    {
        throw DescriptionError("the output is not valid JSON (line " + std::to_string(e.line()) + ": " + e.message() + ")",
                               e.context());
    }
}

DescribeOutcome interpret_describe(const ParticipantConfig& participant, const DescribeRun& run)
{
    const std::string prefix = participant_prefix(participant.name);
    const auto finding = [&](std::string code, std::string message, std::string detail) {
        return DescribeOutcome{std::nullopt, Finding{Severity::Error, std::move(code), prefix + std::move(message),
                                                     participant.command_line, participant.name, "", std::move(detail)}};
    };
    if (run.start_error)
    {
        return finding("describe-start",
                       *run.start_error + (run.start_refused_by_system
                                               ? ": the system did not let vehicle_sil_launch start a process (see the "
                                                 "reason); this is not about the executable"
                                               : ": check that the file is an executable for this computer"),
                       "");
    }
    if (!run.exit_code)
    {
        return finding("describe-timeout",
                       run.command_text + " did not end within " + std::to_string(run.timeout.count()) +
                           " ms and was stopped. --describe only builds the ECU, so something in its set-up waits "
                           "(a connection, input): fix that, or give vehicle_sil_launch check a longer "
                           "--describe-timeout-ms",
                       stderr_detail(run.standard_error, run.error_truncated));
    }
    if (*run.exit_code != 0)
    {
        std::string ended;
        std::string hint;
        if (run.signal)
        {
            ended = "was ended by the signal " + std::to_string(*run.signal);
            hint = "it crashed or was killed while it built the ECU: fix the ECU (its standard error may say where)";
        }
#ifdef _WIN32
        else if (const auto code = static_cast<std::uint32_t>(*run.exit_code); code >= 0xC0000000u)
        {
            // A Windows exception code, such as 0xC0000005.
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(code));
            ended = std::string("ended with the exception code ") + hex;
            hint = "it crashed while it built the ECU: fix the ECU (its standard error may say where)";
        }
#endif
        else
        {
            ended = "ended with exit code " + std::to_string(*run.exit_code);
            hint = *run.exit_code == 4 ? "it did not take --describe or --name: if it is not made with ecu_main, set "
                                         "describe: false"
                                       : "its set-up failed (see its standard error): fix the ECU, or set describe: false "
                                         "if it is not made with ecu_main";
        }
        return finding("describe-failed", run.command_text + " " + ended + "; " + hint,
                       stderr_detail(run.standard_error, run.error_truncated));
    }
    try
    {
        return DescribeOutcome{read_description(run.standard_output), std::nullopt};
    }
    catch (const DescriptionError& e)
    {
        const std::string what = e.what();
        if (what.rfind("format ", 0) == 0)
        {
            return finding("describe-version",
                           run.command_text + " wrote a description in " + what + ", but this vehicle_sil_launch reads format " +
                               std::to_string(kEcuDescriptionFormat) +
                               ": build the executable and vehicle_sil_launch from the same vehicle-sil",
                           "");
        }
        const std::string head = first_lines(run.standard_output, 5);
        std::string detail;
        const auto add_detail = [&detail](const std::string& text) {
            if (!text.empty())
            {
                detail += (detail.empty() ? "" : "\n") + text;
            }
        };
        add_detail(run.output_truncated ? "its standard output was longer than 1 MiB and was cut there" : "");
        add_detail(e.context());
        add_detail(head.empty() ? stderr_detail(run.standard_error, run.error_truncated)
                                : "the start of its standard output:\n" + head);
        return finding("describe-output",
                       run.command_text + " ended with exit code 0, but its output is not a description: " + what +
                           ". Is it made with ecu_main, and does nothing else write to its standard output? If it is "
                           "not made with ecu_main, set describe: false",
                       detail);
    }
}

std::vector<Finding> check_participants(const SimulationConfig& config, const std::vector<ParticipantResult>& results,
                                        std::string_view launcher_version)
{
    std::vector<Finding> out;
    const auto add = [&](Severity severity, const char* code, std::size_t line, std::string message,
                         const std::string& participant, const std::string& network) {
        out.push_back(Finding{severity, code, std::move(message), line, participant, network});
    };
    std::map<std::string, const NetworkConfig*> networks;
    std::vector<std::string> networkNames;
    for (const auto& network : config.networks)
    {
        networks.emplace(network.name, &network);
        networkNames.push_back(network.name);
    }
    // The rules that need every participant's description: not said while a description is
    // missing (the error of that participant says why), only noted while a participant has
    // describe: false (it may be the one).
    // Only the enabled participants count: a disabled one is not in the simulation, whatever its
    // status.
    const auto any_enabled = [&](ParticipantStatus status) {
        for (std::size_t i = 0; i < config.participants.size() && i < results.size(); ++i)
        {
            if (config.participants[i].enabled && results[i].status == status)
            {
                return true;
            }
        }
        return false;
    };
    const bool incomplete = any_enabled(ParticipantStatus::Failed) || any_enabled(ParticipantStatus::Skipped);
    const bool unchecked = any_enabled(ParticipantStatus::NotChecked);
    const std::string notCounted = unchecked ? " (participants with describe: false are not counted)" : "";

    // Each participant against its entry.
    for (std::size_t i = 0; i < config.participants.size() && i < results.size(); ++i)
    {
        const ParticipantConfig& p = config.participants[i];
        const ParticipantResult& result = results[i];
        const std::string prefix = participant_prefix(p.name);
        if (!p.enabled)
        {
            add(Severity::Note, "participant-disabled", p.line,
                (p.name.empty() ? "participants[" + std::to_string(i) + "]: " : prefix) +
                    "disabled (enabled: false, or --disable): it is neither checked nor started, and the other "
                    "findings are about the simulation without it",
                p.name, "");
            continue;
        }
        if (p.manual && !p.name.empty())
        {
            add(Severity::Note, "participant-manual", p.line,
                prefix + "you start it yourself (manual: true, or --manual): run does not start it, but prints the command "
                         "to start it with" +
                    (p.required ? std::string(", and waits for it to join without a time limit (with --timeout-ms, at most that long)")
                                : std::string(", and does not wait for it (required: false)")) +
                    (registry_on_network(config)
                         ? "; start it with " + std::string(kListenOnNetworkVariable) +
                               "=1 in its environment, so that the other computers reach it (registry_uri is not a "
                               "loopback address)"
                         : std::string{}),
                p.name, "");
        }
        if (result.status == ParticipantStatus::NotChecked)
        {
            add(Severity::Note, "not-checked", p.line,
                prefix + "not checked (describe: false): vehicle_sil_launch cannot compare its name, step and networks "
                         "with this file",
                p.name, "");
            continue;
        }
        if (result.status != ParticipantStatus::Described || !result.ecu)
        {
            continue;
        }
        const EcuDescription& ecu = result.ecu->description;
        if (ecu.name != p.name)
        {
            add(Severity::Error, "name-mismatch", p.line,
                prefix + "the executable reports the name '" + ecu.name + "' although it was given --name " + p.name +
                    ": its main() must pass all of its arguments to ecu_main",
                p.name, "");
        }
        if (p.step_ms)
        {
            const SimDuration wanted = std::chrono::milliseconds(*p.step_ms);
            if (!ecu.step)
            {
                add(Severity::Error, "step-mismatch", p.step_line,
                    prefix + "step_ms is " + std::to_string(*p.step_ms) +
                        ", but the executable reports no step (it runs in real time): remove step_ms",
                    p.name, "");
            }
            else if (*ecu.step != wanted)
            {
                const auto ns = ecu.step->count();
                add(Severity::Error, "step-mismatch", p.step_line,
                    prefix + "step_ms is " + std::to_string(*p.step_ms) + ", but the executable's step is " +
                        step_text(*ecu.step) +
                        (ns % 1'000'000 == 0 ? ": write step_ms: " + std::to_string(ns / 1'000'000) + ", or remove step_ms"
                                             : ": remove step_ms (it takes whole milliseconds)"),
                    p.name, "");
            }
        }
        std::set<std::string> reported;
        for (const auto& network : ecu.networks)
        {
            if (!reported.insert(network.name + "\n" + to_string(network.kind())).second)
            {
                continue;
            }
            const auto it = networks.find(network.name);
            if (it == networks.end())
            {
                const std::string guess = detail::closest(network.name, networkNames);
                add(Severity::Error, "unknown-network", p.line,
                    prefix + "the executable uses the network '" + network.name + "' (" + to_string(network.use) +
                        "), which is not in networks: add it to networks with kind: " + to_string(network.kind()) +
                        ", or correct the network's name in the ECU" +
                        (guess.empty() ? "" : " (did you mean '" + guess + "'?)"),
                    p.name, network.name);
            }
            else if (it->second->valid && it->second->kind != network.kind())
            {
                add(Severity::Error, "kind-mismatch", p.line,
                    prefix + "the executable uses the network '" + network.name + "' as " + to_string(network.kind()) +
                        " (" + to_string(network.use) + "), but networks says kind: " + to_string(it->second->kind) +
                        " (line " + std::to_string(it->second->line) + "): correct the kind, or the network in the ECU",
                    p.name, network.name);
            }
        }
        // The variables: whether vehicle_sil_launch run can read and write them (run.hpp, ControlLink).
        if (!result.ecu->description.variables.empty())
        {
            const auto& offer = result.ecu->description.control;
            if (!offer)
            {
                add(Severity::Note, "control-link-not-offered", p.line,
                    prefix + "it describes variables, but does not offer them to the simulation (--no-control, or an "
                             "executable of a vehicle-sil that does not yet): vehicle_sil_launch run cannot read or write "
                             "them",
                    p.name, "");
            }
            else if (offer->transport != "silkit" || offer->wire_version != static_cast<int>(control::wire::kVersion))
            {
                add(Severity::Warning, "control-link-version", p.line,
                    prefix + "it offers its variables as " + offer->transport + " messages of version " +
                        std::to_string(offer->wire_version) + ", and this vehicle_sil_launch speaks silkit messages of version " +
                        std::to_string(control::wire::kVersion) +
                        ", so vehicle_sil_launch run cannot read or write them: build the ECU and vehicle_sil_launch from the "
                        "same version of vehicle-sil",
                    p.name, "");
            }
        }
        if (!result.ecu->vehicle_sil_version.empty() && result.ecu->vehicle_sil_version != launcher_version)
        {
            add(Severity::Note, "version-mismatch", p.line,
                prefix + "the executable was built with vehicle-sil " + result.ecu->vehicle_sil_version +
                    ", vehicle_sil_launch with " + std::string(launcher_version) +
                    ": if they do not work together, build them from the same version",
                p.name, "");
        }
    }

    // Each network: who is on it, J1939 addresses and NAMEs, LIN masters.
    for (const auto& network : config.networks)
    {
        if (!network.valid)
        {
            continue;
        }
        const std::string prefix = "network '" + network.name + "': ";
        std::vector<std::string> users;
        std::vector<NodeOnNetwork> nodes;
        std::vector<std::string> masters;
        std::vector<std::string> slaves;
        for (std::size_t i = 0; i < config.participants.size() && i < results.size(); ++i)
        {
            if (!is_on_any(config.participants[i], results[i]))
            {
                continue;
            }
            const std::string& who = config.participants[i].name;
            std::vector<J1939NodeDescription> own;
            for (const auto& used : results[i].ecu->description.networks)
            {
                if (used.name != network.name || used.kind() != network.kind)
                {
                    continue;
                }
                if (std::find(users.begin(), users.end(), who) == users.end())
                {
                    users.push_back(who);
                }
                own.insert(own.end(), used.nodes.begin(), used.nodes.end());
                if (used.use == NetworkUse::LinMaster)
                {
                    masters.push_back(who);
                }
                else if (used.use == NetworkUse::LinSlave && std::find(slaves.begin(), slaves.end(), who) == slaves.end())
                {
                    slaves.push_back(who);
                }
            }
            for (std::size_t n = 0; n < own.size(); ++n)
            {
                nodes.push_back(
                    NodeOnNetwork{own.size() == 1 ? who : who + " (node " + std::to_string(n + 1) + ")", own[n]});
            }
        }
        const bool sayAll = !incomplete;
        const Severity weight = unchecked ? Severity::Note : Severity::Warning;
        if (users.empty())
        {
            if (sayAll)
            {
                add(weight, "unused-network", network.line,
                    prefix + "no participant uses it" + notCounted +
                        ": remove it, or check the network names in the ECUs",
                    "", network.name);
            }
            continue;
        }
        if (users.size() == 1 && sayAll)
        {
            add(weight, "single-participant", network.line,
                prefix + "only " + users.front() + " is on it, so what it sends there reaches no one" + notCounted +
                    ": add the other participants of this network, or remove it",
                "", network.name);
        }
        // J1939: the same address asked for twice (254: any free address), the same NAME twice.
        // What happens is what vehicle_sil::j1939 does: the lowest NAME wins the address claim; a
        // loser whose NAME is arbitrary address capable claims a free address, any other loser
        // has none (Cannot Claim); a node without a NAME does not claim and keeps sending from its
        // address.
        for (unsigned address = 0; address < j1939::kNullAddress; ++address)
        {
            std::vector<const NodeOnNetwork*> group;
            for (const auto& n : nodes)
            {
                if (n.node.address == address)
                {
                    group.push_back(&n);
                }
            }
            if (group.size() < 2)
            {
                continue;
            }
            std::vector<std::string> labels;
            std::vector<std::string> nameless;
            const NodeOnNetwork* winner = nullptr;
            for (const NodeOnNetwork* n : group)
            {
                labels.push_back(n->label);
                if (!n->node.name)
                {
                    nameless.push_back(n->label);
                }
                else if (winner == nullptr || *n->node.name < *winner->node.name)
                {
                    winner = n;
                }
            }
            std::vector<std::string> stuck;
            std::vector<std::string> movers;
            std::vector<std::string> lowest; // the nodes with the lowest NAME
            for (const NodeOnNetwork* n : group)
            {
                if (winner != nullptr && n->node.name && *n->node.name == *winner->node.name)
                {
                    lowest.push_back(n->label);
                }
                if (n != winner && n->node.name)
                {
                    (n->node.arbitrary_address_capable() ? movers : stuck).push_back(n->label);
                }
            }
            const std::string asked = prefix + and_list(labels) + (labels.size() == 2 ? " both" : " all") +
                                      " ask for the address " + address_text(static_cast<j1939::Address>(address));
            if (!nameless.empty())
            {
                add(Severity::Error, "address-conflict", network.line,
                    asked + ", and " + and_list(nameless) +
                        (nameless.size() == 1 ? " has no NAME, so it does not take part in the address claim and keeps"
                                              : " have no NAME, so they do not take part in the address claim and keep") +
                        " sending from that address: give each node its own address",
                    "", network.name);
            }
            else if (lowest.size() > 1)
            {
                // The same NAME: name-conflict says it, and that neither keeps the address.
            }
            else if (!stuck.empty())
            {
                add(Severity::Error, "address-conflict", network.line,
                    asked + "; " + winner->label + " wins the address claim (its NAME is the lowest), and " +
                        and_list(stuck) + (stuck.size() == 1 ? " is" : " are") +
                        " not arbitrary address capable, so it is left without an address (Cannot Claim): give each "
                        "node its own address",
                    "", network.name);
            }
            else
            {
                add(Severity::Warning, "address-shared", network.line,
                    asked + "; " + winner->label + " keeps it (its NAME is the lowest) and " + and_list(movers) +
                        " will claim another address: give each its own address to know where they are",
                    "", network.name);
            }
        }
        std::map<std::uint64_t, std::vector<const NodeOnNetwork*>> byName;
        for (const auto& n : nodes)
        {
            if (n.node.name)
            {
                byName[n.node.name->value()].push_back(&n);
            }
        }
        for (const auto& [value, same] : byName)
        {
            if (same.size() < 2)
            {
                continue;
            }
            std::vector<std::string> labels;
            bool oneAddress = same.front()->node.address != j1939::kNullAddress;
            for (const NodeOnNetwork* n : same)
            {
                labels.push_back(n->label);
                oneAddress = oneAddress && n->node.address == same.front()->node.address;
            }
            // An equal NAME loses the address claim: on one address, none of them keeps it.
            const std::string address =
                oneAddress ? ", and they ask for the same address " + address_text(same.front()->node.address) +
                                 ", which none of them keeps (an equal NAME loses the address claim)"
                           : std::string{};
            add(Severity::Error, "name-conflict", network.line,
                prefix + and_list(labels) + " have the same J1939 NAME " + vehicle_sil::detail::hex64(value) + address +
                    ": each node needs its own NAME (for a second copy of an ECU, change its identity number)",
                "", network.name);
        }
        // LIN: one master.
        std::vector<std::string> distinctMasters;
        for (const auto& m : masters)
        {
            if (std::find(distinctMasters.begin(), distinctMasters.end(), m) == distinctMasters.end())
            {
                distinctMasters.push_back(m);
            }
        }
        if (distinctMasters.size() > 1)
        {
            add(Severity::Error, "lin-masters", network.line,
                prefix + and_list(distinctMasters) + (distinctMasters.size() == 2 ? " are both" : " are all") +
                    " LIN masters, but a LIN network has one master: make all but one of them slaves",
                "", network.name);
        }
        else if (masters.size() > 1)
        {
            add(Severity::Error, "lin-masters", network.line,
                prefix + masters.front() + " is a LIN master on it " + std::to_string(masters.size()) +
                    " times, but a LIN network has one master: call lin_master() once for this network",
                "", network.name);
        }
        if (masters.empty() && !slaves.empty() && sayAll)
        {
            add(unchecked ? Severity::Note : Severity::Error, "lin-no-master", network.line,
                prefix + and_list(slaves) + (slaves.size() == 1 ? " is a LIN slave" : " are LIN slaves") +
                    ", but no participant is the master" + notCounted + ": add the participant that is the master",
                "", network.name);
        }
    }
    return out;
}

std::size_t CheckReport::errors() const
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [](const Finding& f) { return f.severity == Severity::Error; }));
}

std::size_t CheckReport::warnings() const
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [](const Finding& f) { return f.severity == Severity::Warning; }));
}

std::size_t CheckReport::notes() const { return findings.size() - errors() - warnings(); }

bool CheckReport::passed() const
{
    return !usage && config.has_value() && errors() == 0 && (!strict || warnings() == 0);
}

void sort_findings(std::vector<Finding>& findings)
{
    std::stable_sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) {
        if (a.severity != b.severity)
        {
            return a.severity < b.severity; // errors, warnings, notes
        }
        if (a.file != b.file)
        {
            return a.file.empty(); // the configuration's, then the scenario's
        }
        return a.line < b.line;
    });
}

std::string detail::check_text_body(const CheckReport& report)
{
    std::string out;
    if (report.config)
    {
        const SimulationConfig& config = *report.config;
        const auto disabled = static_cast<std::size_t>(std::count_if(
            config.participants.begin(), config.participants.end(), [](const ParticipantConfig& p) { return !p.enabled; }));
        out += report.file + ": " + count_text(config.participants.size(), "participant", "participants") +
               (disabled > 0 ? " (" + std::to_string(disabled) + " disabled)" : std::string{}) + ", " +
               count_text(config.networks.size(), "network", "networks") + "\n\n";
        std::vector<std::vector<std::string>> rows{{"participant", "step", "networks"}};
        for (std::size_t i = 0; i < config.participants.size(); ++i)
        {
            const ParticipantConfig& p = config.participants[i];
            const ParticipantResult result = i < report.participants.size() ? report.participants[i] : ParticipantResult{};
            std::vector<std::string> row{p.name.empty() ? "participants[" + std::to_string(i) + "]" : p.name};
            if (result.status == ParticipantStatus::Described && result.ecu)
            {
                const EcuDescription& ecu = result.ecu->description;
                row.push_back(ecu.step ? step_text(*ecu.step) : "real time");
                std::string cells;
                for (const auto& network : ecu.networks)
                {
                    cells += (cells.empty() ? "" : ", ") + network_cell(network);
                }
                row.push_back(cells.empty() ? "-" : cells);
            }
            else
            {
                row.push_back("-");
                row.push_back(result.status == ParticipantStatus::NotChecked ? "not checked (describe: false)"
                              : result.status == ParticipantStatus::Failed   ? "no description (see the errors)"
                              : result.status == ParticipantStatus::Disabled ? "disabled"
                                                                             : "not read (see the errors)");
            }
            rows.push_back(std::move(row));
        }
        if (!config.participants.empty())
        {
            out += table(rows) + "\n";
        }
        rows = {{"network", "kind", "participants"}};
        for (const auto& network : config.networks)
        {
            std::string users;
            for (std::size_t i = 0; i < config.participants.size() && i < report.participants.size(); ++i)
            {
                if (is_on(config.participants[i], report.participants[i], network))
                {
                    users += (users.empty() ? "" : ", ") + config.participants[i].name;
                }
            }
            rows.push_back({network.name, network.valid ? to_string(network.kind) : "-", users.empty() ? "-" : users});
        }
        if (!config.networks.empty())
        {
            out += table(rows) + "\n";
        }
    }
    if (report.scenario && report.config)
    {
        out += "scenario " + report.scenario->file + (report.scenario->name.empty() ? "" : " (" + report.scenario->name + ")") +
               ": " + count_text(report.scenario->steps, "step", "steps") + "\n\n";
    }
    out += detail::findings_text(report.file, report.findings);
    return out;
}

std::string detail::findings_text(const std::string& file, const std::vector<Finding>& findings)
{
    std::string out;
    for (const auto& finding : findings)
    {
        out += std::string(severity_name(finding.severity)) + ": " + (finding.file.empty() ? file : finding.file) +
               (finding.line > 0 ? ":" + std::to_string(finding.line) : std::string{}) + ": " + finding.message + "\n";
        if (!finding.detail.empty())
        {
            std::istringstream in(finding.detail);
            std::string line;
            while (std::getline(in, line))
            {
                out += "    " + line + "\n";
            }
        }
    }
    if (!findings.empty())
    {
        out += "\n";
    }
    return out;
}

std::string detail::count_summary(std::size_t errors, std::size_t warnings, std::size_t notes)
{
    return count_text(errors, "error", "errors") + ", " + count_text(warnings, "warning", "warnings") + ", " +
           count_text(notes, "note", "notes");
}

std::string detail::table_text(const std::vector<std::vector<std::string>>& rows) { return table(rows); }

const char* detail::severity_text(Severity severity) { return severity_name(severity); }

std::string format_text(const CheckReport& report)
{
    std::string out = detail::check_text_body(report);
    const std::string counts = count_text(report.errors(), "error", "errors") + ", " +
                               count_text(report.warnings(), "warning", "warnings") + ", " +
                               count_text(report.notes(), "note", "notes");
    if (!report.config)
    {
        out += "cannot read " + report.file + ": " + counts + "\n";
    }
    else if (report.passed())
    {
        out += "passed: " + counts + "\n";
    }
    else
    {
        out += std::string("failed: ") + counts + (report.errors() == 0 ? " (--strict)" : "") + "\n";
    }
    return out;
}

void detail::write_finding_members(vehicle_sil::detail::JsonWriter& w, const std::string& file, const Finding& finding)
{
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
    w.string("severity", severity_name(finding.severity));
    w.string("code", finding.code);
    optional_text("file", finding.file.empty() ? file : finding.file);
    if (finding.line > 0)
    {
        w.integer("line", static_cast<std::int64_t>(finding.line));
    }
    else
    {
        w.null("line");
    }
    optional_text("participant", finding.participant);
    optional_text("network", finding.network);
    w.string("message", finding.message);
    optional_text("detail", finding.detail);
}

void detail::write_findings_json(vehicle_sil::detail::JsonWriter& w, const std::string& file, const std::vector<Finding>& findings)
{
    w.begin_array("findings");
    for (const auto& finding : findings)
    {
        w.begin_object();
        write_finding_members(w, file, finding);
        w.end_object();
    }
    w.end_array();
}

std::string format_json(const CheckReport& report)
{
    vehicle_sil::detail::JsonWriter w;
    detail::write_check_json(w, {}, report);
    std::string out = w.take();
    out += '\n';
    return out;
}

void detail::write_check_json(vehicle_sil::detail::JsonWriter& w, std::string_view member, const CheckReport& report)
{
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
    const auto optional_integer = [&w](std::string_view key, const std::optional<std::int64_t>& value) {
        if (value)
        {
            w.integer(key, *value);
        }
        else
        {
            w.null(key);
        }
    };
    w.begin_object(member);
    w.integer("vehicle_sil_check", kCheckFormat);
    w.string("vehicle_sil", version());
    optional_text("file", report.file);
    w.string("result", report.usage     ? "usage"
                       : !report.config ? "unreadable"
                       : report.passed() ? "passed"
                                         : "failed");
    w.boolean("strict", report.strict);
    w.integer("errors", static_cast<std::int64_t>(report.errors()));
    w.integer("warnings", static_cast<std::int64_t>(report.warnings()));
    w.integer("notes", static_cast<std::int64_t>(report.notes()));
    optional_integer("stop_at_ms", report.config ? report.config->stop_at_ms : std::nullopt);
    optional_integer("start_timeout_ms",
                     report.config ? std::optional<std::int64_t>(report.config->start_timeout_ms) : std::nullopt);
    optional_integer("stop_timeout_ms",
                     report.config ? std::optional<std::int64_t>(report.config->stop_timeout_ms) : std::nullopt);
    optional_text("registry_uri", report.config ? report.config->registry_uri.value_or("") : std::string{});
    w.begin_array("participants");
    if (report.config)
    {
        for (std::size_t i = 0; i < report.config->participants.size(); ++i)
        {
            const ParticipantConfig& p = report.config->participants[i];
            const ParticipantResult result = i < report.participants.size() ? report.participants[i] : ParticipantResult{};
            w.begin_object();
            optional_text("name", p.name);
            w.integer("line", static_cast<std::int64_t>(p.line));
            w.begin_array("command");
            for (const auto& word : p.command)
            {
                w.string({}, word);
            }
            w.end_array();
            w.boolean("describe", p.describe);
            w.boolean("required", p.required);
            w.boolean("enabled", p.enabled);
            w.boolean("manual", p.manual);
            optional_integer("step_ms", p.step_ms);
            w.string("status", status_name(result.status));
            if (!result.ecu)
            {
                w.null("reported");
            }
            else
            {
                const EcuDescription& ecu = result.ecu->description;
                w.begin_object("reported");
                optional_text("vehicle_sil", result.ecu->vehicle_sil_version);
                optional_text("name", ecu.name);
                optional_integer("step_ns", ecu.step ? std::optional<std::int64_t>(ecu.step->count()) : std::nullopt);
                w.begin_array("networks");
                for (const auto& network : ecu.networks)
                {
                    w.begin_object();
                    w.string("name", network.name);
                    w.string("kind", to_string(network.kind()));
                    w.string("use", to_string(network.use));
                    w.begin_array("nodes");
                    for (const auto& node : network.nodes)
                    {
                        w.begin_object();
                        w.integer("address", node.address);
                        optional_text("j1939_name", node.name ? vehicle_sil::detail::hex64(node.name->value()) : "");
                        w.boolean("arbitrary_address_capable", node.arbitrary_address_capable());
                        w.end_object();
                    }
                    w.end_array();
                    w.end_object();
                }
                w.end_array();
                w.end_object();
            }
            if (result.ecu && result.ecu->variables_reported)
            {
                vehicle_sil::detail::write_variables(w, "variables", result.ecu->description.variables);
            }
            else
            {
                w.null("variables");
            }
            if (result.ecu && result.ecu->description.control)
            {
                w.begin_object("control");
                w.string("transport", result.ecu->description.control->transport);
                w.integer("wire_version", result.ecu->description.control->wire_version);
                w.end_object();
            }
            else
            {
                w.null("control");
            }
            w.end_object();
        }
    }
    w.end_array();
    w.begin_array("networks");
    if (report.config)
    {
        for (const auto& network : report.config->networks)
        {
            w.begin_object();
            w.string("name", network.name);
            optional_text("kind", network.valid ? to_string(network.kind) : "");
            w.integer("line", static_cast<std::int64_t>(network.line));
            w.begin_array("participants");
            for (std::size_t i = 0; i < report.config->participants.size() && i < report.participants.size(); ++i)
            {
                if (is_on(report.config->participants[i], report.participants[i], network))
                {
                    w.string({}, report.config->participants[i].name);
                }
            }
            w.end_array();
            w.end_object();
        }
    }
    w.end_array();
    detail::write_findings_json(w, report.file, report.findings);
    if (report.scenario)
    {
        // Only with --scenario: a result without one is as it was before scenarios.
        w.begin_object("scenario");
        w.string("file", report.scenario->file);
        optional_text("name", report.scenario->name);
        w.integer("steps", static_cast<std::int64_t>(report.scenario->steps));
        w.end_object();
    }
    w.end_object();
}

namespace {
/// The largest configuration file read.
constexpr std::uintmax_t kMaxConfigBytes = std::uintmax_t{16} << 20;
} // namespace

namespace {

/// --scenario: reads the scenario file and checks it against the participants of `report`.
void check_scenario_file(CheckReport& report, const CheckOptions& options)
{
    ScenarioCheck check;
    check.file = options.scenario_shown;
    const auto add = [&](Finding finding) {
        finding.file = check.file;
        report.findings.push_back(std::move(finding));
    };
    const std::filesystem::path& path = options.scenario;
    std::error_code ec;
    std::string problem;
    std::string text;
    if (!std::filesystem::exists(path, ec))
    {
        problem = "the scenario file does not exist: check the path given with --scenario";
    }
    else if (!std::filesystem::is_regular_file(path, ec))
    {
        problem = "this is not a regular file: name the scenario file with --scenario";
    }
    else if (std::filesystem::file_size(path, ec) > kMaxConfigBytes)
    {
        problem = "the file is larger than " + std::to_string(kMaxConfigBytes >> 20) + " MiB, too large for a scenario";
    }
    else if (std::ifstream in(path, std::ios::binary); !in)
    {
        problem = "cannot open the scenario file: check its permissions";
    }
    else
    {
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (!problem.empty())
    {
        add(Finding{Severity::Error, "file", problem, 0, "", ""});
    }
    else
    {
        ScenarioRead read = read_scenario(text, scenario_catalog(report));
        check.name = std::move(read.name);
        check.steps = read.steps;
        check.scenario = std::move(read.scenario);
        for (Finding& finding : read.findings)
        {
            add(std::move(finding));
        }
    }
    report.scenario = std::move(check);
}

} // namespace

CheckReport check_file(const std::filesystem::path& path, const std::string& shown_as, const CheckOptions& options)
{
    CheckReport report;
    report.file = shown_as;
    report.strict = options.strict;
    // Known without the file.
    if (auto conflicts = override_conflicts(options.overrides); !conflicts.empty())
    {
        report.usage = true;
        report.findings = std::move(conflicts);
        return report;
    }
    std::error_code ec;
    std::string problem;
    std::string text;
    if (!std::filesystem::exists(path, ec))
    {
        problem = "the file does not exist: check the path";
    }
    else if (std::filesystem::is_directory(path, ec))
    {
        problem = "this is a directory: name the configuration file in it";
    }
    else if (!std::filesystem::is_regular_file(path, ec))
    {
        problem = "this is not a regular file (a device or a pipe?): name the configuration file";
    }
    else if (std::filesystem::file_size(path, ec) > kMaxConfigBytes)
    {
        problem = "the file is larger than " + std::to_string(kMaxConfigBytes >> 20) +
                  " MiB, too large for a configuration: name the configuration file";
    }
    else if (std::ifstream in(path, std::ios::binary); !in)
    {
        problem = "cannot open the file: check its permissions";
    }
    else
    {
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (!problem.empty())
    {
        report.findings.push_back(Finding{Severity::Error, "file", problem, 0, "", ""});
        return report;
    }
    ConfigRead read = read_config(text, options.overrides);
    if (!read.override_errors.empty())
    {
        report.usage = true;
        report.findings = std::move(read.override_errors);
        return report;
    }
    report.findings = std::move(read.findings);
    report.config = std::move(read.config);
    if (!report.config)
    {
        return report;
    }
    const std::string listenVariable(kListenOnNetworkVariable);
    if (auto ignored = inherited_listen_variable(*report.config, std::getenv(listenVariable.c_str())))
    {
        report.findings.push_back(std::move(*ignored));
    }
    const std::filesystem::path base = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    for (const ParticipantConfig& p : report.config->participants)
    {
        ParticipantResult result;
        // Disabled whatever its entry's errors (they are findings).
        if (!p.enabled)
        {
            result.status = ParticipantStatus::Disabled;
            report.participants.push_back(result);
            continue;
        }
        if (!p.valid)
        {
            report.participants.push_back(result);
            continue;
        }
        // "{{word}}" in the path is the literal "{word}" (a valid entry has no other "{word}").
        const std::string written = expand_placeholders({p.command.front()}, "", "").front();
        const FoundExecutable found = find_executable(written, base);
        const std::filesystem::path& executable = found.path;
        if (auto bad = check_executable(p, executable))
        {
            report.findings.push_back(std::move(*bad));
            result.status = ParticipantStatus::Failed;
            report.participants.push_back(result);
            continue;
        }
        result.executable = executable;
        if (found.on_path)
        {
            report.findings.push_back(Finding{Severity::Note, "found-on-path",
                                              "participant '" + p.name + "': " + written +
                                                  " is not in the directory of the configuration file; it runs " +
                                                  process::shown_path(executable) + " (found on PATH)",
                                              p.command_line, p.name, ""});
        }
        if (!p.describe)
        {
            result.status = ParticipantStatus::NotChecked;
            report.participants.push_back(result);
            continue;
        }
        // "{{word}}" stands for "{word}" here too (an ecu_main command has no placeholders).
        std::vector<std::string> arguments =
            expand_placeholders(std::vector<std::string>(p.command.begin() + 1, p.command.end()), "", "");
        arguments.insert(arguments.end(), {"--describe", "--name", p.name});
        std::vector<std::string> shown{p.command.front()};
        shown.insert(shown.end(), arguments.begin(), arguments.end());
        DescribeRun run;
        run.command_text = command_text(shown);
        run.timeout = options.describe_timeout;
        try
        {
            // As vehicle_sil_launch run starts it: in the directory of the configuration file.
            process::Captured captured = process::run_captured(
                std::filesystem::absolute(executable), arguments,
                process::CaptureOptions{options.describe_timeout, 1u << 20, std::filesystem::absolute(base)});
            run.exit_code = captured.exit_code;
            run.signal = captured.signal;
            run.output_truncated = captured.output_truncated;
            run.error_truncated = captured.error_truncated;
            run.standard_output = std::move(captured.standard_output);
            run.standard_error = std::move(captured.standard_error);
        }
        catch (const process::StartError& e)
        {
            run.start_error = e.what();
            run.start_refused_by_system = e.cause() == process::StartError::Cause::System;
        }
        catch (const std::exception& e)
        {
            run.start_error = e.what();
            run.start_refused_by_system = true;
        }
        DescribeOutcome outcome = interpret_describe(p, run);
        if (outcome.error)
        {
            report.findings.push_back(std::move(*outcome.error));
            result.status = ParticipantStatus::Failed;
        }
        else
        {
            result.status = ParticipantStatus::Described;
            result.ecu = std::move(outcome.ecu);
        }
        report.participants.push_back(std::move(result));
    }
    auto compared = check_participants(*report.config, report.participants, version());
    report.findings.insert(report.findings.end(), compared.begin(), compared.end());
    // A run at the file's speed with the file's time_step_ms: known before it runs.
    if (report.config->speed && report.config->time_step_ms)
    {
        RunTiming timing = detail::plan_time_step(*report.config, report, report.config->speed);
        timing.speed = report.config->speed;
        if (auto f = detail::coarse_time_step(timing))
        {
            report.findings.push_back(std::move(*f));
        }
    }
    if (!options.scenario.empty())
    {
        check_scenario_file(report, options);
    }
    sort_findings(report.findings);
    return report;
}

} // namespace vehicle_sil::launch
