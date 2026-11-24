// SPDX-License-Identifier: MIT
//
// The results of a command that did not get to run (error_result.hpp). Pure
// functions.
#include "vehicle_sil/launch/error_result.hpp"

#include "vehicle_sil/launch/connections.hpp"
#include "vehicle_sil/launch/test.hpp"

#include <utility>

namespace vehicle_sil::launch {

namespace {

config::Finding internal_finding(const std::string &message) {
  return config::Finding{config::Severity::Error, "internal", message, 0, "", ""};
}

} // namespace

RunReport usage_run_report(const std::string &file,
                           const std::vector<config::Finding> &errors,
                           const std::vector<std::string> &variants) {
  RunReport report;
  report.check.file = file;
  report.check.usage = true;
  report.check.variants = variants;
  report.check.findings = errors;
  report.usage = true;
  return report;
}

std::string format_usage_json(CommandKind command, const std::string &file,
                              const std::vector<config::Finding> &errors,
                              const std::vector<std::string> &variants,
                              bool file_only) {
  switch (command) {
  case CommandKind::Connections: {
    ConnectionsReport report;
    report.file = file;
    report.usage = true;
    report.findings = errors;
    return format_connections_json(report);
  }
  case CommandKind::Test: {
    TestReport report;
    report.file = file;
    report.usage = true;
    report.findings = errors;
    return format_test_json(report);
  }
  case CommandKind::Run:
    return format_run_json(usage_run_report(file, errors, variants));
  case CommandKind::Check:
    break;
  }
  Checked check;
  check.file = file;
  check.usage = true;
  check.variants = variants;
  check.findings = errors;
  if (file_only) {
    check.file_sha256 = std::string{};
    check.hot_eligibility = evaluate_hot_eligibility(check);
  }
  return format_json(check);
}

RunReport internal_error_run_report(const std::string &message) {
  RunReport report;
  report.findings.push_back(internal_finding(message));
  return report;
}

std::string format_internal_error_json(CommandKind command,
                                       const std::string &file,
                                       const std::string &message) {
  switch (command) {
  case CommandKind::Connections: {
    ConnectionsReport report;
    report.file = file;
    report.findings.push_back(internal_finding(message));
    return format_connections_json(report);
  }
  case CommandKind::Test: {
    TestReport report;
    report.unreadable = true;
    report.findings.push_back(internal_finding(message));
    return format_test_json(report);
  }
  case CommandKind::Run:
    return format_internal_error_json(true, message);
  case CommandKind::Check:
    break;
  }
  return format_internal_error_json(false, message);
}

} // namespace vehicle_sil::launch
