// SPDX-License-Identifier: MIT
//
// Helpers of the vehicle_sil::launch tests: findings as text, one per line, to compare whole; the
// working directory for a while.
#pragma once

#include "test_harness.hpp"

#include "vehicle_sil/launch/check.hpp"

#include <filesystem>
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

/// The working directory is `dir` until it ends: a request names its files as the user wrote
/// them, and the outputs repeat the names, so a test that compares them stands where its files are.
/// The working directory is the whole process's: not for a test in which another thread reads a
/// relative path meanwhile.
class InDirectory
{
public:
    explicit InDirectory(const std::filesystem::path& dir) : before_(std::filesystem::current_path())
    {
        std::filesystem::current_path(dir);
    }
    ~InDirectory()
    {
        std::error_code ignored; // a destructor must not throw
        std::filesystem::current_path(before_, ignored);
    }
    InDirectory(const InDirectory&) = delete;
    InDirectory& operator=(const InDirectory&) = delete;

private:
    std::filesystem::path before_;
};

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
