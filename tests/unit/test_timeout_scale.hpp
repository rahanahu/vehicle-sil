// SPDX-License-Identifier: MIT
#pragma once

// The factor on the tests' upper bounds of waiting: the environment variable
// VEHICLE_SIL_TEST_TIMEOUT_SCALE where the test runs, or, when it is not set, the build's default
// (the CMake cache variable VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT, 1 unless a preset sets it; the
// thread sanitizer presets set 4). A slow machine, a loaded one or a sanitizer build sets it above
// 1, so that a test that waits for something that will happen gives it more time. A test that
// passes ends as soon as what it waits for happens, so it is not slower.
//
// Scale an upper bound of waiting for something that will happen, also when the test hands it to
// the product as a limit (a launcher's --timeout-ms for the simulation it runs). Never scale a
// wait that checks that something does NOT happen, nor a time whose expiry the test checks (a
// receiver's timeout that the test expects to run out), nor periods: that would change what the
// test checks.
//
// The CTest TIMEOUT of the harness and scenario tests is made at configure time for the factor of
// that time (cmake/VehicleSilTestHelpers.cmake), and the tests get it as
// VEHICLE_SIL_TEST_CTEST_TIMEOUT_FACTOR: timeout_scale_warning() says when the factor is larger
// now, since CTest would stop a stuck test before the test says what failed.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

// The build's default factor, as text (tests/CMakeLists.txt defines it from the cache variable).
#ifndef VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT
#define VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT "1"
#endif

namespace vehicle_sil::test {

inline constexpr const char* kTimeoutScaleVariable = "VEHICLE_SIL_TEST_TIMEOUT_SCALE";
inline constexpr const char* kTimeoutScaleDefaultVariable = "VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT";
inline constexpr const char* kCTestTimeoutFactorVariable = "VEHICLE_SIL_TEST_CTEST_TIMEOUT_FACTOR";

/// The factor that `text` gives when it is digits with at most one point and greater than 0 (such
/// as 3, 2.5 or .5); nullopt for anything else (empty, spaces, a sign, an exponent, hex, inf, nan,
/// 0). The same rule as cmake/VehicleSilTestHelpers.cmake, gui/server/tests/conftest.py and
/// gui/web/e2e/support/timeouts.ts.
inline std::optional<double> parse_timeout_scale(std::string_view text)
{
    bool digit = false;
    bool point = false;
    for (const char c : text)
    {
        if (c >= '0' && c <= '9')
        {
            digit = true;
        }
        else if (c == '.' && !point)
        {
            point = true;
        }
        else
        {
            return std::nullopt;
        }
    }
    if (!digit)
    {
        return std::nullopt;
    }
    // Only digits and a point are left, which strtod reads in the "C" locale of a test.
    const std::string copy(text);
    const double value = std::strtod(copy.c_str(), nullptr);
    if (!std::isfinite(value) || value <= 0.0)
    {
        return std::nullopt;
    }
    return value;
}

/// The factor and where it comes from, for messages.
struct TimeoutScale
{
    double factor = 1.0;
    std::string source;
};

/// `factor` as a person writes it (3, 2.5, 0.001).
inline std::string factor_text(double factor)
{
    std::ostringstream out;
    out << factor;
    return out.str();
}

/// The factor from `variable` (the value of VEHICLE_SIL_TEST_TIMEOUT_SCALE; null or empty: not
/// set) or else from `build_default`. Throws std::invalid_argument, naming the variable and how to
/// fix it, when the one that counts is no such number.
inline TimeoutScale read_timeout_scale(const char* variable, std::string_view build_default)
{
    const std::string_view fix = " is not a number greater than 0 in digits (such as 3 or 2.5): set it to a factor such as 3 "
                                 "(every upper bound of waiting in the tests 3 times as long)";
    if (variable != nullptr && *variable != '\0')
    {
        const auto parsed = parse_timeout_scale(variable);
        if (!parsed)
        {
            throw std::invalid_argument(std::string(kTimeoutScaleVariable) + "=\"" + variable + "\"" + std::string(fix) +
                                        ", or unset it");
        }
        return {*parsed, std::string(kTimeoutScaleVariable) + "=" + variable};
    }
    const auto parsed = parse_timeout_scale(build_default);
    if (!parsed)
    {
        throw std::invalid_argument(std::string(kTimeoutScaleDefaultVariable) + "=\"" + std::string(build_default) + "\"" +
                                    std::string(fix) + ", and configure the build again");
    }
    return {*parsed, std::string(kTimeoutScaleDefaultVariable) + "=" + std::string(build_default) + " (the build's default; " +
                         kTimeoutScaleVariable + " is not set)"};
}

/// The factor of this process and where it comes from (read once). Throws as read_timeout_scale().
inline const TimeoutScale& timeout_scale_of_process()
{
    static const TimeoutScale scale = read_timeout_scale(std::getenv(kTimeoutScaleVariable), VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT);
    return scale;
}

/// The factor of this process (read once). Throws as read_timeout_scale().
inline double timeout_scale() { return timeout_scale_of_process().factor; }

/// The whole factor that a CTest TIMEOUT is made for: `factor` rounded up, at least 1 (as
/// cmake/VehicleSilTestHelpers.cmake does).
inline long whole_timeout_factor(double factor)
{
    return std::max(1L, static_cast<long>(std::ceil(std::min(factor, 1e9))));
}

/// What to say when `scale` needs a longer CTest TIMEOUT than the one made for `ctest_factor` (the
/// value of VEHICLE_SIL_TEST_CTEST_TIMEOUT_FACTOR; null or not a whole number: not run by CTest, or
/// nothing to compare); nullopt when it does not.
inline std::optional<std::string> timeout_scale_mismatch(const TimeoutScale& scale, const char* ctest_factor)
{
    if (ctest_factor == nullptr || *ctest_factor == '\0')
    {
        return std::nullopt;
    }
    char* end = nullptr;
    const long made_for = std::strtol(ctest_factor, &end, 10);
    if (*end != '\0' || made_for < 1 || whole_timeout_factor(scale.factor) <= made_for)
    {
        return std::nullopt;
    }
    return "warning: " + scale.source + " makes the upper bounds of waiting " + factor_text(scale.factor) +
           " times as long, but the CTest TIMEOUT of this test was made for " + std::to_string(made_for) +
           " when the build was configured, so CTest may stop the test before it says what failed: configure the build "
           "again with " + kTimeoutScaleVariable + " as it is now (cmake --preset <preset>)";
}

/// timeout_scale_mismatch() for this process. Throws as read_timeout_scale().
inline std::optional<std::string> timeout_scale_warning()
{
    return timeout_scale_mismatch(timeout_scale_of_process(), std::getenv(kCTestTimeoutFactorVariable));
}

/// `limit` times `factor`, in whole milliseconds, rounded up.
template <typename Rep, typename Period>
std::chrono::milliseconds scale_limit(std::chrono::duration<Rep, Period> limit, double factor)
{
    // Far beyond any wait, and far from the overflow of a steady_clock time point.
    constexpr double kMaxMs = 1e12;
    const double ms = std::chrono::duration<double, std::milli>(limit).count() * factor;
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(std::ceil(std::min(ms, kMaxMs))));
}

/// `limit`, an upper bound of waiting for something that will happen, times timeout_scale(); in
/// whole milliseconds, rounded up.
template <typename Rep, typename Period>
std::chrono::milliseconds scaled_timeout(std::chrono::duration<Rep, Period> limit)
{
    return scale_limit(limit, timeout_scale());
}

} // namespace vehicle_sil::test
