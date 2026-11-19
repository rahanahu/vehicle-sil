// SPDX-License-Identifier: MIT
#include "ecu_main_command_line.hpp"

#include "ecu_main_text.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/ecu/executable.hpp"
#include "vehicle_sil/ecu/j1939_node_argument.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h.
#include <shellapi.h>
#endif

namespace vehicle_sil::ecu_main_detail {

namespace {

// The longest --run-for-ms and --start-timeout-ms, as vehicle_sil_launch knows it
// (vehicle_sil/ecu/executable.hpp).
constexpr auto kMaxMillis = static_cast<std::uint64_t>(ecu_executable::kMaxMillis);

/// The options that take a value.
constexpr std::string_view kValueOptions[] = {"--registry-uri", "--name", "--run-for-ms", "--start-timeout-ms",
                                              "--silkit-config", ecu_executable::kMapNetworkArgument,
                                              ecu_executable::kJ1939NodeArgument};

/// An example of --map-network with what it means, for the messages.
constexpr const char* kMapNetworkExample =
    "e.g. --map-network VehicleCAN=J1939_CAN: the network that the ECU's code names J1939_CAN is VehicleCAN in the "
    "simulation";

/// An example of the value of --j1939-node, for the usage (the messages about its value come with
/// their own, from vehicle_sil/ecu/j1939_node_argument.hpp).
constexpr const char* kJ1939NodeExample = "VehicleCAN=from:0x00,address:0x01,identity_number:2";

bool takes_value(std::string_view arg)
{
    for (const auto option : kValueOptions)
    {
        if (arg == option)
        {
            return true;
        }
    }
    return false;
}

/// "10 ms", "500 us" or "250 ns".
std::string duration_text(SimDuration d)
{
    using namespace std::chrono;
    if (d.count() % duration_cast<SimDuration>(milliseconds(1)).count() == 0)
    {
        return std::to_string(duration_cast<milliseconds>(d).count()) + " ms";
    }
    if (d.count() % duration_cast<SimDuration>(microseconds(1)).count() == 0)
    {
        return std::to_string(duration_cast<microseconds>(d).count()) + " us";
    }
    return std::to_string(duration_cast<nanoseconds>(d).count()) + " ns";
}

std::chrono::milliseconds parse_millis(std::string_view option, std::string_view text)
{
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > kMaxMillis)
    {
        throw UsageError(std::string(option) + " needs a whole number of milliseconds from 1 to " +
                         std::to_string(kMaxMillis) + ", not " + in_quotes(text) + " (e.g. " + std::string(option) +
                         " 1000)");
    }
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(value));
}

bool is_help(std::string_view arg) { return arg == "--help" || arg == "-h"; }

} // namespace

std::string program_name(int argc, const char* const* argv, const std::string& fallback)
{
    if (argc < 1 || argv == nullptr || argv[0] == nullptr || *argv[0] == '\0')
    {
        return fallback;
    }
    const std::string_view path = argv[0];
    const auto slash = path.find_last_of("/\\");
    const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return file.empty() ? fallback : std::string(file);
}

std::string usage(const std::string& program, const EcuMainOptions& options)
{
    const std::string pad(program.size(), ' ');
    return "usage: " + program + " --describe [--name <name>] [--silkit-config <file>]\n"
           "       " + pad + " --registry-uri <uri> [--start-timeout-ms <ms>] [--no-control] [--name <name>]\n"
           "       " + pad + "                      [--silkit-config <file>]\n"
           "       " + pad + "                      [--map-network <simulation's name>=<executable's name>]...\n"
           "       " + pad + "                      [--j1939-node <network>=<setting>:<value>[,...]]...\n"
           "       " + pad + "                      [--sample-every-step]\n"
           "       " + pad + " --local --run-for-ms <ms> [--name <name>]\n"
           "       " + pad + " --help\n"
           "the ECU " + in_quotes(options.name) + ", step " + duration_text(options.step) + "\n"
           "  --describe              write what the ECU is (networks, J1939 nodes, variables) as JSON to\n"
           "                          stdout and exit, without connecting or running anything\n"
           "  --registry-uri <uri>    join the SIL Kit simulation at <uri> and run until it ends; a controller\n"
           "                          elsewhere (vehicle_sil_sim_controller) starts and stops it\n"
           "  --start-timeout-ms <ms> with --registry-uri: how long to wait for the start (default 30000)\n"
           "  --no-control            with --registry-uri: do not offer the ECU's variables to the simulation\n"
           "                          (vehicle_sil_launch then cannot read or write them); the ECU runs the same\n"
           "  --silkit-config <file>  with --registry-uri: a SIL Kit participant configuration (YAML or JSON)\n"
           "                          for this participant, e.g. Logging or Middleware; vehicle-sil's defaults\n"
           "                          fill in what it leaves out. With --describe: only checked\n"
           "  --map-network <s>=<e>   with --registry-uri, any number of times: the network that this ECU's code\n"
           "                          names <e> is the network <s> of the simulation (split at the first '=').\n"
           "                          Only the SIL Kit network changes; the code and --describe keep <e>\n"
           "  --j1939-node <n>=<s>    with --registry-uri, any number of times: change the address and NAME\n"
           "                          fields of a J1939 node on the simulation's network <n>; <s> is\n"
           "                          from:<address in the code>, address:<a>, identity_number:<n>,\n"
           "                          function_instance:<n>, ecu_instance:<n>, separated by commas\n"
           "                          (e.g. " + std::string(kJ1939NodeExample) + ")\n"
           "  --sample-every-step     with --registry-uri: send the values of the ECU's variables after every\n"
           "                          step of the ECU, whatever its code says (sample_period); vehicle_sil_launch\n"
           "                          gives it to the ECUs whose values a scenario reads\n"
           "  --local                 run this ECU alone on local virtual buses, for --run-for-ms\n"
           "  --run-for-ms <ms>       how long --local runs (virtual time, > 0)\n"
           "  --name <name>           the participant name instead of " + in_quotes(options.name) + "; only the name\n"
           "                          changes (not the J1939 NAME, the addresses or the variables)\n"
           "  --help                  write this text to stdout and exit\n"
           "exit codes: 0 done, 1 the ECU failed in the run, 3 the set-up failed (the ECU's set-up, the\n"
           "            registry, the start, writing to stdout), 4 a bad command line\n";
}

bool asks_for_help(const std::vector<std::string_view>& args)
{
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        if (takes_value(args[i]) && i + 1 < args.size() && args[i + 1].substr(0, 2) != "--")
        {
            ++i;
        }
        else if (is_help(args[i]))
        {
            return true;
        }
    }
    return false;
}

CommandLine parse(const std::vector<std::string_view>& args)
{
    CommandLine c;
    std::vector<std::string_view> seen;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string_view arg = args[i];
        for (const auto& before : seen)
        {
            // Those two: once for each network (or node); their values say what is given twice.
            if (before == arg && arg != ecu_executable::kMapNetworkArgument && arg != ecu_executable::kJ1939NodeArgument)
            {
                throw UsageError(in_quotes(arg) + " is given twice; give it once");
            }
        }
        // The value after `arg`; an option there is a missing value, not the value.
        const auto value = [&](const char* what, const char* example) {
            if (i + 1 >= args.size() || args[i + 1].substr(0, 2) == "--")
            {
                throw UsageError(std::string(arg) + " needs " + what + " after it (e.g. " + std::string(arg) + " " +
                                 example + ")" +
                                 (i + 1 < args.size() ? ", not the option " + in_quotes(args[i + 1]) : std::string()));
            }
            return std::string(args[++i]);
        };
        if (arg == "--describe")
        {
            c.describe = true;
        }
        else if (arg == "--local")
        {
            c.local = true;
        }
        else if (arg == "--no-control")
        {
            c.no_control = true; // only --registry-uri has anything to leave out; accepted with every mode
        }
        else if (arg == ecu_executable::kSampleEveryStepArgument)
        {
            c.sample_every_step = true;
        }
        else if (arg == "--registry-uri")
        {
            c.registry_uri = value("the registry's URI", "silkit://localhost:8500");
            if (c.registry_uri->empty())
            {
                throw UsageError("--registry-uri needs a URI that is not empty (e.g. --registry-uri silkit://localhost:8500)");
            }
        }
        else if (arg == "--name")
        {
            c.name = value("a name", "Engine2");
            if (c.name->empty())
            {
                throw UsageError("--name needs a name that is not empty (e.g. --name Engine2)");
            }
            if (!detail::is_utf8(*c.name))
            {
                throw UsageError("--name needs ASCII or UTF-8 text, not the bytes " + hex_bytes(*c.name) +
                                 " (a name in another encoding, e.g. a Windows code page)");
            }
        }
        else if (arg == "--silkit-config")
        {
            c.silkit_config = value("the path of a SIL Kit participant configuration file", "engine.silkit.yaml");
            if (c.silkit_config->empty())
            {
                throw UsageError("--silkit-config needs a path that is not empty (e.g. --silkit-config engine.silkit.yaml)");
            }
        }
        else if (arg == ecu_executable::kMapNetworkArgument)
        {
            const std::string text = value("<simulation's name>=<executable's name>", "VehicleCAN=J1939_CAN");
            if (!detail::is_utf8(text))
            {
                throw UsageError("--map-network needs ASCII or UTF-8 text, not the bytes " + hex_bytes(text) +
                                 " (names in another encoding, e.g. a Windows code page)");
            }
            const auto split = text.find('=');
            if (split == std::string::npos)
            {
                throw UsageError("--map-network needs <simulation's name>=<executable's name>, not " + in_quotes(text) +
                                 " (" + kMapNetworkExample + ")");
            }
            std::string simulation = text.substr(0, split);
            std::string executable = text.substr(split + 1);
            if (simulation.empty() || executable.empty())
            {
                throw UsageError("--map-network " + in_quotes(text) + " has an empty name: it needs the simulation's name "
                                 "before the first '=' and the executable's name after it (" + kMapNetworkExample + ")");
            }
            for (const auto& [otherSimulation, otherExecutable] : c.network_map)
            {
                if (otherSimulation == simulation)
                {
                    throw UsageError("--map-network gives the simulation's network " + in_quotes(simulation) +
                                     " twice (" + in_quotes(otherSimulation + "=" + otherExecutable) + " and " +
                                     in_quotes(text) + "): one network of the simulation is one network of the ECU; "
                                     "keep one of the two");
                }
                if (otherExecutable == executable)
                {
                    throw UsageError("--map-network gives the executable's network " + in_quotes(executable) +
                                     " two names in the simulation (" + in_quotes(otherSimulation + "=" + otherExecutable) +
                                     " and " + in_quotes(text) + "): a network of an ECU is on one bus; keep one of the two");
                }
            }
            c.network_map.emplace_back(std::move(simulation), std::move(executable));
        }
        else if (arg == ecu_executable::kJ1939NodeArgument)
        {
            if (i + 1 >= args.size() || args[i + 1].substr(0, 2) == "--")
            {
                throw UsageError(ecu_executable::j1939_node_argument_missing() +
                                 (i + 1 < args.size() ? ", not the option " + in_quotes(args[i + 1]) : std::string()));
            }
            const ecu_executable::J1939NodeArgument read = ecu_executable::parse_j1939_node_argument(args[++i]);
            if (!read.entry)
            {
                throw UsageError(read.error);
            }
            c.j1939_nodes.push_back(*read.entry);
        }
        else if (arg == "--start-timeout-ms")
        {
            c.start_timeout = parse_millis(arg, value("a duration in milliseconds", "60000"));
        }
        else if (arg == "--run-for-ms")
        {
            c.run_for = parse_millis(arg, value("a duration in milliseconds", "1000"));
        }
        else
        {
            std::string hint = "see the usage below";
            for (const auto known : kValueOptions)
            {
                if (arg.size() > known.size() && arg.substr(0, known.size()) == known && arg[known.size()] == '=')
                {
                    hint = "write the value after a space: " + std::string(known) + " " +
                           std::string(arg.substr(known.size() + 1));
                }
            }
            throw UsageError("unknown argument " + in_quotes(arg) + " (" + hint + ")");
        }
        seen.push_back(arg);
    }
    const int modes = (c.describe ? 1 : 0) + (c.local ? 1 : 0) + (c.registry_uri ? 1 : 0);
    if (modes == 0)
    {
        throw UsageError(args.empty() ? "no arguments: give --describe, --registry-uri <uri> or --local --run-for-ms <ms>"
                                      : "nothing to do: give --describe, --registry-uri <uri> or --local --run-for-ms <ms>");
    }
    if (modes > 1)
    {
        throw UsageError("--describe, --registry-uri and --local exclude each other: give one of them");
    }
    if (c.local && !c.run_for)
    {
        throw UsageError("--local needs --run-for-ms <ms>, how much virtual time to run (e.g. --local --run-for-ms 1000)");
    }
    if (c.run_for && !c.local)
    {
        throw UsageError(c.describe ? "--run-for-ms is for --local: --describe runs nothing"
                                    : "--run-for-ms is for --local: with --registry-uri, the controller ends the run "
                                      "(vehicle_sil_sim_controller --stop-at-ms)");
    }
    if (c.start_timeout && !c.registry_uri)
    {
        throw UsageError("--start-timeout-ms is for --registry-uri: " +
                         std::string(c.describe ? "--describe" : "--local") + " starts nothing to wait for");
    }
    if (!c.network_map.empty() && !c.registry_uri)
    {
        throw UsageError(c.describe
                             ? "--map-network is for --registry-uri: --describe says what the ECU's code names its "
                               "networks, whatever a simulation calls them; leave --map-network out"
                             : "--map-network is for --registry-uri: --local runs the ECU alone on local virtual buses, "
                               "where there is no other name for a network; leave --map-network out");
    }
    if (!c.j1939_nodes.empty() && !c.registry_uri)
    {
        throw UsageError(c.describe
                             ? "--j1939-node is for --registry-uri: --describe says the addresses and NAMEs in the ECU's "
                               "code, whatever a simulation changes them to; leave --j1939-node out"
                             : "--j1939-node is for --registry-uri: --local runs the ECU alone on local virtual buses, "
                               "where no other ECU needs another address; leave --j1939-node out");
    }
    if (c.sample_every_step && !c.registry_uri)
    {
        throw UsageError(c.describe
                             ? "--sample-every-step is for --registry-uri: --describe runs nothing, so there are no values "
                               "to sample; leave --sample-every-step out"
                             : "--sample-every-step is for --registry-uri: --local runs the ECU alone, and nothing reads "
                               "its variables from outside; leave --sample-every-step out");
    }
    if (const std::string error = ecu_executable::j1939_node_arguments_error(c.j1939_nodes); !error.empty())
    {
        throw UsageError(error);
    }
    if (c.silkit_config && c.local)
    {
        throw UsageError("--silkit-config is for --registry-uri: --local runs the ECU on local virtual buses, without "
                         "SIL Kit; leave --silkit-config out");
    }
    return c;
}

#ifdef _WIN32
std::optional<std::string> value_not_ascii(int argc, const char* const* argv, std::wstring_view option)
{
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr)
    {
        return std::nullopt;
    }
    const auto is_ascii = [](std::wstring_view text) {
        return std::all_of(text.begin(), text.end(), [](wchar_t c) { return c < 0x80; });
    };
    bool same = argv != nullptr && count == argc;
    for (int i = 1; same && i < count; ++i)
    {
        const std::wstring_view w = wide[i];
        if (argv[i] == nullptr)
        {
            same = false;
        }
        else if (is_ascii(w))
        {
            const std::string_view a = argv[i];
            same = a.size() == w.size() &&
                   std::equal(a.begin(), a.end(), w.begin(), [](char x, wchar_t y) { return static_cast<wchar_t>(static_cast<unsigned char>(x)) == y; });
        }
    }
    std::optional<std::string> found;
    for (int i = 1; same && !found && i + 1 < count; ++i)
    {
        if (std::wstring_view(wide[i]) == option)
        {
            const std::wstring_view value = wide[++i];
            if (!is_ascii(value))
            {
                const int length = static_cast<int>(value.size());
                const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), length, nullptr, 0, nullptr, nullptr);
                std::string text(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
                if (size > 0)
                {
                    WideCharToMultiByte(CP_UTF8, 0, value.data(), length, text.data(), size, nullptr, nullptr);
                }
                found = std::move(text);
            }
        }
    }
    LocalFree(wide);
    return found;
}
#endif

} // namespace vehicle_sil::ecu_main_detail
