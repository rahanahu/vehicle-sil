// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch check as a process, with the ECU executables of examples/ecu_main and a
// helper that misbehaves (tests/process/process_helper.cpp):
//
//   docs        the configuration files and outputs of docs/simulation-config.md,
//               simulation-check.md and simulation-events.md, byte for byte but for the
//               vehicle-sil version: the example's simulation.yaml (exit 0), the
//               file with errors (exit 1), the JSON (exit 1), the example without Dashboard
//   describe    executables that fail --describe: exit code 4, too slow, not JSON, another
//               format version, missing; one with describe: false (exit 1, the whole text)
//   strict      warnings only: exit 0, with --strict exit 1
//   unreadable  a missing file, a directory, a syntax error, another format version (exit 3,
//               also with --json)
//   usage       command lines that are wrong (exit 4, the message) and --help (exit 0)
//   enabled     a disabled participant whose executable does not exist (exit 0, --strict too),
//               --enable and --disable, and the names they must have (exit 4)
//   request     --request <file>: the same report as with the arguments, an argument wins, the
//               file's scenario beside the configuration, no file read that is not named, a file
//               that cannot be read or is no request file (exit 4, also as JSON and as the result
//               event), not for test
//   connect     --connect with the bridges of this repository (on fake libraries) under their real
//               names: found on PATH and next to the launcher, not in the working directory or the
//               configuration file's; the networks' "outside" in the JSON; a bridge that refuses
//               its arguments; the command lines that are wrong (exit 4)
//   connections where each network can be connected, with the same bridges (the fake pcap library's
//               devices, the fake XL driver's channels): the --connect lines, which check takes; the
//               JSON; --network; bridges that cannot list and no bridge (exit 1); the command line
//               (exit 4) and a file that cannot be read (exit 3)
//   descriptions check --write-descriptions and --descriptions: a check with saved descriptions starts no
//               process (participants, role: bridge, --connect and the request file's connect: a copy of
//               the helper marks each start), the same report as the check that saved them, a changed
//               command or executable, a file that is missing, broken or of another version, a file that
//               cannot be written; --text-from-stdin (the text as of a file, CRLF, too large, NUL); run
//               and test refuse the three (exit 4)
//   compare     check --compare-commands: "commands" of the JSON (same, changed, not_compared for a file
//               that cannot be read, null without the option), the notes, with --text-from-stdin and --descriptions
//               (nothing starts), the text for people; run and test refuse it (exit 4)
#include "launcher_support.hpp"
#include "test_timeout_scale.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/process/child_process.hpp"
#include "vehicle_sil/process/process.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil::test;
using vehicle_sil::process::Captured;
using vehicle_sil::process::shown_path;

namespace {

struct Options
{
    fs::path launch;
    fs::path helper;
    fs::path engine;
    fs::path example_dir; ///< the example's directory in the build tree (simulation.yaml, engine/, dashboard/)
    fs::path example_config; ///< examples/ecu_main/simulation.yaml in the source tree
    fs::path docs; ///< the directory docs/ (each example is in the one document that has its marker line)
    fs::path connect_tools; ///< vehicle_sil_launch and the two bridges under their names
    fs::path fake_pcap_dir; ///< the fake pcap library (connections)
    fs::path work_dir;
    std::string scenario;
};

Options parse(int argc, char** argv)
{
    const vehicle_sil::cli::Args args(argc, argv);
    Options o;
    o.launch = args.required("launch");
    o.helper = args.required("helper");
    o.engine = args.required("engine");
    o.example_dir = args.required("example-dir");
    o.example_config = args.required("example-config");
    o.docs = args.required("docs");
    o.connect_tools = args.required("connect-tools");
    o.fake_pcap_dir = args.str("fake-pcap-dir", "");
    o.work_dir = args.required("work-dir");
    o.scenario = args.required("scenario");
    return o;
}

void require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string lf(std::string text)
{
    std::erase(text, '\r');
    return text;
}

void replace_all(std::string& text, const std::string& from, const std::string& to)
{
    if (from.empty())
    {
        return;
    }
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    {
        text.replace(at, from.size(), to);
    }
}

/// `text` with every value of a "vehicle_sil" key (a version) replaced by "<version>".
std::string without_versions(std::string text)
{
    const std::string key = "\"vehicle_sil\": \"";
    for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + 1))
    {
        const auto from = at + key.size();
        const auto to = text.find('"', from);
        require(to != std::string::npos, "a \"vehicle_sil\" value does not end");
        text.replace(from, to - from, "<version>");
    }
    return text;
}

/// The Markdown file directly in the directory `docs` that has the line `marker` (exactly one has it).
fs::path marked_file(const fs::path& docs, const std::string& marker)
{
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(docs))
    {
        if (entry.path().extension() == ".md" && lf(read_file(entry.path())).find(marker + "\n") != std::string::npos)
        {
            found.push_back(entry.path());
        }
    }
    require(!found.empty(), "no document in " + docs.string() + " has the line " + marker);
    if (found.size() > 1)
    {
        require(false, "two documents in " + docs.string() + " have the line " + marker + ": " +
                           found[0].filename().string() + ", " + found[1].filename().string() +
                           " (keep the line in one of them)");
    }
    return found[0];
}

/// The fenced block after the line `marker` in the Markdown file, or in the one document of the
/// directory that has the line, without the fences.
std::string block_after(const fs::path& path, const std::string& marker)
{
    if (fs::is_directory(path))
    {
        return block_after(marked_file(path, marker), marker);
    }
    const std::string text = lf(read_file(path));
    const auto at = text.find(marker + "\n");
    require(at != std::string::npos, path.string() + " has no line " + marker);
    const auto open = text.find('\n', text.find("```", at)) + 1;
    const auto close = text.find("\n```", open);
    require(close != std::string::npos, path.string() + ": the block after " + marker + " does not end");
    return text.substr(open, close - open + 1);
}

void write_file(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
    require(static_cast<bool>(out), "cannot write " + path.string());
}

/// A YAML scalar in single quotes (a path with '\' or spaces as it is).
std::string yaml_quote(const std::string& text)
{
    std::string out = "'";
    for (const char c : text)
    {
        out += c == '\'' ? std::string("''") : std::string(1, c);
    }
    return out + "'";
}

/// `text` as a JSON string (with its quotes): '\\' and '"' escaped.
std::string json_string(const std::string& text)
{
    std::string out = "\"";
    for (const char ch : text)
    {
        out += ch == '\\' || ch == '"' ? std::string("\\") + ch : std::string(1, ch);
    }
    return out + "\"";
}

/// Runs vehicle_sil_launch with `args`.
Captured launch(const Options& o, const std::vector<std::string>& args)
{
    return vehicle_sil::process::run_captured(o.launch, args, vehicle_sil::process::CaptureOptions{120s});
}

void expect_code(const Captured& run, int code, const std::string& what)
{
    require(run.exit_code.has_value(), what + ": vehicle_sil_launch did not end");
    require(*run.exit_code == code, what + ": exit code " + std::to_string(*run.exit_code) + ", not " +
                                        std::to_string(code) + "\n--- stdout ---\n" + run.standard_output +
                                        "--- stderr ---\n" + run.standard_error);
}

void expect_text(const std::string& actual, const std::string& expected, const std::string& what)
{
    if (actual != expected)
    {
        throw std::runtime_error(what + " differs\n--- got ---\n" + actual + "--- expected ---\n" + expected + "---");
    }
    log("CHECK " + what + " -> ok");
}

/// The example's directory as a copy in the work directory: simulation.yaml and the ECUs of engine/
/// and dashboard/ (hard links to the built files, copies where the file system has no links; on
/// Windows the DLLs next to them too). The files that the docs show with mistakes in them are
/// written here, where their relative paths find the ECUs as in the example, so that nothing that
/// fails on purpose lies next to the example that works (build/<preset>/examples/ecu_main/).
fs::path example_copy(const Options& o)
{
    const fs::path dir = o.work_dir / "example";
    for (const std::string sub : {"engine", "dashboard"})
    {
        fs::create_directories(dir / sub);
        for (const fs::directory_entry& entry : fs::directory_iterator(o.example_dir / sub))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const fs::path to = dir / sub / entry.path().filename();
            fs::remove(to);
            std::error_code no_link;
            fs::create_hard_link(entry.path(), to, no_link);
            if (no_link)
            {
                fs::copy_file(entry.path(), to);
            }
        }
    }
    write_file(dir / "simulation.yaml", lf(read_file(o.example_dir / "simulation.yaml")));
    return dir;
}

/// What the program wrote, with `path` (as given, and as JSON writes it) shown as `shown`.
std::string shown_as(std::string text, const fs::path& path, const std::string& shown)
{
    const std::string given = shown_path(path);
    std::string escaped = given;
    replace_all(escaped, "\\", "\\\\");
    replace_all(text, escaped, shown);
    replace_all(text, given, shown);
    return text;
}

/// The copies in docs/howto/*.md: the block after each line "<!-- vehicle_sil_launch {command} ... -->"
/// there is the block after the same line in a document directly in `docs` (which this test compares
/// with the output or, for a check of a scenario, scenario.md, which launch_run.scenario compares).
void howto_copies_agree(const fs::path& docs, const std::string& command)
{
    const std::string head = "<!-- vehicle_sil_launch " + command + " ";
    const fs::path howto = docs / "howto";
    std::size_t copies = 0;
    for (const auto& entry : fs::directory_iterator(howto))
    {
        if (entry.path().extension() != ".md")
        {
            continue;
        }
        std::istringstream in(lf(read_file(entry.path())));
        for (std::string line; std::getline(in, line);)
        {
            if (line.rfind(head, 0) == 0 && line.size() > 4 && line.compare(line.size() - 4, 4, " -->") == 0)
            {
                const fs::path original = marked_file(docs, line);
                expect_text(block_after(entry.path(), line), block_after(original, line),
                            entry.path().generic_string() + ": the copy after " + line +
                                " (copy the block after the same line in " + original.filename().string() + ")");
                ++copies;
            }
        }
    }
    require(copies > 0, "no copy of a vehicle_sil_launch " + command + " example in " + howto.string());
}

void docs(const Options& o)
{
    // The example file is the one the docs show.
    expect_text(lf(read_file(o.example_config)), block_after(o.docs, "<!-- examples/ecu_main/simulation.yaml -->"),
                "examples/ecu_main/simulation.yaml in the docs");

    // The files with mistakes go to a copy of the example, not next to the example that works.
    const fs::path mistakes = example_copy(o);
    const fs::path example = mistakes / "simulation.yaml";
    const Captured passed = launch(o, {"check", shown_path(example)});
    expect_code(passed, 0, "check simulation.yaml");
    expect_text(shown_as(passed.standard_output, example, "simulation.yaml"),
                block_after(o.docs, "<!-- vehicle_sil_launch check simulation.yaml -->"), "check simulation.yaml");
    expect_text(passed.standard_error, "", "check simulation.yaml: stderr");

    const fs::path twoEngines = mistakes / "two-engines.yaml";
    write_file(twoEngines, block_after(o.docs, "<!-- two-engines.yaml -->"));
    const Captured failed = launch(o, {"check", shown_path(twoEngines)});
    expect_code(failed, 1, "check two-engines.yaml");
    expect_text(shown_as(failed.standard_output, twoEngines, "two-engines.yaml"),
                block_after(o.docs, "<!-- vehicle_sil_launch check two-engines.yaml -->"), "check two-engines.yaml");

    // Without Dashboard (--disable).
    const Captured without = launch(o, {"check", shown_path(example), "--disable", "Dashboard"});
    expect_code(without, 0, "check simulation.yaml --disable Dashboard");
    expect_text(shown_as(without.standard_output, example, "simulation.yaml"),
                block_after(o.docs, "<!-- vehicle_sil_launch check simulation.yaml --disable Dashboard -->"),
                "check simulation.yaml --disable Dashboard");

    const fs::path alone = mistakes / "engine-alone.yaml";
    write_file(alone, block_after(o.docs, "<!-- engine-alone.yaml -->"));
    const Captured json = launch(o, {"check", shown_path(alone), "--json"});
    expect_code(json, 1, "check engine-alone.yaml --json");
    // Byte for byte: "\n" also on Windows.
    expect_text(without_versions(shown_as(json.standard_output, alone, "engine-alone.yaml")),
                without_versions(block_after(o.docs, "<!-- vehicle_sil_launch check engine-alone.yaml --json -->")),
                "check engine-alone.yaml --json");

    // The version and what this launcher can do; it reads no file.
    const Captured version = launch(o, {"version", "--json"});
    expect_code(version, 0, "version --json");
    // A build with the control server lists three more features, last; the docs show a build without.
    std::string features = version.standard_output;
    replace_all(features, ",\n    \"control-server\",\n    \"scenario-control\",\n    \"scenario-time\"\n", "\n");
    expect_text(without_versions(features),
                without_versions(block_after(o.docs, "<!-- vehicle_sil_launch version --json -->")), "version --json");
    for (const std::vector<std::string>& args : {std::vector<std::string>{"version"}, {"--version"}, {"check", "--version"}})
    {
        const Captured text = launch(o, args);
        expect_code(text, 0, args.front() + " (text)");
        require(text.standard_output.rfind("vehicle_sil_launch ", 0) == 0 && text.standard_output.find('\n') ==
                                                                                   text.standard_output.size() - 1,
                "the version is one line:\n" + text.standard_output);
    }
    const Captured wrong = launch(o, {"version", "--strict"});
    expect_code(wrong, 4, "version --strict");
    howto_copies_agree(o.docs, "check");
}

/// What vehicle_sil_launch wrote, with the helper's path as "<helper>" and the work directory as
/// "<dir>". The test names every path with '/' (as a person may on Windows too), and
/// vehicle_sil_launch shows the paths it puts together with '/': the output has no '\\' but
/// what the command echoes (such as "\\n").
std::string normalized(const Options& o, std::string out)
{
    const std::string helperPath = shown_path(o.helper);
    const std::string dir = shown_path(o.work_dir);
    replace_all(out, "'" + helperPath + "'", "<helper>");
    replace_all(out, helperPath, "<helper>");
    replace_all(out, dir, "<dir>");
    return out;
}

void describe(const Options& o)
{
    const std::string helper = yaml_quote(shown_path(o.helper));
    // The default time limit for these: they end at once, also on a slow machine.
    const fs::path config = o.work_dir / "failing.yaml";
    write_file(config, "vehicle_sil_simulation: 2\n"
                       "networks: []\n"
                       "participants:\n"
                       "  - name: Usage\n"
                       "    command: [" + helper + ", --err, \"error: Usage: unknown argument --describe\\n\", --exit, \"4\"]\n"
                       "  - name: Chatty\n"
                       "    command: [" + helper + ", --out, \"hello\\n\"]\n"
                       "  - name: Future\n"
                       "    command: [" + helper + ", --out, '{\"vehicle_sil_ecu\": 2}']\n"
                       "  - name: Missing\n"
                       "    command: missing/ecu\n"
                       "  - name: Logger\n"
                       "    command: [" + helper + ", --uri, \"{registry_uri}\"]\n"
                       "    describe: false\n"
                       "  - name: Braces\n"
                       "    command: [" + helper + ", --out, '{\"vehicle_sil_ecu\": 2, \"x\": {\"y\": \"{{name}}\"}}']\n"
                       "  - name: Escaped\n"
                       "    command: [\"{{odd}}/missing\", \"{registry_uri}\"]\n"
                       "    describe: false\n"
                       // A value of a later vehicle-sil: only this participant's error.
                       "  - name: Newer\n"
                       "    command: [" + helper + ", --out, '{\"vehicle_sil_ecu\": 1, \"vehicle_sil\": \"9.0.0\", "
                       "\"name\": \"Newer\", \"step_ns\": null, \"networks\": [{\"name\": \"Ray\", \"kind\": \"flexray\", "
                       "\"use\": \"flexray\", \"nodes\": []}]}']\n"
                       // ... and the others are still read and compared.
                       "  - name: Good\n"
                       "    command: [" + helper + ", --out, '{\"vehicle_sil_ecu\": 1, \"name\": \"Good\", "
                       "\"step_ns\": 2000000, \"networks\": []}']\n"
                       "    step_ms: 2\n");
    const Captured run = launch(o, {"check", shown_path(config)});
    expect_code(run, 1, "check failing.yaml");
    // This launcher's version is in the messages about the description's version and values.
    std::string launcherVersion = launch(o, {"version"}).standard_output;
    launcherVersion = launcherVersion.substr(launcherVersion.find(' ') + 1);
    launcherVersion = launcherVersion.substr(0, launcherVersion.find_first_of("\r\n"));
    std::string out = normalized(o, run.standard_output);
    replace_all(out, "vehicle_sil_launch (vehicle-sil " + launcherVersion + ")", "vehicle_sil_launch (vehicle-sil <version>)");
    replace_all(out, "vehicle_sil_launch with vehicle-sil " + launcherVersion, "vehicle_sil_launch with vehicle-sil <version>");
    expect_text(out,
                "<dir>/failing.yaml: 9 participants, 0 networks\n"
                "\n"
                "participant  step  networks\n"
                "Usage        -     no description (see the errors)\n"
                "Chatty       -     no description (see the errors)\n"
                "Future       -     no description (see the errors)\n"
                "Missing      -     no description (see the errors)\n"
                "Logger       -     not checked (describe: false)\n"
                "Braces       -     no description (see the errors)\n"
                "Escaped      -     no description (see the errors)\n"
                "Newer        -     no description (see the errors)\n"
                "Good         2 ms  -\n"
                "\n"
                "error: <dir>/failing.yaml:5: participant 'Usage': <helper> --err 'error: Usage: unknown argument "
                "--describe\\n' --exit 4 --describe --name Usage ended with exit code 4; it did not take --describe or "
                "--name: if it is not made with ecu_main, set describe: false [describe-failed]\n"
                "    the end of its standard error:\n"
                "    error: Usage: unknown argument --describe\n"
                "error: <dir>/failing.yaml:7: participant 'Chatty': <helper> --out 'hello\\n' --describe --name Chatty "
                "ended with exit code 0, but its output is not a description: the output is not a JSON object. Is it made "
                "with ecu_main, and does nothing else write to its standard output? If it is not made with ecu_main, set "
                "describe: false [describe-output]\n"
                "    the start of its standard output:\n"
                "    hello\n"
                "error: <dir>/failing.yaml:9: participant 'Future': <helper> --out '{\"vehicle_sil_ecu\": 2}' --describe "
                "--name Future wrote a description in format 2, but this vehicle_sil_launch (vehicle-sil <version>) reads "
                "format 1: the executable is of a newer vehicle-sil; use a vehicle_sil_launch of its vehicle-sil or a "
                "newer one [describe-version]\n"
                "error: <dir>/failing.yaml:11: participant 'Missing': the executable missing/ecu does not exist (looked for "
                "<dir>/missing/ecu): build it, or correct the path (a relative path is from the directory of the "
                "configuration file) [executable]\n"
                // A JSON argument passes byte for byte, "{{name}}" as "{name}" (also for an ecu_main executable).
                "error: <dir>/failing.yaml:16: participant 'Braces': <helper> --out '{\"vehicle_sil_ecu\": 2, \"x\": "
                "{\"y\": \"{name}\"}}' --describe "
                "--name Braces wrote a description in format 2, but this vehicle_sil_launch (vehicle-sil <version>) reads "
                "format 1: the executable is of a newer vehicle-sil; use a vehicle_sil_launch of its vehicle-sil or a "
                "newer one [describe-version]\n"
                // "{{odd}}" in the path is the directory "{odd}".
                "error: <dir>/failing.yaml:18: participant 'Escaped': the executable {{odd}}/missing does not exist "
                "(looked for <dir>/{odd}/missing): build it, or correct the path (a relative path is from the directory "
                "of the configuration file) [executable]\n"
                "error: <dir>/failing.yaml:21: participant 'Newer': <helper> --out '{\"vehicle_sil_ecu\": 1, "
                "\"vehicle_sil\": \"9.0.0\", \"name\": \"Newer\", \"step_ns\": null, \"networks\": [{\"name\": \"Ray\", "
                "\"kind\": \"flexray\", \"use\": \"flexray\", \"nodes\": []}]}' --describe --name Newer wrote "
                "networks[0].use \"flexray\", which this vehicle_sil_launch does not know (it knows raw, j1939, "
                "lin_master, lin_slave and ethernet): the executable was built with vehicle-sil 9.0.0, this vehicle_sil_launch with "
                "vehicle-sil <version>. A newer vehicle-sil may add such values without a new format: use a "
                "vehicle_sil_launch of the executable's vehicle-sil or a newer one [describe-unknown-value]\n"
                "note: <dir>/failing.yaml:12: participant 'Logger': not checked (describe: false): vehicle_sil_launch "
                "cannot compare its name, step and networks with this file [not-checked]\n"
                "\n"
                "failed: 7 errors, 0 warnings, 1 note\n",
                "check failing.yaml");

    // A short time limit only for the one that does not end; also one that writes without end.
    const fs::path slow = o.work_dir / "slow.yaml";
    write_file(slow, "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                     "  - name: Slow\n"
                     "    command: [" + helper + ", --err, \"waiting for the bus\\n\", --sleep-ms, \"60000\"]\n"
                     "  - name: Flood\n"
                     "    command: [" + helper + ", --flood]\n");
    // Slow has to write its line before the limit stops it: scaled, as its start takes longer on a
    // slow machine.
    const std::string limit = std::to_string(vehicle_sil::test::scaled_timeout(std::chrono::seconds(1)).count());
    const Captured slowRun = launch(o, {"check", shown_path(slow), "--describe-timeout-ms", limit});
    expect_code(slowRun, 1, "check slow.yaml");
    const std::string slowOut = normalized(o, slowRun.standard_output);
    require(contains(slowOut, "error: <dir>/slow.yaml:5: participant 'Slow': <helper> --err 'waiting for the bus\\n' "
                              "--sleep-ms 60000 --describe --name Slow did not end within " + limit + " ms and was stopped. "
                              "--describe only builds the ECU, so something in its set-up waits (a connection, input): fix "
                              "that, or give vehicle_sil_launch check a longer --describe-timeout-ms [describe-timeout]\n"
                              "    the end of its standard error:\n"
                              "    waiting for the bus\n") &&
                contains(slowOut, "error: <dir>/slow.yaml:7: participant 'Flood': <helper> --flood --describe --name Flood "
                                  "did not end within " + limit + " ms and was stopped.") &&
                contains(slowOut, "\nfailed: 2 errors, 0 warnings, 0 notes\n"),
            "check slow.yaml wrote:\n" + slowOut);
    log("CHECK executables that do not end are stopped -> ok");
}

void strict(const Options& o)
{
    const fs::path config = o.work_dir / "alone.yaml";
    write_file(config, "vehicle_sil_simulation: 2\nnetworks:\n  - name: J1939_CAN\n    kind: can\nparticipants:\n"
                       "  - name: Engine\n    command: " + yaml_quote(shown_path(o.engine)) + "\n    step_ms: 10\n");
    const Captured passed = launch(o, {"check", shown_path(config)});
    expect_code(passed, 0, "check alone.yaml");
    require(contains(passed.standard_output, "\nwarning: ") && contains(passed.standard_output, "\npassed: 0 errors, 1 warning, 0 notes\n"),
            "check alone.yaml wrote:\n" + passed.standard_output);
    const Captured failed = launch(o, {"check", "--strict", shown_path(config)});
    expect_code(failed, 1, "check --strict alone.yaml");
    require(contains(failed.standard_output, "\nfailed: 0 errors, 1 warning, 0 notes (--strict)\n"),
            "check --strict alone.yaml wrote:\n" + failed.standard_output);
    const Captured json = launch(o, {"check", shown_path(config), "--json", "--strict"});
    expect_code(json, 1, "check alone.yaml --json --strict");
    require(contains(json.standard_output, "\n  \"result\": \"failed\",\n  \"strict\": true,\n  \"errors\": 0,\n  \"warnings\": 1,\n"),
            "check alone.yaml --json --strict wrote:\n" + json.standard_output);
    // With a participant that is not checked, the network with one participant is a note:
    // --strict passes.
    write_file(config, "vehicle_sil_simulation: 2\nnetworks:\n  - name: J1939_CAN\n    kind: can\nparticipants:\n"
                       "  - name: Engine\n    command: " + yaml_quote(shown_path(o.engine)) + "\n    step_ms: 10\n"
                       "  - name: Logger\n    command: [" + yaml_quote(shown_path(o.helper)) +
                       ", \"{registry_uri}\"]\n    describe: false\n");
    const Captured noted = launch(o, {"check", "--strict", shown_path(config)});
    expect_code(noted, 0, "check --strict alone.yaml with a participant not checked");
    require(contains(noted.standard_output, "\npassed: 0 errors, 0 warnings, 2 notes\n"),
            "check --strict alone.yaml with a participant not checked wrote:\n" + noted.standard_output);
    log("CHECK --strict fails on a warning, not on a note -> ok");
}

void unreadable(const Options& o)
{
    const fs::path missing = o.work_dir / "nope.yaml";
    const std::string m = shown_path(missing);
    const Captured none = launch(o, {"check", m});
    expect_code(none, 3, "check nope.yaml");
    expect_text(none.standard_output,
                "error: " + m + ": the file does not exist: check the path [file]\n\ncannot read " + m + ": 1 error, 0 warnings, 0 notes\n",
                "check nope.yaml");
    const Captured noneJson = launch(o, {"check", m, "--json"});
    expect_code(noneJson, 3, "check nope.yaml --json");
    require(contains(noneJson.standard_output, "\n  \"result\": \"unreadable\",\n") &&
                contains(noneJson.standard_output, "\n      \"code\": \"file\",\n"),
            "check nope.yaml --json wrote:\n" + noneJson.standard_output);

    const Captured directory = launch(o, {"check", shown_path(o.work_dir)});
    expect_code(directory, 3, "check <directory>");
    require(contains(directory.standard_output, ": this is a directory: name the configuration file in it [file]\n"),
            "check <directory> wrote:\n" + directory.standard_output);

#ifndef _WIN32
    const Captured device = launch(o, {"check", "/dev/null"});
    expect_code(device, 3, "check /dev/null");
    require(contains(device.standard_output, ": this is not a regular file (a device or a pipe?): name the configuration file [file]\n"),
            "check /dev/null wrote:\n" + device.standard_output);
#endif
    const fs::path big = o.work_dir / "big.yaml";
    std::ofstream(big).close();
    fs::resize_file(big, (std::uintmax_t{16} << 20) + 1);
    const Captured huge = launch(o, {"check", shown_path(big)});
    expect_code(huge, 3, "check big.yaml");
    require(contains(huge.standard_output, ": the file is larger than 16 MiB, too large for a configuration: name the "
                                           "configuration file [file]\n"),
            "check big.yaml wrote:\n" + huge.standard_output);

    const fs::path syntax = o.work_dir / "syntax.yaml";
    write_file(syntax, "vehicle_sil_simulation: 2\nnetworks: [a\nparticipants: []\n");
    const Captured bad = launch(o, {"check", shown_path(syntax)});
    expect_code(bad, 3, "check syntax.yaml");
    require(bad.standard_output.rfind("error: " + shown_path(syntax) + ":", 0) == 0 &&
                contains(bad.standard_output, ": syntax error: "),
            "check syntax.yaml wrote:\n" + bad.standard_output);

    const fs::path future = o.work_dir / "future.yaml";
    write_file(future, "vehicle_sil_simulation: 3\nsomething: new\n");
    const Captured later = launch(o, {"check", shown_path(future)});
    expect_code(later, 3, "check future.yaml");
    require(contains(later.standard_output, ":1: vehicle_sil_simulation is 3, but this vehicle_sil_launch reads format 2"),
            "check future.yaml wrote:\n" + later.standard_output);
    // A file that cannot be read comes before the names of --disable (exit 3); a name given to
    // both is a usage error all the same (exit 4).
    const Captured noneDisabled = launch(o, {"check", m, "--disable", "Engine"});
    expect_code(noneDisabled, 3, "check nope.yaml --disable Engine");
    const Captured noneBoth = launch(o, {"check", m, "--disable", "Engine", "--enable", "Engine"});
    expect_code(noneBoth, 4, "check nope.yaml --disable Engine --enable Engine");
    log("CHECK files that cannot be read: exit code 3 -> ok");
}

void usage(const Options& o)
{
    const auto expect_usage = [&](const std::vector<std::string>& args, const std::string& error) {
        const Captured run = launch(o, args);
        expect_code(run, 4, "vehicle_sil_launch " + error);
        const bool json = std::find(args.begin(), args.end(), "--json") != args.end();
        require(json ? contains(run.standard_output, "\"result\": \"usage\"") : run.standard_output.empty(),
                "usage error, stdout: " + run.standard_output);
        const std::string err = lf(run.standard_error);
        require(err.rfind("error: " + error + "\nusage: vehicle_sil_launch check [--json]", 0) == 0,
                "for the error '" + error + "' vehicle_sil_launch wrote:\n" + err);
    };
    expect_usage({}, "no command: give check <file>, run <file>, serve <file>, test <file>, connections <file> or version");
    expect_usage({"start", "x.yaml"}, "unknown command 'start': the commands are check, run, serve, test, connections and version");
    expect_usage({"check"}, "check needs the configuration file: vehicle_sil_launch check <file>");
    expect_usage({"check", "a.yaml", "b.yaml"}, "check takes one file, not 2");
    expect_usage({"check", "a.yaml", "--bogus"}, "unknown option --bogus");
    expect_usage({"check", "a.yaml", "--json", "--json"}, "--json is given twice");
    expect_usage({"check", "a.yaml", "--describe-timeout-ms"}, "missing value for --describe-timeout-ms");
    expect_usage({"check", "a.yaml", "--describe-timeout-ms", "0"}, "--describe-timeout-ms must be at least 1");
    expect_usage({"check", "a.yaml", "--describe-timeout-ms", "soon"}, "invalid value for --describe-timeout-ms: soon");
    expect_usage({"check", "a.yaml", "--time-policy"}, "missing value for --time-policy");
    for (const char* command : {"check", "run", "test"})
    {
        expect_usage({command, "a.yaml", "--time-policy", "warn"},
                     "--time-policy must be realtime (a run that is connected outside the simulation goes at speed 1; the "
                     "default) or free (its speed and time can be changed), not 'warn'");
    }
    // Two mistakes in one command line: the one that is read first is reported. The rule that
    // two fields do not go together is reported where the later field would be read, before the
    // mistakes of the fields that follow.
    const std::string paramNeedsScenario =
        "--param sets a parameter of a scenario: give --scenario <file> too, or leave --param out";
    expect_usage({"run", "a.yaml", "--param", "x=1", "--connect", "nowhere"}, paramNeedsScenario);
    expect_usage({"run", "a.yaml", "--param", "x=1", "--speed", "fast"}, paramNeedsScenario);
    expect_usage({"run", "a.yaml", "--param", "x=1", "--scenario", ""}, "--scenario needs a scenario file");
    expect_usage({"run", "a.yaml", "--timeline", "t.json", "--timeout-ms", "0"},
                 "--timeline writes what a scenario did over the time: give --scenario <file> too, or leave --timeline out");
    expect_usage({"run", "a.yaml", "--scenario", "s.yaml", "--start-paused", "--speed", "fast"},
                 "--speed must be a number of simulation seconds per wall-clock second from 0.000001 to 1000000 (1: real time, "
                 "2: twice as fast) or max (as fast as possible), not 'fast'");
    expect_usage({"run", "a.yaml", "--scenario", "s.yaml", "--start-paused"},
                 "--scenario cannot be used with --start-paused: the scenario writes its values for 0 before the first step and "
                 "runs by itself; pause and step it with --stdin-commands or from a Control Server client (--control) if needed");
    expect_usage({"run", "a.yaml", "--start-paused", "--control-audit-log", "a.log"},
                 "--control-audit-log is for --control: without it there is no control server");
    expect_usage({"check", "a.yaml", "--events"}, "--events is not for check: check starts no participant");
    expect_usage({"check", "a.yaml", "--events-stall-ms", "1000"}, "--events-stall-ms is for serve, not for check");
    expect_usage({"check", "a.yaml", "--stdin-commands"}, "--stdin-commands is for run: check starts no participant");
    for (const std::vector<std::string>& args : {std::vector<std::string>{"--help"}, {"check", "x.yaml", "-h"}})
    {
        const Captured help = launch(o, args);
        expect_code(help, 0, "vehicle_sil_launch --help");
        require(lf(help.standard_output).rfind("usage: vehicle_sil_launch check [--json]", 0) == 0 && help.standard_error.empty() &&
                    contains(help.standard_output, "[--time-policy realtime|free] [--request <file>]\n") &&
                    contains(lf(help.standard_output),
                             "[--stop-at-ms <ms>] [--late-tolerance-ms <ms>]\n"
                             "                                [--participant-start-timeout-ms <ms>] [--participant-stop-timeout-ms <ms>]\n") &&
                    contains(help.standard_output, "\n  --time-policy realtime|free the time of a run that is connected outside"),
                "--help wrote:\n" + help.standard_output + help.standard_error);
    }
    // With --json, a wrong command line is JSON on stdout too.
    const Captured json = launch(o, {"check", "--json", "a.yaml", "--bogus"});
    expect_code(json, 4, "vehicle_sil_launch check --json --bogus");
    // The file is known from the arguments: the JSON names it.
    require(contains(json.standard_output, "\n  \"file\": \"a.yaml\",\n  \"result\": \"usage\",\n") &&
                contains(json.standard_output, "\n      \"code\": \"usage\",\n") &&
                contains(json.standard_output, "\"message\": \"unknown option --bogus\""),
            "check --json --bogus wrote:\n" + json.standard_output);
    // Two files: which one is meant is not known, so no file.
    const Captured twoFiles = launch(o, {"check", "--json", "a.yaml", "b.yaml"});
    expect_code(twoFiles, 4, "vehicle_sil_launch check --json a.yaml b.yaml");
    require(contains(twoFiles.standard_output, "\n  \"file\": null,\n  \"result\": \"usage\",\n"),
            "check --json a.yaml b.yaml wrote:\n" + twoFiles.standard_output);
    // After "--", a name that starts with '-' is the file.
    const Captured dashed = launch(o, {"check", "--", "-h"});
    expect_code(dashed, 3, "vehicle_sil_launch check -- -h");
    require(dashed.standard_output.rfind("error: -h: the file does not exist", 0) == 0,
            "check -- -h wrote:\n" + dashed.standard_output);
    log("CHECK usage errors: exit code 4 and the message; --help: exit code 0 -> ok");
}

void set_environment(const std::string& name, const std::string& value);

/// A file that is removed again, whatever the test does meanwhile (it lies where other tests look).
class FileForAWhile
{
public:
    FileForAWhile(fs::path path, const std::string& text) : path_(std::move(path)) { write_file(path_, text); }
    ~FileForAWhile()
    {
        std::error_code ignored; // a destructor must not throw
        fs::remove(path_, ignored);
    }
    FileForAWhile(const FileForAWhile&) = delete;
    FileForAWhile& operator=(const FileForAWhile&) = delete;

private:
    fs::path path_;
};

/// --request <file>: the request file gives what the arguments give, an argument wins, the file's
/// scenario is from the directory of the configuration file, a file that cannot be read is a usage
/// error, and no file is read that is not named.
void request(const Options& o)
{
    const fs::path dir = example_copy(o);
    const std::string config = shown_path(dir / "simulation.yaml");
    fs::create_directories(dir / "scenarios");
    write_file(dir / "scenarios" / "level.yaml", "vehicle_sil_scenario: 1\n"
                                                 "name: level\n"
                                                 "params: { level: 10 }\n"
                                                 "steps:\n"
                                                 "  - at_ms: 0\n"
                                                 "    set: { Engine.engine.throttle: \"${level}\" }\n"
                                                 "  - expect: { Engine.engine.speed: { ge: 0 } }\n"
                                                 "    within_ms: 100\n");
    // Somewhere else than the configuration, and not the working directory either.
    fs::create_directories(o.work_dir / "bench");
    const std::string bench = shown_path(o.work_dir / "bench" / "bench.json");
    write_file(o.work_dir / "bench" / "bench.json", "{\n"
                                                    "  \"vehicle_sil_run_request\": 1,\n"
                                                    "  \"scenario\": \"scenarios/level.yaml\",\n"
                                                    "  \"params\": { \"level\": 30 },\n"
                                                    "  \"strict\": true\n"
                                                    "}\n");
    const std::string scenario = shown_path(dir / "scenarios" / "level.yaml");
    // check: the same report as with the arguments, but for where the parameter is from.
    const Captured byArguments = launch(o, {"check", "--json", "--scenario", scenario, "--param", "level=30", "--strict", config});
    expect_code(byArguments, 0, "check with the arguments");
    const Captured byFile = launch(o, {"check", "--json", "--request", bench, config});
    expect_code(byFile, 0, "check --request");
    require(contains(byArguments.standard_output, "\"from\": \"command_line\"") &&
                contains(byFile.standard_output, "\"from\": \"request_file\""),
            "check --request: where the parameter is from:\n" + byFile.standard_output);
    std::string same = byFile.standard_output;
    replace_all(same, "\"from\": \"request_file\"", "\"from\": \"command_line\"");
    expect_text(same, byArguments.standard_output, "check --request and check with the same arguments");
    // For people: the parameter's origin in words.
    const Captured text = launch(o, {"check", "--request", bench, config});
    expect_code(text, 0, "check --request (text)");
    require(contains(text.standard_output, "level = 30 (the request file's params)"), "check --request wrote:\n" + text.standard_output);
    // An argument wins, by name.
    const Captured over = launch(o, {"check", "--json", "--request", bench, "--param", "level=40", config});
    expect_code(over, 0, "check --request --param");
    require(contains(over.standard_output, "\"from\": \"command_line\"") && !contains(over.standard_output, "\"request_file\""),
            "check --request --param wrote:\n" + over.standard_output);
    // The same name as an argument is from the working directory, where it is not.
    const Captured typed = launch(o, {"check", "--scenario", "scenarios/level.yaml", config});
    expect_code(typed, 1, "check --scenario with the path of the request file");
    require(contains(typed.standard_output, "the scenario file does not exist"), "check --scenario wrote:\n" + typed.standard_output);
    // Only the file that is named is read: files with likely names next to the configuration and
    // in the working directory, and an environment variable, change nothing.
    const Captured before = launch(o, {"check", "--json", config});
    expect_code(before, 0, "check");
    const std::string lie = "{ \"vehicle_sil_run_request\": 1, \"disable\": [\"Dashboard\"], \"bogus\": 1 }\n";
    for (const fs::path& lying : {dir / "run-request.json", dir / "simulation.request.json", dir / "request.json"})
    {
        write_file(lying, lie);
    }
    set_environment("VEHICLE_SIL_RUN_REQUEST", shown_path(dir / "run-request.json"));
    set_environment("VEHICLE_SIL_REQUEST", shown_path(dir / "run-request.json"));
    Captured after;
    {
        const FileForAWhile inWorkingDirectory(fs::current_path() / "vehicle_sil_run_request.json", lie);
        after = launch(o, {"check", "--json", config});
    }
    expect_text(after.standard_output, before.standard_output, "check without --request, with request files lying around");
    // run: the scenario of the file runs, as with the arguments.
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"run", "--json", "--scenario", scenario, "--param", "level=30", "--speed", "max", "--strict", config},
          std::vector<std::string>{"run", "--json", "--request", bench, "--speed", "max", config}})
    {
        const Captured run = launch(o, args);
        expect_code(run, 0, "vehicle_sil_launch " + args[2] + " (run)");
        require(contains(run.standard_output, "\"result\": \"passed\"") && contains(run.standard_output, "\"name\": \"level\"") &&
                    contains(run.standard_output, "\"strict\": true") && contains(run.standard_output, "\"speed\": \"max\""),
                "run " + args[2] + " wrote:\n" + run.standard_output);
    }
    // A request file that cannot be read, or is no request file: a usage error, also as JSON.
    const auto expect_usage = [&](const std::vector<std::string>& args, const std::string& error) {
        const Captured run = launch(o, args);
        expect_code(run, 4, "vehicle_sil_launch " + error);
        const bool json = std::find(args.begin(), args.end(), "--json") != args.end();
        require(json ? contains(run.standard_output, "\"result\": \"usage\"") && contains(run.standard_output, json_string(error))
                     : run.standard_output.empty(),
                "usage error, stdout: " + run.standard_output);
        require(lf(run.standard_error).rfind("error: " + error + "\nusage: vehicle_sil_launch check [--json]", 0) == 0,
                "for the error '" + error + "' vehicle_sil_launch wrote:\n" + run.standard_error);
    };
    const std::string absent = shown_path(o.work_dir / "bench" / "absent.json");
    for (const char* command : {"check", "run"})
    {
        expect_usage({command, "--request", absent, config}, "--request " + absent + ": the file does not exist: check the path");
        expect_usage({command, "--json", "--request", absent, config},
                     "--request " + absent + ": the file does not exist: check the path");
        expect_usage({command, "--request", shown_path(o.work_dir / "bench"), config},
                     "--request " + shown_path(o.work_dir / "bench") + ": this is not a regular file: name the request file");
    }
    const std::string wrong = shown_path(o.work_dir / "bench" / "wrong.json");
    write_file(o.work_dir / "bench" / "wrong.json", "{\n  \"vehicle_sil_run_request\": 1,\n  \"conect\": {}\n}\n");
    const std::string unknown = wrong + ", line 3: unknown key 'conect': did you mean 'connect'? (the keys here: "
                                        "vehicle_sil_run_request, variant, params, scenario, disable, enable, manual, connect, "
                                        "time_policy, speed, start_paused, strict, stop_at_ms, late_tolerance_ms)";
    expect_usage({"check", "--request", wrong, config}, unknown);
    expect_usage({"run", "--json", "--request", wrong, config}, unknown);
    {
        const Captured json = launch(o, {"check", "--json", "--request", wrong, config});
        require(contains(json.standard_output, "\n  \"file\": " + json_string(config) + ",\n  \"result\": \"usage\",\n") &&
                    contains(json.standard_output, "\n      \"code\": \"usage\",\n"),
                "check --json --request with an unknown key wrote:\n" + json.standard_output);
    }
    expect_usage({"check", "--request", "", config}, "--request needs a request file: --request <file>.json");
    expect_usage({"check", "--request"}, "missing value for --request");
    // A rule about the file's values names the key and the file.
    const std::string held = shown_path(o.work_dir / "bench" / "held.json");
    write_file(o.work_dir / "bench" / "held.json", "{ \"vehicle_sil_run_request\": 1, \"start_paused\": true }\n");
    expect_usage({"run", "--request", held, config},
                 "start_paused in " + held + " needs --stdin-commands or --control: without commands nothing could make the "
                                             "simulation go on (resume, step)");
    {
        const Captured check = launch(o, {"check", "--request", held, config});
        expect_code(check, 0, "check --request with start_paused");
    }
    // With the commands that the rule asks for, the run is taken: it stands still at 0, and the
    // end of its standard input (there is none here) or its time limit ends it. Not a usage error.
    {
        const auto taken = [&](const std::vector<std::string>& args, const std::string& what) {
            const Captured run = launch(o, args);
            require(run.exit_code.has_value() && *run.exit_code != 4 &&
                        !contains(run.standard_error, "needs --stdin-commands or --control") &&
                        !contains(run.standard_error, "usage: vehicle_sil_launch"),
                    what + " is taken; it wrote (exit code " + (run.exit_code ? std::to_string(*run.exit_code) : "none") +
                        "):\n" + run.standard_output + run.standard_error);
        };
        taken({"run", "--request", held, "--stdin-commands", "--timeout-ms", "2000", config}, "run --request with start_paused, --stdin-commands");
        const Captured version = launch(o, {"version", "--json"});
        if (contains(version.standard_output, "\"control-server\""))
        {
            taken({"run", "--request", held, "--control", "0", "--timeout-ms", "1000", config},
                  "run --request with start_paused, --control");
        }
        else
        {
            log("this build has no control server: run --request with start_paused and --control is not run");
        }
    }
    // The rules wait for every argument: a file's scenario is the scenario that --timeline and --param need.
    {
        const std::string timeline = shown_path(o.work_dir / "bench" / "timeline.json");
        const Captured run = launch(o, {"run", "--json", "--request", bench, "--speed", "max", "--timeline", timeline, config});
        expect_code(run, 0, "run --request (with a scenario) --timeline");
        require(fs::exists(o.work_dir / "bench" / "timeline.json"), "run --request --timeline wrote the timeline");
    }
    // The file's params without a scenario are left for a run that has one; --param is not.
    const std::string onlyParams = shown_path(o.work_dir / "bench" / "params.json");
    write_file(o.work_dir / "bench" / "params.json", "{ \"vehicle_sil_run_request\": 1, \"params\": { \"level\": 30 } }\n");
    {
        const Captured check = launch(o, {"check", "--request", onlyParams, config});
        expect_code(check, 0, "check --request with params and no scenario");
    }
    expect_usage({"check", "--request", onlyParams, "--param", "level=40", config},
                 "--param sets a parameter of a scenario: give --scenario <file> too, or leave --param out");
    // null is no value.
    const std::string nothing = shown_path(o.work_dir / "bench" / "null.json");
    write_file(o.work_dir / "bench" / "null.json", "{ \"vehicle_sil_run_request\": 1, \"variant\": null }\n");
    expect_usage({"check", "--request", nothing, config},
                 nothing + ", line 1: variant must be the name of a variant of the configuration as a text, not empty");
    // A participant of the file that the configuration lacks: the key and the file, not an option.
    const std::string names = shown_path(o.work_dir / "bench" / "names.json");
    write_file(o.work_dir / "bench" / "names.json", "{ \"vehicle_sil_run_request\": 1, \"manual\": [\"Nobody\"] }\n");
    {
        const Captured check = launch(o, {"check", "--request", names, config});
        expect_code(check, 4, "check --request with a participant the configuration lacks");
        require(contains(lf(check.standard_error),
                         "error: manual Nobody in " + names + ": the configuration file has no participant with this name; its participants are "),
                "check --request with manual: [Nobody] wrote:\n" + check.standard_error);
        const Captured typed2 = launch(o, {"check", "--manual", "Nobody", config});
        require(contains(lf(typed2.standard_error), "error: --manual Nobody: the file has no participant with this name; its participants are "),
                "check --manual Nobody wrote:\n" + typed2.standard_error);
    }
    // test accepts only the shared timing fields of a request file.
    expect_usage({"test", config, "--request", bench, "--scenarios", shown_path(dir / "scenarios")},
                 "test --request takes only stop_at_ms and late_tolerance_ms; it selects its own runs");
    // --help has the option.
    const Captured help = launch(o, {"--help"});
    require(contains(help.standard_output, "\n  --request <file>            check, run, connections: what to run, from the request file <file>"),
            "--help wrote:\n" + help.standard_output);
    log("CHECK --request: as the arguments, an argument wins, the scenario beside the configuration, usage errors -> ok");
}

void enabled(const Options& o)
{
    // Ghost's executable does not exist: disabled, it is not looked for (and not run).
    const fs::path config = o.work_dir / "ghost.yaml";
    write_file(config, "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                       "  - name: Logger\n    command: [" + yaml_quote(shown_path(o.helper)) +
                       ", \"{registry_uri}\"]\n    describe: false\n"
                       "  - name: Ghost\n    command: [missing/ghost]\n    enabled: false\n");
    const std::string c = shown_path(config);
    // Only notes: --strict passes.
    const Captured passed = launch(o, {"check", "--strict", c});
    expect_code(passed, 0, "check --strict ghost.yaml");
    require(contains(passed.standard_output, ": 2 participants (1 disabled), 0 networks\n") &&
                contains(passed.standard_output, "\nGhost        -     disabled\n") &&
                contains(passed.standard_output, ":7: participant 'Ghost': disabled (enabled: false, or --disable): it is "
                                                 "neither checked nor started") &&
                contains(passed.standard_output, "\npassed: 0 errors, 0 warnings, 2 notes\n"),
            "check --strict ghost.yaml wrote:\n" + passed.standard_output);
    const Captured json = launch(o, {"check", "--json", c});
    expect_code(json, 0, "check --json ghost.yaml");
    require(contains(json.standard_output, "      \"required\": true,\n      \"enabled\": false,\n      \"manual\": false,\n      \"role\": \"ecu\",\n      \"step_ms\": null,\n"
                                           "      \"status\": \"disabled\",\n      \"reported\": null,\n      \"variables\": null,\n      \"network_map\": null,\n      \"j1939_nodes\": null,\n"
                                           "      \"control\": null\n") &&
                contains(json.standard_output, "      \"code\": \"participant-disabled\",\n"),
            "check --json ghost.yaml wrote:\n" + json.standard_output);
    // Taken in again (--enable), its missing executable is an error; Logger left out instead,
    // Ghost is the only one.
    const Captured enabledGhost = launch(o, {"check", c, "--enable", "Ghost", "--disable", "Logger"});
    expect_code(enabledGhost, 1, "check ghost.yaml --enable Ghost --disable Logger");
    require(contains(enabledGhost.standard_output, ": participant 'Ghost': the executable missing/ghost does not exist") &&
                contains(enabledGhost.standard_output, ": participant 'Logger': disabled") &&
                !contains(enabledGhost.standard_output, "participant 'Ghost': disabled"),
            "check ghost.yaml --enable Ghost --disable Logger wrote:\n" + enabledGhost.standard_output);
    // Every participant left out: nothing to wait for.
    const Captured none = launch(o, {"check", c, "--disable", "Logger"});
    expect_code(none, 1, "check ghost.yaml --disable Logger");
    require(contains(none.standard_output, ":3: every participant is disabled (enabled: false, or --disable), so the "
                                           "simulation has no participant to wait for"),
            "check ghost.yaml --disable Logger wrote:\n" + none.standard_output);
    // Names the file does not have, and one name twice: usage errors after the file is read.
    const auto expect_usage = [&](const std::vector<std::string>& args, const std::string& error) {
        const Captured run = launch(o, args);
        expect_code(run, 4, "vehicle_sil_launch " + error);
        const bool isJson = std::find(args.begin(), args.end(), "--json") != args.end();
        require(isJson ? contains(run.standard_output, "\n  \"file\": " + json_string(c) + ",\n  \"result\": \"usage\",\n") &&
                             contains(run.standard_output, "\"message\": \"" + error + "\"")
                       : run.standard_output.empty(),
                "usage error, stdout: " + run.standard_output);
        require(lf(run.standard_error).rfind("error: " + error + "\nusage: vehicle_sil_launch check [--json]", 0) == 0,
                "for the error '" + error + "' vehicle_sil_launch wrote:\n" + run.standard_error);
    };
    expect_usage({"check", c, "--disable", "logger"},
                 "--disable logger: the file has no participant with this name (did you mean 'Logger'?); its participants "
                 "are Logger, Ghost");
    // Every name that does not fit is said, each on a line of its own.
    const Captured two = launch(o, {"check", c, "--disable", "logger", "--enable", "Host"});
    expect_code(two, 4, "check --disable logger --enable Host");
    require(lf(two.standard_error).rfind("error: --disable logger: the file has no participant with this name (did you mean "
                                         "'Logger'?); its participants are Logger, Ghost\nerror: --enable Host: the file "
                                         "has no participant with this name (did you mean 'Ghost'?); its participants are "
                                         "Logger, Ghost\nusage: ",
                                         0) == 0,
            "check --disable logger --enable Host wrote:\n" + two.standard_error);
    expect_usage({"check", c, "--json", "--enable", "Ghost", "--disable", "Ghost"},
                 "--disable and --enable both name Ghost: give one of them");
    expect_usage({"check", c, "--disable"}, "missing value for --disable");

    // A file with an error: its findings first (exit 1); a name it does not have is not said yet.
    const fs::path broken = o.work_dir / "broken.yaml";
    write_file(broken, "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                       "  - name: [Logger]\n    command: [" + yaml_quote(shown_path(o.helper)) +
                       ", \"{registry_uri}\"]\n    describe: false\n"
                       "  - name: Ghost\n    command: [missing/ghost]\n    enabled: false\n");
    const Captured fileFirst = launch(o, {"check", shown_path(broken), "--disable", "Logger"});
    expect_code(fileFirst, 1, "check broken.yaml --disable Logger");
    require(contains(fileFirst.standard_output, "\nerror: ") && !contains(fileFirst.standard_output, "--disable Logger") &&
                !contains(fileFirst.standard_error, "--disable Logger"),
            "check broken.yaml --disable Logger wrote:\n" + fileFirst.standard_output + fileFirst.standard_error);

    // A disabled entry with an error: the error fails the check, the status is disabled, and the
    // network rules still hold (Engine alone on J1939_CAN).
    const fs::path partly = o.work_dir / "partly.yaml";
    write_file(partly, "vehicle_sil_simulation: 2\nnetworks:\n  - name: J1939_CAN\n    kind: can\nparticipants:\n"
                       "  - name: Engine\n    command: " + yaml_quote(shown_path(o.engine)) + "\n    step_ms: 10\n"
                       "  - name: Ghost\n    enabled: false\n");
    const Captured partial = launch(o, {"check", "--json", shown_path(partly)});
    expect_code(partial, 1, "check partly.yaml --json");
    require(contains(partial.standard_output, "      \"status\": \"disabled\",\n") &&
                contains(partial.standard_output, "      \"code\": \"missing-key\",\n") &&
                contains(partial.standard_output, "      \"code\": \"single-participant\",\n"),
            "check partly.yaml --json wrote:\n" + partial.standard_output);

    // --strict: the warning that leaving Dashboard out brings fails it (the note does not).
    const fs::path example = o.example_dir / "simulation.yaml";
    const Captured strictWithout = launch(o, {"check", "--strict", shown_path(example), "--disable", "Dashboard"});
    expect_code(strictWithout, 1, "check --strict simulation.yaml --disable Dashboard");
    require(contains(strictWithout.standard_output, "\nfailed: 0 errors, 1 warning, 1 note (--strict)\n"),
            "check --strict simulation.yaml --disable Dashboard wrote:\n" + strictWithout.standard_output);
    log("CHECK enabled: false, --disable and --enable -> ok");
}

/// PATH of this process (and so of the processes it starts) set to `value` until it ends.
class PathSetting
{
public:
    explicit PathSetting(const std::string& value)
    {
#ifdef _WIN32
        const wchar_t* old = _wgetenv(L"PATH");
        old_ = old != nullptr ? old : L"";
        _wputenv_s(L"PATH", vehicle_sil::process::path_from_utf8(value).wstring().c_str());
#else
        const char* old = std::getenv("PATH");
        old_ = old != nullptr ? old : "";
        setenv("PATH", value.c_str(), 1);
#endif
    }
    ~PathSetting()
    {
#ifdef _WIN32
        _wputenv_s(L"PATH", old_.c_str());
#else
        setenv("PATH", old_.c_str(), 1);
#endif
    }
    PathSetting(const PathSetting&) = delete;
    PathSetting& operator=(const PathSetting&) = delete;

private:
#ifdef _WIN32
    std::wstring old_;
#else
    std::string old_;
#endif
};

#ifdef _WIN32
constexpr const char* kExe = ".exe";
/// The backend that the launcher gives the Ethernet bridge on this OS.
constexpr const char* kOsBackend = "npcap";
#else
constexpr const char* kExe = "";
/// The backend that the launcher gives the Ethernet bridge on this OS.
constexpr const char* kOsBackend = "packet";
#endif

/// PATH without the bridges: empty, but on Windows the directories of PATH with the MinGW
/// runtime DLLs, which the executables of a build that links them dynamically need to start.
std::string runtime_path()
{
#ifdef _WIN32
    const wchar_t* variable = _wgetenv(L"PATH");
    const std::wstring list = variable != nullptr ? variable : L"";
    std::string out;
    std::size_t begin = 0;
    while (begin <= list.size())
    {
        const std::size_t end = std::min(list.find(L';', begin), list.size());
        const fs::path directory(list.substr(begin, end - begin));
        begin = end + 1;
        std::error_code ec;
        if (!directory.empty() && (fs::exists(directory / "libstdc++-6.dll", ec) || fs::exists(directory / "libwinpthread-1.dll", ec)))
        {
            out += (out.empty() ? "" : ";") + vehicle_sil::process::utf8_from_path(directory);
        }
    }
    return out;
#else
    return "";
#endif
}

/// check --time-policy with `text` (connect()'s file, at speed 1) at another speed, and with a
/// role: bridge participant in it.
void time_policy(const Options& o, const std::string& text)
{
    std::string fast = text;
    const fs::path config = o.work_dir / "connect-speed-2.yaml";
    write_file(config, fast);
    const std::string file = shown_path(config);
    // realtime (the default): the run will go at speed 1, which is a note.
    for (const std::vector<std::string>& policy : {std::vector<std::string>{}, std::vector<std::string>{"--time-policy", "realtime"}})
    {
        std::vector<std::string> args{"check", "--json", "--strict", "--connect", "ETH_EXT=eth0", "--connect", "J1939_CAN1=virtual:1"};
        args.insert(args.end(), policy.begin(), policy.end());
        args.push_back(file);
        const Captured locked = launch(o, args);
        expect_code(locked, 0, "check --strict --connect with the default speed");
        require(contains(locked.standard_output,
                         "\"locked\": true,\n    \"because\": [\n      \"ETH_EXT\",\n      \"J1939_CAN1\"\n    ]") &&
                    contains(locked.standard_output, "\"severity\": \"note\",\n      \"code\": \"outside-speed-set\"") &&
                    contains(locked.standard_output, "not as fast as possible") &&
                    !contains(locked.standard_output, "outside-not-real-time"),
                "check --connect with the default speed wrote:\n" + locked.standard_output);
    }
    // free: not locked, but what would lock is named; a note of its own.
    const Captured free = launch(o, {"check", "--json", "--strict", "--time-policy", "free", "--connect", "ETH_EXT=eth0",
                                      "--connect", "J1939_CAN1=virtual:1", file});
    expect_code(free, 0, "check --strict --time-policy free --connect with the default speed");
    require(contains(free.standard_output,
                     "\"locked\": false,\n    \"because\": [\n      \"ETH_EXT\",\n      \"J1939_CAN1\"\n    ]") &&
                contains(free.standard_output, "\"severity\": \"note\",\n      \"code\": \"outside-not-real-time\"") &&
                !contains(free.standard_output, "outside-speed-set"),
            "check --time-policy free wrote:\n" + free.standard_output);
    const Captured table = launch(o, {"check", "--connect", "ETH_EXT=eth0", file});
    expect_code(table, 0, "check --connect with the default speed (text)");
    require(contains(table.standard_output, "the run goes at speed 1 (real time), not as fast as possible") &&
                contains(table.standard_output, "[outside-speed-set]"),
            "check --connect with the default speed wrote:\n" + table.standard_output);
    // A role: bridge participant locks the time without --connect, also with describe: false;
    // --disable takes it out.
    const fs::path bridged = o.work_dir / "connect-own-bridge.yaml";
    write_file(bridged, fast + "  - name: Gateway\n"
                               "    role: bridge\n"
                               "    describe: false\n"
                               "    required: false\n"
                               "    command: [" + yaml_quote(shown_path(o.helper)) + "]\n");
    const Captured own = launch(o, {"check", "--json", shown_path(bridged)});
    expect_code(own, 0, "check of a file with a role: bridge participant");
    require(contains(own.standard_output, "\"locked\": true,\n    \"because\": [\n      \"Gateway\"\n    ]") &&
                contains(own.standard_output, "\"code\": \"outside-speed-set\"") &&
                contains(own.standard_output, "participant 'Gateway' is a bridge (role: bridge)") &&
                contains(own.standard_output, "run with --disable Gateway"),
            "check of a file with a role: bridge participant wrote:\n" + own.standard_output);
    const Captured disabled = launch(o, {"check", "--json", "--disable", "Gateway", shown_path(bridged)});
    expect_code(disabled, 0, "check --disable of the role: bridge participant");
    require(contains(disabled.standard_output, "\"locked\": false,\n    \"because\": []") &&
                !contains(disabled.standard_output, "\"outside-"),
            "check --disable Gateway wrote:\n" + disabled.standard_output);
}

void connect(const Options& o)
{
    const std::string helper = yaml_quote(shown_path(o.helper));
    const auto described = [&](const std::string& name, const std::string& network) {
        return "[" + helper + ", --out, '{\"vehicle_sil_ecu\": 1, \"name\": \"" + name +
               "\", \"step_ns\": 1000000, \"networks\": [" + network + "]}']";
    };
    const std::string text =
        "vehicle_sil_simulation: 2\n"
        "networks:\n"
        "  - name: ETH_EXT\n"
        "    kind: ethernet\n"
        "  - name: J1939_CAN1\n"
        "    kind: can\n"
        "    external:\n"
        "      bitrate: 250000\n"
        "participants:\n"
        "  - name: Camera\n"
        "    command: " + described("Camera", "{\"name\": \"ETH_EXT\", \"kind\": \"ethernet\", \"use\": \"ethernet\", "
                                              "\"nodes\": [], \"mac\": \"02:00:00:00:00:01\"}") + "\n"
        "  - name: Engine\n"
        "    command: " + described("Engine", "{\"name\": \"J1939_CAN1\", \"kind\": \"can\", \"use\": \"raw\", \"nodes\": []}") +
        "\n";
    const fs::path config = o.work_dir / "connect.yaml";
    write_file(config, text);
    const std::vector<std::string> both{"check", "--json", "--connect", "ETH_EXT=eth0", "--connect", "J1939_CAN1=virtual:1",
                                        shown_path(config)};
    const std::string tools = shown_path(o.connect_tools);
    // `found`: the directory the bridges were found in as the JSON shows it (where the path of
    // the launcher itself is read from the system, it may be another spelling of the directory).
    const auto expect_outside = [&](const Captured& run, const std::string& what, const std::string& found) {
        expect_code(run, 0, what);
        for (const std::string& part : std::vector<std::string>{
                 "        \"tool_path\": \"" + found + "/vehicle_sil_ethernet_bridge" + kExe + "\",\n"
              "        \"target\": \"eth0\",\n",
              "        \"tool_path\": \"" + found + "/vehicle_sil_vector_xl_bridge" + kExe + "\",\n"
              "        \"target\": \"virtual:1\",\n",
              "      \"outside\": {\n"
              "        \"tool\": \"vehicle_sil_ethernet_bridge\",\n",
              "        \"participant\": \"bridge.ETH_EXT\",\n"
              "        \"command\": [\n"
              "          \"vehicle_sil_ethernet_bridge\",\n"
              "          \"--network\",\n"
              "          \"ETH_EXT\",\n"
              "          \"--backend\",\n"
              "          \"" + std::string(kOsBackend) + "\",\n"
              "          \"--interface\",\n"
              "          \"eth0\",\n"
              "          \"--src-mac\",\n"
              "          \"02:00:00:00:00:01\"\n"
              "        ]\n",
              "        \"participant\": \"bridge.J1939_CAN1\",\n"
              "        \"command\": [\n"
              "          \"vehicle_sil_vector_xl_bridge\",\n"
              "          \"--network\",\n"
              "          \"J1939_CAN1\",\n"
              "          \"--xl-target\",\n"
              "          \"virtual\",\n"
              "          \"--xl-channel\",\n"
              "          \"1\",\n"
              "          \"--bitrate\",\n"
              "          \"250000\"\n"
              "        ]\n"})
        {
            require(contains(run.standard_output, part), what + ": the JSON has not\n" + part + "--- it is ---\n" +
                                                              run.standard_output);
        }
        // The bridge is on each network: one ECU there is no single participant.
        require(!contains(run.standard_output, "single-participant"), what + ": " + run.standard_output);
        // Connected outside: the run keeps real time (the file has speed: 1, so there is nothing to say).
        require(contains(run.standard_output, "  \"time_lock\": {\n"
                                              "    \"locked\": true,\n"
                                              "    \"because\": [\n"
                                              "      \"ETH_EXT\",\n"
                                              "      \"J1939_CAN1\"\n"
                                              "    ]\n"
                                              "  },\n") &&
                    contains(run.standard_output, "\"outside-speed-set\""),
                what + ": the time lock:\n" + run.standard_output);
    };
    {
        // On PATH (the launcher's own directory has no bridge).
#ifdef _WIN32
        const std::string runtime = runtime_path();
        const std::string native = vehicle_sil::process::utf8_from_path(o.connect_tools);
        const PathSetting path(runtime.empty() ? native : native + ";" + runtime);
#else
        const PathSetting path(tools);
#endif
        expect_outside(launch(o, both), "check --connect with the bridges on PATH", tools);
        // The table for people has the column "outside".
        const Captured table = launch(o, {"check", "--connect", "ETH_EXT=eth0", shown_path(config)});
        expect_code(table, 0, "check --connect (text)");
        require(contains(lf(table.standard_output), "network     kind      participants  outside\n"
                                                    "ETH_EXT     ethernet  Camera        eth0\n"
                                                    "J1939_CAN1  can       Engine        -\n"),
                "check --connect wrote:\n" + table.standard_output);
        // A bridge that refuses what the launcher gives it: the bridge's own words.
        std::string low = text;
        replace_all(low, "bitrate: 250000", "bitrate: 5000");
        const fs::path refused = o.work_dir / "connect-5000.yaml";
        write_file(refused, low);
        const Captured failed = launch(o, {"check", "--connect", "J1939_CAN1=virtual:1", shown_path(refused)});
        expect_code(failed, 1, "check --connect with a bit rate the XL bridge refuses");
        require(contains(failed.standard_output, "[connect-tool-failed]") &&
                    contains(failed.standard_output, "--bitrate must be between 15000 and 1000000 bit/s"),
                "check --connect with bitrate 5000 wrote:\n" + failed.standard_output);
        // none: SIL Kit only, as without --connect.
        const Captured none = launch(o, {"check", "--json", "--connect", "ETH_EXT=none", shown_path(config)});
        expect_code(none, 0, "check --connect ETH_EXT=none");
        require(!contains(none.standard_output, "\"outside\": {") && contains(none.standard_output, "\"outside\": null") &&
                    contains(none.standard_output, "single-participant"),
                "check --connect ETH_EXT=none wrote:\n" + none.standard_output);
        require(contains(none.standard_output, "  \"time_lock\": {\n    \"locked\": false,\n    \"because\": []\n  },\n"),
                "check --connect ETH_EXT=none: no time lock:\n" + none.standard_output);
        // The same targets from a request file: the same check. A network of the file that this
        // configuration lacks is a note, and --connect <network>=none takes the file's target away.
        const fs::path bench = o.work_dir / "bench.json";
        write_file(bench, "{ \"vehicle_sil_run_request\": 1,\n"
                          "  \"connect\": { \"ETH_EXT\": \"eth0\", \"J1939_CAN1\": \"virtual:1\", \"LIN_BODY\": \"0\" } }\n");
        const Captured asked = launch(o, {"check", "--json", "--request", shown_path(bench), shown_path(config)});
        expect_outside(asked, "check --request with connect", tools);
        require(contains(asked.standard_output, "\"code\": \"connect-network-unused\"") &&
                    contains(asked.standard_output,
                             "\"message\": " + json_string("the network LIN_BODY of connect in " + shown_path(bench) +
                                                           " is not in this configuration: it is left out")) &&
                    contains(asked.standard_output, "\"errors\": 0,"),
                "check --request with a network the configuration lacks wrote:\n" + asked.standard_output);
        const Captured typed = launch(o, {"check", "--json", "--connect", "LIN_BODY=0", shown_path(config)});
        expect_code(typed, 1, "check --connect with a network the configuration lacks");
        require(contains(typed.standard_output, "\"code\": \"connect-network\""), "check --connect LIN_BODY=0 wrote:\n" + typed.standard_output);
        const Captured taken = launch(o, {"check", "--json", "--request", shown_path(bench), "--connect", "ETH_EXT=none", "--connect",
                                          "J1939_CAN1=none", shown_path(config)});
        expect_code(taken, 0, "check --request --connect <network>=none");
        require(contains(taken.standard_output, "  \"time_lock\": {\n    \"locked\": false,\n    \"because\": []\n  },\n") &&
                    !contains(taken.standard_output, "\"tool_path\"") &&
                    contains(taken.standard_output, "\"code\": \"connect-network-unused\""),
                "check --request --connect <network>=none wrote:\n" + taken.standard_output);
        const Captured one = launch(o, {"check", "--json", "--request", shown_path(bench), "--connect", "ETH_EXT=eth1", "--connect",
                                        "J1939_CAN1=none", shown_path(config)});
        expect_code(one, 0, "check --request --connect <network>=<another target>");
        require(contains(one.standard_output, "        \"target\": \"eth1\",\n") && !contains(one.standard_output, "\"target\": \"eth0\"") &&
                    !contains(one.standard_output, "\"target\": \"virtual:1\""),
                "check --request --connect ETH_EXT=eth1 wrote:\n" + one.standard_output);
        time_policy(o, text);
    }
    {
        // Next to the launcher, without the bridges on PATH.
        const PathSetting path(runtime_path());
        const Captured beside = vehicle_sil::process::run_captured(o.connect_tools / ("vehicle_sil_launch" + std::string(kExe)),
                                                                   both, vehicle_sil::process::CaptureOptions{120s});
        // The directory as the system names this launcher's (/proc/self/exe resolves symbolic links).
        std::error_code ec;
        const fs::path canonical = fs::canonical(o.connect_tools, ec);
        const std::string found = shown_path(o.connect_tools);
        const std::string resolved = ec ? found : shown_path(canonical);
        expect_outside(beside, "check --connect with the bridges next to the launcher",
                       contains(beside.standard_output, "\"tool_path\": \"" + found + "/") ? found : resolved);
        // Neither next to this launcher nor on PATH: copies in the configuration file's directory,
        // which is the working directory too, are not run.
        for (const char* tool : {"vehicle_sil_ethernet_bridge", "vehicle_sil_vector_xl_bridge"})
        {
            fs::copy_file(o.connect_tools / (tool + std::string(kExe)), o.work_dir / (tool + std::string(kExe)),
                          fs::copy_options::overwrite_existing);
        }
        const Captured missing = vehicle_sil::process::run_captured(
            o.launch, {"check", "--connect", "ETH_EXT=eth0", "--connect", "J1939_CAN1=virtual:1", "connect.yaml"},
            vehicle_sil::process::CaptureOptions{120s, 1u << 20, o.work_dir});
        expect_code(missing, 1, "check --connect without the bridges");
        const std::string launcherDir = shown_path(o.launch.parent_path());
        // The CAN bridge runs on Windows only: elsewhere there is nothing to build.
#ifdef _WIN32
        const std::string can = "--connect needs vehicle_sil_vector_xl_bridge, which is neither next to vehicle_sil_launch (" +
                                launcherDir + ") nor on PATH";
#else
        const std::string can = "--connect needs vehicle_sil_vector_xl_bridge, and the Vector XL bridge is for Windows: on "
                                "this computer the network runs on SIL Kit only: leave --connect J1939_CAN1 out (or give "
                                "J1939_CAN1=none)";
#endif
        require(contains(missing.standard_output, "--connect needs vehicle_sil_ethernet_bridge, which is neither next to "
                                                  "vehicle_sil_launch (" + launcherDir + ") nor on PATH") &&
                    contains(missing.standard_output, can),
                "check --connect without the bridges wrote:\n" + missing.standard_output);
    }
    log("CHECK --connect: the bridges on PATH and next to the launcher, not in the working directory -> ok");

    const auto expect_usage = [&](const std::vector<std::string>& args, const std::string& error) {
        const Captured run = launch(o, args);
        expect_code(run, 4, "vehicle_sil_launch " + error);
        require(lf(run.standard_error).rfind("error: " + error + "\n", 0) == 0,
                "for the error '" + error + "' vehicle_sil_launch wrote:\n" + run.standard_error);
    };
    expect_usage({"check", "--connect", "ETH_EXT", shown_path(config)},
                 "--connect ETH_EXT: write --connect <network>=<target>, the network as the file names it, e.g. --connect "
                 "\"ETH_EXT=vEthernet (A)\" (none as the target: SIL Kit only)");
    expect_usage({"check", "--connect", "ETH_EXT=eth0", "--connect", "ETH_EXT=eth1", shown_path(config)},
                 "--connect ETH_EXT is given twice (eth0 and eth1): connect each network once");
    // The bridge that --connect starts is not a participant of the file.
    const std::string bridgeNamed = "--disable bridge.ETH_EXT: bridge.ETH_EXT is the bridge that --connect starts for the "
                                    "network ETH_EXT, not a participant of the file: to run ETH_EXT on SIL Kit only, give "
                                    "--connect ETH_EXT=none or leave --connect out";
    {
        const Captured run = launch(o, {"run", "--disable", "bridge.ETH_EXT", "--connect", "ETH_EXT=eth0", shown_path(config)});
        expect_code(run, 4, "vehicle_sil_launch run --disable bridge.ETH_EXT");
        require(contains(lf(run.standard_output) + lf(run.standard_error), bridgeNamed),
                "run --disable bridge.ETH_EXT wrote:\n" + run.standard_output + run.standard_error);
    }
    expect_usage({"test", shown_path(config), "--connect", "ETH_EXT=eth0", "--scenarios", shown_path(o.work_dir)},
                 "--connect is not for test: test is made for runs that go as fast as possible, one after the other, and a real bus does not wait for "
                 "the virtual time; for a test with a device, run one scenario: vehicle_sil_launch run --scenario "
                 "<scenario> --speed 1 --strict --connect <network>=<target> <file>");
    // The value after --connect is not the file.
    const Captured json =
        launch(o, {"run", "--json", "--connect", "ETH_EXT=eth0", "--disable", "bridge.ETH_EXT", shown_path(config)});
    expect_code(json, 4, "vehicle_sil_launch run --json --connect");
    require(contains(json.standard_output, "\n  \"file\": " + json_string(shown_path(config)) + ",\n"),
            "run --json --connect wrote:\n" + json.standard_output);
    log("CHECK --connect usage errors: exit code 4 and the message -> ok");
}

void set_environment(const std::string& name, const std::string& value)
{
#ifdef _WIN32
    const bool ok = _putenv_s(name.c_str(), value.c_str()) == 0;
#else
    const bool ok = ::setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
    require(ok, "cannot set the environment variable " + name);
}

/// The argument of the first --connect line of `network` in the output of connections, as the
/// command line has it (the quotes that the line has for the shells taken away).
std::string first_connect(const std::string& text, const std::string& network)
{
    const std::string head = "\n" + network + " (";
    const auto at = text.find(head);
    require(at != std::string::npos, "connections wrote no line for " + network + ":\n" + text);
    const std::string mark = "\n  --connect ";
    const auto line = text.find(mark, at + 1);
    require(line != std::string::npos && text.find('\n', at + 1) == line,
            "connections wrote no --connect line right after the line of " + network + ":\n" + text);
    const auto begin = line + mark.size();
    std::string argument = text.substr(begin, text.find_first_of(" \n", begin) - begin);
    require(argument.rfind(network + "=", 0) == 0 && argument.find('"') == std::string::npos &&
                argument.find('\'') == std::string::npos,
            "the line for " + network + " is not --connect " + network + "=<target> without quotes: " + argument);
    return argument;
}

void connections(const Options& o)
{
#if !defined(_WIN32) && !defined(__linux__)
    log("SKIP: the Ethernet bridge has no pcap backend for this operating system");
    return;
#endif
    require(!o.fake_pcap_dir.empty(), "the scenario connections needs --fake-pcap-dir");
    // The backend that the launcher gives (packet on Linux, npcap on Windows): the bridge for the tests opens the fake library for it.
#ifdef _WIN32
    const std::string library = shown_path(o.fake_pcap_dir / "vehicle_sil_fake_pcap.dll");
    const std::string backend = "npcap";
#else
    const std::string library = shown_path(o.fake_pcap_dir / "vehicle_sil_fake_pcap.so");
    const std::string backend = "packet";
#endif
    set_environment("VEHICLE_SIL_TEST_PCAP_LIBRARY", library);
    set_environment("VEHICLE_SIL_FAKE_XL_FAIL_OPEN", "0");
    const std::string helper = yaml_quote(shown_path(o.helper));
    const auto described = [&](const std::string& name, const std::string& network) {
        return "[" + helper + ", --out, '{\"vehicle_sil_ecu\": 1, \"name\": \"" + name +
               "\", \"step_ns\": 1000000, \"networks\": [" + network + "]}']";
    };
    const fs::path config = o.work_dir / "connections.yaml";
    write_file(config,
               "vehicle_sil_simulation: 2\n"
                      "networks:\n"
               "  - name: ETH_EXT\n"
               "    kind: ethernet\n"
               "  - name: J1939_CAN1\n"
               "    kind: can\n"
               "  - name: BODY_LIN\n"
               "    kind: lin\n"
               "participants:\n"
               "  - name: Camera\n"
               "    command: " + described("Camera", "{\"name\": \"ETH_EXT\", \"kind\": \"ethernet\", \"use\": \"ethernet\", "
                                                    "\"nodes\": [], \"mac\": \"02:00:00:00:00:01\"}") + "\n"
               "  - name: Engine\n"
               "    command: " + described("Engine", "{\"name\": \"J1939_CAN1\", \"kind\": \"can\", \"use\": \"raw\", "
                                                    "\"nodes\": []}") + "\n");
    const fs::path beside = o.connect_tools / ("vehicle_sil_launch" + std::string(kExe));
    const PathSetting path(runtime_path());
    const auto connections_of = [&](const std::vector<std::string>& args) {
        return vehicle_sil::process::run_captured(beside, args, vehicle_sil::process::CaptureOptions{120s});
    };
    const std::string file = shown_path(config);

    // The bridges next to the launcher: every network, the lines to copy.
    const Captured text = connections_of({"connections", file});
    expect_code(text, 0, "connections");
    const std::string out = lf(text.standard_output);
    for (const std::string& part : std::vector<std::string>{
             file + ": where each network can be connected on this computer\n"
                    "ETH_EXT (ethernet; vehicle_sil_ethernet_bridge, " + backend + ": fake pcap version 1.0 (TEST ONLY)):\n"
                    "  --connect ETH_EXT=fake0  ",
             "\n  --connect ETH_EXT=fake-echo  ",
#ifdef _WIN32
             "\nJ1939_CAN1 (can; vehicle_sil_vector_xl_bridge):\n"
             "  --connect J1939_CAN1=virtual:1   Virtual Channel 1; virtual 1; ",
#else
             "\nETH_EXT (ethernet; vehicle_sil_ethernet_bridge, tap",
             "\nJ1939_CAN1 (can; vehicle_sil_socketcan_bridge):\n  cannot list: vehicle_sil_socketcan_bridge is neither next to ",
#endif
             "\nBODY_LIN (lin): SIL Kit only (there is no bridge for LIN)\n"})
    {
        require(contains(out, part), "connections has not\n" + part + "--- it wrote ---\n" + out);
    }
    require(text.standard_error.empty(), "connections wrote on stderr:\n" + text.standard_error);
    // What a line says is what check --connect takes.
#ifdef _WIN32
    const std::string can_line = first_connect(out, "J1939_CAN1");
#else
    // Linux lists SocketCAN interfaces for a CAN network (that bridge is not among the tools of this test): the Vector XL
    // channel is given by hand.
    const std::string can_line = "J1939_CAN1=virtual:1";
#endif
    const Captured checked = connections_of({"check", "--connect", first_connect(out, "ETH_EXT"), "--connect", can_line, file});
    expect_code(checked, 0, "check with the --connect lines of connections");
    require(contains(lf(checked.standard_output), "ETH_EXT     ethernet  Camera        fake0\n") &&
                contains(lf(checked.standard_output), "J1939_CAN1  can       Engine        virtual:1\n"),
            "check with the --connect lines of connections wrote:\n" + checked.standard_output);
    log("CHECK connections: the --connect lines of each network, which check takes -> ok");

    // JSON: the bridge's list as it is; LIN without one.
    const Captured json = connections_of({"connections", "--json", file});
    expect_code(json, 0, "connections --json");
    // The directory as the system names this launcher's (/proc/self/exe resolves symbolic links).
    std::error_code ec;
    const fs::path canonical = fs::canonical(o.connect_tools, ec);
    const std::string given = shown_path(o.connect_tools);
    const std::string found =
        contains(json.standard_output, "\"tool_path\": " + json_string(given + "/vehicle_sil_ethernet_bridge" + kExe)) || ec
            ? given
            : shown_path(canonical);
    for (const std::string& part : std::vector<std::string>{
             "{\n  \"vehicle_sil_connections\": 1,\n",
             "  \"file\": " + json_string(file) + ",\n  \"result\": \"listed\",\n",
             "      \"name\": \"ETH_EXT\",\n      \"kind\": \"ethernet\",\n      \"tool\": \"vehicle_sil_ethernet_bridge\",\n"
             "      \"backend\": \"" + backend + "\",\n"
             "      \"tool_path\": " + json_string(found + "/vehicle_sil_ethernet_bridge" + kExe) + ",\n"
             "      \"list\": {\n        \"vehicle_sil_targets\": 1,\n        \"tool\": \"vehicle_sil_ethernet_bridge\",\n"
             "        \"backend\": \"" + backend + "\",\n",
             "            \"value\": \"fake0\",\n",
#ifdef _WIN32
             "            \"can\": {\n              \"target\": \"virtual\",\n              \"number\": 1,\n",
#endif
             "      \"name\": \"BODY_LIN\",\n      \"kind\": \"lin\",\n      \"tool\": null,\n      \"backend\": null,\n"
             "      \"tool_path\": null,\n"
             "      \"list\": null\n",
             "  \"findings\": []\n}\n"})
    {
        require(contains(lf(json.standard_output), part), "connections --json has not\n" + part + "--- it wrote ---\n" +
                                                            json.standard_output);
    }
    // --network: that one only.
    const Captured one = connections_of({"connections", "--network", "J1939_CAN1", "--json", file});
    expect_code(one, 0, "connections --network J1939_CAN1 --json");
    require(contains(one.standard_output, "\"name\": \"J1939_CAN1\"") && !contains(one.standard_output, "ETH_EXT") &&
                !contains(one.standard_output, "BODY_LIN"),
            "connections --network J1939_CAN1 --json wrote:\n" + one.standard_output);
    const Captured lin = connections_of({"connections", "--network", "BODY_LIN", file});
    expect_code(lin, 0, "connections --network BODY_LIN");
    expect_text(lf(lin.standard_output),
                file + ": where each network can be connected on this computer\n"
                       "BODY_LIN (lin): SIL Kit only (there is no bridge for LIN)\n",
                "connections --network BODY_LIN");

    // The bridges cannot list: their own error, exit code 1.
    set_environment("VEHICLE_SIL_TEST_PCAP_LIBRARY", shown_path(o.work_dir / "no-such-library"));
    set_environment("VEHICLE_SIL_FAKE_XL_FAIL_OPEN", "1");
    const Captured failed = connections_of({"connections", file});
    set_environment("VEHICLE_SIL_TEST_PCAP_LIBRARY", library);
    set_environment("VEHICLE_SIL_FAKE_XL_FAIL_OPEN", "0");
    expect_code(failed, 1, "connections with bridges that cannot list");
#ifdef _WIN32
    const bool canFailed = contains(lf(failed.standard_output), "J1939_CAN1 (can; vehicle_sil_vector_xl_bridge):\n  cannot list: "
                                                                "cannot open the Vector XL driver: ") &&
                           contains(failed.standard_output, " [driver-missing]\n");
#else
    const bool canFailed = true;
#endif
    require(contains(lf(failed.standard_output), "ETH_EXT (ethernet; vehicle_sil_ethernet_bridge, " + backend + "):\n  cannot list: ") &&
                contains(failed.standard_output, " [library-missing]\n") && canFailed,
            "connections with bridges that cannot list wrote:\n" + failed.standard_output);

    // Without the bridges (the launcher of the build, PATH without them): tool-missing, which is an
    // answer (exit code 0); the CAN bridge, elsewhere than on Windows: none for the kind, as LIN.
    const Captured missing = launch(o, {"connections", "--json", file});
    expect_code(missing, 0, "connections without the bridges");
    const std::string launcherDir = shown_path(o.launch.parent_path());
    for (const std::string& part : std::vector<std::string>{
             "  \"result\": \"listed\",\n",
#ifdef _WIN32
             "      \"tool\": \"vehicle_sil_vector_xl_bridge\",\n      \"backend\": \"xl\",\n      \"tool_path\": null,\n",
#else
             // Linux: the CAN network is listed by the SocketCAN bridge, which is not found either.
             "      \"name\": \"J1939_CAN1\",\n      \"kind\": \"can\",\n      \"tool\": \"vehicle_sil_socketcan_bridge\",\n"
             "      \"backend\": \"socketcan\",\n      \"tool_path\": null,\n",
#endif
             "          \"code\": \"tool-missing\",\n          \"message\": " +
                 json_string("vehicle_sil_ethernet_bridge is neither next to vehicle_sil_launch (" + launcherDir +
                             ") nor on PATH: build it (docs/ethernet-bridge.md) and put it next to vehicle_sil_launch, or its "
                             "directory on PATH") + "\n"})
    {
        require(contains(lf(missing.standard_output), part), "connections without the bridges has not\n" + part +
                                                                 "--- it wrote ---\n" + missing.standard_output);
    }
    log("CHECK connections: --json, --network, a bridge that cannot list, no bridge -> ok");

    // --request: the targets of a request file against the lists. Only the networks it connects are listed; a network that
    // the file has and the configuration lacks is a note.
    fs::create_directories(o.work_dir / "bench");
    const auto bench = [&](const std::string& name, const std::string& connect) {
        write_file(o.work_dir / "bench" / name, "{ \"vehicle_sil_run_request\": 1, \"connect\": " + connect + " }\n");
        return shown_path(o.work_dir / "bench" / name);
    };
    const std::string usable = bench("usable.json", "{ \"ETH_EXT\": \"" + backend + ":fake0\", \"OTHER\": \"eth0\" }");
    const Captured byRequest = connections_of({"connections", "--json", "--request", usable, file});
    expect_code(byRequest, 0, "connections --request with a usable target");
    for (const std::string& part : std::vector<std::string>{
             "  \"result\": \"listed\",\n  \"request\": {\n    \"file\": " + json_string(usable) + ",\n    \"connect\": [\n      {\n"
             "        \"network\": \"ETH_EXT\",\n        \"target\": \"" + backend + ":fake0\",\n        \"usable\": true,\n"
             "        \"code\": null,\n        \"reason\": null\n",
             "\"code\": \"connect-network-unused\""})
    {
        require(contains(lf(byRequest.standard_output), part),
                "connections --request has not\n" + part + "--- it wrote ---\n" + byRequest.standard_output);
    }
    require(!contains(byRequest.standard_output, "\"J1939_CAN1\"") && !contains(byRequest.standard_output, "\"BODY_LIN\""),
            "connections --request listed a network that the file does not connect:\n" + byRequest.standard_output);
    const Captured textRequest = connections_of({"connections", "--request", usable, file});
    expect_code(textRequest, 0, "connections --request (text)");
    require(contains(lf(textRequest.standard_output), "):\n  requested in " + usable + ": --connect ETH_EXT=" + backend + ":fake0  usable\n"),
            "connections --request (text) wrote:\n" + textRequest.standard_output);
    // A target that is not in the list: exit code 1, and why.
    const std::string absent = bench("absent.json", "{ \"ETH_EXT\": \"nosuch0\" }");
    const Captured notListed = connections_of({"connections", "--request", absent, file});
    expect_code(notListed, 1, "connections --request with a target that is not listed");
    require(contains(lf(notListed.standard_output), "  requested in " + absent + ": --connect ETH_EXT=nosuch0  not usable: nosuch0 is not in the list of "
                                                    "vehicle_sil_ethernet_bridge (" + backend + ") for ETH_EXT: choose a target from the list, or fix "
                                                    "the request file [target-not-listed]\n"),
            "connections --request with a target that is not listed wrote:\n" + notListed.standard_output);
    const Captured notListedJson = connections_of({"connections", "--json", "--request", absent, file});
    expect_code(notListedJson, 1, "connections --json --request with a target that is not listed");
    require(contains(notListedJson.standard_output, "\"result\": \"failed\"") && contains(notListedJson.standard_output, "\"usable\": false"),
            "connections --json --request with a target that is not listed wrote:\n" + notListedJson.standard_output);
    // --network for a network the file does not connect: its list only, nothing to check.
    const Captured other = connections_of({"connections", "--json", "--network", "J1939_CAN1", "--request", absent, file});
    expect_code(other, 0, "connections --network --request for a network the request does not connect");
    require(contains(lf(other.standard_output), "\"request\": {\n    \"file\": " + json_string(absent) + ",\n    \"connect\": []\n  },\n") &&
                contains(other.standard_output, "\"name\": \"J1939_CAN1\""),
            "connections --network --request wrote:\n" + other.standard_output);
    log("CHECK connections: --request -> ok");

    // The command line and the file.
    const auto expect_usage = [&](const std::vector<std::string>& args, const std::string& error) {
        const Captured run = launch(o, args);
        expect_code(run, 4, "vehicle_sil_launch " + error);
        require(lf(run.standard_error).rfind("error: " + error + "\n", 0) == 0,
                "for the error '" + error + "' vehicle_sil_launch wrote:\n" + run.standard_error);
    };
    expect_usage({"connections", "--network", "ETH", file},
                 "--network names the network 'ETH', which the file does not have: did you mean 'ETH_EXT'? (the networks: "
                 "ETH_EXT, J1939_CAN1, BODY_LIN)");
    expect_usage({"connections", "--variant", "a", file},
                 "connections takes no --variant: every variant has the networks of the file, so leave it out");
    expect_usage({"connections"}, "connections needs the configuration file: vehicle_sil_launch connections <file>");
    expect_usage({"connections", "--strict", file}, "unknown option --strict");
    // A request file that cannot be read: a usage error, as for check --request.
    const std::string noRequest = shown_path(o.work_dir / "bench" / "no-such.json");
    expect_usage({"connections", "--request", noRequest, file}, "--request " + noRequest + ": the file does not exist: check the path");
    const std::string wrongRequest = bench("wrong.json", "[]");
    const Captured wrong = launch(o, {"connections", "--request", wrongRequest, file});
    expect_code(wrong, 4, "connections --request with a request file that is wrong");
    require(contains(wrong.standard_error, "error: " + wrongRequest + ", line 1: connect must be a mapping"),
            "connections --request with a request file that is wrong wrote:\n" + wrong.standard_error);
    const Captured usageJson = launch(o, {"connections", "--json", "--network", "NOPE", file});
    expect_code(usageJson, 4, "connections --json --network NOPE");
    require(contains(usageJson.standard_output, "  \"result\": \"usage\",\n  \"networks\": [],\n"),
            "connections --json --network NOPE wrote:\n" + usageJson.standard_output);
    const Captured unreadable = launch(o, {"connections", "--json", shown_path(o.work_dir / "missing.yaml")});
    expect_code(unreadable, 3, "connections of a file that does not exist");
    require(contains(unreadable.standard_output, "  \"result\": \"unreadable\",\n") &&
                contains(unreadable.standard_output, "\"code\": \"file\""),
            "connections of a file that does not exist wrote:\n" + unreadable.standard_output);
    log("CHECK connections: the command line (exit 4) and a file that cannot be read (exit 3) -> ok");
}

} // namespace

/// Runs vehicle_sil_launch with `args` and `input` on its standard input (then closed). Its
/// standard output and error as lines, each with "\n".
Captured launch_with_input(const Options& o, const std::vector<std::string>& args, const std::string& input)
{
#ifndef _WIN32
    // A launcher that stops reading a text that is too large: the write fails, it does not end us.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::mutex mutex;
    Captured captured;
    vehicle_sil::process::ProcessOptions options;
    options.input = true;
    options.max_line = std::size_t{1} << 20;
    options.on_line = [&](vehicle_sil::process::Stream stream, std::string_view line) {
        const std::lock_guard lock(mutex);
        (stream == vehicle_sil::process::Stream::Output ? captured.standard_output : captured.standard_error) +=
            std::string(line) + "\n";
    };
    vehicle_sil::process::Process process(o.launch, args, std::move(options));
    static_cast<void>(process.write_input(input)); // false once the launcher has stopped reading
    process.close_input();
    const std::optional<vehicle_sil::process::ExitStatus> status = process.wait_for(scaled_timeout(120s));
    if (!status)
    {
        process.kill_tree();
    }
    process.finish_output(scaled_timeout(10s));
    const std::lock_guard lock(mutex);
    if (status)
    {
        captured.exit_code = status->code;
    }
    return captured;
}

/// The value of "status" of the participant `name` in the JSON of check.
std::string status_of(const std::string& json, const std::string& name)
{
    const auto at = json.find("      \"name\": \"" + name + "\",\n");
    require(at != std::string::npos, "the JSON has no participant " + name + ":\n" + json);
    const std::string key = "\"status\": \"";
    const auto from = json.find(key, at) + key.size();
    return json.substr(from, json.find('"', from) - from);
}

std::size_t count_of(const std::string& text, const std::string& part)
{
    std::size_t n = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
    {
        ++n;
    }
    return n;
}

void descriptions(const Options& o)
{
    const fs::path dir = o.work_dir;
    // Every start of a copy of the helper adds a line to this file (process_helper.cpp).
    const fs::path mark = dir / "started.txt";
    set_environment("VEHICLE_SIL_HELPER_MARK", vehicle_sil::process::utf8_from_path(mark));
    const auto starts = [&] { return fs::exists(mark) ? count_of(read_file(mark), "started\n") : std::size_t{0}; };
    // Engine's executable is a copy of its own (built again below); the bridge of --connect is a
    // copy under the bridge's name on PATH.
    const fs::path engine = dir / ("engine_ecu" + std::string(kExe));
    fs::copy_file(o.helper, engine, fs::copy_options::overwrite_existing);
    fs::create_directories(dir / "bridges");
    fs::copy_file(o.helper, dir / "bridges" / ("vehicle_sil_ethernet_bridge" + std::string(kExe)),
                  fs::copy_options::overwrite_existing);
#ifdef _WIN32
    const std::string runtime = runtime_path();
    const std::string bridges = vehicle_sil::process::utf8_from_path(dir / "bridges");
    const PathSetting path(runtime.empty() ? bridges : bridges + ";" + runtime);
#else
    const PathSetting path(shown_path(dir / "bridges"));
#endif
    const auto description = [](const std::string& name, const std::string& networks) {
        return "{\"vehicle_sil_ecu\": 1, \"name\": \"" + name + "\", \"step_ns\": 1000000, \"networks\": [" + networks + "]}";
    };
    const std::string can = "{\"name\": \"CAN1\", \"kind\": \"can\", \"use\": \"raw\", \"nodes\": []}";
    const auto eth = [](const std::string& mac) {
        return "{\"name\": \"ETH\", \"kind\": \"ethernet\", \"use\": \"ethernet\", \"nodes\": [], \"mac\": \"" + mac + "\"}";
    };
    write_file(dir / "engine.json", description("Engine", eth("02:00:00:00:00:01") + ", " + can));
    write_file(dir / "dashboard.json", description("Dashboard", eth("02:00:00:00:00:02") + ", " + can));
    write_file(dir / "gateway.json", description("Gateway", can));
    const std::string helper = yaml_quote(shown_path(o.helper));
    const std::string text = "vehicle_sil_simulation: 2\n"
                             "networks:\n"
                             "  - name: ETH\n"
                             "    kind: ethernet\n"
                             "  - name: CAN1\n"
                             "    kind: can\n"
                             "participants:\n"
                             "  - name: Engine\n"
                             "    command: [engine_ecu, --out-file, engine.json]\n"
                             "  - name: Dashboard\n"
                             "    command: [" + helper + ", --out-file, dashboard.json]\n"
                             "  - name: Gateway\n"
                             "    role: bridge\n"
                             "    command: [" + helper + ", --out-file, gateway.json]\n";
    const fs::path config = dir / "sim.yaml";
    write_file(config, text);
    const std::string file = shown_path(config);
    const std::string saved = shown_path(dir / "saved.json");
    write_file(dir / "request.json", "{\"vehicle_sil_run_request\": 1, \"connect\": {\"ETH\": \"eth0\"}}");
    const std::string request = shown_path(dir / "request.json");

    // The fixture: a check runs each participant and the bridge of --connect (which says nothing:
    // connect-tool-failed).
    const Captured connected = launch(o, {"check", "--json", "--connect", "ETH=eth0", file});
    expect_code(connected, 1, "check --connect");
    require(contains(connected.standard_output, "\"code\": \"connect-tool-failed\"") && starts() == 4,
            "check --connect did not start the 3 participants and the bridge (" + std::to_string(starts()) + " starts):\n" +
                connected.standard_output);
    fs::remove(mark);
    const Captured written = launch(o, {"check", "--json", "--write-descriptions", saved, file});
    expect_code(written, 0, "check --write-descriptions");
    require(starts() == 3 && fs::exists(dir / "saved.json"), "check --write-descriptions: " + std::to_string(starts()) +
                                                                 " starts, or no file");
    fs::remove(mark);

    // With the saved descriptions: nothing starts, whatever asks for a bridge.
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"check", "--json", "--descriptions", saved, file},
          {"check", "--json", "--descriptions", saved, "--connect", "ETH=eth0", file},
          {"check", "--json", "--descriptions", saved, "--request", request, file},
          {"check", "--json", "--descriptions", shown_path(dir / "none.json"), "--connect", "ETH=eth0", file}})
    {
        const Captured run = launch(o, args);
        require(run.exit_code == 0 && starts() == 0,
                "check " + args.at(args.size() - 2) + " with saved descriptions started " + std::to_string(starts()) +
                    " processes or did not pass:\n" + run.standard_output + run.standard_error);
        require(!contains(run.standard_output, "\"tool_path\": \"/") && !contains(run.standard_output, "connect-tool"),
                "check --descriptions looked for the bridge:\n" + run.standard_output);
    }
    // The bridge's command is the one a run gives it (with the ECUs' MAC addresses), and none while
    // a participant is not described.
    const std::string mac = "          \"--src-mac\",\n          \"02:00:00:00:00:01,02:00:00:00:00:02\"\n";
    const Captured withMacs = launch(o, {"check", "--json", "--descriptions", saved, "--connect", "ETH=eth0", file});
    require(contains(withMacs.standard_output, mac), "check --descriptions --connect: no --src-mac of both ECUs:\n" +
                                                         withMacs.standard_output);
    const Captured unknown =
        launch(o, {"check", "--json", "--descriptions", shown_path(dir / "none.json"), "--connect", "ETH=eth0", file});
    require(contains(unknown.standard_output, "        \"command\": null\n") && !contains(unknown.standard_output, "--src-mac"),
            "check --descriptions <missing file> --connect: a command for the bridge:\n" + unknown.standard_output);
    // The same report as the check that saved them; written again, the same file.
    const Captured replayed =
        launch(o, {"check", "--json", "--descriptions", saved, "--write-descriptions", shown_path(dir / "again.json"), file});
    expect_code(replayed, 0, "check --descriptions --write-descriptions");
    expect_text(replayed.standard_output, written.standard_output, "check --descriptions (the report of the check that saved them)");
    expect_text(read_file(dir / "again.json"), read_file(dir / "saved.json"), "the descriptions written again");
    const Captured table = launch(o, {"check", "--descriptions", saved, file});
    expect_text(table.standard_output, launch(o, {"check", file}).standard_output, "check --descriptions (text)");
    fs::remove(mark);

    // No file yet: no participant is described, and nothing else is said.
    const Captured none = launch(o, {"check", "--json", "--descriptions", shown_path(dir / "none.json"), file});
    expect_code(none, 0, "check --descriptions <missing file>");
    require(status_of(none.standard_output, "Engine") == "not_described" &&
                status_of(none.standard_output, "Dashboard") == "not_described" &&
                status_of(none.standard_output, "Gateway") == "not_described" &&
                count_of(none.standard_output, "\"code\": \"not-described\"") == 3 &&
                !contains(none.standard_output, "descriptions-not-used"),
            "check --descriptions <missing file>:\n" + none.standard_output);
    // A file that is not one, and one of another version: one note, and no participant is described.
    write_file(dir / "broken.json", "{\"vehicle_sil_descriptions\": 1, \"vehicle_sil\": ");
    std::string other = read_file(dir / "saved.json");
    const std::string version_key = "\"vehicle_sil\": \"";
    other.insert(other.find(version_key) + version_key.size(), "0.0.1-other-");
    write_file(dir / "other.json", other);
    for (const std::string name : {"broken.json", "other.json"})
    {
        const Captured unusable = launch(o, {"check", "--json", "--descriptions", shown_path(dir / name), file});
        expect_code(unusable, 0, "check --descriptions " + name);
        require(count_of(unusable.standard_output, "\"code\": \"descriptions-not-used\"") == 1 &&
                    status_of(unusable.standard_output, "Engine") == "not_described" &&
                    status_of(unusable.standard_output, "Gateway") == "not_described",
                "check --descriptions " + name + ":\n" + unusable.standard_output);
    }
    require(contains(launch(o, {"check", "--descriptions", shown_path(dir / "other.json"), file}).standard_output,
                     "the saved descriptions (--descriptions) are not used: vehicle-sil 0.0.1-other-"),
            "check --descriptions other.json does not say which version wrote it");

    // Another command, and an executable built again: that participant only.
    std::string changed = text;
    replace_all(changed, "--out-file, dashboard.json]", "--out-file, dashboard.json, --verbose]");
    write_file(dir / "changed.yaml", changed);
    const Captured command = launch(o, {"check", "--json", "--descriptions", saved, shown_path(dir / "changed.yaml")});
    require(status_of(command.standard_output, "Dashboard") == "not_described" &&
                status_of(command.standard_output, "Engine") == "described" &&
                count_of(command.standard_output, "\"code\": \"not-described\"") == 1 &&
                contains(command.standard_output, "\"participant 'Dashboard': not described: "),
            "check --descriptions with another command of Dashboard:\n" + command.standard_output);
    {
        std::ofstream out(engine, std::ios::binary | std::ios::app);
        out << "built again";
    }
    const Captured rebuilt = launch(o, {"check", "--json", "--descriptions", saved, file});
    require(status_of(rebuilt.standard_output, "Engine") == "not_described" &&
                status_of(rebuilt.standard_output, "Dashboard") == "described" &&
                count_of(rebuilt.standard_output, "\"code\": \"not-described\"") == 1,
            "check --descriptions after Engine was built again:\n" + rebuilt.standard_output);
    require(starts() == 0, "a check with saved descriptions started a process");
    fs::copy_file(o.helper, engine, fs::copy_options::overwrite_existing);
    expect_text(launch(o, {"check", "--json", "--write-descriptions", saved, file}).standard_output, written.standard_output,
                "check --write-descriptions after Engine was copied again");
    log("CHECK --descriptions: nothing starts, the same report, a changed command or executable, unusable files -> ok");

    // A file that cannot be written: an error, the rest as without it.
    const Captured unwritable =
        launch(o, {"check", "--json", "--write-descriptions", shown_path(dir / "no-such-dir" / "saved.json"), file});
    expect_code(unwritable, 1, "check --write-descriptions <a file in a missing directory>");
    require(count_of(unwritable.standard_output, "\"code\": \"descriptions-write\"") == 1 &&
                status_of(unwritable.standard_output, "Engine") == "described",
            "check --write-descriptions <a file in a missing directory>:\n" + unwritable.standard_output);
    fs::remove(mark);

    // The text from stdin: as the same text in the file named, which need not exist; its relative
    // paths are from that file's directory. With CRLF and an error on a known line.
    std::string crlf = text + "colour: red\n";
    replace_all(crlf, "\n", "\r\n");
    write_file(dir / "crlf.yaml", crlf);
    const Captured from_file = launch(o, {"check", "--json", shown_path(dir / "crlf.yaml")});
    expect_code(from_file, 1, "check crlf.yaml");
    require(contains(from_file.standard_output, "\"line\": 15,"), "check crlf.yaml:\n" + from_file.standard_output);
    Captured piped = launch_with_input(o, {"check", "--json", "--text-from-stdin", shown_path(dir / "stdin.yaml")}, crlf);
    expect_code(piped, 1, "check --text-from-stdin");
    expect_text(shown_as(piped.standard_output, dir / "stdin.yaml", shown_path(dir / "crlf.yaml")), from_file.standard_output,
                "check --text-from-stdin (as the file)");
    require(!fs::exists(dir / "stdin.yaml"), "check --text-from-stdin made the file it names");
    // Ctrl+Z (0x1A) is a byte like any other: stdin read as text on Windows would end there.
    std::string ctrlZ = text;
    ctrlZ.insert(ctrlZ.find("participants:\n"), "# \x1A\n");
    write_file(dir / "ctrl-z.yaml", ctrlZ);
    const Captured ctrlZFile = launch(o, {"check", "--json", shown_path(dir / "ctrl-z.yaml")});
    piped = launch_with_input(o, {"check", "--json", "--text-from-stdin", shown_path(dir / "ctrl-z.yaml")}, ctrlZ);
    expect_code(piped, *ctrlZFile.exit_code, "check --text-from-stdin with 0x1A");
    expect_text(piped.standard_output, ctrlZFile.standard_output, "check --text-from-stdin with 0x1A (as the file)");
    // With the saved descriptions too: what the screen of a configuration that is being edited does.
    fs::remove(mark);
    piped = launch_with_input(o, {"check", "--json", "--text-from-stdin", "--descriptions", saved, file}, text);
    expect_text(piped.standard_output, written.standard_output, "check --text-from-stdin --descriptions");
    require(starts() == 0, "check --text-from-stdin --descriptions started a process");
    // What a file may not be either.
    const Captured large = launch_with_input(o, {"check", "--json", "--text-from-stdin", file},
                                             std::string((std::size_t{16} << 20) + 1, '#'));
    expect_code(large, 3, "check --text-from-stdin with more than 16 MiB");
    require(contains(large.standard_output, "the text on standard input (--text-from-stdin) is larger than 16 MiB"),
            "check --text-from-stdin with more than 16 MiB:\n" + large.standard_output);
    static constexpr char kWithNul[] = "vehicle_sil_simulation: 2\n\0networks: []\n";
    const Captured nul = launch_with_input(o, {"check", "--json", "--text-from-stdin", file},
                                           std::string(kWithNul, sizeof kWithNul - 1));
    expect_code(nul, 3, "check --text-from-stdin with NUL");
    require(contains(nul.standard_output, "\"code\": \"encoding\""), "check --text-from-stdin with NUL:\n" + nul.standard_output);
    log("CHECK --text-from-stdin: as the file, CRLF and lines, too large, NUL -> ok");

    // Only check takes them.
    fs::create_directories(dir / "scenarios");
    for (const auto& [args, message] : std::vector<std::pair<std::vector<std::string>, std::string>>{
             {{"run", "--text-from-stdin", file}, "--text-from-stdin is for check"},
             {{"run", "--descriptions", saved, file}, "--descriptions is for check"},
             {{"run", "--request", request, "--write-descriptions", saved, file}, "--write-descriptions is for check"},
             {{"test", file, "--descriptions", saved, "--scenarios", shown_path(dir / "scenarios")}, "--descriptions is for check"}})
    {
        const Captured refused = launch(o, args);
        expect_code(refused, 4, args.front() + " " + args.at(1));
        require(contains(refused.standard_error, "error: " + message), args.front() + " " + args.at(1) + ":\n" +
                                                                         refused.standard_error);
    }
    require(starts() == 0, "run or test started a process with a usage error");
    log("CHECK run and test refuse --text-from-stdin, --descriptions and --write-descriptions -> ok");
}

/// The value of the top-level "commands" of the JSON of check (its text up to the end of the line).
std::string commands_of(const std::string& json)
{
    const std::string key = "\n  \"commands\": ";
    const auto at = json.find(key);
    require(at != std::string::npos, "the JSON of check has no \"commands\":\n" + json);
    const auto from = at + key.size();
    return json.substr(from, json.find('\n', from) - from);
}

void compare(const Options& o)
{
    const fs::path dir = o.work_dir;
    // Every start of a copy of the helper adds a line to this file (process_helper.cpp).
    const fs::path mark = dir / "started.txt";
    set_environment("VEHICLE_SIL_HELPER_MARK", vehicle_sil::process::utf8_from_path(mark));
    const auto starts = [&] { return fs::exists(mark) ? count_of(read_file(mark), "started\n") : std::size_t{0}; };
    const std::string can = "{\"name\": \"CAN1\", \"kind\": \"can\", \"use\": \"raw\", \"nodes\": []}";
    for (const std::string name : {"Engine", "Dashboard"})
    {
        write_file(dir / (name + ".json"),
                   "{\"vehicle_sil_ecu\": 1, \"name\": \"" + name + "\", \"step_ns\": 1000000, \"networks\": [" + can + "]}");
    }
    const std::string helper = yaml_quote(shown_path(o.helper));
    const std::string text = "vehicle_sil_simulation: 2\n"
                             "networks:\n"
                             "  - name: CAN1\n"
                             "    kind: can\n"
                             "participants:\n"
                             "  - name: Engine\n"
                             "    command: [" + helper + ", --out-file, Engine.json]\n"
                             "  - name: Dashboard\n"
                             "    command: [" + helper + ", --out-file, Dashboard.json]\n";
    const fs::path config = dir / "sim.yaml";
    write_file(config, text);
    const std::string file = shown_path(config);
    const std::string none = shown_path(dir / "none.json");

    // Without the option: null, and nothing said.
    const Captured plain = launch(o, {"check", "--json", file});
    expect_code(plain, 0, "check --json");
    require(commands_of(plain.standard_output) == "null," && !contains(plain.standard_output, "\"code\": \"commands-"),
            "check --json without --compare-commands:\n" + plain.standard_output);
    // The file with itself: the same, and the rest of the report as without the option.
    const Captured same = launch(o, {"check", "--json", "--compare-commands", file, file});
    expect_code(same, 0, "check --compare-commands <the same file>");
    require(commands_of(same.standard_output) == "\"same\",", "check --compare-commands <the same file>:\n" + same.standard_output);
    std::string withoutKey = same.standard_output;
    replace_all(withoutKey, "\"commands\": \"same\",", "\"commands\": null,");
    expect_text(withoutKey, plain.standard_output, "check --compare-commands <the same file> (but for commands)");
    // A file that cannot be read: not_compared and the note; a note does not fail the check.
    const Captured missing = launch(o, {"check", "--json", "--compare-commands", shown_path(dir / "missing.yaml"), file});
    expect_code(missing, 0, "check --compare-commands <a missing file>");
    require(commands_of(missing.standard_output) == "\"not_compared\"," &&
                count_of(missing.standard_output, "\"code\": \"commands-not-compared\"") == 1 &&
                contains(missing.standard_output, "missing.yaml: the file does not exist: check the path"),
            "check --compare-commands <a missing file>:\n" + missing.standard_output);
    log("CHECK --compare-commands: null without it, the same file, a missing file -> ok");

    // What a screen that edits the file does: the text from stdin, the saved descriptions (none
    // here), the file on disk to compare with. Nothing starts.
    fs::remove(mark);
    std::string networks = text;
    replace_all(networks, "    kind: can\n", "    kind: can\n  - name: CAN2\n    kind: can\n");
    const Captured edited =
        launch_with_input(o, {"check", "--json", "--text-from-stdin", "--descriptions", none, "--compare-commands", file, file},
                          networks);
    require(commands_of(edited.standard_output) == "\"same\",", "a network added (stdin):\n" + edited.standard_output);
    std::string command = text;
    replace_all(command, "--out-file, Dashboard.json]", "--out-file, Dashboard.json, --verbose]");
    const Captured changed =
        launch_with_input(o, {"check", "--json", "--text-from-stdin", "--descriptions", none, "--compare-commands", file, file},
                          command);
    require(commands_of(changed.standard_output) == "\"changed\"," &&
                count_of(changed.standard_output, "\"code\": \"commands-changed\"") == 1 &&
                contains(changed.standard_output,
                         "\"message\": " +
                             json_string("the commands that run differ from " + file + ": participant 'Dashboard': command differs")),
            "a command changed (stdin):\n" + changed.standard_output);
    std::string broken = text;
    replace_all(broken, "    command: [" + helper + ", --out-file, Dashboard.json]\n", "");
    const Captured unread =
        launch_with_input(o, {"check", "--json", "--text-from-stdin", "--descriptions", none, "--compare-commands", file, file},
                          broken);
    require(commands_of(unread.standard_output) == "\"not_compared\"," &&
                count_of(unread.standard_output, "\"code\": \"commands-not-compared\"") == 1,
            "a participant without a command (stdin):\n" + unread.standard_output);
    require(starts() == 0, "check --descriptions --compare-commands started " + std::to_string(starts()) + " processes");
    // For people: the note.
    write_file(dir / "changed.yaml", command);
    const Captured people = launch(o, {"check", "--descriptions", none, "--compare-commands", file, shown_path(dir / "changed.yaml")});
    expect_code(people, 0, "check --compare-commands (text)");
    require(contains(people.standard_output, "note: " + shown_path(dir / "changed.yaml") +
                                                 ": the commands that run differ from " + file +
                                                 ": participant 'Dashboard': command differs [commands-changed]\n"),
            "check --compare-commands (text):\n" + people.standard_output);
    require(starts() == 0, "check --descriptions --compare-commands started a process");
    log("CHECK --compare-commands with --text-from-stdin and --descriptions: same, changed, not compared, nothing starts -> ok");

    // Only check takes it, and with a file.
    fs::create_directories(dir / "scenarios");
    for (const auto& [args, message] : std::vector<std::pair<std::vector<std::string>, std::string>>{
             {{"run", "--compare-commands", file, file}, "--compare-commands is for check"},
             {{"test", file, "--compare-commands", file, "--scenarios", shown_path(dir / "scenarios")},
              "--compare-commands is for check"},
             {{"check", "--compare-commands", "", file}, "--compare-commands needs a file"}})
    {
        const Captured refused = launch(o, args);
        expect_code(refused, 4, args.front() + " " + args.at(1));
        require(contains(refused.standard_error, "error: " + message), args.front() + " " + args.at(1) + ":\n" +
                                                                         refused.standard_error);
    }
    require(starts() == 0, "run or test started a process with a usage error");
    log("CHECK run and test refuse --compare-commands -> ok");
}

void file_only(const Options& o)
{
    const fs::path dir = o.work_dir / "file_only";
    fs::create_directories(dir);
    const std::string lf_text =
        "vehicle_sil_simulation: 2\n"
        "networks:\n"
        "  - name: CAN1\n"
        "    kind: can\n"
        "participants:\n"
        "  - name: Engine\n"
        "    command: [\"./does_not_exist_ecu\"]\n"
        "variants:\n"
        "  fast: {}\n";
    const std::string crlf_text =
        "vehicle_sil_simulation: 2\r\n"
        "networks:\n"
        "  - name: CAN1\r\n"
        "    kind: can\r\n"
        "participants:\n"
        "  - name: Engine\r\n"
        "    command: [\"./does_not_exist_ecu\"]\r\n"
        "variants:\r\n"
        "  fast: {}\r\n";
    const fs::path lf = dir / "lf.yaml";
    const fs::path crlf = dir / "crlf.yaml";
    write_file(lf, lf_text);
    write_file(crlf, crlf_text);

    // 1. check --file-only --json succeeds even when ECU executable does not exist.
    const Captured lf_res = launch(o, {"check", "--file-only", "--json", "--variant", "fast", shown_path(lf)});
    expect_code(lf_res, 0, "check --file-only --json lf.yaml");
    require(contains(lf_res.standard_output, "\"result\": \"passed\""), lf_res.standard_output);
    require(contains(lf_res.standard_output, "\"file_sha256\": \""), lf_res.standard_output);
    require(contains(lf_res.standard_output, "\"hot_eligibility\": {\n    \"eligible\": true"), lf_res.standard_output);

    const Captured crlf_res = launch(o, {"check", "--file-only", "--json", "--variant", "fast", shown_path(crlf)});
    expect_code(crlf_res, 0, "check --file-only --json crlf.yaml");
    require(lf_res.standard_output != crlf_res.standard_output, "LF and CRLF file_sha256 must differ");

    // 2. check --json without --file-only also includes file_sha256 and hot_eligibility.
    const Captured plain = launch(o, {"check", "--json", "--variant", "fast", shown_path(lf)});
    expect_code(plain, 1, "check --json without --file-only");
    require(contains(plain.standard_output, "\"file_sha256\": \""), plain.standard_output);
    require(contains(plain.standard_output, "\"hot_eligibility\": {\n    \"eligible\": true"), plain.standard_output);

    // 3. check --file-only --json with fixed registry_uri and manual participant reports ineligible.
    const fs::path fixed = dir / "fixed.yaml";
    write_file(fixed,
               "vehicle_sil_simulation: 2\n"
               "registry_uri: silkit://127.0.0.1:8500\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./does_not_exist_ecu\"]\n");
    const Captured fixed_res = launch(o, {"check", "--file-only", "--json", shown_path(fixed)});
    expect_code(fixed_res, 0, "check --file-only --json fixed.yaml");
    require(contains(fixed_res.standard_output, "\"reason\": \"fixed-registry\""), fixed_res.standard_output);

    const fs::path manual = dir / "manual.yaml";
    write_file(manual,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./does_not_exist_ecu\"]\n    manual: true\n");
    const Captured manual_res = launch(o, {"check", "--file-only", "--json", shown_path(manual)});
    expect_code(manual_res, 0, "check --file-only --json manual.yaml");
    require(contains(manual_res.standard_output, "\"reason\": \"manual-participants\""), manual_res.standard_output);

    // 4. check --file-only --json preserves file_sha256 and hot_eligibility on usage error (exit 4).
    const Captured bad_var = launch(o, {"check", "--file-only", "--json", "--variant", "nope", shown_path(lf)});
    expect_code(bad_var, 4, "check --file-only --json --variant nope");
    require(contains(bad_var.standard_output, "\"result\": \"usage\""), bad_var.standard_output);
    require(contains(bad_var.standard_output, "\"file_sha256\": \""), bad_var.standard_output);
    require(contains(bad_var.standard_output, "\"reason\": \"usage\""), bad_var.standard_output);

    // 5. check --file-only --text-from-stdin and run/test/serve --file-only are refused (exit 4).
    const Captured stdin_ref = launch(o, {"check", "--file-only", "--text-from-stdin", shown_path(lf)});
    expect_code(stdin_ref, 4, "check --file-only --text-from-stdin");
    require(contains(stdin_ref.standard_error, "--file-only cannot be used with --text-from-stdin"), stdin_ref.standard_error);

    fs::create_directories(dir / "scenarios");
    for (const auto& [args, expected_err] : std::vector<std::pair<std::vector<std::string>, std::string>>{
             {{"run", "--file-only", shown_path(lf)}, "--file-only is for check"},
             {{"test", "--file-only", shown_path(lf), "--scenarios", shown_path(dir / "scenarios")}, "--file-only is for check"},
             {{"serve", "--file-only", shown_path(lf)}, "--file-only is not for serve"}})
    {
        const Captured refused = launch(o, args);
        expect_code(refused, 4, args.front() + " --file-only");
        require(contains(refused.standard_error, expected_err), refused.standard_error);
        if (args.front() == "serve")
        {
            require(contains(refused.standard_output, "\"event\":\"hello\""), refused.standard_output);
            require(contains(refused.standard_output, "\"launcher_ends\":true"), refused.standard_output);
        }
    }
    log("CHECK check --file-only --json and refusals -> ok");
}

int main(int argc, char** argv)
{
    set_log_tag("launch-cli");
    try
    {
        vehicle_sil::test::check_timeout_scale();
        const Options o = parse(argc, argv);
        fs::remove_all(o.work_dir);
        fs::create_directories(o.work_dir);
        log("scenario " + o.scenario);
        if (o.scenario == "docs")
        {
            docs(o);
        }
        else if (o.scenario == "describe")
        {
            describe(o);
        }
        else if (o.scenario == "strict")
        {
            strict(o);
        }
        else if (o.scenario == "unreadable")
        {
            unreadable(o);
        }
        else if (o.scenario == "usage")
        {
            usage(o);
        }
        else if (o.scenario == "enabled")
        {
            enabled(o);
        }
        else if (o.scenario == "request")
        {
            request(o);
        }
        else if (o.scenario == "connect")
        {
            connect(o);
        }
        else if (o.scenario == "connections")
        {
            connections(o);
        }
        else if (o.scenario == "descriptions")
        {
            descriptions(o);
        }
        else if (o.scenario == "compare")
        {
            compare(o);
        }
        else if (o.scenario == "file_only")
        {
            file_only(o);
        }
        else
        {
            throw std::invalid_argument("unknown scenario " + o.scenario);
        }
        log("PASS");
        return 0;
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL: ") + e.what());
        return 1;
    }
}
