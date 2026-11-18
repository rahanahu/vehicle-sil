// SPDX-License-Identifier: MIT
//
// A parsed YAML or JSON text with the line of each node (rapidyaml, private to vehicle_sil_launch).
#pragma once

#include <rapidyaml.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vehicle_sil::launch::detail {

/// A syntax error, with its line (from 1; 0 if unknown).
class YamlSyntaxError : public std::runtime_error
{
public:
    YamlSyntaxError(const std::string& text, std::size_t line) : std::runtime_error(text), line_(line) {}
    [[nodiscard]] std::size_t line() const noexcept { return line_; }

    /// The first line of the parser's text, without its "ERROR: ".
    [[nodiscard]] std::string message() const
    {
        std::string first = std::string(what()).substr(0, std::string(what()).find('\n'));
        if (first.rfind("ERROR: ", 0) == 0)
        {
            first.erase(0, 7);
        }
        return first;
    }

    /// The parser's further lines (the source line and a mark under the place), without the sizes
    /// and columns it adds at their ends, and without empty lines at the end.
    [[nodiscard]] std::string context() const
    {
        const std::string text = what();
        const auto newline = text.find('\n');
        std::string out;
        for (std::size_t at = newline == std::string::npos ? text.size() : newline + 1; at < text.size();)
        {
            const auto end = std::min(text.find('\n', at), text.size());
            std::string line = text.substr(at, end - at);
            for (const char* tail : {"  (size=", "  (cols "})
            {
                if (const auto cut = line.rfind(tail); cut != std::string::npos)
                {
                    line.erase(cut);
                }
            }
            while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
            {
                line.pop_back();
            }
            out += line + "\n";
            at = end + 1;
        }
        while (!out.empty() && out.back() == '\n')
        {
            out.pop_back();
        }
        return out;
    }

private:
    std::size_t line_;
};

[[nodiscard]] inline std::string to_string(ryml::csubstr s)
{
    return std::string(s.str == nullptr ? "" : s.str, s.len);
}

/// One parsed text. YAML, or JSON if the first character that is not white space is '{' (a
/// UTF-8 byte order mark first is skipped). Throws YamlSyntaxError.
class YamlDocument
{
public:
    explicit YamlDocument(std::string_view text) :
      handler_(ryml::Callbacks(nullptr, nullptr, nullptr, &on_error)),
      parser_(std::make_unique<ryml::Parser>(&handler_, ryml::ParserOptions().locations(true)))
    {
        if (text.substr(0, 3) == "\xEF\xBB\xBF")
        {
            text.remove_prefix(3);
        }
        const auto first = text.find_first_not_of(" \t\r\n");
        empty_ = first == std::string_view::npos;
        if (empty_)
        {
            return;
        }
        const ryml::csubstr content(text.data(), text.size());
        tree_ = text[first] == '{' ? ryml::parse_json_in_arena(parser_.get(), content)
                                   : ryml::parse_in_arena(parser_.get(), content);
    }

    YamlDocument(const YamlDocument&) = delete;
    YamlDocument& operator=(const YamlDocument&) = delete;

    /// true if the text has nothing but white space.
    [[nodiscard]] bool empty() const noexcept { return empty_; }
    [[nodiscard]] const ryml::Tree& tree() const noexcept { return tree_; }

    /// The node of the (first) document, or ryml::NONE.
    [[nodiscard]] ryml::id_type root() const
    {
        if (empty_ || tree_.empty())
        {
            return ryml::NONE;
        }
        ryml::id_type root = tree_.root_id();
        if (tree_.is_stream(root))
        {
            root = tree_.first_child(root); // a file with "---": its first document
        }
        return root;
    }

    /// A node of the first document after the first one that has content ("---" and more), or
    /// ryml::NONE. An empty document (a "---" at the end) does not count.
    [[nodiscard]] ryml::id_type second_document() const
    {
        if (empty_ || tree_.empty() || !tree_.is_stream(tree_.root_id()))
        {
            return ryml::NONE;
        }
        const ryml::id_type first = tree_.first_child(tree_.root_id());
        for (ryml::id_type doc = first == ryml::NONE ? ryml::NONE : tree_.next_sibling(first); doc != ryml::NONE;
             doc = tree_.next_sibling(doc))
        {
            if (tree_.has_children(doc))
            {
                return tree_.first_child(doc); // its line is the document's first line of content
            }
            if (tree_.has_val(doc) && !tree_.val_is_null(doc))
            {
                return doc;
            }
        }
        return ryml::NONE;
    }

    /// The line of `node` (from 1).
    [[nodiscard]] std::size_t line(ryml::id_type node) const { return parser_->location(tree_, node).line + 1; }

private:
    [[noreturn]] static void on_error(const char* message, std::size_t length, ryml::Location location, void*)
    {
        // The parser counts the lines of its errors from 1 (those of the nodes from 0).
        throw YamlSyntaxError(std::string(message, length), location.line);
    }

    ryml::EventHandlerTree handler_;
    std::unique_ptr<ryml::Parser> parser_;
    ryml::Tree tree_;
    bool empty_ = false;
};

} // namespace vehicle_sil::launch::detail
