// SPDX-License-Identifier: MIT
//
// The values that vehicle_sil_launch, ecu_main, the other tools and the GUI (its server in Python,
// its client and its pages in TypeScript) each write down: the longest duration in milliseconds,
// the range of a speed, the exit codes. The C++ ones are compared at build time; the GUI's are read
// from its sources, so that changing one side without the other fails here, with the file to fix.
#include "launch_test_util.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/ecu/executable.hpp"
#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>

using namespace vehicle_sil;

// The tools take the same exit codes and longest duration as ecu_main and the launcher.
static_assert(cli::kExitOk == ecu_executable::kExitOk && cli::kExitFailed == ecu_executable::kExitFailed &&
              cli::kExitSetupFailed == ecu_executable::kExitSetupFailed && cli::kExitUsage == ecu_executable::kExitUsage);
static_assert(launch::kExitInterrupted == ecu_executable::kExitInterrupted);
static_assert(cli::Args::kMaxMillis == static_cast<std::uint64_t>(config::kMaxMillis));
static_assert(config::kMaxMillis == ecu_executable::kMaxMillis);
static_assert(launch::kMaxRecordAfterFailMs == config::kMaxRecordAfterFailMs);

namespace {

const std::filesystem::path kSource = VEHICLE_SIL_SOURCE_DIR;

std::string read_source(const std::string& relative)
{
    std::ifstream in(kSource / relative, std::ios::binary);
    if (!in)
    {
        throw unit::Failure("cannot read " + relative + " (the source tree: " + kSource.string() + ")");
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// A number as Python or TypeScript writes a constant: "0.000001", "1e12", "60_000", "10 ** 12".
std::optional<double> number(std::string text)
{
    std::erase(text, '_');
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r'))
    {
        text.pop_back();
    }
    if (const auto power = text.find("**"); power != std::string::npos)
    {
        const auto base = number(text.substr(0, power));
        const auto exponent = number(text.substr(power + 2));
        if (!base || !exponent)
        {
            return std::nullopt;
        }
        double out = 1;
        for (int i = 0; i < static_cast<int>(*exponent); ++i)
        {
            out *= *base;
        }
        return out;
    }
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos)
    {
        return std::nullopt;
    }
    double out = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data() + first, end, out);
    return ec == std::errc{} && ptr == end ? std::optional<double>(out) : std::nullopt;
}

/// The number after `before` in the file `relative`, up to `until`; with `after`, inside the
/// object that starts there (up to its first "};").
double value_in(const std::string& relative, const std::string& before, const std::string& until,
                const std::string& after = {})
{
    const std::string text = read_source(relative);
    std::size_t from = 0;
    std::size_t to = text.size();
    if (!after.empty())
    {
        from = text.find(after);
        if (from == std::string::npos)
        {
            throw unit::Failure(relative + ": \"" + after + "\" is not there any more; change this test with it");
        }
        to = std::min(text.find("};", from), text.size());
    }
    const std::size_t at = text.find(before, from);
    if (at == std::string::npos || at >= to)
    {
        throw unit::Failure(relative + ": \"" + before + "\" is not there any more" +
                            (after.empty() ? std::string() : " (in " + after + ")") + "; change this test with it");
    }
    const std::size_t start = at + before.size();
    const std::size_t end = text.find_first_of(until, start);
    const auto out = number(text.substr(start, end - start));
    if (!out)
    {
        throw unit::Failure(relative + ": the value after \"" + before + "\" is not a number this test reads: " +
                            text.substr(start, end - start));
    }
    return *out;
}

/// `actual` (in `where`) must be `expected` (the C++ value `name`).
void same(double actual, double expected, const std::string& where, const std::string& name)
{
    if (actual != expected)
    {
        throw unit::Failure(where + " is " + std::to_string(actual) + ", but " + name + " is " + std::to_string(expected) +
                            ": make them the same (one of them changed without the other)");
    }
}

} // namespace

TEST_CASE(the_gui_takes_the_launchers_limits)
{
    const auto max_millis = static_cast<double>(config::kMaxMillis);
    // The GUI server checks what it passes to the launcher (vehicle_sil_control/protocol.py, re-exported by
    // launcher.py) and tells the pages (api/app's limits).
    const std::string control_protocol = "python/vehicle_sil_control/protocol.py";
    same(value_in(control_protocol, "\nSPEED_MIN: float =", "\n#"), config::kMinSpeed,
         control_protocol + " SPEED_MIN", "launch::kMinSpeed");
    same(value_in(control_protocol, "\nSPEED_MAX: float =", "\n#"), config::kMaxSpeed,
         control_protocol + " SPEED_MAX", "launch::kMaxSpeed");
    same(value_in(control_protocol, "\nSTEP_MS_MAX: int =", "\n#"), max_millis,
         control_protocol + " STEP_MS_MAX", "launch::kMaxMillis");
    const auto max_record_after_fail = static_cast<double>(config::kMaxRecordAfterFailMs);
    same(value_in(control_protocol, "\nRECORD_AFTER_FAIL_MS_MAX: int =", "\n#"), max_record_after_fail,
         control_protocol + " RECORD_AFTER_FAIL_MS_MAX", "config::kMaxRecordAfterFailMs");
    const std::string variables = "gui/server/src/vehicle_sil_gui/variables.py";
    same(value_in(variables, "\nAT_MS_MAX =", "\n#"), max_millis, variables + " AT_MS_MAX", "launch::kMaxMillis");
    // The client's values before the server's answer, and those it takes from an older server.
    const std::string client = "gui/client/src/client.ts";
    const std::string limits = "export const DEFAULT_LIMITS";
    same(value_in(client, "speed_min:", ",\n", limits), config::kMinSpeed, client + " speed_min", "launch::kMinSpeed");
    same(value_in(client, "speed_max:", ",\n", limits), config::kMaxSpeed, client + " speed_max", "launch::kMaxSpeed");
    same(value_in(client, "step_ms_max:", ",\n", limits), max_millis, client + " step_ms_max", "launch::kMaxMillis");
    same(value_in(client, "write_at_ms_max:", ",\n", limits), max_millis, client + " write_at_ms_max", "launch::kMaxMillis");
    const std::string protocol = "gui/client/src/protocol/server.ts";
    same(value_in(protocol, "'step_ms_max', optional(number,", ")"), max_millis, protocol + " step_ms_max",
         "launch::kMaxMillis");
    same(value_in(protocol, "'write_at_ms_max', optional(number,", ")"), max_millis, protocol + " write_at_ms_max",
         "launch::kMaxMillis");
    // The speed picker's range before the page has the server's limits.
    const std::string picker = "gui/web/src/app/run/speed-picker.ts";
    same(value_in(picker, "readonly min = input(", ")"), config::kMinSpeed, picker + " min", "launch::kMinSpeed");
    same(value_in(picker, "readonly max = input(", ")"), config::kMaxSpeed, picker + " max", "launch::kMaxSpeed");
    const std::string run_page = "gui/web/src/app/run/run-page.ts";
    same(value_in(run_page, "\nconst RECORD_AFTER_FAIL_MS_MAX =", ";\n"), max_record_after_fail,
         run_page + " RECORD_AFTER_FAIL_MS_MAX", "config::kMaxRecordAfterFailMs");
}

TEST_CASE(the_gui_reads_the_launchers_manual_stays_line)
{
    // The standard GUI shows that a participant the user started has not left, by the start of the
    // launcher's log line (after the name): the text it looks for must be the launcher's.
    const std::string page = "gui/web/src/app/run/run-page.ts";
    const std::string text = read_source(page);
    const std::string before = "const MANUAL_STAYS = '";
    const std::size_t at = text.find(before);
    if (at == std::string::npos)
    {
        throw unit::Failure(page + ": \"" + before + "\" is not there any more; change this test with it");
    }
    const std::size_t start = at + before.size();
    const std::size_t end = text.find('\'', start);
    const std::string gui = text.substr(start, end - start);
    // The launcher writes the line as <name> + this text + the rest (run_participants.cpp).
    const std::string writer = "libs/runner/src/run_participants.cpp";
    if (read_source(writer).find("\"" + gui) == std::string::npos)
    {
        throw unit::Failure(page + " looks for MANUAL_STAYS \"" + gui + "\", but " + writer +
                            " no longer writes a line with that text: make them the same (one of them changed "
                            "without the other)");
    }
}

TEST_CASE(the_gui_and_the_launcher_name_the_scenario_client_alike)
{
    // A scenario's writes carry one client name: the GUI leaves its own operations out of the
    // "in flight" count by that name, and the launcher sets it on the writes (docs/control-api.md).
    const std::string gui_file = "gui/server/src/vehicle_sil_gui/control.py";
    const std::string gui_text = read_source(gui_file);
    const std::string gui_before = "SCENARIO_CLIENT = \"";
    const std::size_t gui_at = gui_text.find(gui_before);
    if (gui_at == std::string::npos)
    {
        throw unit::Failure(gui_file + ": \"" + gui_before + "\" is not there any more; change this test with it");
    }
    const std::size_t gui_start = gui_at + gui_before.size();
    const std::string gui = gui_text.substr(gui_start, gui_text.find('"', gui_start) - gui_start);

    const std::string writer = "libs/runner/src/run_scenario.cpp";
    const std::string launcher_text = read_source(writer);
    const std::string launcher_before = "request.client = \"";
    const std::size_t launcher_at = launcher_text.find(launcher_before);
    if (launcher_at == std::string::npos)
    {
        throw unit::Failure(writer + ": \"" + launcher_before + "\" is not there any more; change this test with it");
    }
    const std::size_t launcher_start = launcher_at + launcher_before.size();
    const std::string launcher =
        launcher_text.substr(launcher_start, launcher_text.find('"', launcher_start) - launcher_start);
    if (gui != launcher)
    {
        throw unit::Failure(gui_file + " names the scenario's client \"" + gui + "\", but " + writer + " sets \"" +
                            launcher + "\": make them the same (one of them changed without the other)");
    }
}
