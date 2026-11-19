// SPDX-License-Identifier: MIT
//
// The participants' --describe outputs that check --write-descriptions saves and check
// --descriptions takes instead of running the executables (probe.hpp). The file is JSON in a form
// of this launcher's own, which only the same vehicle-sil version reads: no other program is meant
// to read or write it, so it changes without a new format number.
#include "vehicle_sil/launch/probe.hpp"

#include "vehicle_sil/config/detail/yaml_tree.hpp"
#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/json/json_writer.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace vehicle_sil::launch {

namespace {

/// The first key of the file, with the number of its form.
constexpr const char* kFormatKey = "vehicle_sil_descriptions";
constexpr int kFormat = 1;
/// The largest file read: each output is at most 1 MiB (run_tool()).
constexpr std::uintmax_t kMaxBytes = std::uintmax_t{64} << 20;

/// What is wrong with the file's content.
class NotSaved : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Reads the parsed file: every key is required, of its type.
class Reader
{
public:
    explicit Reader(const detail::YamlDocument& doc) : tree_(doc.tree()), root_(doc.root()) {}

    /// The entries, or throws NotSaved; `written_by` gets the vehicle-sil version that wrote it.
    std::vector<SavedDescription> read(std::string& written_by) const
    {
        if (root_ == ryml::NONE || !tree_.is_map(root_) || find(root_, kFormatKey) == ryml::NONE)
        {
            throw NotSaved(std::string("it has no ") + kFormatKey);
        }
        if (integer(find(root_, kFormatKey), kFormatKey) != kFormat)
        {
            throw NotSaved(std::string("it is not of the form ") + std::to_string(kFormat));
        }
        written_by = text(child(root_, "vehicle_sil"), "vehicle_sil");
        if (written_by != vehicle_sil::version())
        {
            return {};
        }
        const ryml::id_type list = child(root_, "participants");
        if (!tree_.is_seq(list))
        {
            throw NotSaved("participants is not an array");
        }
        std::vector<SavedDescription> out;
        for (auto item = tree_.first_child(list); item != ryml::NONE; item = tree_.next_sibling(item))
        {
            if (!tree_.is_map(item))
            {
                throw NotSaved("an element of participants is not an object");
            }
            SavedDescription& entry = out.emplace_back();
            entry.name = text(child(item, "name"), "name");
            const ryml::id_type command = child(item, "command");
            if (!tree_.is_seq(command))
            {
                throw NotSaved("command is not an array");
            }
            for (auto word = tree_.first_child(command); word != ryml::NONE; word = tree_.next_sibling(word))
            {
                entry.command.push_back(text(word, "command"));
            }
            entry.executable.path = text(child(item, "executable"), "executable");
            const std::int64_t size = integer(child(item, "size"), "size");
            if (size < 0)
            {
                throw NotSaved("size is negative");
            }
            entry.executable.size = static_cast<std::uintmax_t>(size);
            entry.executable.modified = integer(child(item, "modified"), "modified");
            entry.output = text(child(item, "output"), "output");
        }
        return out;
    }

private:
    ryml::id_type find(ryml::id_type node, const char* key) const
    {
        return tree_.find_child(node, ryml::csubstr(key, std::char_traits<char>::length(key)));
    }
    ryml::id_type child(ryml::id_type node, const char* key) const
    {
        const ryml::id_type found = find(node, key);
        if (found == ryml::NONE)
        {
            throw NotSaved(std::string("the key ") + key + " is missing");
        }
        return found;
    }
    bool is_scalar(ryml::id_type node) const { return tree_.has_val(node) && !tree_.is_container(node); }
    std::int64_t integer(ryml::id_type node, const std::string& what) const
    {
        const std::string value = is_scalar(node) && !tree_.is_val_quoted(node) ? detail::to_string(tree_.val(node)) : "";
        std::int64_t out = 0;
        const char* end = value.data() + value.size();
        const auto [ptr, ec] = std::from_chars(value.data(), end, out);
        if (value.empty() || ec != std::errc{} || ptr != end)
        {
            throw NotSaved(what + " is not an integer");
        }
        return out;
    }
    std::string text(ryml::id_type node, const std::string& what) const
    {
        if (!is_scalar(node) || !tree_.is_val_quoted(node))
        {
            throw NotSaved(what + " is not a string");
        }
        return detail::to_string(tree_.val(node));
    }

    const ryml::Tree& tree_;
    ryml::id_type root_;
};

/// `text` is UTF-8 throughout: the JSON writer keeps it as it is (it would replace another byte).
bool is_utf8(std::string_view text)
{
    while (!text.empty())
    {
        const std::size_t n = json::utf8_sequence_length(text);
        if (n == 0)
        {
            return false;
        }
        text.remove_prefix(n);
    }
    return true;
}

std::string file_text(const std::vector<SavedDescription>& entries)
{
    json::JsonWriter w;
    w.begin_object();
    w.integer(kFormatKey, kFormat);
    w.string("vehicle_sil", vehicle_sil::version());
    w.begin_array("participants");
    for (const SavedDescription& entry : entries)
    {
        w.begin_object();
        w.string("name", entry.name);
        w.begin_array("command");
        for (const std::string& word : entry.command)
        {
            w.string({}, word);
        }
        w.end_array();
        w.string("executable", entry.executable.path);
        w.integer("size", static_cast<std::int64_t>(entry.executable.size));
        w.integer("modified", entry.executable.modified);
        w.string("output", entry.output);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.take() + "\n";
}

} // namespace

std::optional<ExecutableStamp> executable_stamp(const std::filesystem::path& executable)
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::absolute(executable, ec);
    if (ec)
    {
        return std::nullopt;
    }
    // One file under two spellings (a "..", a link to it) is one executable.
    if (std::filesystem::path real = std::filesystem::weakly_canonical(path, ec); !ec)
    {
        path = std::move(real);
    }
    ExecutableStamp stamp;
    stamp.path = process::utf8_from_path(path);
    stamp.size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        return std::nullopt;
    }
    const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        return std::nullopt;
    }
    stamp.modified = static_cast<std::int64_t>(modified.time_since_epoch().count());
    return stamp;
}

SavedDescriptions read_saved_descriptions(const std::filesystem::path& file)
{
    SavedDescriptions out;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && !ec)
    {
        return out; // nothing saved yet
    }
    if (!std::filesystem::is_regular_file(file, ec))
    {
        out.problem = "the file is not a regular file";
        return out;
    }
    if (std::filesystem::file_size(file, ec) > kMaxBytes)
    {
        out.problem = "the file is larger than " + std::to_string(kMaxBytes >> 20) + " MiB";
        return out;
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in && !in.eof())
    {
        out.problem = "the file cannot be read (check its permissions)";
        return out;
    }
    try
    {
        const detail::YamlDocument doc(text);
        std::string written_by;
        out.entries = Reader(doc).read(written_by);
        if (written_by != vehicle_sil::version())
        {
            out.problem = "vehicle-sil " + written_by + " wrote the file, and this vehicle_sil_launch is " +
                          vehicle_sil::version() + " (only the same version reads it)";
        }
    }
    catch (const detail::YamlSyntaxError& e)
    {
        out.entries.clear();
        out.problem = "the file is not one that check --write-descriptions wrote (" + e.message() + ")";
    }
    catch (const NotSaved& e)
    {
        out.entries.clear();
        out.problem = "the file is not one that check --write-descriptions wrote (" + std::string(e.what()) + ")";
    }
    return out;
}

std::string write_saved_descriptions(const std::filesystem::path& file, const std::vector<SavedDescription>& entries)
{
    std::vector<SavedDescription> kept;
    for (const SavedDescription& entry : entries)
    {
        // An output that is not UTF-8 would not come back as it was: it is left out (not described).
        if (is_utf8(entry.output))
        {
            kept.push_back(entry);
        }
    }
    const std::string text = file_text(kept);
    std::filesystem::path beside = file;
    beside += ".writing-" + std::to_string(std::random_device{}());
    {
        std::ofstream out(beside, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return "it cannot be created (does its directory exist, and may it be written?)";
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out)
        {
            std::error_code ignored;
            std::filesystem::remove(beside, ignored);
            return "it cannot be written (is the disk full?)";
        }
    }
    std::error_code ec;
    std::filesystem::rename(beside, file, ec);
    if (ec)
    {
        std::error_code ignored;
        std::filesystem::remove(beside, ignored);
        return "the file written beside it cannot replace it (" + ec.message() +
               "; on Windows, another program may have it open)";
    }
    return {};
}

DescribeSource describe_from_saved(std::vector<SavedDescription> saved)
{
    return [saved = std::move(saved)](const ParticipantConfig& participant, const std::filesystem::path& executable,
                                      const std::vector<std::string>& arguments) -> std::optional<DescribeRun> {
        const std::optional<ExecutableStamp> now = executable_stamp(executable);
        if (!now)
        {
            return std::nullopt;
        }
        for (const SavedDescription& entry : saved)
        {
            if (entry.name == participant.name && entry.command == participant.command && entry.executable == *now)
            {
                std::vector<std::string> words{participant.command.front()};
                words.insert(words.end(), arguments.begin(), arguments.end());
                std::optional<DescribeRun> run(std::in_place);
                run->command_text = detail::command_text(words);
                run->exit_code = 0;
                run->standard_output = entry.output;
                return run;
            }
        }
        return std::nullopt;
    };
}

} // namespace vehicle_sil::launch
