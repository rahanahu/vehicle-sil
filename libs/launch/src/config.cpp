// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/config.hpp"

#include "vehicle_sil/process/child_process.hpp"
#include "yaml_tree.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <system_error>

namespace vehicle_sil::launch {

namespace {

constexpr std::int64_t kMaxMillis = 1'000'000'000'000; // about 31 years, as ecu_main allows

const std::vector<std::string> kTopKeys = {"vehicle_sil_simulation", "stop_at_ms", "start_timeout_ms", "networks",
                                           "participants"};
const std::vector<std::string> kNetworkKeys = {"name", "kind"};
const std::vector<std::string> kParticipantKeys = {"name", "command", "step_ms", "describe", "required"};

std::string join(const std::vector<std::string>& items)
{
    std::string out;
    for (const auto& item : items)
    {
        out += (out.empty() ? "" : ", ") + item;
    }
    return out;
}

std::string lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool is_word_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// The length of the placeholder-like word "{word}" (word: letters, digits, '_') at `at`, or 0.
std::size_t braced_word_at(std::string_view text, std::size_t at)
{
    if (at >= text.size() || text[at] != '{')
    {
        return 0;
    }
    std::size_t end = at + 1;
    while (end < text.size() && is_word_char(text[end]))
    {
        ++end;
    }
    return end > at + 1 && end < text.size() && text[end] == '}' ? end - at + 1 : 0;
}

/// One piece of a command's text: as it is, "{{word}}" (written for the literal "{word}") or a
/// placeholder-like "{word}". Only these forms are special: any other brace is text.
struct BracePiece
{
    enum class Kind
    {
        Text,
        Escaped, ///< `text` is the "{word}" it stands for
        Word,    ///< `text` is "{word}"
    };
    Kind kind;
    std::string text;
};

std::vector<BracePiece> brace_pieces(std::string_view text)
{
    std::vector<BracePiece> out;
    const auto add_text = [&out](char c) {
        if (out.empty() || out.back().kind != BracePiece::Kind::Text)
        {
            out.push_back(BracePiece{BracePiece::Kind::Text, {}});
        }
        out.back().text += c;
    };
    for (std::size_t i = 0; i < text.size();)
    {
        if (text.substr(i, 2) == "{{")
        {
            const std::size_t n = braced_word_at(text, i + 1);
            if (n > 0 && i + 1 + n < text.size() && text[i + 1 + n] == '}')
            {
                out.push_back(BracePiece{BracePiece::Kind::Escaped, std::string(text.substr(i + 1, n))});
                i += n + 2;
                continue;
            }
        }
        if (const std::size_t n = braced_word_at(text, i); n > 0)
        {
            out.push_back(BracePiece{BracePiece::Kind::Word, std::string(text.substr(i, n))});
            i += n;
            continue;
        }
        add_text(text[i++]);
    }
    return out;
}

/// The placeholder-like words "{word}" in `text` (not those written "{{word}}").
std::vector<std::string> braced_words(std::string_view text)
{
    std::vector<std::string> out;
    for (const auto& piece : brace_pieces(text))
    {
        if (piece.kind == BracePiece::Kind::Word)
        {
            out.push_back(piece.text);
        }
    }
    return out;
}

bool is_placeholder(std::string_view word)
{
    return std::find(std::begin(kPlaceholders), std::end(kPlaceholders), word) != std::end(kPlaceholders);
}

/// The launcher argument that `argument` is (also "--name=x"), or "".
std::string_view launcher_argument(std::string_view argument)
{
    for (const std::string_view reserved : kLauncherArguments)
    {
        if (argument == reserved ||
            (argument.size() > reserved.size() && argument.substr(0, reserved.size()) == reserved &&
             argument[reserved.size()] == '='))
        {
            return reserved;
        }
    }
    return {};
}

/// Reads the tree of a configuration into a SimulationConfig, collecting the findings.
class Reader
{
public:
    Reader(const detail::YamlDocument& doc, std::vector<Finding>& findings) : doc_(doc), tree_(doc.tree()), findings_(findings) {}

    /// Empty if the file is not a configuration of a known version (the finding says why).
    std::optional<SimulationConfig> read()
    {
        const ryml::id_type root = doc_.root();
        if (root == ryml::NONE || !tree_.is_map(root))
        {
            unreadable("not-a-configuration", root == ryml::NONE ? 0 : doc_.line(root),
                       "the file is not a configuration: it must be a mapping with vehicle_sil_simulation: 1, networks "
                       "and participants (docs/simulation-config.md)");
            return std::nullopt;
        }
        // Anchors and aliases are not read as YAML means them (an alias would stay "*name"):
        // refused, so that no value is taken that the file does not say.
        std::vector<std::size_t> refs;
        find_references(root, refs);
        if (!refs.empty())
        {
            findings_.clear();
            for (const std::size_t line : refs)
            {
                error("unsupported-yaml", line,
                      "anchors and aliases (&name, *name, <<) are not read: write the values out (a value that "
                      "starts with * or & must be in quotes)");
            }
            return std::nullopt;
        }
        SimulationConfig config;
        if (const ryml::id_type second = doc_.second_document(); second != ryml::NONE)
        {
            error("unsupported-yaml", doc_.line(second),
                  "a second document (after ---): a file has one configuration; only the first is read, so remove the "
                  "others or put them in files of their own");
        }
        bool hasVersion = false;
        bool hasNetworks = false;
        bool hasParticipants = false;
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(root); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string key = detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, key, child, ""))
            {
                continue;
            }
            if (key == "vehicle_sil_simulation")
            {
                hasVersion = true;
                std::int64_t version = 0;
                if (plain_integer(child, version) && version != kSimulationFormat)
                {
                    unreadable("format-version", doc_.line(child),
                               "vehicle_sil_simulation is " + std::to_string(version) +
                                   ", but this vehicle_sil_launch reads format " + std::to_string(kSimulationFormat) +
                                   " only: use the vehicle-sil that the file was written for");
                    return std::nullopt;
                }
                if (!plain_integer(child, version))
                {
                    error("format-version", doc_.line(child),
                          "vehicle_sil_simulation must be the format version " + std::to_string(kSimulationFormat) +
                              ", not " + shown_value(child) + ": write vehicle_sil_simulation: " +
                              std::to_string(kSimulationFormat));
                }
            }
            else if (key == "stop_at_ms")
            {
                config.stop_at_ms = millis(child, "", key, "milliseconds of virtual time; leave stop_at_ms out to run until stopped");
            }
            else if (key == "start_timeout_ms")
            {
                config.start_timeout_ms =
                    millis(child, "", key, "milliseconds to wait for the participants").value_or(config.start_timeout_ms);
            }
            else if (key == "networks")
            {
                hasNetworks = true;
                read_networks(child, config);
            }
            else if (key == "participants")
            {
                hasParticipants = true;
                read_participants(child, config);
            }
            else
            {
                unknown_key(child, key, "", kTopKeys);
            }
        }
        if (!hasVersion)
        {
            error("missing-key", 0,
                  "the file has no vehicle_sil_simulation (the format version): add vehicle_sil_simulation: " +
                      std::to_string(kSimulationFormat));
        }
        if (!hasNetworks)
        {
            error("missing-key", 0,
                  "the file has no networks: add networks, a list of the networks with name and kind (can or lin)");
        }
        if (!hasParticipants)
        {
            error("missing-key", 0,
                  "the file has no participants: add participants, a list of the participants with name and command");
        }
        return config;
    }

private:
    /// The lines of the anchors and aliases under `node` (and of `node` itself), in order.
    void find_references(ryml::id_type node, std::vector<std::size_t>& lines) const
    {
        if (tree_.is_ref(node) || tree_.has_anchor(node) || (tree_.has_key(node) && tree_.key(node) == "<<"))
        {
            lines.push_back(doc_.line(node));
        }
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            find_references(child, lines);
        }
    }

    // --- findings ---

    void add(Severity severity, std::string code, std::size_t line, std::string message, std::string participant = {},
             std::string network = {})
    {
        findings_.push_back(Finding{severity, std::move(code), std::move(message), line, std::move(participant),
                                    std::move(network)});
    }
    void error(std::string code, std::size_t line, std::string message, std::string participant = {}, std::string network = {})
    {
        add(Severity::Error, std::move(code), line, std::move(message), std::move(participant), std::move(network));
    }
    void note(std::string code, std::size_t line, std::string message, std::string participant = {})
    {
        add(Severity::Note, std::move(code), line, std::move(message), std::move(participant));
    }
    void unreadable(std::string code, std::size_t line, std::string message)
    {
        findings_.clear();
        error(std::move(code), line, std::move(message));
    }

    /// false (and an error) if `key` was seen before in this mapping.
    bool first_occurrence(std::map<std::string, std::size_t>& seen, const std::string& key, ryml::id_type node,
                          const std::string& prefix)
    {
        const auto [it, inserted] = seen.emplace(key, doc_.line(node));
        if (!inserted)
        {
            error("duplicate-key", doc_.line(node),
                  prefix + "'" + key + "' appears twice (first at line " + std::to_string(it->second) +
                      "); keep one of them");
        }
        return inserted;
    }

    void unknown_key(ryml::id_type node, const std::string& key, const std::string& prefix,
                     const std::vector<std::string>& known)
    {
        const std::string guess = detail::closest(key, known);
        error("unknown-key", doc_.line(node),
              prefix + "unknown key '" + key + "': " +
                  (guess.empty() ? "remove it, or use one of the keys here (" + join(known) + ")"
                                 : "did you mean '" + guess + "'? (the keys here: " + join(known) + ")"));
    }

    // --- values ---

    bool is_scalar(ryml::id_type node) const { return tree_.has_val(node) && !tree_.is_container(node); }
    bool is_null(ryml::id_type node) const { return is_scalar(node) && tree_.val_is_null(node); }

    /// How a value is shown in a message: 'text', or what it is.
    std::string shown_value(ryml::id_type node) const
    {
        if (tree_.is_map(node))
        {
            return "a mapping";
        }
        if (tree_.is_seq(node))
        {
            return "a list";
        }
        if (is_null(node))
        {
            return "empty";
        }
        const std::string text = detail::to_string(tree_.val(node));
        return tree_.is_val_quoted(node) ? "the text \"" + text + "\"" : "'" + text + "'";
    }

    /// An unquoted decimal integer (with an optional '-').
    bool plain_integer(ryml::id_type node, std::int64_t& value) const
    {
        if (!is_scalar(node) || tree_.is_val_quoted(node) || is_null(node))
        {
            return false;
        }
        const std::string text = detail::to_string(tree_.val(node));
        const char* end = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(text.data(), end, value);
        return !text.empty() && ec == std::errc{} && ptr == end;
    }

    /// A whole number of milliseconds from 1 to kMaxMillis, or empty (and an error).
    std::optional<std::int64_t> millis(ryml::id_type node, const std::string& prefix, const std::string& key,
                                       const std::string& meaning, const std::string& participant = {})
    {
        std::int64_t value = 0;
        if (!plain_integer(node, value))
        {
            const bool quoted = is_scalar(node) && tree_.is_val_quoted(node);
            const std::string text = is_scalar(node) ? detail::to_string(tree_.val(node)) : std::string{};
            const bool digits = !quoted && !text.empty() && std::all_of(text.begin() + (text[0] == '-' ? 1 : 0), text.end(), [](char c) {
                return std::isdigit(static_cast<unsigned char>(c)) != 0;
            }) && text != "-";
            if (digits) // only too many digits for an integer
            {
                error("value", doc_.line(node),
                      prefix + key + " is " + text + ", but it must be from 1 to " + std::to_string(kMaxMillis) + " (" +
                          meaning + ")",
                      participant);
                return std::nullopt;
            }
            error("value", doc_.line(node),
                  prefix + key + " must be a whole number (" + meaning + "), not " + shown_value(node) +
                      (quoted ? ": write the number without quotes" : ""),
                  participant);
            return std::nullopt;
        }
        if (value < 1 || value > kMaxMillis)
        {
            error("value", doc_.line(node),
                  prefix + key + " is " + std::to_string(value) + ", but it must be from 1 to " +
                      std::to_string(kMaxMillis) + " (" + meaning + ")",
                  participant);
            return std::nullopt;
        }
        return value;
    }

    std::optional<bool> boolean(ryml::id_type node, const std::string& prefix, const std::string& key,
                                const std::string& participant)
    {
        if (is_scalar(node) && !tree_.is_val_quoted(node))
        {
            const std::string text = detail::to_string(tree_.val(node));
            if (text == "true" || text == "True" || text == "TRUE")
            {
                return true;
            }
            if (text == "false" || text == "False" || text == "FALSE")
            {
                return false;
            }
        }
        error("value", doc_.line(node), prefix + key + " must be true or false, not " + shown_value(node), participant);
        return std::nullopt;
    }

    /// A text that is not empty, or empty (and an error).
    std::optional<std::string> name(ryml::id_type node, const std::string& prefix, const std::string& what,
                                    const std::string& participant = {})
    {
        if (!is_scalar(node) || is_null(node) || tree_.val(node).empty())
        {
            error("value", doc_.line(node), prefix + "name must be the " + what + "'s name, not " + shown_value(node),
                  participant);
            return std::nullopt;
        }
        const std::string text = detail::to_string(tree_.val(node));
        const auto control = std::find_if(text.begin(), text.end(), [](char c) {
            return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
        });
        if (control != text.end() || !vehicle_sil::detail::is_utf8(text))
        {
            char code[8];
            std::snprintf(code, sizeof(code), "%02X", control != text.end() ? static_cast<unsigned char>(*control) : 0);
            error("value", doc_.line(node),
                  prefix + "the " + what + "'s name must be text on one line" +
                      (control != text.end() ? std::string(" (it has the control character U+00") + code + ")"
                                             : std::string(" (it is not UTF-8)")) +
                      ": write the name without it",
                  participant);
            return std::nullopt;
        }
        return text;
    }

    // --- networks ---

    void read_networks(ryml::id_type node, SimulationConfig& config)
    {
        if (!tree_.is_seq(node))
        {
            error("value", doc_.line(node),
                  "networks must be a list of networks (\"- name: <name>\" and \"kind: can\" or \"kind: lin\"), not " +
                      shown_value(node));
            return;
        }
        std::map<std::string, std::size_t> names;
        std::size_t index = 0;
        for (auto item = tree_.first_child(node); item != ryml::NONE; item = tree_.next_sibling(item), ++index)
        {
            const std::string at = "networks[" + std::to_string(index) + "]";
            if (!tree_.is_map(item))
            {
                error("value", doc_.line(item),
                      at + " must be a network with name and kind (can or lin), not " + shown_value(item));
                continue;
            }
            NetworkConfig network;
            network.line = doc_.line(item);
            const auto nameNode = tree_.find_child(item, "name");
            const auto name = nameNode == ryml::NONE ? std::nullopt : this->name(nameNode, at + ": ", "network");
            const std::string prefix = name ? "network '" + *name + "': " : at + ": ";
            bool kindRead = false;
            bool hasKind = false;
            std::map<std::string, std::size_t> seen;
            for (auto child = tree_.first_child(item); child != ryml::NONE; child = tree_.next_sibling(child))
            {
                const std::string key = detail::to_string(tree_.key(child));
                if (!first_occurrence(seen, key, child, prefix))
                {
                    continue;
                }
                if (key == "kind")
                {
                    if (const auto kind = read_kind(child, prefix, name.value_or("")))
                    {
                        network.kind = *kind;
                        kindRead = true;
                    }
                    hasKind = true;
                }
                else if (key != "name")
                {
                    unknown_key(child, key, prefix, kNetworkKeys);
                }
            }
            if (nameNode == ryml::NONE)
            {
                error("missing-key", network.line, at + " has no name: add name: <the network's name>");
            }
            if (!hasKind)
            {
                error("missing-key", network.line, prefix + "no kind: add kind: can or kind: lin", "", name.value_or(""));
            }
            if (!name)
            {
                continue;
            }
            const auto [it, inserted] = names.emplace(*name, network.line);
            if (!inserted)
            {
                error("duplicate-name", network.line,
                      "network '" + *name + "' is listed twice (first at line " + std::to_string(it->second) +
                          "); remove one, or give it another name",
                      "", *name);
                continue;
            }
            // A network without a valid kind stays in the list, so that the ECUs on it are not
            // reported again as on an unknown network; it is not checked.
            network.name = *name;
            network.valid = kindRead;
            config.networks.push_back(std::move(network));
        }
    }

    std::optional<NetworkKind> read_kind(ryml::id_type node, const std::string& prefix, const std::string& network)
    {
        const std::string text = is_scalar(node) && !is_null(node) ? detail::to_string(tree_.val(node)) : std::string{};
        if (text == "can")
        {
            return NetworkKind::Can;
        }
        if (text == "lin")
        {
            return NetworkKind::Lin;
        }
        const std::string low = lower(text);
        error("value", doc_.line(node),
              prefix + "kind is " + shown_value(node) + ": write " +
                  (low == "can" || low == "lin" ? low + " (lower case)" : "can or lin (the kinds of network there are)"),
              "", network);
        return std::nullopt;
    }

    // --- participants ---

    void read_participants(ryml::id_type node, SimulationConfig& config)
    {
        if (!tree_.is_seq(node))
        {
            error("value", doc_.line(node),
                  "participants must be a list of participants (\"- name: <name>\" and \"command: [<executable>]\"), not " +
                      shown_value(node));
            return;
        }
        if (tree_.num_children(node) == 0)
        {
            error("value", doc_.line(node), "participants is empty: list the participants of the simulation");
            return;
        }
        std::map<std::string, std::size_t> names;
        std::size_t index = 0;
        for (auto item = tree_.first_child(node); item != ryml::NONE; item = tree_.next_sibling(item), ++index)
        {
            const std::string at = "participants[" + std::to_string(index) + "]";
            if (!tree_.is_map(item))
            {
                error("value", doc_.line(item),
                      at + " must be a participant with name and command, not " + shown_value(item));
                continue;
            }
            ParticipantConfig participant;
            participant.line = doc_.line(item);
            participant.command_line = participant.line;
            participant.step_line = participant.line;
            const auto nameNode = tree_.find_child(item, "name");
            const auto name = nameNode == ryml::NONE ? std::nullopt : this->name(nameNode, at + ": ", "participant");
            const std::string who = name.value_or("");
            const std::string prefix = name ? "participant '" + *name + "': " : at + ": ";
            // Whether the entry can be described and compared: a name, a command, a known
            // describe; an unknown key or a bad step_ms does not stop that.
            bool usable = name.has_value();
            bool hasCommand = false;
            bool describeRead = true;
            std::map<std::string, std::size_t> seen;
            for (auto child = tree_.first_child(item); child != ryml::NONE; child = tree_.next_sibling(child))
            {
                const std::string key = detail::to_string(tree_.key(child));
                if (!first_occurrence(seen, key, child, prefix))
                {
                    continue;
                }
                if (key == "command")
                {
                    hasCommand = true;
                    participant.command_line = doc_.line(child);
                    read_command(child, prefix, who, participant);
                    usable = usable && !participant.command.empty();
                }
                else if (key == "step_ms")
                {
                    participant.step_line = doc_.line(child);
                    participant.step_ms = millis(child, prefix, key, "the step the executable reports", who);
                }
                else if (key == "describe")
                {
                    const auto describe = boolean(child, prefix, key, who);
                    participant.describe = describe.value_or(true);
                    describeRead = describe.has_value();
                    usable = usable && describeRead;
                }
                else if (key == "required")
                {
                    participant.required = boolean(child, prefix, key, who).value_or(true);
                }
                else if (key != "name")
                {
                    unknown_key(child, key, prefix, kParticipantKeys);
                }
            }
            if (nameNode == ryml::NONE)
            {
                error("missing-key", participant.line,
                      at + " has no name: add name: <the participant's name> (an ecu_main executable gets it with --name)");
            }
            else if (name && name->front() == '-')
            {
                error("value", doc_.line(nameNode),
                      prefix + "a participant's name must not start with '-' (the executable would read it as an option)",
                      who);
                usable = false;
            }
            else if (name && *name == kControllerName)
            {
                error("reserved-name", doc_.line(nameNode),
                      prefix + "the name " + std::string(kControllerName) +
                          " is the controller's, which vehicle_sil_launch runs: choose another name",
                      who);
                usable = false;
            }
            if (!hasCommand)
            {
                error("missing-key", participant.line, prefix + "no command: add command: [<path of the executable>]", who);
                usable = false;
            }
            // Which arguments are allowed depends on describe: not checked while it is unknown.
            if (!participant.command.empty() && describeRead)
            {
                usable = check_arguments(prefix, who, participant) && usable;
            }
            if (!participant.describe && participant.step_ms)
            {
                note("step-not-checked", participant.step_line,
                     prefix + "step_ms is not compared for a participant with describe: false (it does not report its "
                              "step): remove it, or set describe: true for an ecu_main executable",
                     who);
            }
            if (name)
            {
                const auto [it, inserted] = names.emplace(*name, participant.line);
                if (!inserted)
                {
                    error("duplicate-name", participant.line,
                          "participant '" + *name + "' is listed twice (first at line " + std::to_string(it->second) +
                              "); give each participant its own name (for a second copy of an ECU, e.g. " + *name + "_2)",
                          *name);
                    usable = false;
                }
            }
            participant.name = who;
            participant.valid = usable;
            config.participants.push_back(std::move(participant));
        }
    }

    void read_command(ryml::id_type node, const std::string& prefix, const std::string& who, ParticipantConfig& participant)
    {
        const std::size_t line = doc_.line(node);
        if (is_scalar(node) && !is_null(node))
        {
            participant.command.push_back(detail::to_string(tree_.val(node)));
        }
        else if (tree_.is_seq(node))
        {
            std::size_t index = 0;
            for (auto item = tree_.first_child(node); item != ryml::NONE; item = tree_.next_sibling(item), ++index)
            {
                if (is_scalar(item) && !is_null(item))
                {
                    participant.command.push_back(detail::to_string(tree_.val(item)));
                    continue;
                }
                // "- {name}" or "[..., {name}]" without quotes is a YAML mapping.
                std::string hint;
                if (tree_.is_map(item) && tree_.num_children(item) == 1)
                {
                    const std::string word = "{" + detail::to_string(tree_.key(tree_.first_child(item))) + "}";
                    hint = ": write \"" + word + "\" in quotes (without them, YAML reads {...} as a mapping)";
                }
                error("value", doc_.line(item),
                      prefix + "command[" + std::to_string(index) + "] must be a text, not " + shown_value(item) + hint, who);
                participant.command.clear();
                return;
            }
        }
        if (participant.command.empty() || participant.command.front().empty())
        {
            participant.command.clear();
            error("value", line,
                  prefix + "command must be the executable and its arguments, e.g. command: [engine/engine_ecu], not " +
                      shown_value(node),
                  who);
        }
    }

    /// The arguments after the executable: no launcher argument and no placeholder in an ecu_main
    /// command; only known placeholders (and {registry_uri}) in the others.
    /// false if an argument has an error.
    bool check_arguments(const std::string& prefix, const std::string& who, ParticipantConfig& participant)
    {
        const std::size_t line = participant.command_line;
        const std::size_t before = findings_.size();
        bool hasRegistry = false;
        for (const auto& word : braced_words(participant.command.front()))
        {
            const std::string inner = word.substr(1, word.size() - 2);
            error("value", line,
                  prefix + "the executable's path has " + word + ", but a path has no placeholders: write {{" + inner +
                      "}} for a literal " + word + ", or write the path",
                  who);
        }
        for (std::size_t i = 1; i < participant.command.size(); ++i)
        {
            const std::string& argument = participant.command[i];
            if (participant.describe)
            {
                if (const auto reserved = launcher_argument(argument); !reserved.empty())
                {
                    error("launcher-argument", line,
                          prefix + "the command has '" + argument + "', but vehicle_sil_launch gives an ecu_main executable " +
                              std::string(reserved) +
                              " itself: remove it (or set describe: false for an executable not made with ecu_main)",
                          who);
                }
            }
            for (const auto& word : braced_words(argument))
            {
                if (participant.describe && is_placeholder(word))
                {
                    error("launcher-argument", line,
                          prefix + "the command has " + word +
                              ", but only a command with describe: false has placeholders (vehicle_sil_launch gives an "
                              "ecu_main executable --name and --registry-uri itself): remove it, or set describe: false",
                          who);
                }
                else if (participant.describe)
                {
                    const std::string inner = word.substr(1, word.size() - 2);
                    error("unknown-placeholder", line,
                          prefix + "the command has " + word + ", but an ecu_main command has no placeholders: write {{" +
                              inner + "}} to pass " + word + " as it is",
                          who);
                }
                else if (!is_placeholder(word))
                {
                    // Compared without the braces.
                    std::vector<std::string> known;
                    for (const std::string_view placeholder : kPlaceholders)
                    {
                        known.emplace_back(placeholder.substr(1, placeholder.size() - 2));
                    }
                    const std::string inner = word.substr(1, word.size() - 2);
                    const std::string guess = detail::closest(inner, known);
                    error("unknown-placeholder", line,
                          prefix + "unknown placeholder " + word + " in the command" +
                              (guess.empty() ? "" : ": did you mean {" + guess + "}?") +
                              " (the placeholders: {registry_uri}, {name}; write {{" + inner + "}} to pass " + word +
                              " as it is)",
                          who);
                }
                hasRegistry = hasRegistry || word == "{registry_uri}";
            }
        }
        if (!participant.describe && !hasRegistry)
        {
            note("no-registry-uri", line,
                 prefix + "the command has no {registry_uri}, so the executable is not told where the simulation's "
                          "registry is: if it needs it, add its option for the registry URI with \"{registry_uri}\" "
                          "as the value",
                 who);
        }
        return std::none_of(findings_.begin() + static_cast<std::ptrdiff_t>(before), findings_.end(),
                            [](const Finding& f) { return f.severity == Severity::Error; });
    }

    const detail::YamlDocument& doc_;
    const ryml::Tree& tree_;
    std::vector<Finding>& findings_;
};

} // namespace

namespace detail {

std::size_t edit_distance(std::string_view a, std::string_view b)
{
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
    {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i)
    {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j)
        {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

std::string closest(std::string_view word, const std::vector<std::string>& candidates)
{
    const std::string low = lower(word);
    std::string best;
    std::size_t bestDistance = 0;
    for (const auto& candidate : candidates)
    {
        const std::string other = lower(candidate);
        // The same but for case, one a beginning of the other, or a few typing errors.
        const bool prefix = std::min(low.size(), other.size()) >= 3 &&
                            (other.rfind(low, 0) == 0 || low.rfind(other, 0) == 0);
        const std::size_t distance = prefix ? 0 : edit_distance(low, other);
        const std::size_t limit = other.size() <= 3 ? 1 : 2;
        if (distance <= limit && (best.empty() || distance < bestDistance))
        {
            best = candidate;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace detail

ConfigRead read_config(std::string_view text)
{
    ConfigRead out;
    if (text.substr(0, 2) == "\xFF\xFE" || text.substr(0, 2) == "\xFE\xFF" || text.find('\0') != std::string_view::npos)
    {
        out.findings.push_back(Finding{Severity::Error, "encoding",
                                       "the file is not UTF-8 text (it looks like UTF-16, or has NUL bytes): save it as "
                                       "UTF-8",
                                       0, "", ""});
        return out;
    }
    try
    {
        const detail::YamlDocument doc(text);
        if (doc.empty())
        {
            out.findings.push_back(Finding{Severity::Error, "not-a-configuration",
                                           "the file is empty: write vehicle_sil_simulation: 1, networks and participants "
                                           "(docs/simulation-config.md)",
                                           0, "", ""});
            return out;
        }
        out.config = Reader(doc, out.findings).read();
    }
    catch (const detail::YamlSyntaxError& e)
    {
        out.findings.push_back(Finding{Severity::Error, "syntax", "syntax error: " + e.message(), e.line(), "", "",
                                       e.context()});
    }
    return out;
}

std::filesystem::path resolve_executable(std::string_view executable, const std::filesystem::path& base_dir)
{
    std::filesystem::path path = process::path_from_utf8(executable);
    if (path.is_relative())
    {
        path = base_dir / path;
    }
#ifdef _WIN32
    std::error_code ec;
    if (lower(process::utf8_from_path(path.extension())) != ".exe" && !std::filesystem::exists(path, ec))
    {
        auto withExe = path;
        withExe += L".exe";
        if (std::filesystem::exists(withExe, ec))
        {
            return withExe;
        }
    }
#endif
    return path;
}

std::optional<Finding> check_executable(const ParticipantConfig& participant, const std::filesystem::path& path)
{
    const std::string shown = process::shown_path(path);
    const std::string prefix = "participant '" + participant.name + "': the executable " + participant.command.front();
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    std::string problem;
    const std::string& written = participant.command.front();
    const std::string extension = lower(process::utf8_from_path(path.extension()));
    if (!std::filesystem::exists(status))
    {
        std::string hint;
        if (participant.command.size() == 1 && written.find(' ') != std::string::npos)
        {
            hint = "; if the text after a space is an argument, write the command as a list, e.g. command: [" +
                   written.substr(0, written.find(' ')) + ", ...]";
        }
        else if (written.find_first_of("/\\") == std::string::npos)
        {
            hint = "; PATH is not searched: write the path of the executable";
        }
        problem = prefix + " does not exist (looked for " + shown +
                  "): build it, or correct the path (a relative path is from the directory of the configuration file)" + hint;
    }
    else if (extension == ".bat" || extension == ".cmd")
    {
        problem = prefix + " is a batch file (" + shown +
                  "), which runs through cmd.exe with other quoting rules: name the executable it starts";
    }
    else if (std::filesystem::is_directory(status))
    {
        problem = prefix + " is a directory (" + shown + "): name the executable file in it";
    }
#ifndef _WIN32
    else if ((status.permissions() & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                      std::filesystem::perms::others_exec)) == std::filesystem::perms::none)
    {
        problem = prefix + " is not executable (" + shown + "): make it executable (chmod +x), or name the right file";
    }
#endif
    if (problem.empty())
    {
        return std::nullopt;
    }
    return Finding{Severity::Error, "executable", problem, participant.command_line, participant.name, ""};
}

std::vector<std::string> expand_placeholders(const std::vector<std::string>& arguments, std::string_view registry_uri,
                                             std::string_view name)
{
    std::vector<std::string> out;
    out.reserve(arguments.size());
    for (const auto& argument : arguments)
    {
        std::string text;
        for (const auto& piece : brace_pieces(argument))
        {
            if (piece.kind == BracePiece::Kind::Word && piece.text == "{registry_uri}")
            {
                text += registry_uri;
            }
            else if (piece.kind == BracePiece::Kind::Word && piece.text == "{name}")
            {
                text += name;
            }
            else
            {
                text += piece.text;
            }
        }
        out.push_back(std::move(text));
    }
    return out;
}

} // namespace vehicle_sil::launch
