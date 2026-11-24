// SPDX-License-Identifier: MIT
//
// The result of a command that did not get to run: the command line was wrong
// ("usage") or the launcher failed on its own ("internal"). For each command,
// the report that says so and the JSON or result event that writes it, so that
// a program that reads the output gets an answer in the form of the command.
// Pure functions; the caller writes the strings and decides the exit code (the
// table: exit_codes.hpp). Internal; the outputs: docs/simulation-run.md.
#pragma once

#include "vehicle_sil/launch/run.hpp"

#include <string>
#include <vector>

namespace vehicle_sil::launch {

/// The commands of vehicle_sil_launch that write a result (version has none).
enum class CommandKind {
  Check,
  Run,
  Test,
  Connections,
};

/// The JSON (result "usage") that a wrong command line gets: the form of
/// `command`'s output. `file`: the configuration file if the arguments name
/// one, else "". `variants`: the variants the file defines, if it was read
/// (check and run list what --variant takes).
[[nodiscard]] std::string
format_usage_json(CommandKind command, const std::string &file,
                  const std::vector<config::Finding> &errors,
                  const std::vector<std::string> &variants = {});

/// The same as the report of `serve`, whose result event says "usage".
[[nodiscard]] RunReport
usage_run_report(const std::string &file, const std::vector<config::Finding> &errors,
                 const std::vector<std::string> &variants = {});

/// The JSON of an error of the launcher itself (finding code "internal"), in
/// the form of `command`'s output.
[[nodiscard]] std::string
format_internal_error_json(CommandKind command, const std::string &file,
                           const std::string &message);

/// The same as the report of `serve`.
[[nodiscard]] RunReport internal_error_run_report(const std::string &message);

} // namespace vehicle_sil::launch
