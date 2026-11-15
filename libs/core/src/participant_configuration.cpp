// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/participant_configuration.hpp"

#include <silkit/SilKit.hpp>

#ifdef VEHICLE_SIL_CORE_HAS_RAPIDYAML
#include <rapidyaml.hpp>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace vehicle_sil {

namespace {

// The one place that says what vehicle-sil changes of SIL Kit's defaults
// (participant_configuration.hpp says why).

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

    /// The items of the list `key` of `parent`, in their order; none if there is no such list.
    [[nodiscard]] std::vector<ryml::id_type> items(ryml::id_type parent, ryml::csubstr key) const
    {
        std::vector<ryml::id_type> out;
        const ryml::id_type list = member(parent, key);
        if (list != ryml::NONE && tree_.is_seq(list))
        {
            for (ryml::id_type item = tree_.first_child(list); item != ryml::NONE; item = tree_.next_sibling(item))
            {
                out.push_back(item);
            }
        }
        return out;
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

bool can_read_controller_networks(std::string_view configuration)
{
#ifdef VEHICLE_SIL_CORE_HAS_RAPIDYAML
    static_cast<void>(configuration);
    return true;
#else
    return is_blank_configuration(configuration);
#endif
}

std::vector<std::string> configured_controller_networks(std::string_view configuration)
{
    std::vector<std::string> out;
    if (is_blank_configuration(configuration))
    {
        return out;
    }
#ifdef VEHICLE_SIL_CORE_HAS_RAPIDYAML
    try
    {
        // As merged_participant_configuration(): what the configuration says once SIL Kit has read
        // it, whatever file a member came from.
        const std::string json = SilKit::Config::ParticipantConfigurationToJson(
            SilKit::Config::ParticipantConfigurationFromString(std::string(configuration) /* only read, as in merged_participant_configuration(): the participant is not made from it */));
        const ConfigurationText said(json);
        for (const char* section : {"CanControllers", "LinControllers", "EthernetControllers", "FlexrayControllers"})
        {
            const ryml::csubstr key = ryml::to_csubstr(section);
            std::size_t index = 0;
            for (const ryml::id_type item : said.items(said.root(), key))
            {
                if (const auto network = said.scalar(item, "Network"))
                {
                    const auto name = said.scalar(item, "Name");
                    out.push_back(std::string(section) + "[" + std::to_string(index) + "]" +
                                  (name ? " (Name " + in_quotes(*name) + ")" : std::string()) + ": Network " +
                                  in_quotes(*network));
                }
                ++index;
            }
        }
    }
    catch (const std::exception&)
    {
        out.clear(); // not for this function to say: whoever makes the participant does
    }
#endif
    return out;
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

} // namespace vehicle_sil
