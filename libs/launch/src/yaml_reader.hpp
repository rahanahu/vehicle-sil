// SPDX-License-Identifier: MIT
//
// What the readers of the configuration (config.cpp) and of a scenario (scenario.cpp) share: the
// findings they collect, the YAML they refuse (anchors and aliases), a key written twice, an
// unknown key with its "did you mean", and how a value is shown and read as an integer. A new
// reader derives from it too, so that these messages and their lines stay the same. Internal.
#pragma once

#include "vehicle_sil/launch/config.hpp"

#include "check_words.hpp"
#include "yaml_tree.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace vehicle_sil::launch::detail {

class YamlReader
{
protected:
    /// Reads `doc`; the findings go to `findings`.
    YamlReader(const YamlDocument& doc, std::vector<Finding>& findings) : doc_(doc), tree_(doc.tree()), findings_(findings) {}

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

    /// false (and an error) if `key` was seen before in this mapping.
    bool first_occurrence(std::map<std::string, std::size_t>& seen, const std::string& key, ryml::id_type node,
                          const std::string& prefix = {})
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

    /// The error of a key that is not one of `known` (the keys of this mapping).
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

    /// How a value is shown in a message: 'text', the text "text" (quoted), or what it is.
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

    /// `text` as a decimal integer (with an optional '-').
    static bool read_integer(const std::string& text, std::int64_t& value)
    {
        const char* end = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(text.data(), end, value);
        return !text.empty() && ec == std::errc{} && ptr == end;
    }

    /// `text` as a finite decimal number ("1", "-2.5", "1e3").
    static bool read_number(const std::string& text, double& value)
    {
        const bool plain = !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+';
        }) && (std::isdigit(static_cast<unsigned char>(text.front())) != 0 || text.front() == '-');
        const char* end = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(text.data(), end, value);
        return plain && ec == std::errc{} && ptr == end && std::isfinite(value);
    }

    /// An unquoted decimal integer (with an optional '-').
    bool plain_integer(ryml::id_type node, std::int64_t& value) const
    {
        return is_scalar(node) && !tree_.is_val_quoted(node) && !is_null(node) &&
               read_integer(detail::to_string(tree_.val(node)), value);
    }

    /// An unquoted finite decimal number ("1", "-2.5", "1e3").
    bool plain_number(ryml::id_type node, double& value) const
    {
        return is_scalar(node) && !tree_.is_val_quoted(node) && !is_null(node) &&
               read_number(detail::to_string(tree_.val(node)), value);
    }

    // --- parameters ---

    /// A "params" mapping (of the configuration, of a variant, of a scenario): each name with its
    /// value as written. An entry with an error is left out (and reported).
    std::vector<ParamValue> read_params(ryml::id_type node, const std::string& prefix, ParamSource source)
    {
        std::vector<ParamValue> out;
        if (!tree_.is_map(node))
        {
            error("value", doc_.line(node),
                  prefix + "params must be a mapping of parameter names to values (params: { idle_rpm: 800 }), not " +
                      shown_value(node));
            return out;
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            ParamValue param;
            param.name = detail::to_string(tree_.key(child));
            param.source = source;
            param.line = doc_.line(child);
            if (!first_occurrence(seen, param.name, child, prefix + "params: "))
            {
                continue;
            }
            if (!is_param_name(param.name))
            {
                error("value", param.line,
                      prefix + "params: '" + param.name +
                          "' cannot be a parameter's name: use letters, digits and _ (not a digit first), as a scenario "
                          "writes it in ${...}");
                continue;
            }
            if (!is_scalar(child) || is_null(child))
            {
                error("value", param.line,
                      prefix + "params: " + param.name + " must be one value (a number, true or false, or a text), not " +
                          shown_value(child));
                continue;
            }
            param.text = detail::to_string(tree_.val(child));
            param.quoted = tree_.is_val_quoted(child);
            if (param.text.find("${") != std::string::npos)
            {
                error("value", param.line,
                      prefix + "params: " + param.name + " is " + shown_value(child) +
                          ", but a parameter's value cannot use another parameter (${...}): write the value out");
                continue;
            }
            out.push_back(std::move(param));
        }
        return out;
    }

    const YamlDocument& doc_;
    const ryml::Tree& tree_;
    std::vector<Finding>& findings_;
};

} // namespace vehicle_sil::launch::detail
