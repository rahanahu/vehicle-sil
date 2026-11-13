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

#include <charconv>
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

    const YamlDocument& doc_;
    const ryml::Tree& tree_;
    std::vector<Finding>& findings_;
};

} // namespace vehicle_sil::launch::detail
