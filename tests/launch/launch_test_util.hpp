// SPDX-License-Identifier: MIT
//
// Helpers of the vehicle_sil::launch tests: findings as text, one per line, to compare whole.
#pragma once

#include "test_harness.hpp"

#include "vehicle_sil/launch/check.hpp"

#include <string>
#include <vector>

namespace vehicle_sil::launch::test {

/// "E 12 code: message", "W 0 code: message" or "N 3 code: message" (error, warning, note), one
/// line each; the detail indented below.
inline std::string lines(const std::vector<Finding>& findings)
{
    std::string out;
    for (const auto& f : findings)
    {
        out += std::string(f.severity == Severity::Error ? "E " : f.severity == Severity::Warning ? "W " : "N ") + std::to_string(f.line) + " " + f.code + ": " +
               f.message + "\n";
        if (!f.detail.empty())
        {
            out += "  | " + f.detail + "\n";
        }
    }
    return out;
}

/// Throws a Failure that shows both texts if they differ.
inline void check_text(const std::string& actual, const std::string& expected, const char* what = "text")
{
    if (actual != expected)
    {
        std::size_t at = 0;
        while (at < actual.size() && at < expected.size() && actual[at] == expected[at])
        {
            ++at;
        }
        throw unit::Failure(std::string(what) + " differs at byte " + std::to_string(at) + ":\n--- got ---\n" + actual +
                            "--- expected ---\n" + expected + "---");
    }
}

} // namespace vehicle_sil::launch::test
