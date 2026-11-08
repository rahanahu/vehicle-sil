// SPDX-License-Identifier: MIT
#include "vehicle_sil/can_netsim/network_simulator_config.hpp"

#include <rapidyaml.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>

namespace vehicle_sil::can {

namespace {

[[noreturn]] void on_parse_error(const char* msg, std::size_t length, ryml::Location location, void*)
{
    std::string text(msg, length);
    if (location.line > 0 || location.col > 0)
    {
        text = "line " + std::to_string(location.line + 1) + ", column " + std::to_string(location.col + 1) + ": " + text;
    }
    throw NetworkSimulatorConfigError(text);
}

std::string str(ryml::csubstr s)
{
    return std::string(s.str == nullptr ? "" : s.str, s.len);
}

/// Reads the configuration tree; `where` prefixes the error messages.
class Reader
{
public:
    Reader(const ryml::Tree& tree, std::string where) : tree_(tree), where_(std::move(where)) {}

    NetworkSimulatorConfig read()
    {
        NetworkSimulatorConfig config;
        auto root = tree_.root_id();
        if (tree_.is_stream(root))
        {
            // A file with "---": the first document.
            root = tree_.first_child(root);
            if (root == ryml::NONE)
            {
                fail("the configuration is empty");
            }
        }
        if (!tree_.is_map(root))
        {
            fail("the configuration must be a mapping");
        }
        bool hasNetworks = false;
        std::set<std::string> keys;
        for (auto child = tree_.first_child(root); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string key = str(tree_.key(child));
            if (!keys.insert(key).second)
            {
                fail("'" + key + "' appears twice");
            }
            if (key == "$schema")
            {
                continue;
            }
            if (key == "SchemaVersion")
            {
                const std::int64_t version = integer(child, key);
                if (version != 1)
                {
                    fail("SchemaVersion " + std::to_string(version) + " is not supported (only 1)");
                }
                config.schema_version = 1;
            }
            else if (key == "Description")
            {
                config.description = scalar(child, key);
            }
            else if (key == "SimulationStepSizeNanoseconds")
            {
                const std::int64_t ns = integer(child, key);
                if (ns <= 0)
                {
                    fail("SimulationStepSizeNanoseconds must be > 0");
                }
                config.step_size = SimDuration{ns};
            }
            else if (key == "SimulatedNetworks")
            {
                hasNetworks = true;
                read_networks(child, config);
            }
            else if (key == "Switches")
            {
                config.warnings.push_back("Switches (Ethernet) are ignored: only CAN networks are simulated");
            }
            else
            {
                config.warnings.push_back("unknown key '" + key + "' is ignored");
            }
        }
        if (!hasNetworks)
        {
            fail("SimulatedNetworks is missing");
        }
        return config;
    }

private:
    [[noreturn]] void fail(const std::string& message) const { throw NetworkSimulatorConfigError(where_ + ": " + message); }

    std::string scalar(ryml::id_type node, const std::string& key) const
    {
        if (!tree_.has_val(node) || tree_.is_seq(node) || tree_.is_map(node))
        {
            fail(key + " must be a scalar");
        }
        return str(tree_.val(node));
    }

    std::int64_t integer(ryml::id_type node, const std::string& key) const
    {
        const std::string text = scalar(node, key);
        std::int64_t value = 0;
        const auto* end = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(text.data(), end, value);
        if (text.empty() || ec != std::errc{} || ptr != end)
        {
            fail(key + " must be an integer (got '" + text + "')");
        }
        return value;
    }

    void read_networks(ryml::id_type node, NetworkSimulatorConfig& config) const
    {
        if (!tree_.is_seq(node))
        {
            fail("SimulatedNetworks must be a list");
        }
        std::set<std::string> names;
        std::size_t index = 0;
        for (auto item = tree_.first_child(node); item != ryml::NONE; item = tree_.next_sibling(item), ++index)
        {
            const std::string at = "SimulatedNetworks[" + std::to_string(index) + "]";
            if (!tree_.is_map(item))
            {
                fail(at + " must be a mapping with Name and Type");
            }
            SimulatedNetworkConfig network;
            bool hasName = false;
            bool hasType = false;
            std::set<std::string> keys;
            for (auto child = tree_.first_child(item); child != ryml::NONE; child = tree_.next_sibling(child))
            {
                const std::string key = str(tree_.key(child));
                if (!keys.insert(key).second)
                {
                    fail(at + ": '" + key + "' appears twice");
                }
                if (key == "Name")
                {
                    network.name = scalar(child, at + ".Name");
                    hasName = true;
                }
                else if (key == "Type")
                {
                    network.type = scalar(child, at + ".Type");
                    hasType = true;
                }
                else
                {
                    config.warnings.push_back("unknown key '" + key + "' in " + at + " is ignored");
                }
            }
            if (!hasName || network.name.empty())
            {
                fail(at + ".Name is missing");
            }
            if (!hasType)
            {
                fail(at + ".Type is missing");
            }
            static const std::set<std::string> kTypes{"CAN", "LIN", "Ethernet", "FlexRay"};
            if (kTypes.count(network.type) == 0)
            {
                fail(at + ".Type '" + network.type + "' is not CAN, LIN, Ethernet or FlexRay");
            }
            if (!names.insert(network.name).second)
            {
                fail("network '" + network.name + "' is listed twice");
            }
            if (network.type != "CAN")
            {
                config.warnings.push_back("network '" + network.name + "' (" + network.type +
                                          ") is not simulated: only CAN networks are");
            }
            config.networks.push_back(std::move(network));
        }
    }

    const ryml::Tree& tree_;
    std::string where_;
};

} // namespace

std::vector<std::string> NetworkSimulatorConfig::can_networks() const
{
    std::vector<std::string> out;
    for (const auto& network : networks)
    {
        if (network.type == "CAN")
        {
            out.push_back(network.name);
        }
    }
    return out;
}

NetworkSimulatorConfig parse_network_simulator_config(std::string_view text, std::string_view source)
{
    const std::string where(source);
    const auto first = std::find_if(text.begin(), text.end(), [](char c) { return !std::isspace(static_cast<unsigned char>(c)); });
    if (first == text.end())
    {
        throw NetworkSimulatorConfigError(where + ": the configuration is empty");
    }
    const bool json = *first == '{';
    try
    {
        ryml::Callbacks callbacks(nullptr, nullptr, nullptr, &on_parse_error);
        ryml::EventHandlerTree handler(callbacks);
        ryml::Parser parser(&handler);
        const ryml::csubstr name(where.data(), where.size());
        const ryml::csubstr content(text.data(), text.size());
        const ryml::Tree tree = json ? ryml::parse_json_in_arena(&parser, name, content) : ryml::parse_in_arena(&parser, name, content);
        return Reader(tree, where).read();
    }
    catch (const NetworkSimulatorConfigError& e)
    {
        const std::string message = e.what();
        if (message.rfind(where + ":", 0) == 0)
        {
            throw;
        }
        throw NetworkSimulatorConfigError(where + ": " + message);
    }
    catch (const std::exception& e)
    {
        throw NetworkSimulatorConfigError(where + ": " + e.what());
    }
}

NetworkSimulatorConfig load_network_simulator_config(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw NetworkSimulatorConfigError(path + ": cannot open the file");
    }
    std::ostringstream content;
    content << in.rdbuf();
    if (in.bad())
    {
        throw NetworkSimulatorConfigError(path + ": cannot read the file");
    }
    return parse_network_simulator_config(content.str(), path);
}

} // namespace vehicle_sil::can
