// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/participant_runtime.hpp"

#include "participant_runtime_checks.hpp"
#include "participant_runtime_probe.hpp"

#include <silkit/services/orchestration/all.hpp>
#include <silkit/services/orchestration/string_utils.hpp>

#ifdef VEHICLE_SIL_CORE_HAS_RAPIDYAML
#include <rapidyaml.hpp>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil {
namespace {

namespace orch = SilKit::Services::Orchestration;

bool is_terminal_or_stopping(orch::ParticipantState state) noexcept
{
    switch (state)
    {
    case orch::ParticipantState::Stopping:
    case orch::ParticipantState::Stopped:
    case orch::ParticipantState::Error:
    case orch::ParticipantState::ShuttingDown:
    case orch::ParticipantState::Shutdown:
    case orch::ParticipantState::Aborting:
        return true;
    default:
        return false;
    }
}

/// Only tests set one (participant_runtime_probe.hpp).
std::atomic<const detail::StartProbe*> g_start_probe{nullptr};

void start_probe(detail::StartProbePoint point)
{
    if (const auto* probe = g_start_probe.load(std::memory_order_acquire))
    {
        (*probe)(point);
    }
}

} // namespace

namespace detail {

void set_start_probe(const StartProbe* probe) noexcept
{
    g_start_probe.store(probe, std::memory_order_release);
}

std::string step_width_error(std::chrono::nanoseconds given, SimDuration own, SimTime now)
{
    if (given == own)
    {
        return {};
    }
    // SIL Kit cut the step (dynamic simulation steps).
    return "SIL Kit gave this participant a step of " + std::to_string(given.count()) + " ns at t=" + std::to_string(to_ns(now)) +
           " ns, not its own " + std::to_string(std::chrono::nanoseconds(own).count()) +
           " ns: this participant's SIL Kit configuration asks for dynamic simulation steps itself, or does not opt out "
           "while another participant asks for them. Set "
           R"("Experimental":{"TimeSynchronization":{"DynamicSimulationStep":false}})"
           " in that configuration, or pass none (the default configuration has it)";
}

} // namespace detail

namespace {

// The one place that says what vehicle-sil changes of SIL Kit's defaults (participant_runtime.hpp
// says why).

/// The host of "silkit://<host>:<port>...", without the brackets of an IPv6 address; empty if
/// `uri` is not of that form.
std::string_view registry_host(std::string_view uri) noexcept
{
    constexpr std::string_view scheme = "silkit://";
    if (uri.substr(0, scheme.size()) != scheme)
    {
        return {};
    }
    uri.remove_prefix(scheme.size());
    if (!uri.empty() && uri.front() == '[')
    {
        const auto end = uri.find(']');
        return end == std::string_view::npos ? std::string_view{} : uri.substr(1, end - 1);
    }
    return uri.substr(0, uri.find_first_of(":/"));
}

bool equals_ignoring_case(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

/// An IPv4 address in one of the numeric forms that inet_aton() and getaddrinfo() read: one to four
/// parts, each decimal, octal (leading 0) or hexadecimal (0x), the last one filling the rest
/// ("127.1" is 127.0.0.1, "2130706433" too). Empty if `host` is not one.
std::optional<std::uint32_t> parse_ipv4(std::string_view host) noexcept
{
    std::uint64_t parts[4] = {};
    std::size_t count = 0;
    while (true)
    {
        if (count == 4)
        {
            return std::nullopt;
        }
        const auto dot = host.find('.');
        std::string_view part = host.substr(0, dot);
        int base = 10;
        if (part.size() > 1 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X'))
        {
            base = 16;
            part.remove_prefix(2);
        }
        else if (part.size() > 1 && part[0] == '0')
        {
            base = 8;
            part.remove_prefix(1);
        }
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value, base);
        if (part.empty() || error != std::errc{} || end != part.data() + part.size() || value > 0xFFFFFFFFULL)
        {
            return std::nullopt;
        }
        parts[count++] = value;
        if (dot == std::string_view::npos)
        {
            break;
        }
        host.remove_prefix(dot + 1);
    }
    std::uint64_t address = 0;
    for (std::size_t i = 0; i + 1 < count; ++i)
    {
        if (parts[i] > 255)
        {
            return std::nullopt;
        }
        address |= parts[i] << (8 * (3 - i));
    }
    const std::uint64_t last = parts[count - 1];
    if (last > (0xFFFFFFFFULL >> (8 * (count - 1))))
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(address | last);
}

/// The groups of an IPv6 address in text form ("::1", "0:0:0:0:0:0:0:1", "::ffff:127.0.0.1"; a zone
/// "%..." is left out). Empty if `host` is not one.
std::optional<std::array<std::uint16_t, 8>> parse_ipv6(std::string_view host)
{
    host = host.substr(0, host.find('%'));
    // The groups of one side of "::"; a dotted IPv4 address may end the address.
    const auto groups_of = [](std::string_view text, bool last, std::vector<std::uint16_t>& out) {
        if (text.empty())
        {
            return true;
        }
        while (true)
        {
            const auto colon = text.find(':');
            const std::string_view part = text.substr(0, colon);
            if (colon == std::string_view::npos && last && part.find('.') != std::string_view::npos)
            {
                if (std::count(part.begin(), part.end(), '.') != 3)
                {
                    return false;
                }
                const auto v4 = parse_ipv4(part);
                if (!v4)
                {
                    return false;
                }
                out.push_back(static_cast<std::uint16_t>(*v4 >> 16));
                out.push_back(static_cast<std::uint16_t>(*v4 & 0xFFFF));
                return true;
            }
            unsigned value = 0;
            const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value, 16);
            if (part.empty() || part.size() > 4 || error != std::errc{} || end != part.data() + part.size())
            {
                return false;
            }
            out.push_back(static_cast<std::uint16_t>(value));
            if (colon == std::string_view::npos)
            {
                return true;
            }
            text.remove_prefix(colon + 1);
        }
    };
    std::vector<std::uint16_t> head;
    std::vector<std::uint16_t> tail;
    const auto gap = host.find("::");
    if (gap == std::string_view::npos)
    {
        if (!groups_of(host, true, head) || head.size() != 8)
        {
            return std::nullopt;
        }
    }
    else if (host.find("::", gap + 1) != std::string_view::npos || !groups_of(host.substr(0, gap), false, head) ||
             !groups_of(host.substr(gap + 2), true, tail) || head.size() + tail.size() > 7)
    {
        return std::nullopt;
    }
    std::array<std::uint16_t, 8> groups{};
    std::copy(head.begin(), head.end(), groups.begin());
    std::copy(tail.begin(), tail.end(), groups.end() - static_cast<std::ptrdiff_t>(tail.size()));
    return groups;
}

/// ::1.
bool is_ipv6_loopback(const std::array<std::uint16_t, 8>& groups) noexcept
{
    return std::all_of(groups.begin(), groups.end() - 1, [](std::uint16_t g) { return g == 0; }) && groups[7] == 1;
}

/// The value of kListenOnNetworkVariable: true for "1", false when it is not set, empty or "0".
bool listen_on_network_from_environment()
{
    const std::string name(kListenOnNetworkVariable);
    const char* value = std::getenv(name.c_str());
    if (value == nullptr || *value == '\0' || std::string_view(value) == "0")
    {
        return false;
    }
    if (std::string_view(value) == "1")
    {
        return true;
    }
    throw std::invalid_argument(name + " must be 1 (listen on every address) or 0 (listen on the loopback address when "
                                       "the registry URI is one), not '" + value + "'");
}

std::string json_string(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (static_cast<unsigned char>(c) < 0x20)
        {
            constexpr char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
            out += kHex[static_cast<unsigned char>(c) & 0xF];
        }
        else
        {
            out += c;
        }
    }
    out += '"';
    return out;
}

#ifndef _WIN32
/// "local://<temporary directory>/vsil-<16 random hex digits>.silkit", or empty if there is no
/// temporary directory. SIL Kit's own name for it is made from the participant's name and the
/// registry URI, which this does not have; a random one is as unique.
std::string new_local_acceptor_uri()
{
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error || directory.empty())
    {
        return {};
    }
    std::random_device device;
    const std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    char name[32];
    std::snprintf(name, sizeof name, "vsil-%016llx.silkit", static_cast<unsigned long long>(value));
    return "local://" + (directory / name).string();
}
#endif

/// The acceptors (Middleware.AcceptorUris) of a participant that is given none; empty for SIL
/// Kit's own (every address): a registry URI that is not a loopback address, or
/// `listen_on_network` (kListenOnNetworkVariable when that is empty).
std::vector<std::string> default_acceptor_uris(std::string_view registry_uri, std::optional<bool> listen_on_network)
{
    if (!is_loopback_registry_uri(registry_uri) || (listen_on_network ? *listen_on_network : listen_on_network_from_environment()))
    {
        return {};
    }
    std::vector<std::string> acceptors;
#ifndef _WIN32
    if (std::string local = new_local_acceptor_uri(); !local.empty())
    {
        acceptors.push_back(std::move(local));
    }
#endif
    // The registry's address family: an IPv6 registry URI may be all that this computer has.
    const auto v6 = parse_ipv6(registry_host(registry_uri));
    acceptors.emplace_back(v6 && is_ipv6_loopback(*v6) ? "tcp://[::1]:0" : "tcp://127.0.0.1:0");
    return acceptors;
}

/// The members of "Middleware", without the braces.
std::string middleware_members(std::string_view registry_uri, std::optional<bool> listen_on_network)
{
    std::string members;
#ifdef _WIN32
    members = R"("EnableDomainSockets":false)";
#endif
    if (const auto acceptors = default_acceptor_uris(registry_uri, listen_on_network); !acceptors.empty())
    {
        members += members.empty() ? R"("AcceptorUris":[)" : R"(,"AcceptorUris":[)";
        for (std::size_t i = 0; i < acceptors.size(); ++i)
        {
            members += (i == 0 ? "" : ",") + json_string(acceptors[i]);
        }
        members += ']';
    }
    return members;
}

#ifdef VEHICLE_SIL_CORE_HAS_RAPIDYAML
/// What vehicle-sil adds to a configuration of the caller's that leaves it out.
struct AddedDefaults
{
    std::vector<std::string> acceptor_uris;   ///< Middleware.AcceptorUris; empty: not added
    bool no_domain_sockets = false;           ///< Middleware.EnableDomainSockets: false
    bool no_dynamic_simulation_step = false;  ///< Experimental.TimeSynchronization.DynamicSimulationStep: false
    [[nodiscard]] bool any() const { return !acceptor_uris.empty() || no_domain_sockets || no_dynamic_simulation_step; }
};

/// rapidyaml cannot read a text, or refuses a change of its tree. Never leaves this file.
struct YamlError
{
    std::string message;    ///< rapidyaml's
    std::size_t line = 0;   ///< the place in the text, counted from 1; 0 if the error is not about a place in it
    std::size_t column = 0;
    /// " (at line <n>, column <n> of the configuration text)", or nothing.
    [[nodiscard]] std::string place() const
    {
        return line == 0 ? std::string()
                         : " (at line " + std::to_string(line) + ", column " + std::to_string(column) + " of the configuration text)";
    }
};

/// A YAML or JSON text as rapidyaml (the parser that SIL Kit reads its configurations with)
/// reads it: to see what its members are, and to set members in it while what the caller wrote
/// stays as it is written. Everything that rapidyaml refuses here is a YamlError.
class ConfigurationText
{
public:
    explicit ConfigurationText(std::string_view text) : handler_(callbacks()), parser_(&handler_)
    {
        tree_ = ryml::parse_in_arena(&parser_, ryml::csubstr(text.data(), text.size())); // as SIL Kit does, JSON too
        tree_.callbacks(callbacks()); // the checks of the changes below: not rapidyaml's own handler, which writes to stderr
        root_ = tree_.root_id();
        if (tree_.is_stream(root_) && tree_.has_children(root_))
        {
            root_ = tree_.first_child(root_); // "---": SIL Kit reads the first document
        }
    }
    ConfigurationText(const ConfigurationText&) = delete;
    ConfigurationText& operator=(const ConfigurationText&) = delete;

    /// false for a text without members at its top (only comments, or "~").
    [[nodiscard]] bool has_members() const { return tree_.is_map(root_); }
    [[nodiscard]] ryml::id_type root() const { return root_; }
    [[nodiscard]] bool is_map(ryml::id_type node) const { return node != ryml::NONE && tree_.is_map(node); }

    /// The member `key` of `parent`; ryml::NONE if there is none, or `parent` is no mapping (or ryml::NONE).
    [[nodiscard]] ryml::id_type member(ryml::id_type parent, ryml::csubstr key) const
    {
        return is_map(parent) ? tree_.find_child(parent, key) : ryml::NONE;
    }

    /// The scalar that the member `key` of `parent` is; empty if there is no such member, or it is no scalar.
    [[nodiscard]] std::optional<std::string> scalar(ryml::id_type parent, ryml::csubstr key) const
    {
        const ryml::id_type node = member(parent, key);
        if (node == ryml::NONE || !tree_.has_val(node))
        {
            return std::nullopt;
        }
        const ryml::csubstr value = tree_.val(node);
        return std::string(value.str, value.len); // (nullptr, 0) is the empty string
    }

    /// The mapping `key` of the mapping `parent`, made if it is not there or has nothing under it
    /// (SIL Kit reads "Experimental:" with nothing, or only comments, under it as left out).
    /// `key` must outlive this.
    ryml::id_type map_at(ryml::id_type parent, ryml::csubstr key)
    {
        ryml::id_type node = tree_.find_child(parent, key);
        if (node != ryml::NONE && tree_.is_map(node))
        {
            return node;
        }
        if (node != ryml::NONE && !is_without_value(node))
        {
            throw std::invalid_argument("the SIL Kit participant configuration has a " + std::string(key.str, key.len) +
                                        " that is not a mapping; write its members under it (" +
                                        std::string(key.str, key.len) + ":, then one member a line)");
        }
        node = remade(parent, key);
        tree_.to_map(node, key);
        return node;
    }

    /// Sets the member `key` of the mapping `parent` to the plain scalar `value`. Both must outlive this.
    void set(ryml::id_type parent, ryml::csubstr key, ryml::csubstr value)
    {
        tree_.to_keyval(remade(parent, key), key, value);
    }

    /// Sets the member `key` of the mapping `parent` to the list of the strings `values`. `key` must outlive this.
    void set(ryml::id_type parent, ryml::csubstr key, const std::vector<std::string>& values)
    {
        const ryml::id_type node = remade(parent, key);
        tree_.to_seq(node, key);
        for (const auto& value : values)
        {
            append_string(node, value);
        }
    }

    /// Adds the string `value` to the list `key` of the mapping `parent`, made if it is not there
    /// or has nothing under it. false, and nothing changed, if the member is something else than
    /// a list (a single string, a mapping). `key` must outlive this.
    [[nodiscard]] bool add(ryml::id_type parent, ryml::csubstr key, const std::string& value)
    {
        ryml::id_type node = tree_.find_child(parent, key);
        if (node == ryml::NONE || !tree_.is_seq(node))
        {
            if (node != ryml::NONE && !is_without_value(node))
            {
                return false;
            }
            node = remade(parent, key);
            tree_.to_seq(node, key);
        }
        append_string(node, value);
        return true;
    }

    [[nodiscard]] std::string text() const { return ryml::emitrs_yaml<std::string>(tree_); }

private:
    [[noreturn]] static void on_error(const char* message, std::size_t length, ryml::Location location, void*)
    {
        // A place in the text has no file name. A place with one is in rapidyaml's own source (a
        // check that failed): the path on the computer that built it says nothing to the reader.
        const bool in_text = location.name.empty() && location;
        throw YamlError{std::string(message, length), in_text ? location.line : 0, in_text ? location.col : 0};
    }
    static ryml::Callbacks callbacks() { return ryml::Callbacks(nullptr, nullptr, nullptr, &on_error); }

    /// A member that is written with nothing as its value ("Key:", "Key: ~", "Key: null").
    [[nodiscard]] bool is_without_value(ryml::id_type node) const
    {
        return !tree_.has_children(node) && !tree_.is_container(node) && tree_.has_val(node) && tree_.val_is_null(node);
    }

    /// The member `key` of `parent` as a new node at the same place, without type, value or
    /// children; a new last member if there is none. rapidyaml turns only such a node into a
    /// mapping, a list or a scalar: one that has a value (an empty one too) must be made anew.
    ryml::id_type remade(ryml::id_type parent, ryml::csubstr key)
    {
        const ryml::id_type node = tree_.find_child(parent, key);
        if (node == ryml::NONE)
        {
            return tree_.append_child(parent);
        }
        const ryml::id_type before = tree_.prev_sibling(node);
        tree_.remove(node);
        return tree_.insert_child(parent, before);
    }

    void append_string(ryml::id_type list, const std::string& value)
    {
        tree_.to_val(tree_.append_child(list), tree_.to_arena(ryml::csubstr(value.data(), value.size())), ryml::VAL_DQUO);
    }

    ryml::EventHandlerTree handler_;
    ryml::Parser parser_;
    ryml::Tree tree_;
    ryml::id_type root_ = ryml::NONE;
};

/// For what should not happen: rapidyaml does not take a text that SIL Kit (the same parser) took,
/// or SIL Kit's own JSON, or refuses a change. `detail` is rapidyaml's message.
[[noreturn]] void throw_cannot_add_defaults(std::string_view participant_name, std::string_view what, const std::string& detail)
{
    throw std::runtime_error(
        "could not " + std::string(what) + " the SIL Kit configuration of the participant \"" + std::string(participant_name) +
        "\", which SIL Kit itself accepts (" + detail + "). Write the members that vehicle-sil adds into the configuration "
        "yourself, so that nothing is added (Middleware.AcceptorUris; on Windows Middleware.EnableDomainSockets: false; "
        "Experimental.TimeSynchronization.DynamicSimulationStep: false), and report this as an error of vehicle-sil with "
        "the configuration");
}

/// `text` (of the file `file`) with the file's directory added to Includes.SearchPathHints;
/// empty if `text` is to be used as it is: it has no Includes with members, or it is not for
/// this function to say what is wrong with it (SIL Kit does, with the line).
std::optional<std::string> with_search_path_hint(const std::string& text, const std::filesystem::path& file)
{
    try
    {
        ConfigurationText tree(text);
        const ryml::id_type includes = tree.member(tree.root(), "Includes");
        if (!tree.is_map(includes))
        {
            return std::nullopt; // none, or "Includes:" with nothing under it: nothing is included
        }
        std::error_code error;
        const auto directory = std::filesystem::absolute(file, error).parent_path();
        if (error || directory.empty() || !tree.add(includes, "SearchPathHints", directory.generic_string()))
        {
            return std::nullopt;
        }
        return tree.text();
    }
    catch (const YamlError&)
    {
        return std::nullopt;
    }
}
#endif

bool is_blank_configuration(std::string_view configuration)
{
    constexpr std::string_view space = " \t\r\n";
    const auto first = configuration.find_first_not_of(space);
    const std::string_view text =
        first == std::string_view::npos ? std::string_view{} : configuration.substr(first, configuration.find_last_not_of(space) - first + 1);
    return text.empty() || text == "{}";
}

std::string in_quotes(std::string_view text) { return "\"" + std::string(text) + "\""; }

} // namespace

bool is_loopback_registry_uri(std::string_view registry_uri)
{
    std::string_view host = registry_host(registry_uri);
    if (host.find(':') != std::string_view::npos)
    {
        // ::1, or 127.x.x.x mapped to IPv6 (::ffff:127.0.0.1).
        const auto v6 = parse_ipv6(host);
        return v6 && (is_ipv6_loopback(*v6) || (std::all_of(v6->begin(), v6->begin() + 5, [](std::uint16_t g) { return g == 0; }) &&
                                                (*v6)[5] == 0xFFFF && ((*v6)[6] >> 8) == 127));
    }
    if (const auto v4 = parse_ipv4(host))
    {
        return (*v4 >> 24) == 127;
    }
    if (!host.empty() && host.back() == '.')
    {
        host.remove_suffix(1); // the root of the names: "localhost." is "localhost"
    }
    return equals_ignoring_case(host, "localhost");
}

std::string default_participant_configuration(std::string_view registry_uri, std::optional<bool> listen_on_network)
{
    return default_participant_configuration_with(registry_uri, "", listen_on_network);
}

std::string default_participant_configuration_with(std::string_view registry_uri, std::string_view members,
                                                   std::optional<bool> listen_on_network)
{
    const std::string middleware = middleware_members(registry_uri, listen_on_network);
    std::string text = "{";
    if (!middleware.empty())
    {
        text += R"("Middleware":{)" + middleware + "},";
    }
    // The opt-out of dynamic simulation steps: without the key, SIL Kit cuts this participant's
    // steps down to the smallest step of the simulation as soon as any participant asks for it.
    text += R"("Experimental":{"TimeSynchronization":{"DynamicSimulationStep":false}})";
    if (!members.empty())
    {
        text += ',';
        text += members;
    }
    text += '}';
    return text;
}

std::string merged_participant_configuration(std::string_view configuration, std::string_view participant_name,
                                             std::string_view registry_uri, std::optional<bool> listen_on_network)
{
    if (is_blank_configuration(configuration))
    {
        return default_participant_configuration(registry_uri, listen_on_network);
    }
#ifndef VEHICLE_SIL_CORE_HAS_RAPIDYAML
    // Built with a SIL Kit that gives no YAML library (rapidyaml): the configuration is used as
    // it is, unread. SIL Kit reads it when the participant is made.
    static_cast<void>(participant_name);
    return std::string(configuration);
#else
    // What the configuration says once SIL Kit has read it (its schema, its Includes): SIL Kit's
    // JSON has the members that differ from SIL Kit's defaults, whatever file they came from. It
    // is only looked at: it leaves out some of what SIL Kit read (a log sink's Format, for one),
    // so the defaults are added to the caller's text.
    std::string json;
    try
    {
        json = SilKit::Config::ParticipantConfigurationToJson(SilKit::Config::ParticipantConfigurationFromString(
            std::string(configuration) /* the caller's own: the rest of merged_participant_configuration() adds the defaults */));
    }
    catch (const SilKit::ConfigurationError& e)
    {
        // SIL Kit's words. For a text that is not YAML they do not say where (SIL Kit's parser
        // writes the place to stderr only): the same parser, asked again, does.
        std::string place;
        try
        {
            const ConfigurationText again(configuration);
        }
        catch (const YamlError& yaml)
        {
            place = yaml.place();
        }
        if (place.empty())
        {
            throw;
        }
        std::string what = e.what();
        what.erase(what.find_last_not_of(" \t\r\n") + 1);
        throw SilKit::ConfigurationError(what + place);
    }
    const std::string who = "the SIL Kit configuration of the participant " + in_quotes(participant_name);
    std::string_view doing = "read what SIL Kit makes of";
    try
    {
        // A scalar is its text, however SIL Kit's JSON writes it: a name like 42 or true without quotes.
        const ConfigurationText said(json);
        if (const auto configured = said.scalar(said.root(), "ParticipantName"); configured && *configured != participant_name)
        {
            throw std::invalid_argument(
                who + " sets ParticipantName to " + in_quotes(*configured) + ": SIL Kit would join the simulation under that "
                "name, and whatever waits for " + in_quotes(participant_name) + " (the controller's required participants, "
                "vehicle_sil_launch) would wait until its timeout. Remove ParticipantName from the configuration (the "
                "program gives the name; --name of an ECU's executable changes it), or set it to " +
                in_quotes(participant_name));
        }

        AddedDefaults added;
        const ryml::id_type middleware = said.member(said.root(), "Middleware");
        if (const auto configured = said.scalar(middleware, "RegistryUri");
            configured && !registry_uri.empty() && *configured != registry_uri)
        {
            throw std::invalid_argument(
                who + " sets Middleware.RegistryUri to " + in_quotes(*configured) + ", but the participant is told to join " +
                in_quotes(registry_uri) + ": SIL Kit would connect to the configuration's registry. Remove "
                "Middleware.RegistryUri from the configuration (the program gives the URI: --registry-uri, or registry_uri "
                "of vehicle_sil_launch's configuration), or write the same URI in both");
        }
        if (said.member(middleware, "AcceptorUris") == ryml::NONE) // SIL Kit's JSON leaves an empty list out
        {
            added.acceptor_uris = default_acceptor_uris(registry_uri, listen_on_network);
        }
#ifdef _WIN32
        added.no_domain_sockets = said.member(middleware, "EnableDomainSockets") == ryml::NONE; // SIL Kit's JSON has only a false
#endif

        // SIL Kit's JSON writes a boolean as 1 or 0.
        const auto dynamic_step = said.scalar(said.member(said.member(said.root(), "Experimental"), "TimeSynchronization"),
                                              "DynamicSimulationStep");
        if (dynamic_step && *dynamic_step != "0" && *dynamic_step != "false")
        {
            throw std::invalid_argument(
                who + " sets Experimental.TimeSynchronization.DynamicSimulationStep to true: SIL Kit would cut this "
                "participant's steps down to the smallest step of the simulation, and vehicle-sil's participants run on "
                "their own fixed step. Remove the key from the configuration (vehicle-sil then sets it to false for this "
                "participant), or set it to false");
        }
        added.no_dynamic_simulation_step = !dynamic_step.has_value();

        if (!added.any())
        {
            return std::string(configuration);
        }
        // The caller's text with the members that it leaves out: every other member stays as written.
        doing = "add vehicle-sil's defaults to";
        ConfigurationText text(configuration);
        if (!text.has_members())
        {
            return default_participant_configuration(registry_uri, listen_on_network); // only comments
        }
        if (!added.acceptor_uris.empty() || added.no_domain_sockets)
        {
            const auto node = text.map_at(text.root(), "Middleware");
            if (!added.acceptor_uris.empty())
            {
                text.set(node, "AcceptorUris", added.acceptor_uris);
            }
            if (added.no_domain_sockets)
            {
                text.set(node, "EnableDomainSockets", "false");
            }
        }
        if (added.no_dynamic_simulation_step)
        {
            text.set(text.map_at(text.map_at(text.root(), "Experimental"), "TimeSynchronization"), "DynamicSimulationStep", "false");
        }
        return text.text();
    }
    catch (const YamlError& e)
    {
        throw_cannot_add_defaults(participant_name, doing, e.message + e.place());
    }
#endif
}

std::string participant_configuration_from_file(const std::string& path)
{
    const std::filesystem::path file(path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error))
    {
        std::error_code ignored;
        const std::filesystem::path directory = std::filesystem::current_path(ignored);
        throw std::invalid_argument("there is no SIL Kit participant configuration file " + in_quotes(path) +
                                    (file.is_absolute() || ignored ? std::string()
                                                                   : " in the working directory " + in_quotes(directory.string())) +
                                    ": give the path of a YAML or JSON file that exists (a relative path is taken "
                                    "from the working directory of the process)");
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.is_open() || in.bad())
    {
        throw std::invalid_argument("could not read the SIL Kit participant configuration file " + in_quotes(path) +
                                    ": check that this user may read it");
    }
#ifdef VEHICLE_SIL_CORE_HAS_RAPIDYAML
    // SIL Kit looks for the files that a text includes from the working directory and where the
    // text says (Includes.SearchPathHints): name the file's directory there, so that a file beside
    // it is found from any working directory. (SIL Kit's own ParticipantConfigurationFromFile()
    // means to, but joins the directory and the name without a separator, as of v5.0.8.)
    if (auto with_hint = with_search_path_hint(text, file))
    {
        return std::move(*with_hint);
    }
#endif
    return text;
}

std::string_view default_registry_configuration() noexcept
{
    // TCP only, on every system: a registry that cannot listen on its TCP port then fails at once.
    // With a local-domain socket, SIL Kit's registry goes on without the TCP port (another program,
    // or another registry, has it) and takes the local-domain path of a registry with the same URI.
    return R"({"Middleware":{"EnableDomainSockets":false}})";
}

bool is_running(const PeerStatus& status) noexcept
{
    return status.state == orch::ParticipantState::Running;
}

bool has_finished(const PeerStatus& status) noexcept
{
    return status.state && is_terminal_or_stopping(*status.state);
}

std::unique_ptr<SilKit::IParticipant> create_participant(std::shared_ptr<SilKit::Config::IParticipantConfiguration> config,
                                                         const std::string& name, const std::string& registry_uri,
                                                         std::chrono::milliseconds name_in_use_wait)
{
    if (name_in_use_wait < std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument("name_in_use_wait must not be negative");
    }
    const auto give_up = std::chrono::steady_clock::now() + name_in_use_wait;
    for (;;)
    {
        try
        {
            return SilKit::CreateParticipant(config, name, registry_uri);
        }
        catch (const std::exception& e)
        {
            const std::string what = e.what();
            if (what.find("already exists in the simulation") == std::string::npos)
            {
                throw;
            }
            if (std::chrono::steady_clock::now() >= give_up)
            {
                throw SilKit::ProtocolError(
                    what + " (the registry refused the name \"" + name + "\" for " + std::to_string(name_in_use_wait.count()) +
                    " ms: another participant of this simulation uses it, or the registry has not yet seen the end of "
                    "an earlier participant with this name; give each participant a name of its own, or end the other "
                    "one)");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

ParticipantRuntime::ParticipantRuntime(ParticipantRuntimeOptions options) :
  options_(std::move(options))
{
    if (options_.participant_name.empty())
    {
        throw std::invalid_argument("participant_name must not be empty");
    }
    // Validate before CreateParticipant(): once handlers that capture `this` are registered, a
    // throwing constructor would destroy members the SIL Kit threads may still use.
    if (options_.virtual_time)
    {
        const auto& vt = *options_.virtual_time;
        if (vt.step <= SimDuration::zero())
        {
            throw std::invalid_argument("virtual_time.step must be > 0");
        }
        if (vt.real_time_factor)
        {
            pacer_.emplace(*vt.real_time_factor); // validates the factor
        }
    }

    auto config = SilKit::Config::ParticipantConfigurationFromString(merged_participant_configuration(
        options_.participant_configuration, options_.participant_name, options_.registry_uri, options_.listen_on_network));
    participant_ = create_participant(config, options_.participant_name, options_.registry_uri, options_.name_in_use_wait);

    // Handlers registered below capture `this`. If a later call throws, destroy the
    // participant (which removes them) before the members they use go away.
    try
    {
        monitor_ = participant_->CreateSystemMonitor();
        // Only the participant status handler is used. ISystemMonitor::SetParticipantConnectedHandler
        // and SetParticipantDisconnectedHandler are not: in SIL Kit v5.0.8 they assign the handler
        // without synchronisation while the I/O thread may already be calling it (a data race found
        // with ThreadSanitizer, see docs/dependencies.md). A lost connection still shows up here as
        // ParticipantState::Error.
        monitor_->AddParticipantStatusHandler([this](const orch::ParticipantStatus& status) { on_status(status, false); });

        const auto mode = options_.lifecycle_mode == LifecycleMode::Coordinated ? orch::OperationMode::Coordinated
                                                                                 : orch::OperationMode::Autonomous;
        lifecycle_ = participant_->CreateLifecycleService({mode});

        if (options_.virtual_time)
        {
            time_sync_ = lifecycle_->CreateTimeSyncService();
            // The asynchronous variant, completed at the end of the handler. SIL Kit runs no
            // other handler of this participant while the step runs (it runs the handlers on its
            // I/O thread). CompleteSimulationStep() defers the completion on that thread, behind
            // everything the step queued: the messages sent in the step, and also the messages
            // SIL Kit sends while it delivers those back to this participant (a LIN master's own
            // frame is generated when its header comes back to it). All of them go out before the
            // participant announces its next step. The blocking variant announces the next step
            // before that second round, and may even run the next step from a batch of received
            // messages before it.
            time_sync_->SetSimulationStepHandlerAsync(
                [this](std::chrono::nanoseconds now, std::chrono::nanoseconds duration) {
                    struct CompleteOnExit
                    {
                        SilKit::Services::Orchestration::ITimeSyncService* time_sync;
                        bool enabled = true; // false: a StepHold keeps the step until release_step()
                        ~CompleteOnExit()
                        {
                            if (!enabled)
                            {
                                return;
                            }
                            try
                            {
                                time_sync->CompleteSimulationStep();
                            }
                            catch (const std::exception&)
                            {
                                // Only queues a task; nothing to recover if even that fails.
                            }
                        }
                    } complete{time_sync_};
                    complete.enabled = on_step(now, duration);
                },
                options_.virtual_time->step);
        }
        else
        {
            // Without time synchronisation no step proves that the lifecycle got to Running;
            // this handler does (start()). SIL Kit calls it only without a TimeSyncService.
            lifecycle_->SetStartingHandler([this] {
                starting_entered_.store(true, std::memory_order_release);
                start_probe(detail::StartProbePoint::StartingHandlerEntered);
            });
        }
    }
    catch (...)
    {
        participant_.reset();
        throw;
    }
}

ParticipantRuntime::~ParticipantRuntime()
{
    stop("ParticipantRuntime destroyed");
    // Destroying the participant removes all handlers registered above before `this` dies.
    participant_.reset();
}

void ParticipantRuntime::set_step_handler(StepHandler handler)
{
    if (!time_sync_)
    {
        throw std::logic_error("ParticipantRuntime::set_step_handler() needs virtual_time");
    }
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_step_handler() must be called before start()");
    }
    step_handler_ = std::move(handler);
}

void ParticipantRuntime::set_communication_ready_handler(std::function<void()> handler)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_communication_ready_handler() must be called before start()");
    }
    lifecycle_->SetCommunicationReadyHandler([this, handler = std::move(handler)] {
        const std::lock_guard gate(communication_ready_gate_);
        if (communication_ready_disabled_)
        {
            return; // stop() has returned: what the handlers use may be gone
        }
        run_added_communication_ready();
        if (handler)
        {
            handler();
        }
    });
    communication_ready_registered_ = true;
}

void ParticipantRuntime::add_communication_ready_handler(std::function<void()> handler)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::add_communication_ready_handler() must be called before start()");
    }
    added_communication_ready_.push_back(std::move(handler));
    if (!communication_ready_registered_)
    {
        lifecycle_->SetCommunicationReadyHandler([this] {
            const std::lock_guard gate(communication_ready_gate_);
            if (!communication_ready_disabled_)
            {
                run_added_communication_ready();
            }
        });
        communication_ready_registered_ = true;
    }
}

void ParticipantRuntime::run_added_communication_ready()
{
    for (const auto& handler : added_communication_ready_)
    {
        std::string error;
        try
        {
            if (handler)
            {
                handler();
            }
            continue;
        }
        catch (const std::exception& e)
        {
            error = *e.what() != '\0' ? e.what() : "exception without message";
        }
        catch (...)
        {
            error = "unknown exception";
        }
        report_failure("CommunicationReady handler failed: " + error);
    }
}

void ParticipantRuntime::set_communication_ready_handler_async(std::function<void()> handler)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_communication_ready_handler_async() must be called before start()");
    }
    communication_ready_ = std::move(handler);
    communication_ready_registered_ = true;
    lifecycle_->SetCommunicationReadyHandlerAsync([this] {
        const std::lock_guard gate(communication_ready_gate_);
        const bool disabled = communication_ready_disabled_;
        if (!disabled)
        {
            run_added_communication_ready();
        }
        {
            // Before the handler: a thread it wakes may complete at once. The lock also orders
            // SIL Kit's own (unsynchronised) record of the call before the completing thread's
            // read of it.
            const std::lock_guard lock(communication_ready_mutex_);
            communication_ready_state_ = CommunicationReady::Open;
        }
        if (disabled)
        {
            // stop() has returned: nobody is told, so nobody would complete. Completed here, the
            // lifecycle goes on as after a handler that is not asynchronous, to where the stop
            // can reach it. SIL Kit only queues the transition, so this is fine from its handler.
            complete_communication_ready();
            return;
        }
        if (communication_ready_)
        {
            communication_ready_();
        }
    });
}

bool ParticipantRuntime::complete_communication_ready() noexcept
{
    const std::lock_guard lock(communication_ready_mutex_);
    if (communication_ready_state_ != CommunicationReady::Open)
    {
        return false;
    }
    communication_ready_state_ = CommunicationReady::Completed;
    try
    {
        lifecycle_->CompleteCommunicationReadyHandlerAsync(); // queues the transition on the SIL Kit thread
    }
    catch (const std::exception&)
    {
        // Only queues a task; nothing to recover if even that fails.
    }
    return true;
}

void ParticipantRuntime::set_peer_status_observer(std::function<void(const std::string&, const PeerStatus&)> observer)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_peer_status_observer() must be called before start()");
    }
    // The SIL Kit thread may deliver a status (on_status()) already.
    const std::lock_guard lock(peer_observer_mutex_);
    peer_observer_ = std::move(observer);
}

void ParticipantRuntime::start(std::chrono::milliseconds timeout, const std::function<std::string()>& give_up)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::start() called twice");
    }
    if (time_sync_ && !step_handler_)
    {
        throw std::logic_error("ParticipantRuntime with virtual_time needs set_step_handler() before start()");
    }
    // Set first: the first step (and a request_stop() from it) can run before StartLifecycle()
    // returns.
    lifecycle_started_.store(true, std::memory_order_release);
    lifecycle_done_ = lifecycle_->StartLifecycle().share();
    {
        // Only now does SIL Kit take a reported error (report_failure()). Not held across
        // StartLifecycle(): that registers status handlers under SIL Kit's handler lock, which
        // the SIL Kit thread holds while it calls on_status(), which takes this lock.
        start_probe(detail::StartProbePoint::AfterStartLifecycle);
        const std::lock_guard lock(control_mutex_);
        error_reportable_ = true;
        if (early_failure_)
        {
            // A handler on the SIL Kit thread (CommunicationReady of an autonomous participant)
            // failed before this thread came back from StartLifecycle(): report it now.
            lifecycle_->ReportError(std::move(*early_failure_));
            early_failure_.reset();
            failed_ = true;
        }
    }

    const auto deadline = timeout == kNoStartLimit ? std::chrono::steady_clock::time_point::max()
                                                   : std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        // A short simulation can pass through Running between two polls (to Stopping or on to
        // Shutdown); a step handler call (virtual time) or the starting handler (otherwise)
        // proves that it got there. The starting handler runs in ReadyToRun, and SIL Kit sets
        // Running only after it returned: until then, state() would still say ReadyToRun.
        //
        // The order of the reads matters: the flags first, the state after them. SIL Kit sets
        // ReadyToRun before it calls the starting handler and never goes back from there, so a
        // state read after the flag was seen set is ReadyToRun or a later one, and "not
        // ReadyToRun" then means Running or beyond. With the state read first, the SIL Kit thread
        // could set ReadyToRun and the flag between the two reads, and start() would return on
        // an older state while state() says ReadyToRun.
        const bool stepEntered = step_entered_.load(std::memory_order_acquire);
        const bool startingEntered = starting_entered_.load(std::memory_order_acquire);
        start_probe(detail::StartProbePoint::BetweenFlagsAndState);
        const auto current = lifecycle_->State();
        if (current == orch::ParticipantState::Running || stepEntered
            || (startingEntered && current != orch::ParticipantState::ReadyToRun))
        {
            deliver_stop(false); // a stop requested before Running
            return;
        }
        if (is_terminal_or_stopping(current))
        {
            // The other side of the same order: the lifecycle can have gone through Running and
            // on to this state after the flags were read. They never go back, so they say now
            // whether it did: that is a start that succeeded, not one that failed.
            if (step_entered_.load(std::memory_order_acquire) || starting_entered_.load(std::memory_order_acquire))
            {
                deliver_stop(false);
                return;
            }
            throw std::runtime_error("SIL Kit lifecycle of '" + options_.participant_name
                                     + "' failed to start, state: " + orch::to_string(current));
        }
        if (give_up)
        {
            if (std::string why = give_up(); !why.empty())
            {
                throw std::runtime_error("SIL Kit lifecycle of '" + options_.participant_name +
                                         "': the start was given up, state: " + orch::to_string(current) + ": " + why);
            }
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            throw StartTimeoutError("SIL Kit lifecycle of '" + options_.participant_name
                                     + "' did not reach Running within " + std::to_string(timeout.count())
                                     + " ms, state: " + orch::to_string(current));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ParticipantRuntime::stop(std::string_view reason) noexcept
{
    if (std::exchange(stopped_, true))
    {
        return;
    }
    // Whatever happens below, the step handler and the CommunicationReady handlers must not run
    // once stop() returns: callers destroy what they use. A handler that is running is waited
    // for (a long tick() delays stop() accordingly).
    struct DisableSteps
    {
        ParticipantRuntime& self;
        ~DisableSteps()
        {
            {
                const std::lock_guard gate(self.step_gate_);
                self.steps_disabled_ = true;
            }
            // Waits for a StepHold that is deciding, and keeps later ones from running.
            {
                const std::lock_guard hold(self.hold_mutex_);
                self.hold_disabled_ = true;
            }
            // Last, when the wait for the shutdown is over: a lifecycle that gets to
            // CommunicationReady while stop() waits (a required participant that came late) still
            // has its controllers started and its start window opened, and goes on to where the
            // stop reaches it. Only after stop() can it come too late: after a start that timed
            // out, SIL Kit takes no stop and the lifecycle lives until the participant is
            // destroyed.
            const std::lock_guard ready(self.communication_ready_gate_);
            self.communication_ready_disabled_ = true;
        }
    } disableSteps{*this};
    if (!lifecycle_done_)
    {
        return;
    }
    {
        const std::lock_guard lock(control_mutex_);
        if (!stop_requested_)
        {
            stop_requested_ = true;
            try
            {
                stop_reason_ = reason;
            }
            catch (const std::exception&)
            {
            }
        }
    }
    stop_cv_.notify_all();
    notify_stop_observer();
    {
        // A step that is failing now reports its error before the stop goes out (see
        // deliver_stop()). The caller may have seen the failure before the step handler
        // returned: Peer::failed() looks at the Host, which fails inside the handler. No
        // handler starts afterwards, so no failure can begin after this point either: without
        // that, a step that started right after the gate was let go and failed would report
        // its error behind the Stop() that deliver_stop() queued from Running.
        const std::lock_guard gate(step_gate_);
        steps_disabled_ = true;
    }
    // One deadline for the Error (deliver_stop()) and the Shutdown below.
    const auto deadline = std::chrono::steady_clock::now() + options_.shutdown_timeout;
    deliver_stop(true, deadline); // this participant ends now: no waiting for a first step
    // The lifecycle proceeds to Shutdown on its own after Stop(), in both modes and from Error.
    const auto done = *lifecycle_done_; // own copy: wait_until_finished() may use it meanwhile
    try
    {
        if (!final_state() && done.wait_until(deadline) == std::future_status::ready)
        {
            set_final_state(done.get());
        }
    }
    catch (const std::exception&)
    {
        // The lifecycle ended by an error or abort.
        // Shutdown is best effort: the participant is destroyed (and disconnected) afterwards.
        set_final_state(lifecycle_->State());
    }
}

std::optional<orch::ParticipantState> ParticipantRuntime::final_state() const
{
    const std::lock_guard lock(final_mutex_);
    return final_state_;
}

void ParticipantRuntime::set_final_state(orch::ParticipantState state)
{
    const std::lock_guard lock(final_mutex_);
    if (!final_state_)
    {
        final_state_ = state;
    }
}

void ParticipantRuntime::request_stop(std::string_view reason) noexcept
{
    {
        const std::lock_guard lock(control_mutex_);
        if (!stop_requested_)
        {
            stop_requested_ = true;
            try
            {
                stop_reason_ = reason;
            }
            catch (const std::exception&)
            {
                // The reason is informative; an empty one is fine.
            }
        }
    }
    stop_cv_.notify_all(); // ends a pacing wait early
    notify_stop_observer();
    deliver_stop(false);
}

void ParticipantRuntime::deliver_stop(bool now, std::chrono::steady_clock::time_point deadline) noexcept
{
    if (stop_delivered_.load(std::memory_order_acquire) || !stop_requested())
    {
        return;
    }
    // lifecycle_done_ may still be being assigned by start() on another thread.
    if (!started())
    {
        return;
    }
    try
    {
        const auto is_running_or_paused = [](orch::ParticipantState s) {
            return s == orch::ParticipantState::Running || s == orch::ParticipantState::Paused;
        };
        auto state = lifecycle_->State();
        // A participant that failed (its step handler, or report_failure()) leaves through
        // Error. ReportError() and Stop() are both carried out later on the SIL Kit thread; a
        // Stop() taken there while still Running would be an ordinary stop (in Coordinated
        // mode, of the whole simulation), and nobody would see the failure. So no Stop() from
        // Running after a failure: a request waits for the Error (on_status() delivers it),
        // stop() waits here until `deadline`. Nor while a step handler runs, which may be
        // failing (the caller may have seen it fail): a request then goes out when the handler
        // has returned (on_step()); stop() waited for the handler already.
        //
        // The failure flag is set and ReportError() queued under one lock, so the error is
        // queued before any Stop() that this function could queue after seeing the flag. The
        // two branches under has_failed() are what makes the result independent of the order
        // in which SIL Kit carries out queued calls: asio does not promise the order of
        // post(), and it was not confirmed for Windows. Where queued calls run in order (seen
        // on Linux), removing the branches changes nothing that a test can observe.
        if (!now && is_running_or_paused(state) && handler_running_.load(std::memory_order_acquire))
        {
            return;
        }
        if (is_running_or_paused(state) && has_failed())
        {
            if (!now)
            {
                return;
            }
            while (is_running_or_paused(state) && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                state = lifecycle_->State();
            }
        }
        const bool running = is_running_or_paused(state);
        // With virtual time, a request waits for the first step: a participant can be Running
        // while others (the system controller among them) are not there yet, and a stop then
        // can leave those stuck before Running; the first step needs every synchronised
        // participant to have started its time. stop() does not wait.
        if (running && !now && time_sync_ && !step_entered_.load(std::memory_order_acquire))
        {
            return;
        }
        // Error is included: SIL Kit keeps a participant in Error until it is stopped or
        // aborted, and Stop() takes it from Error to Shutdown.
        if (running || state == orch::ParticipantState::Error)
        {
            if (stop_delivered_.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            std::string reason;
            {
                const std::lock_guard lock(control_mutex_);
                reason = stop_reason_;
            }
            lifecycle_->Stop(std::move(reason));
        }
        else if (is_terminal_or_stopping(state))
        {
            stop_delivered_.store(true, std::memory_order_release); // it ends anyway
        }
        // Before Running, SIL Kit ignores Stop() (with a warning): the request is kept and
        // delivered by start() once Running is reached or, with virtual time, by the first step.
    }
    catch (const std::exception&)
    {
        // Stop() may throw if the state machine cannot stop from its current state; the
        // caller then waits or destroys the participant, which disconnects it.
    }
}

std::optional<orch::ParticipantState> ParticipantRuntime::wait_until_finished(std::chrono::milliseconds timeout)
{
    if (auto state = final_state())
    {
        return state;
    }
    if (!lifecycle_done_)
    {
        return std::nullopt;
    }
    const auto done = *lifecycle_done_; // own copy: stop() may use it meanwhile
    try
    {
        // The future completes at Shutdown. A participant in Error stays there until someone
        // stops or aborts it, so Error also counts as finished (stop() then shuts it down).
        constexpr auto kPoll = std::chrono::milliseconds(20);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        // wait_for() returns at once for a non-positive duration.
        while (done.wait_for(std::min<std::chrono::steady_clock::duration>(
                   kPoll, deadline - std::chrono::steady_clock::now()))
               != std::future_status::ready)
        {
            if (lifecycle_->State() == orch::ParticipantState::Error)
            {
                return orch::ParticipantState::Error;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return std::nullopt;
            }
        }
        set_final_state(done.get());
    }
    catch (const std::exception&)
    {
        // The lifecycle ended by an error or abort.
        set_final_state(lifecycle_->State());
    }
    return final_state();
}

bool ParticipantRuntime::stop_requested() const
{
    const std::lock_guard lock(control_mutex_);
    return stop_requested_;
}

std::string ParticipantRuntime::step_error() const
{
    const std::lock_guard lock(control_mutex_);
    return step_error_;
}

bool ParticipantRuntime::has_failed() const
{
    const std::lock_guard lock(control_mutex_);
    return failed_;
}

void ParticipantRuntime::report_failure(std::string_view reason) noexcept
{
    try
    {
        std::string text(reason);
        // The flag and the queued error together (deliver_stop()): whoever sees the flag can
        // count on the Error to come. ReportError() only queues a call for the SIL Kit thread
        // (Participant::ExecuteDeferred() is asio::post()), so nothing runs under the lock.
        const std::lock_guard lock(control_mutex_);
        if (!error_reportable_)
        {
            // SIL Kit ignores an error before StartLifecycle(); so does the stop logic. One that
            // comes while start() is in StartLifecycle() (from a handler SIL Kit already calls)
            // is kept, and start() reports it.
            if (lifecycle_started_.load(std::memory_order_acquire) && !early_failure_)
            {
                early_failure_ = std::move(text);
            }
            return;
        }
        if (failed_)
        {
            return; // only the first failure is reported
        }
        lifecycle_->ReportError(std::move(text));
        failed_ = true;
    }
    catch (const std::exception&)
    {
        // Already in a state that cannot report errors; the simulation is ending anyway.
    }
}

bool ParticipantRuntime::on_step(std::chrono::nanoseconds now, std::chrono::nanoseconds duration)
{
    const auto wallStart = pacer_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const SimTime time = from_silkit_time(now);
    now_ns_.store(now.count(), std::memory_order_release);
    step_entered_.store(true, std::memory_order_release);
    if (!stop_delivered_.load(std::memory_order_acquire))
    {
        deliver_stop(false); // a stop requested before Running: this step is the first chance
    }
    {
        const std::lock_guard gate(step_gate_);
        // Only this thread writes step_error_, so reading it here needs no lock.
        if (steps_disabled_ || !step_error_.empty())
        {
            return true;
        }
        if (const auto& end = options_.virtual_time->end; !end || time < *end)
        {
            struct Running
            {
                std::atomic<bool>& flag;
                explicit Running(std::atomic<bool>& f) : flag(f) { flag.store(true, std::memory_order_release); }
                ~Running() { flag.store(false, std::memory_order_release); }
                Running(const Running&) = delete;
                Running& operator=(const Running&) = delete;
            };
            bool ok = false;
            if (std::string cut = detail::step_width_error(duration, options_.virtual_time->step, time); !cut.empty())
            {
                // Everything above assumes the fixed step that was asked for (the grid of the
                // steps, TickScheduler, the time gate).
                fail_step(std::move(cut));
            }
            else
            {
                const Running running(handler_running_);
                ok = run_step_handler(time, duration);
            }
            if (!stop_delivered_.load(std::memory_order_acquire))
            {
                deliver_stop(false); // a request made while the handler ran (deliver_stop())
            }
            if (!ok)
            {
                return true;
            }
            steps_completed_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    if (pacer_)
    {
        pace_until(pacer_->step_deadline(time, duration, wallStart));
    }
    if (!step_hold_ || stop_requested())
    {
        return true;
    }
    // held_ is set before the hold decides, under hold_mutex_: a release_step() that the hold
    // triggers on another thread waits for the decision and then finds the step held.
    const std::lock_guard lock(hold_mutex_);
    if (hold_disabled_)
    {
        return true;
    }
    held_ = true;
    bool hold = false;
    try
    {
        hold = step_hold_(time, duration);
    }
    catch (...)
    {
        hold = false; // a failing hold must not stall the simulation
    }
    held_ = hold;
    return !hold;
}

void ParticipantRuntime::set_step_hold(StepHold hold)
{
    if (!time_sync_)
    {
        throw std::logic_error("ParticipantRuntime::set_step_hold() needs virtual_time");
    }
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_step_hold() must be called before start()");
    }
    step_hold_ = std::move(hold);
}

void ParticipantRuntime::set_stop_observer(std::function<void()> observer)
{
    if (started())
    {
        throw std::logic_error("ParticipantRuntime::set_stop_observer() must be called before start()");
    }
    // The SIL Kit thread may deliver a status (on_status()) already.
    const std::lock_guard lock(stop_observer_mutex_);
    stop_observer_ = std::move(observer);
}

void ParticipantRuntime::notify_stop_observer() noexcept
{
    const std::lock_guard lock(stop_observer_mutex_);
    if (stop_observer_)
    {
        try
        {
            stop_observer_();
        }
        catch (...)
        {
            // An observer must not keep the stop from going on.
        }
    }
}

void ParticipantRuntime::release_step() noexcept
{
    const std::lock_guard lock(hold_mutex_);
    if (!std::exchange(held_, false))
    {
        return;
    }
    try
    {
        time_sync_->CompleteSimulationStep(); // queues the completion on the SIL Kit thread
    }
    catch (const std::exception&)
    {
        // Only queues a task; nothing to recover if even that fails.
    }
}

bool ParticipantRuntime::run_step_handler(SimTime now, SimDuration step)
{
    std::string error;
    try
    {
        step_handler_(now, step);
        return true;
    }
    catch (const std::exception& e)
    {
        error = *e.what() != '\0' ? e.what() : "exception without message";
    }
    catch (...)
    {
        error = "unknown exception";
    }
    fail_step("step handler at t=" + std::to_string(to_ns(now)) + " ns failed: " + error);
    return false;
}

void ParticipantRuntime::fail_step(std::string error)
{
    // The flag and the queued error under one lock, as in report_failure(). A step runs only
    // after StartLifecycle(), so SIL Kit takes the error.
    const std::lock_guard lock(control_mutex_);
    step_error_ = error;
    if (std::exchange(failed_, true))
    {
        return; // report_failure() reported a failure already
    }
    try
    {
        lifecycle_->ReportError(std::move(error));
    }
    catch (const std::exception&)
    {
        // Already in a state that cannot report errors; the simulation is ending anyway.
    }
}

void ParticipantRuntime::pace_until(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(control_mutex_);
    stop_cv_.wait_until(lock, deadline, [this] { return stop_requested_; });
}

orch::ParticipantState ParticipantRuntime::state() const
{
    return lifecycle_->State();
}

PeerStatus ParticipantRuntime::peer_status(const std::string& participant_name) const
{
    const std::lock_guard lock(peers_mutex_);
    const auto it = peers_.find(participant_name);
    return it == peers_.end() ? PeerStatus{} : it->second.status;
}

std::vector<std::string> ParticipantRuntime::peer_names() const
{
    const std::lock_guard lock(peers_mutex_);
    std::vector<std::string> names;
    for (const auto& [name, record] : peers_)
    {
        if (record.status.state)
        {
            names.push_back(name);
        }
    }
    return names;
}

bool ParticipantRuntime::wait_for_peer(const std::string& participant_name,
                                       const std::function<bool(const PeerStatus&)>& predicate,
                                       std::chrono::milliseconds timeout)
{
    constexpr auto kResyncInterval = std::chrono::milliseconds(200);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        {
            std::unique_lock lock(peers_mutex_);
            const auto now = std::chrono::steady_clock::now();
            const auto until = std::min(deadline, now + kResyncInterval);
            const bool holds = peers_cv_.wait_until(lock, until, [&] {
                const auto it = peers_.find(participant_name);
                return predicate(it == peers_.end() ? PeerStatus{} : it->second.status);
            });
            if (holds)
            {
                return true;
            }
            if (now >= deadline)
            {
                return false;
            }
        }
        resync_peers(); // outside peers_mutex_: the replay calls on_status()
    }
}

void ParticipantRuntime::resync_peers()
{
    // AddParticipantStatusHandler() replays the current status of every connected participant to
    // the new handler synchronously, under SIL Kit's own lock.
    // One thread at a time: SIL Kit's C++ API keeps a system monitor's handlers in a table
    // without a lock, which adding and removing change. Not peers_mutex_: the replay calls
    // on_status(), which takes that.
    const std::lock_guard lock(resync_mutex_);
    const auto id =
        monitor_->AddParticipantStatusHandler([this](const orch::ParticipantStatus& status) { on_status(status, true); });
    monitor_->RemoveParticipantStatusHandler(id);
}

void ParticipantRuntime::on_status(const orch::ParticipantStatus& status, bool replayed)
{
    {
        const std::lock_guard lock(peers_mutex_);
        auto& record = peers_[status.participantName];
        // A replayed status may be older than one delivered meanwhile; keep the newer one. Live
        // statuses always apply: a "Connection Lost" Error is stamped with the local clock, which
        // must not be compared with a remote participant's clock.
        if (replayed && record.status.state && status.enterTime < record.enter_time)
        {
            return;
        }
        record.status.state = status.state;
        record.enter_time = status.enterTime;
    }
    peers_cv_.notify_all();
    {
        const std::lock_guard lock(peer_observer_mutex_);
        if (peer_observer_)
        {
            try
            {
                peer_observer_(status.participantName, PeerStatus{status.state});
            }
            catch (...)
            {
                // An observer must not disturb the status handling.
            }
        }
    }
    // This participant's own status comes here too: a stop requested after a step failure
    // waited for the Error (deliver_stop()).
    if (status.participantName == options_.participant_name)
    {
        if (has_finished(PeerStatus{status.state}))
        {
            notify_stop_observer(); // also a stop that another participant asked for
        }
        if (status.state == orch::ParticipantState::Error)
        {
            deliver_stop(false);
        }
    }
}

} // namespace vehicle_sil
