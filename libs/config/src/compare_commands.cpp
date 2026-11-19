// SPDX-License-Identifier: MIT
//
// compare_commands(): whether two configurations run the same commands (check --compare-commands).
// What counts is listed in check.hpp and in docs/simulation-check.md; a text whose compared parts
// do not read is not compared rather than taken as the same.
#include "vehicle_sil/config/check.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil::config {

namespace {

/// "a", "a and b", "a, b and c".
std::string and_list(const std::vector<std::string>& words)
{
    std::string out;
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        out += (i == 0 ? "" : i + 1 == words.size() ? " and " : ", ") + words[i];
    }
    return out;
}

/// The common part (no variant), then each variant laid over it: what runs in each.
struct View
{
    std::string variant; ///< "" for the common part
    SimulationConfig config;
};

std::vector<View> views_of(const SimulationConfig& common)
{
    std::vector<View> views;
    View& base = views.emplace_back();
    base.config = common;
    for (const VariantConfig& variant : common.variants)
    {
        View& view = views.emplace_back();
        view.variant = variant.name;
        view.config = common;
        detail::apply_variant(view.config, variant);
    }
    return views;
}

/// One of the two texts, read for the comparison: its views, or why it cannot be compared.
struct Side
{
    std::vector<View> views{};
    std::string problem{}; ///< "" if it can be compared
};

/// How the problems of one text are said.
struct Wording
{
    std::string subject;    ///< "the configuration being checked", or "it" for the other file
    std::string unreadable; ///< the problem of a text that is not a configuration
    std::string see_errors; ///< how to see and correct its errors
};

Side read_side(std::string_view text, const Wording& wording)
{
    Side side;
    ConfigRead read = read_config(text);
    if (!read.config)
    {
        side.problem = wording.unreadable;
        return side;
    }
    if (!read.command_parts_with_errors.empty())
    {
        side.problem = wording.subject + " has errors in " + and_list(read.command_parts_with_errors) +
                       ", so what it runs is not known for sure: " + wording.see_errors;
        return side;
    }
    side.views = views_of(*read.config);
    for (const View& view : side.views)
    {
        for (const ParticipantConfig& p : view.config.participants)
        {
            if (!p.valid)
            {
                side.problem = wording.subject + " has errors in participant '" + p.name + "'" +
                               (view.variant.empty() ? std::string{} : " in variant '" + view.variant + "'") +
                               ", so what it runs is not known for sure: " + wording.see_errors;
                side.views.clear();
                return side;
            }
        }
    }
    return side;
}

const ParticipantConfig* find_participant(const SimulationConfig& config, const std::string& name)
{
    const auto at = std::find_if(config.participants.begin(), config.participants.end(),
                                 [&name](const ParticipantConfig& p) { return p.name == name; });
    return at == config.participants.end() ? nullptr : &*at;
}

const View* find_view(const std::vector<View>& views, const std::string& variant)
{
    const auto at = std::find_if(views.begin(), views.end(), [&variant](const View& v) { return v.variant == variant; });
    return at == views.end() ? nullptr : &*at;
}

/// "participant 'X' is new" and "... was removed" for the names of `ours` and `theirs`.
void added_and_removed(const std::vector<std::string>& ours, const std::vector<std::string>& theirs, const std::string& what,
                       std::vector<std::string>& differences)
{
    for (const std::string& name : ours)
    {
        if (std::find(theirs.begin(), theirs.end(), name) == theirs.end())
        {
            differences.push_back(what + " '" + name + "' is new");
        }
    }
    for (const std::string& name : theirs)
    {
        if (std::find(ours.begin(), ours.end(), name) == ours.end())
        {
            differences.push_back(what + " '" + name + "' was removed");
        }
    }
}

std::vector<std::string> participant_names(const SimulationConfig& config)
{
    std::vector<std::string> names;
    for (const ParticipantConfig& p : config.participants)
    {
        names.push_back(p.name);
    }
    return names;
}

std::vector<std::string> variant_names(const SimulationConfig& config)
{
    std::vector<std::string> names;
    for (const VariantConfig& v : config.variants)
    {
        names.push_back(v.name);
    }
    return names;
}

/// What of `ours` runs otherwise than `theirs`, in the order of check.hpp.
std::vector<std::string> keys_that_differ(const ParticipantConfig& ours, const ParticipantConfig& theirs)
{
    std::vector<std::string> keys;
    if (ours.command != theirs.command)
    {
        keys.emplace_back("command");
    }
    if (ours.describe != theirs.describe)
    {
        keys.emplace_back("describe");
    }
    if (ours.manual != theirs.manual)
    {
        keys.emplace_back("manual");
    }
    if (ours.enabled != theirs.enabled)
    {
        keys.emplace_back("enabled");
    }
    return keys;
}

/// How many differences the note names; the rest are counted.
constexpr std::size_t kShownDifferences = 3;

/// Makes `note` a note with `code` and `message`, in its place (a Finding made beside it and moved
/// in is what GCC's -Wmaybe-uninitialized of MinGW takes for a use before it is set).
void set_note(std::optional<Finding>& note, std::string code, std::string message)
{
    Finding& f = note.emplace();
    f.severity = Severity::Note;
    f.code = std::move(code);
    f.message = std::move(message);
}

std::string not_compared_message(std::string_view other_file, std::string_view why)
{
    return "the commands that run are not compared with " + std::string(other_file) + ": " + std::string(why);
}

} // namespace

Finding commands_not_compared(std::string_view other_file, std::string_view why)
{
    return Finding{Severity::Note, "commands-not-compared", not_compared_message(other_file, why), 0, "", ""};
}

CommandsComparison compare_commands(std::string_view text, std::string_view other_text, std::string_view other_file)
{
    CommandsComparison out;
    const Side ours = read_side(text, Wording{"the configuration being checked",
                                              "the configuration being checked cannot be read: see the errors",
                                              "see the errors, and correct them"});
    if (!ours.problem.empty())
    {
        set_note(out.note, "commands-not-compared", not_compared_message(other_file, ours.problem));
        return out;
    }
    const Side theirs =
        read_side(other_text, Wording{"it", "it cannot be read as a configuration: check it with vehicle_sil_launch check to see why",
                                      "check it with vehicle_sil_launch check to see them, and correct them"});
    if (!theirs.problem.empty())
    {
        set_note(out.note, "commands-not-compared", not_compared_message(other_file, theirs.problem));
        return out;
    }
    const SimulationConfig& ourCommon = ours.views.front().config;
    const SimulationConfig& theirCommon = theirs.views.front().config;
    std::vector<std::string> differences;
    if (ourCommon.registry_uri != theirCommon.registry_uri)
    {
        differences.emplace_back("registry_uri differs");
    }
    added_and_removed(participant_names(ourCommon), participant_names(theirCommon), "participant", differences);
    added_and_removed(variant_names(ourCommon), variant_names(theirCommon), "variant", differences);
    const bool hasVariants = !ourCommon.variants.empty() || !theirCommon.variants.empty();
    for (const View& view : ours.views)
    {
        const View* other = find_view(theirs.views, view.variant);
        if (other == nullptr)
        {
            continue; // a new variant: said above
        }
        const std::string where = view.variant.empty() ? (hasVariants ? " (the common part)" : "")
                                                       : " (variant '" + view.variant + "')";
        for (const ParticipantConfig& p : view.config.participants)
        {
            const ParticipantConfig* before = find_participant(other->config, p.name);
            if (before == nullptr)
            {
                continue; // a new participant: said above
            }
            if (const std::vector<std::string> keys = keys_that_differ(p, *before); !keys.empty())
            {
                differences.push_back("participant '" + p.name + "': " + and_list(keys) +
                                      (keys.size() == 1 ? " differs" : " differ") + where);
            }
        }
    }
    if (differences.empty())
    {
        out.result = CommandsCompared::Same;
        return out;
    }
    std::string list;
    for (std::size_t i = 0; i < differences.size() && i < kShownDifferences; ++i)
    {
        list += (i == 0 ? "" : "; ") + differences[i];
    }
    if (differences.size() > kShownDifferences)
    {
        const std::size_t more = differences.size() - kShownDifferences;
        list += "; and " + std::to_string(more) + " more " + (more == 1 ? "difference" : "differences");
    }
    out.result = CommandsCompared::Changed;
    set_note(out.note, "commands-changed", "the commands that run differ from " + std::string(other_file) + ": " + list);
    return out;
}

} // namespace vehicle_sil::config
