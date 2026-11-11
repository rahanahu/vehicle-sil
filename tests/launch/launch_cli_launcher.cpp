// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch check as a process, with the ECU executables of examples/ecu_main and a
// helper that misbehaves (tests/process/process_helper.cpp):
//
//   docs        the configuration files and outputs of docs/simulation-config.md, byte for byte
//               but for the vehicle-sil version: the example's simulation.yaml (exit 0), the
//               file with errors (exit 1), the JSON (exit 1)
//   describe    executables that fail --describe: exit code 4, too slow, not JSON, another
//               format version, missing; one with describe: false (exit 1, the whole text)
//   strict      warnings only: exit 0, with --strict exit 1
//   unreadable  a missing file, a directory, a syntax error, another format version (exit 3,
//               also with --json)
//   usage       command lines that are wrong (exit 4, the message) and --help (exit 0)
#include "launcher_support.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
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

    const fs::path alone = o.example_dir / "engine-alone.yaml";
    write_file(alone, block_after(o.docs, "<!-- engine-alone.yaml -->"));
    const Captured json = launch(o, {"check", shown_path(alone), "--json"});
    expect_code(json, 1, "check engine-alone.yaml --json");
    // Byte for byte: "\n" also on Windows.
    expect_text(without_versions(shown_as(json.standard_output, alone, "engine-alone.yaml")),
                without_versions(block_after(o.docs, "<!-- vehicle_sil_launch check engine-alone.yaml --json -->")),
                "check engine-alone.yaml --json");
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
                       "    describe: false\n");
    const Captured run = launch(o, {"check", shown_path(config)});
    expect_code(run, 1, "check failing.yaml");
    expect_text(normalized(o, run.standard_output),
                "<dir>/failing.yaml: 7 participants, 0 networks\n"
                "\n"
                "participant  step  networks\n"
                "Usage        -     no description (see the errors)\n"
                "Chatty       -     no description (see the errors)\n"
                "Future       -     no description (see the errors)\n"
                "Missing      -     no description (see the errors)\n"
                "Logger       -     not checked (describe: false)\n"
                "Braces       -     no description (see the errors)\n"
                "Escaped      -     no description (see the errors)\n"
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
                "--name Future wrote a description in format 2, but this vehicle_sil_launch reads format 1: build the "
                "executable and vehicle_sil_launch from the same vehicle-sil\n"
                "error: <dir>/failing.yaml:11: participant 'Missing': the executable missing/ecu does not exist (looked for "
                "<dir>/missing/ecu): build it, or correct the path (a relative path is from the directory of the "
                "configuration file)\n"
                // A JSON argument passes byte for byte, "{{name}}" as "{name}" (also for an ecu_main executable).
                "error: <dir>/failing.yaml:16: participant 'Braces': <helper> --out '{\"vehicle_sil_ecu\": 2, \"x\": "
                "{\"y\": \"{name}\"}}' --describe "
                "--name Braces wrote a description in format 2, but this vehicle_sil_launch reads format 1: build the "
                "executable and vehicle_sil_launch from the same vehicle-sil\n"
                // "{{odd}}" in the path is the directory "{odd}".
                "error: <dir>/failing.yaml:18: participant 'Escaped': the executable {{odd}}/missing does not exist "
                "(looked for <dir>/{odd}/missing): build it, or correct the path (a relative path is from the directory "
                "of the configuration file)\n"
                "note: <dir>/failing.yaml:12: participant 'Logger': not checked (describe: false): vehicle_sil_launch "
                "cannot compare its name, step and networks with this file\n"
                "\n"
                "failed: 6 errors, 0 warnings, 1 note\n",
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
    expect_usage({}, "no command: give check <file> or run <file>");
    expect_usage({"start", "x.yaml"}, "unknown command 'start': the commands are check and run");
    expect_usage({"check"}, "check needs the configuration file: vehicle_sil_launch check <file>");
    expect_usage({"check", "a.yaml", "b.yaml"}, "check takes one file, not 2");
    expect_usage({"check", "a.yaml", "--bogus"}, "unknown option --bogus");
    expect_usage({"check", "a.yaml", "--json", "--json"}, "--json is given twice");
    expect_usage({"check", "a.yaml", "--describe-timeout-ms"}, "missing value for --describe-timeout-ms");
    expect_usage({"check", "a.yaml", "--describe-timeout-ms", "0"}, "--describe-timeout-ms must be at least 1");
    expect_usage({"check", "a.yaml", "--describe-timeout-ms", "soon"}, "invalid value for --describe-timeout-ms: soon");
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
    require(contains(json.standard_output, "\n  \"file\": null,\n  \"result\": \"usage\",\n") &&
                contains(json.standard_output, "\n      \"code\": \"usage\",\n") &&
                contains(json.standard_output, "\"message\": \"unknown option --bogus\""),
            "check --json --bogus wrote:\n" + json.standard_output);
    // After "--", a name that starts with '-' is the file.
    const Captured dashed = launch(o, {"check", "--", "-h"});
    expect_code(dashed, 3, "vehicle_sil_launch check -- -h");
    require(dashed.standard_output.rfind("error: -h: the file does not exist", 0) == 0,
            "check -- -h wrote:\n" + dashed.standard_output);
    log("CHECK usage errors: exit code 4 and the message; --help: exit code 0 -> ok");
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
