// SPDX-License-Identifier: MIT
//
// The command line of ecu_main() (vehicle_sil/simulation/ecu_main.hpp): reading the arguments
// and the usage text. Internal to the library; not installed, not part of the API.
#pragma once

#include "vehicle_sil/ecu/j1939_node_argument.hpp"
#include "vehicle_sil/simulation/ecu_main.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil::ecu_main_detail {

class UsageError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct CommandLine
{
    bool describe = false;
    bool local = false;
    bool no_control = false;
    bool sample_every_step = false; ///< --sample-every-step
    std::optional<std::string> registry_uri;
    std::optional<std::string> name;
    std::optional<std::string> silkit_config;
    std::optional<std::chrono::milliseconds> run_for;
    std::optional<std::chrono::milliseconds> start_timeout;
    /// --map-network, in the order given: the simulation's name and the executable's.
    std::vector<std::pair<std::string, std::string>> network_map;
    /// --j1939-node, in the order given.
    std::vector<ecu_executable::J1939NodeOverride> j1939_nodes;
};

/// The start timeout without --start-timeout-ms (SilKitOptions::start_timeout).
constexpr std::chrono::milliseconds kDefaultStartTimeout{30000};

/// The file name of argv[0] without its directories ("/" and "\"); `fallback` if there is none.
std::string program_name(int argc, const char* const* argv, const std::string& fallback);

/// The usage text (--help, and after a bad command line) of the ECU `options` named `program`.
std::string usage(const std::string& program, const EcuMainOptions& options);

/// true if `args` ask for the usage: --help or -h where an option goes, not as the value of an
/// option ("--name -h" names the ECU "-h"; a value that starts with "--" is a missing value).
bool asks_for_help(const std::vector<std::string_view>& args);

/// The command line `args` (without argv[0]); UsageError if it is not one that ecu_main() takes.
CommandLine parse(const std::vector<std::string_view>& args);

#ifdef _WIN32
/// The value of the first `option` (--map-network, --j1939-node) of this process's command line
/// that has a character that is not ASCII, as UTF-8. `argv` cannot tell: it is the command line in
/// the code page, where a character that the code page lacks has become '?' or a similar ASCII
/// letter, which would be the name of another network without any error. None if `argv` is not
/// this process's command line (a caller's own arguments: where the wide command line is ASCII,
/// the two must be the same).
std::optional<std::string> value_not_ascii(int argc, const char* const* argv, std::wstring_view option);
#endif

} // namespace vehicle_sil::ecu_main_detail
