// SPDX-License-Identifier: MIT
//
// Reading the JSON of --describe (read_description()): the reverse of to_json() of
// vehicle_sil/ecu/describe.hpp. Every error names the key.
#include "vehicle_sil/launch/check.hpp"

#include "check_words.hpp"
#include "vehicle_sil/ecu/detail/json_writer.hpp"
#include "yaml_tree.hpp"

#include <array>
#include <charconv>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace vehicle_sil::launch {

namespace {

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
        // The vehicle-sil it was built with, for the messages about a newer or older one.
        const auto versionNode = find(root_, "vehicle_sil");
        if (versionNode != ryml::NONE && is_scalar(versionNode) && tree_.is_val_quoted(versionNode))
        {
            version_ = detail::to_string(tree_.val(versionNode));
        }
        if (format != kEcuDescriptionFormat)
        {
            throw DescriptionFormatError(format, version_);
        }
        ReportedEcu out;
        if (versionNode != ryml::NONE)
        {
            out.vehicle_sil_version = text(versionNode, "vehicle_sil");
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
        std::vector<std::string> known;
        for (const Enum value : values)
        {
            if (given == name(value))
            {
                return value;
            }
            known.emplace_back(name(value));
        }
        unknown_value(what + " is \"" + given + "\", not one of " + detail::join(known), what, given, known);
    }

    /// A value this launcher does not know of a key with a fixed set of values (`known`).
    [[noreturn]] void unknown_value(const std::string& message, const std::string& key, const std::string& value,
                                    std::vector<std::string> known) const
    {
        throw DescriptionValueError(message, key, value, std::move(known), version_);
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
        const auto known = network_use_named(use);
        if (!known)
        {
            const auto uses = detail::network_use_names();
            unknown_value(at + ".use is \"" + use + "\", not " + detail::or_list(uses), at + ".use", use, uses);
        }
        out.use = *known;
        const std::string kind = text(member(node, "kind", at), at + ".kind");
        if (!network_kind_named(kind))
        {
            std::vector<std::string> kinds;
            for (const NetworkKind k : kNetworkKinds)
            {
                kinds.emplace_back(to_string(k));
            }
            unknown_value(at + ".kind is \"" + kind + "\", not " + detail::or_list(kinds), at + ".kind", kind, kinds);
        }
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
        // Absent in the descriptions of the versions before Ethernet: as null.
        if (const auto mac = find(node, "mac"); mac != ryml::NONE && !is_null(mac))
        {
            const std::string address = text(mac, at + ".mac");
            out.mac = eth::MacAddress::parse(address);
            if (!out.mac)
            {
                fail(at + ".mac is \"" + address + "\", not six pairs of hexadecimal digits like \"02:00:00:00:00:01\"");
            }
            if (out.use != NetworkUse::Ethernet)
            {
                fail(at + " has a mac, but only an ethernet network has one");
            }
        }
        return out;
    }

    const ryml::Tree& tree_;
    ryml::id_type root_;
    std::string version_{}; ///< "vehicle_sil" of the description, or ""
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

} // namespace vehicle_sil::launch
