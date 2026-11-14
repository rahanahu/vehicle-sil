// SPDX-License-Identifier: MIT
//
// Helpers shared by the process-level test launchers: logging, log files, a sil-kit-registry
// process on a free port, and waiting for child processes against a deadline.
#pragma once

#include "vehicle_sil/process/child_process.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace vehicle_sil::test {

using process::ChildProcess;

/// Sets the tag of log() lines, e.g. "launcher" -> "[launcher] text".
void set_log_tag(std::string tag);

/// Prints "[tag] text" to stdout and flushes.
void log(const std::string& text);

/// Reads the factor on the upper bounds of waiting (test_timeout_scale.hpp) and logs a warning when
/// it is larger than the one the test's CTest TIMEOUT was made for. Each launcher calls it first, so
/// that a wrong value fails every scenario (std::invalid_argument, saying what to set).
void check_timeout_scale();

/// Logs "CHECK what -> ok|MISMATCH" and returns `condition`.
bool expect(bool condition, const std::string& what);

[[nodiscard]] std::string read_file(const std::filesystem::path& path);

/// Prints a log file between markers, for diagnostics.
void dump_log(const std::filesystem::path& path);

/// "3", or "none (killed)" for a process that did not exit.
[[nodiscard]] std::string exit_code_text(const std::optional<int>& code);

/// Waits for `process` until `deadline`; kills it if it is still running then. `deadline` is not
/// scaled here: each launcher scales its --timeout-ms where it reads it (scaled_timeout()).
/// @returns its exit code, or nullopt if it had to be killed.
std::optional<int> wait_or_kill(ChildProcess& process, std::chrono::steady_clock::time_point deadline);

/// A sil-kit-registry process listening on a free port (port 0), logging to
/// `<work_dir>/registry.log`. The constructor waits until the registry has published its URI
/// and throws std::runtime_error (after dumping the log) if it exits or takes too long.
class RegistryProcess
{
public:
    RegistryProcess(const std::filesystem::path& executable, const std::filesystem::path& work_dir);

    [[nodiscard]] const std::string& uri() const noexcept { return uri_; }
    [[nodiscard]] const std::filesystem::path& log_file() const noexcept { return log_; }
    void stop() { process_->kill(); }

private:
    std::filesystem::path log_;
    std::unique_ptr<ChildProcess> process_;
    std::string uri_;
};

} // namespace vehicle_sil::test
