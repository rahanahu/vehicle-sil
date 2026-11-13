// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch check as a process, with the ECU executables of examples/ecu_main and a
// helper that misbehaves (tests/process/process_helper.cpp):
//
//   docs        the configuration files and outputs of docs/simulation-config.md, byte for byte
//               but for the vehicle-sil version: the example's simulation.yaml (exit 0), the
//               file with errors (exit 1), the JSON (exit 1), the example without Dashboard
//   describe    executables that fail --describe: exit code 4, too slow, not JSON, another
//               format version, missing; one with describe: false (exit 1, the whole text)
//   strict      warnings only: exit 0, with --strict exit 1
//   unreadable  a missing file, a directory, a syntax error, another format version (exit 3,
//               also with --json)
//   usage       command lines that are wrong (exit 4, the message) and --help (exit 0)
//   enabled     a disabled participant whose executable does not exist (exit 0, --strict too),
//               --enable and --disable, and the names they must have (exit 4)
#include "launcher_support.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
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
    fs::path docs;
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

/// The fenced block after the line `marker` in the Markdown file, without the fences.
std::string block_after(const fs::path& path, const std::string& marker)
{
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
/// there is the block after the same line in `docs` (which this test compares with the output) or,
/// for a check of a scenario, in scenario.md (which launch_run.scenario compares).
void howto_copies_agree(const fs::path& docs, const std::string& command)
{
    const std::string head = "<!-- vehicle_sil_launch " + command + " ";
    const fs::path howto = docs.parent_path() / "howto";
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
                const fs::path scenario = docs.parent_path() / "scenario.md";
                const bool in_docs = lf(read_file(docs)).find(line + "\n") != std::string::npos;
                const fs::path& original = in_docs ? docs : scenario;
                expect_text(block_after(entry.path(), line), block_after(original, line),
                            "docs/howto/" + entry.path().filename().string() + ": the copy after " + line +
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

    const fs::path example = o.example_dir / "simulation.yaml";
    const Captured passed = launch(o, {"check", shown_path(example)});
    expect_code(passed, 0, "check simulation.yaml");
    expect_text(shown_as(passed.standard_output, example, "simulation.yaml"),
                block_after(o.docs, "<!-- vehicle_sil_launch check simulation.yaml -->"), "check simulation.yaml");
    expect_text(passed.standard_error, "", "check simulation.yaml: stderr");

    const fs::path twoEngines = o.example_dir / "two-engines.yaml";
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

    const fs::path alone = o.example_dir / "engine-alone.yaml";
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
    // A build with the control server lists two more features, last; the docs show a build without.
    std::string features = version.standard_output;
    replace_all(features, ",\n    \"control-server\",\n    \"scenario-control\"\n", "\n");
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
    write_file(config, "vehicle_sil_simulation: 1\n"
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
                       "\"name\": \"Newer\", \"step_ns\": null, \"networks\": [{\"name\": \"Eth\", \"kind\": \"ethernet\", "
                       "\"use\": \"ethernet\", \"nodes\": []}]}']\n"
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
                "--name: if it is not made with ecu_main, set describe: false\n"
                "    the end of its standard error:\n"
                "    error: Usage: unknown argument --describe\n"
                "error: <dir>/failing.yaml:7: participant 'Chatty': <helper> --out 'hello\\n' --describe --name Chatty "
                "ended with exit code 0, but its output is not a description: the output is not a JSON object. Is it made "
                "with ecu_main, and does nothing else write to its standard output? If it is not made with ecu_main, set "
                "describe: false\n"
                "    the start of its standard output:\n"
                "    hello\n"
                "error: <dir>/failing.yaml:9: participant 'Future': <helper> --out '{\"vehicle_sil_ecu\": 2}' --describe "
                "--name Future wrote a description in format 2, but this vehicle_sil_launch (vehicle-sil <version>) reads "
                "format 1: the executable is of a newer vehicle-sil; use a vehicle_sil_launch of its vehicle-sil or a "
                "newer one\n"
                "error: <dir>/failing.yaml:11: participant 'Missing': the executable missing/ecu does not exist (looked for "
                "<dir>/missing/ecu): build it, or correct the path (a relative path is from the directory of the "
                "configuration file)\n"
                // A JSON argument passes byte for byte, "{{name}}" as "{name}" (also for an ecu_main executable).
                "error: <dir>/failing.yaml:16: participant 'Braces': <helper> --out '{\"vehicle_sil_ecu\": 2, \"x\": "
                "{\"y\": \"{name}\"}}' --describe "
                "--name Braces wrote a description in format 2, but this vehicle_sil_launch (vehicle-sil <version>) reads "
                "format 1: the executable is of a newer vehicle-sil; use a vehicle_sil_launch of its vehicle-sil or a "
                "newer one\n"
                // "{{odd}}" in the path is the directory "{odd}".
                "error: <dir>/failing.yaml:18: participant 'Escaped': the executable {{odd}}/missing does not exist "
                "(looked for <dir>/{odd}/missing): build it, or correct the path (a relative path is from the directory "
                "of the configuration file)\n"
                "error: <dir>/failing.yaml:21: participant 'Newer': <helper> --out '{\"vehicle_sil_ecu\": 1, "
                "\"vehicle_sil\": \"9.0.0\", \"name\": \"Newer\", \"step_ns\": null, \"networks\": [{\"name\": \"Eth\", "
                "\"kind\": \"ethernet\", \"use\": \"ethernet\", \"nodes\": []}]}' --describe --name Newer wrote "
                "networks[0].use \"ethernet\", which this vehicle_sil_launch does not know (it knows raw, j1939, "
                "lin_master and lin_slave): the executable was built with vehicle-sil 9.0.0, this vehicle_sil_launch with "
                "vehicle-sil <version>. A newer vehicle-sil may add such values without a new format: use a "
                "vehicle_sil_launch of the executable's vehicle-sil or a newer one\n"
                "note: <dir>/failing.yaml:12: participant 'Logger': not checked (describe: false): vehicle_sil_launch "
                "cannot compare its name, step and networks with this file\n"
                "\n"
                "failed: 7 errors, 0 warnings, 1 note\n",
                "check failing.yaml");

    // A short time limit only for the one that does not end; also one that writes without end.
    const fs::path slow = o.work_dir / "slow.yaml";
    write_file(slow, "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                     "  - name: Slow\n"
                     "    command: [" + helper + ", --err, \"waiting for the bus\\n\", --sleep-ms, \"60000\"]\n"
                     "  - name: Flood\n"
                     "    command: [" + helper + ", --flood]\n");
    const Captured slowRun = launch(o, {"check", shown_path(slow), "--describe-timeout-ms", "1000"});
    expect_code(slowRun, 1, "check slow.yaml");
    const std::string slowOut = normalized(o, slowRun.standard_output);
    require(contains(slowOut, "error: <dir>/slow.yaml:5: participant 'Slow': <helper> --err 'waiting for the bus\\n' "
                              "--sleep-ms 60000 --describe --name Slow did not end within 1000 ms and was stopped. "
                              "--describe only builds the ECU, so something in its set-up waits (a connection, input): fix "
                              "that, or give vehicle_sil_launch check a longer --describe-timeout-ms\n"
                              "    the end of its standard error:\n"
                              "    waiting for the bus\n") &&
                contains(slowOut, "error: <dir>/slow.yaml:7: participant 'Flood': <helper> --flood --describe --name Flood "
                                  "did not end within 1000 ms and was stopped.") &&
                contains(slowOut, "\nfailed: 2 errors, 0 warnings, 0 notes\n"),
            "check slow.yaml wrote:\n" + slowOut);
    log("CHECK executables that do not end are stopped -> ok");
}

void strict(const Options& o)
{
    const fs::path config = o.work_dir / "alone.yaml";
    write_file(config, "vehicle_sil_simulation: 1\nnetworks:\n  - name: J1939_CAN\n    kind: can\nparticipants:\n"
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
    write_file(config, "vehicle_sil_simulation: 1\nnetworks:\n  - name: J1939_CAN\n    kind: can\nparticipants:\n"
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
                "error: " + m + ": the file does not exist: check the path\n\ncannot read " + m + ": 1 error, 0 warnings, 0 notes\n",
                "check nope.yaml");
    const Captured noneJson = launch(o, {"check", m, "--json"});
    expect_code(noneJson, 3, "check nope.yaml --json");
    require(contains(noneJson.standard_output, "\n  \"result\": \"unreadable\",\n") &&
                contains(noneJson.standard_output, "\n      \"code\": \"file\",\n"),
            "check nope.yaml --json wrote:\n" + noneJson.standard_output);

    const Captured directory = launch(o, {"check", shown_path(o.work_dir)});
    expect_code(directory, 3, "check <directory>");
    require(contains(directory.standard_output, ": this is a directory: name the configuration file in it\n"),
            "check <directory> wrote:\n" + directory.standard_output);

#ifndef _WIN32
    const Captured device = launch(o, {"check", "/dev/null"});
    expect_code(device, 3, "check /dev/null");
    require(contains(device.standard_output, ": this is not a regular file (a device or a pipe?): name the configuration file\n"),
            "check /dev/null wrote:\n" + device.standard_output);
#endif
    const fs::path big = o.work_dir / "big.yaml";
    std::ofstream(big).close();
    fs::resize_file(big, (std::uintmax_t{16} << 20) + 1);
    const Captured huge = launch(o, {"check", shown_path(big)});
    expect_code(huge, 3, "check big.yaml");
    require(contains(huge.standard_output, ": the file is larger than 16 MiB, too large for a configuration: name the "
                                           "configuration file\n"),
            "check big.yaml wrote:\n" + huge.standard_output);

    const fs::path syntax = o.work_dir / "syntax.yaml";
    write_file(syntax, "vehicle_sil_simulation: 1\nnetworks: [a\nparticipants: []\n");
    const Captured bad = launch(o, {"check", shown_path(syntax)});
    expect_code(bad, 3, "check syntax.yaml");
    require(bad.standard_output.rfind("error: " + shown_path(syntax) + ":", 0) == 0 &&
                contains(bad.standard_output, ": syntax error: "),
            "check syntax.yaml wrote:\n" + bad.standard_output);

    const fs::path future = o.work_dir / "future.yaml";
    write_file(future, "vehicle_sil_simulation: 2\nsomething: new\n");
    const Captured later = launch(o, {"check", shown_path(future)});
    expect_code(later, 3, "check future.yaml");
    require(contains(later.standard_output, ":1: vehicle_sil_simulation is 2, but this vehicle_sil_launch reads format 1"),
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
    expect_usage({}, "no command: give check <file>, run <file> or version");
    expect_usage({"start", "x.yaml"}, "unknown command 'start': the commands are check, run and version");
    expect_usage({"check"}, "check needs the configuration file: vehicle_sil_launch check <file>");
    expect_usage({"check", "a.yaml", "b.yaml"}, "check takes one file, not 2");
    expect_usage({"check", "a.yaml", "--bogus"}, "unknown option --bogus");
    expect_usage({"check", "a.yaml", "--json", "--json"}, "--json is given twice");
    expect_usage({"check", "a.yaml", "--describe-timeout-ms"}, "missing value for --describe-timeout-ms");
    expect_usage({"check", "a.yaml", "--describe-timeout-ms", "0"}, "--describe-timeout-ms must be at least 1");
    expect_usage({"check", "a.yaml", "--describe-timeout-ms", "soon"}, "invalid value for --describe-timeout-ms: soon");
    expect_usage({"check", "a.yaml", "--events"}, "--events is for run: check starts no participant");
    expect_usage({"check", "a.yaml", "--stdin-commands"}, "--stdin-commands is for run: check starts no participant");
    for (const std::vector<std::string>& args : {std::vector<std::string>{"--help"}, {"check", "x.yaml", "-h"}})
    {
        const Captured help = launch(o, args);
        expect_code(help, 0, "vehicle_sil_launch --help");
        require(lf(help.standard_output).rfind("usage: vehicle_sil_launch check [--json]", 0) == 0 && help.standard_error.empty(),
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

void enabled(const Options& o)
{
    // Ghost's executable does not exist: disabled, it is not looked for (and not run).
    const fs::path config = o.work_dir / "ghost.yaml";
    write_file(config, "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
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
    require(contains(json.standard_output, "      \"required\": true,\n      \"enabled\": false,\n      \"manual\": false,\n      \"step_ms\": null,\n"
                                           "      \"status\": \"disabled\",\n      \"reported\": null,\n      \"variables\": null,\n      \"control\": null\n") &&
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
    write_file(broken, "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
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
    write_file(partly, "vehicle_sil_simulation: 1\nnetworks:\n  - name: J1939_CAN\n    kind: can\nparticipants:\n"
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

} // namespace

int main(int argc, char** argv)
{
    set_log_tag("launch-cli");
    try
    {
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
