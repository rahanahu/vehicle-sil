// SPDX-License-Identifier: MIT
#pragma once

// The factor on the tests' upper bounds of waiting, from the environment variable
// VEHICLE_SIL_TEST_TIMEOUT_SCALE (unset: 1). A slow machine, a loaded one or a sanitizer build sets
// it above 1, so that a test that waits for something that will happen gives it more time. Only
// such upper bounds are scaled: a test that passes ends as soon as what it waits for happens, so it
// is not slower. Never scale a wait that checks that something does NOT happen, nor the times that a
// test gives the product (periods, its own timeouts): that would change what the test checks.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace vehicle_sil::test {

inline constexpr const char* kTimeoutScaleVariable = "VEHICLE_SIL_TEST_TIMEOUT_SCALE";

/// The factor that `text` (a value of VEHICLE_SIL_TEST_TIMEOUT_SCALE) gives: 1 for null or empty,
/// nullopt when it is not a finite number greater than 0.
inline std::optional<double> parse_timeout_scale(const char* text)
{
    if (text == nullptr || *text == '\0')
    {
        return 1.0;
    }
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value <= 0.0)
    {
        return std::nullopt;
    }
    return value;
}

/// The factor of this process (read once). Throws std::invalid_argument, naming the variable, when
/// the variable holds something else than a number greater than 0.
inline double timeout_scale()
{
    static const double scale = [] {
        const char* text = std::getenv(kTimeoutScaleVariable);
        const auto parsed = parse_timeout_scale(text);
        if (!parsed)
        {
            throw std::invalid_argument(std::string(kTimeoutScaleVariable) + "=\"" + text +
                                        "\" is not a number greater than 0: set it to a factor such as 3 "
                                        "(every upper bound of waiting in the tests 3 times as long), or unset it");
        }
        return *parsed;
    }();
    return scale;
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
