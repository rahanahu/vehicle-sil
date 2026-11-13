// SPDX-License-Identifier: MIT
//
// The values that vehicle_sil_launch, ecu_main, the other tools and the GUI (its server in Python,
// its client and its pages in TypeScript) each write down: the longest duration in milliseconds,
// the range of a speed, the exit codes. The C++ ones are compared at build time; the GUI's are read
// from its sources, so that changing one side without the other fails here, with the file to fix.
#include "launch_test_util.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/ecu/executable.hpp"
#include "vehicle_sil/launch/config.hpp"
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
static_assert(cli::Args::kMaxMillis == static_cast<std::uint64_t>(launch::kMaxMillis));
static_assert(launch::kMaxMillis == ecu_executable::kMaxMillis);

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
    const auto max_millis = static_cast<double>(launch::kMaxMillis);
    // The GUI server checks what it passes to the launcher (launcher.py) and tells the pages
    // (api/app's limits).
    const std::string launcher = "gui/server/src/vehicle_sil_gui/launcher.py";
    same(value_in(launcher, "\nSPEED_MIN =", "\n#"), launch::kMinSpeed, launcher + " SPEED_MIN", "launch::kMinSpeed");
    same(value_in(launcher, "\nSPEED_MAX =", "\n#"), launch::kMaxSpeed, launcher + " SPEED_MAX", "launch::kMaxSpeed");
    same(value_in(launcher, "\nSTEP_MS_MAX =", "\n#"), max_millis, launcher + " STEP_MS_MAX", "launch::kMaxMillis");
    const std::string variables = "gui/server/src/vehicle_sil_gui/variables.py";
    same(value_in(variables, "\nAT_MS_MAX =", "\n#"), max_millis, variables + " AT_MS_MAX", "launch::kMaxMillis");
    // The client's values before the server's answer, and those it takes from an older server.
    const std::string client = "gui/client/src/client.ts";
    const std::string limits = "export const DEFAULT_LIMITS";
    same(value_in(client, "speed_min:", ",\n", limits), launch::kMinSpeed, client + " speed_min", "launch::kMinSpeed");
    same(value_in(client, "speed_max:", ",\n", limits), launch::kMaxSpeed, client + " speed_max", "launch::kMaxSpeed");
    same(value_in(client, "step_ms_max:", ",\n", limits), max_millis, client + " step_ms_max", "launch::kMaxMillis");
    same(value_in(client, "write_at_ms_max:", ",\n", limits), max_millis, client + " write_at_ms_max", "launch::kMaxMillis");
    const std::string protocol = "gui/client/src/protocol.ts";
    same(value_in(protocol, "'step_ms_max', optional(number,", ")"), max_millis, protocol + " step_ms_max",
         "launch::kMaxMillis");
    same(value_in(protocol, "'write_at_ms_max', optional(number,", ")"), max_millis, protocol + " write_at_ms_max",
         "launch::kMaxMillis");
    // The speed picker's range before the page has the server's limits.
    const std::string picker = "gui/web/src/app/run/speed-picker.ts";
    same(value_in(picker, "readonly min = input(", ")"), launch::kMinSpeed, picker + " min", "launch::kMinSpeed");
    same(value_in(picker, "readonly max = input(", ")"), launch::kMaxSpeed, picker + " max", "launch::kMaxSpeed");
}
