// SPDX-License-Identifier: MIT
//
// read_request_file(): the text of a request file (run --request, check --request) as a
// RequestFile. JSON, read by the reader that the configuration and the scenarios share, so that an
// unknown key and a key written twice are said in the same words. See request.hpp.
#include "vehicle_sil/launch/request.hpp"

#include "vehicle_sil/config/detail/yaml_reader.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::launch {
namespace {

const std::vector<std::string> kKeys{"vehicle_sil_run_request", "variant", "params",      "scenario",     "disable", "enable",
                                     "manual",                  "connect", "time_policy", "speed",        "start_paused",
                                     "strict"};

class Reader : private config::detail::YamlReader
{
public:
    Reader(const config::detail::YamlDocument& doc, std::vector<config::Finding>& findings) : config::detail::YamlReader(doc, findings) {}

    RequestFile read()
    {
        RequestFile out;
        RunIntent& intent = out.intent;
        const ryml::id_type root = doc_.root();
        bool version = false;
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(root); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string key = config::detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, key, child))
            {
                continue;
            }
            if (key == "vehicle_sil_run_request")
            {
                version = true;
                std::int64_t value = 0;
                if (!plain_integer(child, value) || value != kRunRequestFormat)
                {
                    error("value", doc_.line(child),
                          "vehicle_sil_run_request must be " + std::to_string(kRunRequestFormat) +
                              " (the version of the request file that this vehicle_sil_launch reads), not " +
                              shown_value(child));
                }
                continue;
            }
            if (key == "$schema")
            {
                continue; // an editor's: where the schema of this file is
            }
            if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end())
            {
                unknown_key(child, key, "", kKeys);
                continue;
            }
            const std::size_t before = findings_.size();
            if (key == "variant")
            {
                intent.variant = text(child, "variant", "the name of a variant of the configuration");
            }
            else if (key == "scenario")
            {
                intent.scenario = text(child, "scenario",
                                       "the path of a scenario file (from the directory of the configuration file)");
            }
            else if (key == "params")
            {
                intent.params = read_params(child, "", config::ParamSource::RequestFile);
                for (config::ParamValue& param : intent.params)
                {
                    param.line = 0; // a line of the configuration file, which this is not
                }
            }
            else if (key == "disable")
            {
                intent.disable = names(child, key);
            }
            else if (key == "enable")
            {
                intent.enable = names(child, key);
            }
            else if (key == "manual")
            {
                intent.manual = names(child, key);
            }
            else if (key == "connect")
            {
                connect(child, intent.connect);
            }
            else if (key == "time_policy")
            {
                const std::optional<config::TimePolicy> policy =
                    quoted(child) ? config::time_policy_named(config::detail::to_string(tree_.val(child))) : std::nullopt;
                if (policy)
                {
                    intent.time_policy = *policy;
                }
                else
                {
                    error("value", doc_.line(child),
                          "time_policy must be \"realtime\" (a run that is connected outside the simulation goes at speed "
                          "1; the default) or \"free\" (its speed and time can be changed), not " +
                              shown_value(child));
                }
            }
            else if (key == "speed")
            {
                // A number, or the text "max".
                const std::string written = is_scalar(child) && !is_null(child) ? config::detail::to_string(tree_.val(child)) : "";
                std::optional<double> speed;
                if (is_scalar(child) && !is_null(child) && config::parse_speed(written, speed) && quoted(child) == !speed)
                {
                    intent.speed.emplace(speed);
                }
                else
                {
                    error("value", doc_.line(child),
                          "speed must be a number of simulation seconds per wall-clock second from 0.000001 to 1000000 (1: "
                          "real time, 2: twice as fast) or \"max\" (as fast as possible), not " +
                              shown_value(child));
                }
            }
            else if (key == "start_paused")
            {
                intent.start_paused = flag(child, key);
            }
            else if (key == "strict")
            {
                intent.strict = flag(child, key);
            }
            if (findings_.size() == before)
            {
                out.fields.push_back(key);
            }
        }
        if (!version)
        {
            error("value", root == ryml::NONE ? 0 : doc_.line(root),
                  "vehicle_sil_run_request is missing: write \"vehicle_sil_run_request\": " +
                      std::to_string(kRunRequestFormat) + " (it says that this is a request file, and of which version)");
        }
        both(intent.disable, intent.enable, "enable", "");
        both(intent.disable, intent.manual, "manual", " (disable leaves it out, manual has you start it)");
        return out;
    }

private:
    bool quoted(ryml::id_type node) const { return is_scalar(node) && !is_null(node) && tree_.is_val_quoted(node); }

    /// A text that is not empty; "" with an error.
    std::string text(ryml::id_type node, const std::string& key, const std::string& meaning)
    {
        std::string out = quoted(node) ? config::detail::to_string(tree_.val(node)) : std::string{};
        if (out.empty())
        {
            error("value", doc_.line(node), key + " must be " + meaning + " as a text, not " + shown_value(node));
        }
        return out;
    }

    bool flag(ryml::id_type node, const std::string& key)
    {
        const std::string written = is_scalar(node) && !quoted(node) && !is_null(node) ? config::detail::to_string(tree_.val(node)) : "";
        if (written != "true" && written != "false")
        {
            error("value", doc_.line(node), key + " must be true or false, not " + shown_value(node));
        }
        return written == "true";
    }

    /// A list of participant names.
    std::vector<std::string> names(ryml::id_type node, const std::string& key)
    {
        std::vector<std::string> out;
        if (!tree_.is_seq(node))
        {
            error("value", doc_.line(node),
                  key + " must be a list of participant names (" + key + ": [\"Engine\"]), not " + shown_value(node));
            return out;
        }
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            if (!quoted(child) || tree_.val(child).empty())
            {
                error("value", doc_.line(child),
                      key + " must be a list of participant names, each a text; one of them is " + shown_value(child));
                continue;
            }
            out.push_back(config::detail::to_string(tree_.val(child)));
        }
        return out;
    }

    /// "connect": { "<network>": "<target>" }.
    void connect(ryml::id_type node, std::vector<config::ConnectRequest>& out)
    {
        if (!tree_.is_map(node))
        {
            error("value", doc_.line(node),
                  "connect must be a mapping of network names to targets (connect: { \"CAN1\": \"physical:1\" }; "
                  "the target as --connect takes it, \"none\": SIL Kit only), not " +
                      shown_value(node));
            return;
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string network = config::detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, network, child, "connect: "))
            {
                continue;
            }
            if (network.empty() || !quoted(child))
            {
                error("value", doc_.line(child),
                      "connect: " + (network.empty() ? std::string("a network needs its name") : network + " must be a target") +
                          " as a text (\"CAN1\": \"physical:1\"; \"none\": SIL Kit only), not " + shown_value(child));
                continue;
            }
            config::ConnectRequest& request = out.emplace_back();
            request.network = network;
            request.target = config::detail::to_string(tree_.val(child));
        }
    }

    /// The names that are in both lists of the file: it cannot mean both.
    void both(const std::vector<std::string>& disable, const std::vector<std::string>& other, const std::string& key,
              const std::string& meaning)
    {
        std::vector<std::string> said;
        for (const std::string& name : other)
        {
            if (std::find(disable.begin(), disable.end(), name) != disable.end() &&
                std::find(said.begin(), said.end(), name) == said.end())
            {
                said.push_back(name);
                error("value", 0, "disable and " + key + " both name " + name + ": keep one of them" + meaning);
            }
        }
    }
};

} // namespace

RequestFileRead read_request_file(std::string_view text, std::string_view shown)
{
    RequestFileRead out;
    std::vector<config::Finding> found;
    std::string_view content = text;
    if (content.substr(0, 3) == "\xEF\xBB\xBF")
    {
        content.remove_prefix(3);
    }
    const auto first = content.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos || content[first] != '{')
    {
        found.push_back(config::Finding{config::Severity::Error, "value",
                                std::string(first == std::string_view::npos ? "the file is empty" : "this is not a JSON object") +
                                    ": a request file is { \"vehicle_sil_run_request\": " + std::to_string(kRunRequestFormat) +
                                    ", ... } with what to run (docs/simulation-run.md; schema/run-request.schema.json)",
                                0, "", ""});
    }
    else
    {
        try
        {
            const config::detail::YamlDocument doc(text);
            RequestFile file = Reader(doc, found).read();
            if (found.empty())
            {
                file.shown = std::string(shown);
                out.file = std::move(file);
            }
        }
        catch (const config::detail::YamlSyntaxError& e)
        {
            found.push_back(config::Finding{config::Severity::Error, "syntax", "syntax error: " + e.message(), e.line(), "", "", e.context()});
        }
    }
    // A usage error of the command: it is said before any file of the simulation is read.
    for (config::Finding& finding : found)
    {
        finding.code = "usage";
        finding.file = std::string(shown);
        finding.message = std::string(shown) + (finding.line > 0 ? ", line " + std::to_string(finding.line) : "") + ": " +
                          finding.message;
        out.findings.push_back(std::move(finding));
    }
    return out;
}

} // namespace vehicle_sil::launch
