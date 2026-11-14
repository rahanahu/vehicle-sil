// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/config.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"

#include "vehicle_sil/process/child_process.hpp"
#include "yaml_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <numeric>
#include <system_error>

namespace vehicle_sil::launch {

namespace {

// The lists of keys (k...Keys) are what the message of an unknown key shows. Most readers take a key in a branch of
// their own (key == "..."), not from the list: a key with a new branch must be added to its list too, or the JSON
// Schema (schema/) and its test (tests/schema), which compare with the lists in the messages, pass without the key.
const std::vector<std::string> kTopKeys = {"vehicle_sil_simulation", "stop_at_ms", "start_timeout_ms", "stop_timeout_ms",
                                           "registry_uri", "time_step_ms", "speed", "late_tolerance_ms", "networks",
                                           "participants", "variants", "params"};
/// What a variant may write: its own keys, and the keys of one participant in it. A new thing a
/// variant can change gets its key here, a member of VariantParticipant (or VariantConfig), a
/// branch in read_variant() and a line in apply_variant().
const std::vector<std::string> kVariantKeys = {"participants", "params"};
const std::vector<std::string> kVariantParticipantKeys = {"command", "variables", "enabled"};
constexpr const char* kVariantsForm =
    "variants: { small: {}, medium: { participants: { Engine: { command: [./engine_medium] } }, params: { idle_rpm: "
    "650 } } }";

/// What a speed is, in messages.
constexpr const char* kSpeedMeaning =
    "simulation seconds per wall-clock second from 0.000001 to 1000000 (1: real time, 2: twice as fast, 0.5: half as "
    "fast), or max (as fast as possible)";
const std::vector<std::string> kNetworkKeys = {"name", "kind"};
const std::vector<std::string> kParticipantKeys = {"name",     "command", "step_ms", "describe",
                                                   "required", "enabled", "manual",  "variables"};
/// The keys of one mapping of a participant's variables (control::VariableMapping). A new form of
/// mapping gets its key here and in read_variable().
const std::vector<std::string> kVariableMapKeys = {"from", "scale", "offset", "map", "unit", "absent"};
constexpr const char* kVariableForms =
    "the executable's name of the variable (engine.speed: rpm), { from: rpm, scale: 0.1, offset: 0 } (the value seen "
    "is the executable's value * scale + offset), { from: gear_raw, map: { P: 0, R: 1 } } (names for its values) or "
    "{ absent: true } (this executable does not have it)";

using detail::join;

/// The kinds a network may have (kNetworkKinds), each between `before` and `after`: "can or lin",
/// "kind: can or kind: lin".
std::string kinds_text(const std::string& before = {}, const std::string& after = {})
{
    std::vector<std::string> names;
    for (const NetworkKind kind : kNetworkKinds)
    {
        names.push_back(before + to_string(kind) + after);
    }
    return detail::or_list(names);
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
class Reader : detail::YamlReader
{
public:
    Reader(const detail::YamlDocument& doc, std::vector<Finding>& findings) : YamlReader(doc, findings) {}

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
        std::size_t participantsLine = 0;
        std::size_t timeStepLine = 0;
        ryml::id_type variantsNode = ryml::NONE;
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(root); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string key = detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, key, child, ""))
            {
                continue;
            }
            if (key == "variants")
            {
                variantsNode = child; // read after the participants it changes
            }
            else if (key == "params")
            {
                config.params = read_params(child, "", ParamSource::Configuration);
            }
            else if (key == "vehicle_sil_simulation")
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
            else if (key == "stop_timeout_ms")
            {
                config.stop_timeout_ms =
                    millis(child, "", key, "milliseconds to wait for each participant to end").value_or(config.stop_timeout_ms);
            }
            else if (key == "registry_uri")
            {
                config.registry_uri = registry_uri(child);
                if (config.registry_uri && !is_loopback_registry_uri(*config.registry_uri))
                {
                    note("registry-on-network", doc_.line(child),
                         "registry_uri " + *config.registry_uri +
                             " is not a loopback address: the registry and the participants listen on the network, where "
                             "anyone who reaches this computer can join the simulation (SIL Kit does not authenticate); run "
                             "gives the participants it starts " + std::string(kListenOnNetworkVariable) +
                             "=1 and shows it with the command of a participant you start yourself: set it for every "
                             "participant on this computer that connects to the registry at a loopback address; to keep "
                             "the simulation on this computer, use 127.0.0.1 or leave registry_uri out");
                }
            }
            else if (key == "time_step_ms")
            {
                config.time_step_ms =
                    millis(child, "", key, "milliseconds of virtual time between two steps of the launcher's controller");
                timeStepLine = doc_.line(child);
            }
            else if (key == "speed")
            {
                config.speed = speed(child);
            }
            else if (key == "late_tolerance_ms")
            {
                config.late_tolerance_ms =
                    millis(child, "", key, "wall-clock milliseconds a step may begin late before it counts as late");
            }
            else if (key == "networks")
            {
                hasNetworks = true;
                read_networks(child, config);
            }
            else if (key == "participants")
            {
                hasParticipants = true;
                participantsLine = doc_.line(child);
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
                  "the file has no networks: add networks, a list of the networks with name and kind (" + kinds_text() +
                      ")");
        }
        if (!hasParticipants)
        {
            error("missing-key", 0,
                  "the file has no participants: add participants, a list of the participants with name and command");
        }
        if (config.time_step_ms && config.stop_at_ms && *config.stop_at_ms % *config.time_step_ms != 0)
        {
            error("value", timeStepLine,
                  "stop_at_ms (" + std::to_string(*config.stop_at_ms) + ") is not a multiple of time_step_ms (" +
                      std::to_string(*config.time_step_ms) +
                      "): the launcher's controller can stop the simulation only at one of its steps; make stop_at_ms a "
                      "multiple of time_step_ms, or leave time_step_ms out");
        }
        config.participants_line = participantsLine;
        if (variantsNode != ryml::NONE)
        {
            read_variants(variantsNode, config);
        }
        return config;
    }

private:
    void note(std::string code, std::size_t line, std::string message, std::string participant = {})
    {
        add(Severity::Note, std::move(code), line, std::move(message), std::move(participant));
    }
    void unreadable(std::string code, std::size_t line, std::string message)
    {
        findings_.clear();
        error(std::move(code), line, std::move(message));
    }

    // --- values ---

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

    /// A speed (parse_speed()); empty for "max" and for an error (with the error).
    std::optional<double> speed(ryml::id_type node)
    {
        const bool quoted = is_scalar(node) && tree_.is_val_quoted(node);
        const std::string text = is_scalar(node) && !is_null(node) ? detail::to_string(tree_.val(node)) : std::string{};
        std::optional<double> value;
        if (is_scalar(node) && !is_null(node) && parse_speed(text, value) && (!quoted || !value))
        {
            return value;
        }
        error("value", doc_.line(node),
              "speed must be " + std::string(kSpeedMeaning) + ", not " + shown_value(node) +
                  (quoted && value ? ": write the number without quotes" : ""));
        return std::nullopt;
    }

    /// "silkit://<host>:<port>", or empty (and an error).
    std::optional<std::string> registry_uri(ryml::id_type node)
    {
        const std::string text = is_scalar(node) && !is_null(node) ? detail::to_string(tree_.val(node)) : std::string{};
        const std::string scheme = "silkit://";
        const auto colon = text.rfind(':');
        std::int64_t port = -1;
        const bool hasPort = colon != std::string::npos && colon > scheme.size() &&
                             std::from_chars(text.data() + colon + 1, text.data() + text.size(), port).ptr ==
                                 text.data() + text.size() &&
                             colon + 1 < text.size();
        const bool plain = std::none_of(text.begin(), text.end(), [](char c) {
            return static_cast<unsigned char>(c) <= 0x20 || c == 0x7F;
        });
        if (text.rfind(scheme, 0) != 0 || !hasPort || port < 1 || port > 65535 || !plain)
        {
            error("value", doc_.line(node),
                  "registry_uri must be where the registry of the simulation listens, silkit://<host>:<port> with a port "
                  "from 1 to 65535 (e.g. silkit://127.0.0.1:8500, or silkit://0.0.0.0:8500 for other computers too), not " +
                      shown_value(node) + "; leave registry_uri out for a free port on 127.0.0.1");
            return std::nullopt;
        }
        return text;
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
                  "networks must be a list of networks (\"- name: <name>\" and " + kinds_text("\"kind: ", "\"") + "), not " +
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
                      at + " must be a network with name and kind (" + kinds_text() + "), not " + shown_value(item));
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
                error("missing-key", network.line, prefix + "no kind: add " + kinds_text("kind: "), "", name.value_or(""));
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
        if (const auto kind = network_kind_named(text))
        {
            return kind;
        }
        const std::string low = lower(text);
        error("value", doc_.line(node),
              prefix + "kind is " + shown_value(node) + ": write " +
                  (network_kind_named(low) ? low + " (lower case)" : kinds_text() + " (the kinds of network there are)"),
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
            std::size_t requiredLine = 0;
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
                    requiredLine = doc_.line(child);
                    participant.required = boolean(child, prefix, key, who).value_or(true);
                }
                else if (key == "enabled")
                {
                    const auto enabled = boolean(child, prefix, key, who);
                    participant.enabled = enabled.value_or(true);
                    // Not known whether to leave it out: neither described nor started.
                    usable = usable && enabled.has_value();
                }
                else if (key == "manual")
                {
                    participant.manual = boolean(child, prefix, key, who).value_or(false);
                }
                else if (key == "variables")
                {
                    read_variables(child, prefix, who, participant);
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
            if (participant.describe && describeRead && !participant.required)
            {
                error("not-required-ecu", requiredLine,
                      prefix + "required: false is only for executables with describe: false: an ecu_main executable joins "
                               "as a coordinated participant, which SIL Kit starts only among the required participants; "
                               "remove required: false (to run the simulation without this ECU, write enabled: false "
                               "instead, or give vehicle_sil_launch --disable " + (who.empty() ? std::string("<name>") : who) + ")",
                      who);
            }
            if (!participant.describe && participant.step_ms)
            {
                note("step-not-checked", participant.step_line,
                     prefix + "step_ms is not compared for a participant with describe: false (it does not report its "
                              "step): remove it, or set describe: true for an ecu_main executable",
                     who);
            }
            if (!participant.describe && !participant.variables.empty())
            {
                note("variable-map-not-checked", participant.variable_lines.front(),
                     prefix + "variables is not compared for a participant with describe: false (the launcher does not "
                              "know its variables before the run): the mappings apply when its control agent connects, "
                              "and vehicle_sil_launch run reports those that do not fit",
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

    // --- variants ---

    void read_variants(ryml::id_type node, SimulationConfig& config)
    {
        if (!tree_.is_map(node) || tree_.num_children(node) == 0)
        {
            error("value", doc_.line(node),
                  std::string("variants must be a mapping of variant names to what each changes of the common part (") +
                      kVariantsForm + "), not " + (tree_.is_map(node) ? std::string("an empty mapping") : shown_value(node)) +
                      "; for a simulation without variants, remove variants");
            return;
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            VariantConfig variant;
            variant.name = detail::to_string(tree_.key(child));
            variant.line = doc_.line(child);
            if (!first_occurrence(seen, variant.name, child, "variants: "))
            {
                continue;
            }
            const bool plain = std::none_of(variant.name.begin(), variant.name.end(), [](char c) {
                return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
            });
            if (variant.name.empty() || variant.name.front() == '-' || !plain)
            {
                error("value", variant.line,
                      "variants: a variant's name must be a text on one line that does not start with '-' (it is given "
                      "with --variant), not '" + variant.name + "'");
                continue;
            }
            read_variant(child, "variant '" + variant.name + "': ", config, variant);
            config.variants.push_back(std::move(variant));
        }
    }

    /// What one variant changes. A variant with an error stays in the list (its name is known).
    void read_variant(ryml::id_type node, const std::string& prefix, const SimulationConfig& config, VariantConfig& variant)
    {
        if (is_null(node))
        {
            return; // "small:" is the common part as it is, as "small: {}"
        }
        if (!tree_.is_map(node))
        {
            error("value", doc_.line(node),
                  prefix + "must be a mapping of what the variant changes (" + join(kVariantKeys) +
                      "), or {} for the common part as it is, not " + shown_value(node));
            return;
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string key = detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, key, child, prefix))
            {
                continue;
            }
            if (key == "participants")
            {
                read_variant_participants(child, prefix, config, variant);
            }
            else if (key == "params")
            {
                variant.params = read_params(child, prefix, ParamSource::Variant);
            }
            else
            {
                unknown_key(child, key, prefix, kVariantKeys);
            }
        }
    }

    void read_variant_participants(ryml::id_type node, const std::string& prefix, const SimulationConfig& config,
                                   VariantConfig& variant)
    {
        if (!tree_.is_map(node))
        {
            error("value", doc_.line(node),
                  prefix + "participants must be a mapping of the participants' names to what the variant changes of each "
                           "(participants: { Engine: { command: [./engine_medium] } }), not " + shown_value(node));
            return;
        }
        std::vector<std::string> names;
        for (const auto& p : config.participants)
        {
            if (!p.name.empty())
            {
                names.push_back(p.name);
            }
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            VariantParticipant change;
            change.name = detail::to_string(tree_.key(child));
            change.line = doc_.line(child);
            if (!first_occurrence(seen, change.name, child, prefix + "participants: "))
            {
                continue;
            }
            const auto base = std::find_if(config.participants.begin(), config.participants.end(),
                                           [&change](const ParticipantConfig& p) { return !p.name.empty() && p.name == change.name; });
            if (base == config.participants.end())
            {
                const std::string guess = detail::closest(change.name, names);
                error("variant-participant", change.line,
                      prefix + "participants has '" + change.name + "', but the file's participants has no participant with "
                               "this name" + (guess.empty() ? "" : " (did you mean '" + guess + "'?)") +
                          ": a variant changes the participants of the common part (for a participant of this variant "
                          "only, list it there with enabled: false and write enabled: true here)" +
                          (names.empty() ? std::string{} : "; the participants are " + join(names)));
                continue;
            }
            const std::string who = change.name;
            const std::string at = prefix + "participant '" + who + "': ";
            if (!tree_.is_map(child))
            {
                error("value", change.line,
                      at + "must be a mapping of what the variant changes of it (" + join(kVariantParticipantKeys) + "), not " +
                          shown_value(child),
                      who);
                continue;
            }
            std::map<std::string, std::size_t> keys;
            for (auto part = tree_.first_child(child); part != ryml::NONE; part = tree_.next_sibling(part))
            {
                const std::string key = detail::to_string(tree_.key(part));
                if (!first_occurrence(keys, key, part, at))
                {
                    continue;
                }
                if (key == "command")
                {
                    // Read and checked as the common one, for the participant's describe.
                    ParticipantConfig with;
                    with.name = who;
                    with.describe = base->describe;
                    with.command_line = doc_.line(part);
                    read_command(part, at, who, with);
                    change.command_line = with.command_line;
                    change.valid = !with.command.empty() && check_arguments(at, who, with);
                    if (!with.command.empty())
                    {
                        change.command = std::move(with.command);
                    }
                }
                else if (key == "variables")
                {
                    ParticipantConfig with;
                    read_variables(part, at, who, with);
                    change.variables = std::move(with.variables);
                    change.variable_lines = std::move(with.variable_lines);
                }
                else if (key == "enabled")
                {
                    change.enabled = boolean(part, at, key, who);
                }
                else
                {
                    unknown_key(part, key, at, kVariantParticipantKeys);
                }
            }
            variant.participants.push_back(std::move(change));
        }
    }

    // --- a participant's variable map ---

    void read_variables(ryml::id_type node, const std::string& prefix, const std::string& who, ParticipantConfig& participant)
    {
        if (!tree_.is_map(node))
        {
            error("value", doc_.line(node),
                  prefix + "variables must be a mapping of the names the simulation uses to the executable's variables "
                           "(engine.speed: rpm), not " + shown_value(node),
                  who);
            return;
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string name = detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, name, child, prefix + "variables: "))
            {
                continue;
            }
            if (auto mapping = read_variable(child, name, prefix + "variables: " + name + ": ", who))
            {
                participant.variables.push_back(std::move(*mapping));
                participant.variable_lines.push_back(doc_.line(child));
            }
        }
    }

    /// One mapping, or empty (and an error) if it is not one of the forms.
    std::optional<control::VariableMapping> read_variable(ryml::id_type node, const std::string& name,
                                                          const std::string& prefix, const std::string& who)
    {
        const std::size_t line = doc_.line(node);
        control::VariableMapping out;
        out.name = name;
        if (name.empty())
        {
            error("value", line, prefix + "the name the simulation uses for the variable is empty: write it before the ':'", who);
            return std::nullopt;
        }
        if (is_scalar(node) && !is_null(node) && !tree_.val(node).empty())
        {
            out.from = detail::to_string(tree_.val(node));
            return out;
        }
        if (!tree_.is_map(node))
        {
            error("value", line, prefix + "must be " + kVariableForms + ", not " + shown_value(node), who);
            return std::nullopt;
        }
        bool ok = true;
        const auto bad = [&](ryml::id_type at, const std::string& text) {
            error("value", doc_.line(at), prefix + text, who);
            ok = false;
        };
        std::map<std::string, std::size_t> seen;
        bool absentWritten = false;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string key = detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, key, child, prefix))
            {
                ok = false;
            }
            else if (key == "from")
            {
                if (!is_scalar(child) || is_null(child) || tree_.val(child).empty())
                {
                    bad(child, "from must be the executable's name of the variable, not " + shown_value(child));
                    continue;
                }
                out.from = detail::to_string(tree_.val(child));
            }
            else if (key == "scale" || key == "offset")
            {
                double value = 0;
                if (!plain_number(child, value) || (key == "scale" && value == 0.0))
                {
                    bad(child, key + " must be a number" + (key == "scale" ? " other than 0" : "") +
                                   " (the value seen is the executable's value * scale + offset), not " + shown_value(child));
                    continue;
                }
                (key == "scale" ? out.scale : out.offset) = value;
            }
            else if (key == "map")
            {
                out.map = read_value_names(child, prefix, who);
                ok = ok && out.map.has_value();
            }
            else if (key == "unit")
            {
                if (!is_scalar(child))
                {
                    bad(child, "unit must be the unit of the value seen (a text such as rpm), not " + shown_value(child));
                    continue;
                }
                out.unit = is_null(child) ? std::string{} : detail::to_string(tree_.val(child));
            }
            else if (key == "absent")
            {
                absentWritten = true;
                // Only true: a variable that is there needs no entry.
                const auto absent = boolean(child, prefix, key, who);
                if (!absent)
                {
                    ok = false;
                }
                else if (!*absent)
                {
                    bad(child, "absent: false says nothing: remove it (a variable that is not listed is seen under its "
                               "own name)");
                }
                out.absent = true;
            }
            else
            {
                unknown_key(child, key, prefix, kVariableMapKeys);
                ok = false;
            }
        }
        if (!ok)
        {
            return std::nullopt;
        }
        if (absentWritten && seen.size() > 1)
        {
            bad(node, "absent: true says that this executable does not have the variable, so it takes no other key: "
                      "remove the others, or absent");
        }
        else if (!absentWritten && out.from.empty())
        {
            bad(node, "it does not name the executable's variable: add from: <the executable's name of it>");
        }
        else if (out.map && out.linear())
        {
            bad(node, "scale and offset cannot be combined with map: use one of the two (scale and offset convert a "
                      "number, map names the values of an integer or an enum)");
        }
        return ok ? std::optional<control::VariableMapping>(std::move(out)) : std::nullopt;
    }

    /// The names of a variable's values ("map": { P: 0, R: 1 }), or empty (and an error).
    std::optional<std::vector<control::EnumItem>> read_value_names(ryml::id_type node, const std::string& prefix,
                                                                   const std::string& who)
    {
        if (!tree_.is_map(node) || tree_.num_children(node) == 0)
        {
            error("value", doc_.line(node),
                  prefix + "map must give the names of the executable's values ({ P: 0, R: 1 }), not " + shown_value(node), who);
            return std::nullopt;
        }
        std::vector<control::EnumItem> items;
        std::map<std::string, std::size_t> seen;
        bool ok = true;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            control::EnumItem item;
            item.name = detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, item.name, child, prefix + "map: "))
            {
                ok = false;
                continue;
            }
            if (item.name.empty() || !plain_integer(child, item.value))
            {
                error("value", doc_.line(child),
                      prefix + "map: " + (item.name.empty() ? "a name is empty" : item.name + " must be the executable's value "
                                                                                  "for that name, a whole number, not " +
                                                                                      shown_value(child)),
                      who);
                ok = false;
                continue;
            }
            const auto same = std::find_if(items.begin(), items.end(),
                                           [&](const control::EnumItem& e) { return e.value == item.value; });
            if (same != items.end())
            {
                error("value", doc_.line(child),
                      prefix + "map: " + same->name + " and " + item.name + " are both " + std::to_string(item.value) +
                          ": give each value one name",
                      who);
                ok = false;
                continue;
            }
            items.push_back(std::move(item));
        }
        return ok ? std::optional<std::vector<control::EnumItem>>(std::move(items)) : std::nullopt;
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

namespace {

Finding usage(std::string message) { return Finding{Severity::Error, "usage", std::move(message), 0, "", ""}; }

/// Applies --disable and --enable to `config` and returns their usage errors (then nothing is
/// applied). Names the file does not have are errors only if `check_names`; else they are ignored.
std::vector<Finding> apply_overrides(SimulationConfig& config, const EnabledOverrides& overrides, bool check_names)
{
    std::vector<Finding> errors = override_conflicts(overrides);
    std::vector<std::string> names;
    for (const auto& p : config.participants)
    {
        if (!p.name.empty() && std::find(names.begin(), names.end(), p.name) == names.end())
        {
            names.push_back(p.name);
        }
    }
    std::vector<std::string> reported;
    const auto unknown = [&](const char* option, const std::string& name) {
        const std::string key = std::string(option) + " " + name;
        if (!check_names || std::find(names.begin(), names.end(), name) != names.end() ||
            std::find(reported.begin(), reported.end(), key) != reported.end())
        {
            return;
        }
        reported.push_back(key);
        const std::string guess = detail::closest(name, names);
        errors.push_back(usage(key + ": the file has no participant with this name" +
                               (guess.empty() ? "" : " (did you mean '" + guess + "'?)") + "; its participants are " +
                               join(names)));
    };
    for (const auto& name : overrides.disable)
    {
        unknown("--disable", name);
    }
    for (const auto& name : overrides.enable)
    {
        unknown("--enable", name);
    }
    for (const auto& name : overrides.manual)
    {
        unknown("--manual", name);
    }
    if (!errors.empty())
    {
        return errors;
    }
    for (auto& p : config.participants)
    {
        const auto named = [&p](const std::vector<std::string>& list) {
            return std::find(list.begin(), list.end(), p.name) != list.end();
        };
        if (named(overrides.disable))
        {
            p.enabled = false;
        }
        else if (named(overrides.enable))
        {
            p.enabled = true;
            p.left_out_by_variant = false;
        }
        if (named(overrides.manual))
        {
            p.manual = true;
        }
    }
    return errors;
}

/// Lays `variant` over the common part of `config`: every key a variant may write (see
/// kVariantParticipantKeys).
void apply_variant(SimulationConfig& config, const VariantConfig& variant)
{
    config.variant = variant.name;
    for (const VariantParticipant& change : variant.participants)
    {
        const auto p = std::find_if(config.participants.begin(), config.participants.end(),
                                    [&change](const ParticipantConfig& c) { return c.name == change.name; });
        if (p == config.participants.end())
        {
            continue;
        }
        if (change.command)
        {
            p->command = *change.command;
            p->command_line = change.command_line;
        }
        p->valid = p->valid && change.valid;
        for (std::size_t i = 0; i < change.variables.size(); ++i)
        {
            const auto same = std::find_if(p->variables.begin(), p->variables.end(), [&](const control::VariableMapping& m) {
                return m.name == change.variables[i].name;
            });
            if (same == p->variables.end())
            {
                p->variables.push_back(change.variables[i]);
                p->variable_lines.push_back(change.variable_lines[i]);
            }
            else
            {
                p->variable_lines[static_cast<std::size_t>(same - p->variables.begin())] = change.variable_lines[i];
                *same = change.variables[i];
            }
        }
        if (change.enabled)
        {
            p->enabled = *change.enabled;
            p->left_out_by_variant = !*change.enabled;
        }
    }
    for (const ParamValue& param : variant.params)
    {
        const auto same = std::find_if(config.params.begin(), config.params.end(),
                                       [&param](const ParamValue& c) { return c.name == param.name; });
        if (same == config.params.end())
        {
            config.params.push_back(param);
        }
        else
        {
            *same = param;
        }
    }
}

/// Applies --variant to `config` and returns its usage errors. A file that defines variants needs
/// one of them: no --variant and a name that is none of them are errors whatever else is wrong
/// with the text (checking the common part alone would be a check of no variant). A --variant
/// for a file without variants is an error only if `check_names` (as for apply_overrides()).
std::vector<Finding> choose_variant(SimulationConfig& config, const std::optional<std::string>& variant, bool check_names)
{
    std::vector<std::string> names;
    for (const auto& v : config.variants)
    {
        names.push_back(v.name);
    }
    if (!variant)
    {
        if (names.empty())
        {
            return {};
        }
        return {usage("the file defines variants, and a check or a run is of one of them: give --variant <name> with one of " +
                      join(names) + " (none of them is the default)")};
    }
    const auto chosen = std::find_if(config.variants.begin(), config.variants.end(),
                                     [&variant](const VariantConfig& v) { return v.name == *variant; });
    if (chosen != config.variants.end())
    {
        apply_variant(config, *chosen);
        return {};
    }
    if (names.empty())
    {
        if (!check_names)
        {
            return {}; // the errors of the text may be why no variant could be read
        }
        return {usage("--variant " + *variant +
                      ": the file defines no variants: leave --variant out, or add variants to the file "
                      "(docs/simulation-config.md)")};
    }
    const std::string guess = detail::closest(*variant, names);
    return {usage("--variant " + *variant + ": the file has no variant with this name" +
                  (guess.empty() ? "" : " (did you mean '" + guess + "'?)") + "; its variants are " + join(names) +
                  // Not said by anything else: a usage error stands alone.
                  (check_names ? "" : "; the file has errors too, which a check with one of these variants reports"))};
}

/// The simulation waits for its required participants: at least one enabled participant must be
/// required.
void check_required(const SimulationConfig& config, std::vector<Finding>& findings)
{
    if (config.participants.empty() || std::any_of(config.participants.begin(), config.participants.end(),
                                                   [](const ParticipantConfig& p) { return p.enabled && p.required; }))
    {
        return;
    }
    // The disabled participants that would be waited for: enabling one of them helps.
    std::vector<std::string> disabled;
    bool anyEnabled = false;
    for (const auto& p : config.participants)
    {
        anyEnabled = anyEnabled || p.enabled;
        if (!p.enabled && p.required && !p.name.empty())
        {
            disabled.push_back(p.name);
        }
    }
    std::string message;
    if (!anyEnabled)
    {
        message = "every participant is disabled (enabled: false, or --disable), so the simulation has no participant "
                  "to wait for: enable at least one (enabled: true, or --enable <name>)";
    }
    else if (disabled.empty())
    {
        message = "every participant has required: false, but the simulation starts when its required participants "
                  "have joined, so it needs at least one: remove required: false from the participants it must wait "
                  "for (an ecu_main executable is always required)";
    }
    else
    {
        message = "every enabled participant has required: false (disabled: " + join(disabled) +
                  "), but the simulation starts when its required participants have joined, so it needs at least one: "
                  "enable a required participant, or remove required: false from the participants it must wait for "
                  "(an ecu_main executable is always required)";
    }
    findings.push_back(Finding{Severity::Error, "no-required-participant", std::move(message), config.participants_line,
                               "", ""});
}

} // namespace

std::vector<Finding> override_conflicts(const EnabledOverrides& overrides)
{
    std::vector<Finding> out;
    std::vector<std::string> seen;
    for (const auto& name : overrides.enable)
    {
        if (std::find(overrides.disable.begin(), overrides.disable.end(), name) != overrides.disable.end() &&
            std::find(seen.begin(), seen.end(), name) == seen.end())
        {
            seen.push_back(name);
            out.push_back(usage("--disable and --enable both name " + name + ": give one of them"));
        }
    }
    for (const auto& name : overrides.manual)
    {
        if (std::find(overrides.disable.begin(), overrides.disable.end(), name) != overrides.disable.end() &&
            std::find(seen.begin(), seen.end(), name) == seen.end())
        {
            seen.push_back(name);
            out.push_back(usage("--disable and --manual both name " + name +
                                ": give one of them (--disable leaves it out, --manual has you start it)"));
        }
    }
    return out;
}

const char* param_source_name(ParamSource source)
{
    switch (source)
    {
    case ParamSource::Scenario: return "scenario";
    case ParamSource::Configuration: return "configuration";
    case ParamSource::Variant: return "variant";
    case ParamSource::CommandLine: return "command_line";
    }
    return "configuration";
}

const char* param_source_text(ParamSource source)
{
    switch (source)
    {
    case ParamSource::Scenario: return "the scenario's params";
    case ParamSource::Configuration: return "the configuration's params";
    case ParamSource::Variant: return "the variant's params";
    case ParamSource::CommandLine: return "--param";
    }
    return "the configuration's params";
}

bool is_param_name(std::string_view name)
{
    return !name.empty() && std::isdigit(static_cast<unsigned char>(name.front())) == 0 &&
           std::all_of(name.begin(), name.end(), [](char c) { return is_word_char(c); });
}

bool waits_for_manual(const SimulationConfig& config)
{
    return std::any_of(config.participants.begin(), config.participants.end(),
                       [](const ParticipantConfig& p) { return p.enabled && p.required && p.manual; });
}

bool registry_on_network(const SimulationConfig& config)
{
    return config.registry_uri && !is_loopback_registry_uri(*config.registry_uri);
}

std::string_view listen_variable_value(const SimulationConfig& config)
{
    return registry_on_network(config) ? "1" : "0";
}

std::optional<Finding> inherited_listen_variable(const SimulationConfig& config, const char* inherited)
{
    const std::string_view given = listen_variable_value(config);
    if (inherited == nullptr || given == inherited)
    {
        return std::nullopt;
    }
    const std::string name(kListenOnNetworkVariable);
    return Finding{Severity::Note, "listen-variable-ignored",
                   name + "=" + inherited + " in the environment of vehicle_sil_launch is not used: " +
                       (registry_on_network(config)
                            ? "registry_uri " + *config.registry_uri + " is not a loopback address, so run gives the participants it starts " +
                                  name + "=1 and they listen on every address"
                            : "the registry is on a loopback address, so run gives the participants it starts " + name +
                                  "=0 and they listen on the loopback address only; to let other computers join, write "
                                  "registry_uri with an address that they reach (e.g. silkit://0.0.0.0:8500)"),
                   0, "", ""};
}

ConfigRead read_config(std::string_view text, const EnabledOverrides& overrides)
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
        if (out.config)
        {
            const bool textHasErrors = std::any_of(out.findings.begin(), out.findings.end(),
                                                   [](const Finding& f) { return f.severity == Severity::Error; });
            out.override_errors = choose_variant(*out.config, overrides.variant, !textHasErrors);
            if (out.override_errors.empty())
            {
                out.override_errors = apply_overrides(*out.config, overrides, !textHasErrors);
            }
            check_required(*out.config, out.findings);
        }
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

FoundExecutable find_executable(std::string_view executable, const std::filesystem::path& base_dir)
{
    FoundExecutable found{resolve_executable(executable, base_dir), false};
    std::error_code ec;
    const auto runnable = [&ec](const std::filesystem::path& candidate) {
        if (!std::filesystem::is_regular_file(candidate, ec))
        {
            return false;
        }
#ifndef _WIN32
        const auto perms = std::filesystem::status(candidate, ec).permissions();
        return (perms & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                         std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
#else
        return true;
#endif
    };
    // Next to the file only an executable file counts (a data file of the same name does not
    // hide the program on PATH).
    if (runnable(found.path) || executable.empty() || executable.find_first_of("/\\") != std::string_view::npos)
    {
        return found;
    }
#ifdef _WIN32
    const wchar_t* variable = _wgetenv(L"PATH");
    const std::wstring list = variable != nullptr ? variable : L"";
    constexpr wchar_t kSeparator = L';';
#else
    const char* variable = std::getenv("PATH");
    const std::string list = variable != nullptr ? variable : "";
    constexpr char kSeparator = ':';
#endif
    std::size_t begin = 0;
    while (begin <= list.size())
    {
        const std::size_t end = std::min(list.find(kSeparator, begin), list.size());
        const std::filesystem::path directory(list.substr(begin, end - begin));
        begin = end + 1;
        if (directory.empty() || directory.is_relative())
        {
            continue; // a relative entry would depend on the directory the launcher runs in
        }
        const std::filesystem::path candidate = resolve_executable(executable, directory);
        if (runnable(candidate))
        {
            return FoundExecutable{candidate, true};
        }
    }
    return found;
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
            hint = "; it is not on PATH either";
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

std::int64_t time_step_ms(const SimulationConfig& config)
{
    if (config.time_step_ms)
    {
        return *config.time_step_ms;
    }
    if (config.stop_at_ms)
    {
        return std::gcd(kDefaultTimeStepMs, *config.stop_at_ms);
    }
    return kDefaultTimeStepMs;
}

std::int64_t late_tolerance_ms(const SimulationConfig& config)
{
    return config.late_tolerance_ms.value_or(kDefaultLateToleranceMs);
}

bool parse_speed(std::string_view text, std::optional<double>& speed)
{
    if (text == "max")
    {
        speed.reset();
        return true;
    }
    // A plain decimal number: digits, at most one '.', an exponent; no sign, no "inf" or "nan".
    const bool plain = !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+';
    }) && std::isdigit(static_cast<unsigned char>(text.front())) != 0;
    double value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (!plain || ec != std::errc{} || ptr != text.data() + text.size() || !std::isfinite(value) || value < kMinSpeed ||
        value > kMaxSpeed)
    {
        return false;
    }
    speed = value;
    return true;
}

std::string speed_text(const std::optional<double>& speed)
{
    if (!speed)
    {
        return "max";
    }
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *speed);
    return std::string(buffer.data(), result.ptr);
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
