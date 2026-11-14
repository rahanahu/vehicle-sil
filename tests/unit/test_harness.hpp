// SPDX-License-Identifier: MIT
//
// A deliberately tiny test harness: tests register themselves, main() runs one test by name so
// CTest can run every test in its own process (a failure or a crash stays in its test).
#pragma once

#include "test_timeout_scale.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace vehicle_sil::unit {

using TestFunction = std::function<void()>;

inline std::map<std::string, TestFunction>& registry()
{
    static std::map<std::string, TestFunction> tests;
    return tests;
}

struct Registrar
{
    Registrar(const char* name, TestFunction function) { registry().emplace(name, std::move(function)); }
};

class Failure : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// The test cannot run here (what() says why). A test run alone then exits with kExitSkipped,
/// which CTest reports as skipped (SKIP_RETURN_CODE).
class Skipped : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

inline constexpr int kExitSkipped = 77;

template <typename A, typename B>
void check_equal(const A& actual, const B& expected, const char* actualText, const char* expectedText, const char* file, int line)
{
    if (!(actual == expected))
    {
        std::ostringstream message;
        message << file << ':' << line << ": CHECK_EQ(" << actualText << ", " << expectedText << ") failed: " << +actual
                << " != " << +expected;
        throw Failure(message.str());
    }
}

/// true if `f()` throws an exception of type E (or derived from it).
template <typename E, typename F>
bool throws(F&& f)
{
    try
    {
        f();
    }
    catch (const E&)
    {
        return true;
    }
    return false;
}

/// true if `f()` throws an exception of type E (or derived from it) for which `predicate(e)` is
/// true. An E that fails the predicate yields false; other exceptions propagate.
template <typename E, typename F, typename P>
bool throws_if(F&& f, P&& predicate)
{
    try
    {
        f();
    }
    catch (const E& e)
    {
        return predicate(e);
    }
    return false;
}

/// true if `f()` throws an exception of type E whose what() contains `substring`.
template <typename E, typename F>
bool throws_with(F&& f, const std::string& substring)
{
    return throws_if<E>(std::forward<F>(f), [&](const E& e) { return std::string(e.what()).find(substring) != std::string::npos; });
}

using vehicle_sil::test::scaled_timeout;
using vehicle_sil::test::timeout_scale;

/// Polls `predicate` every `poll` until it returns true or `timeout` has passed. `timeout` is an
/// upper bound for something that will happen: it is scaled (scaled_timeout(),
/// VEHICLE_SIL_TEST_TIMEOUT_SCALE). To check that something does not happen, do not use this.
/// @returns the last result of `predicate`.
template <typename Predicate>
bool eventually(Predicate&& predicate, std::chrono::milliseconds timeout, std::chrono::milliseconds poll = std::chrono::milliseconds(1))
{
    const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(timeout);
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return predicate();
        }
        std::this_thread::sleep_for(poll);
    }
    return true;
}

} // namespace vehicle_sil::unit

#define VEHICLE_SIL_CONCAT_(a, b) a##b
#define VEHICLE_SIL_CONCAT(a, b) VEHICLE_SIL_CONCAT_(a, b)

#define TEST_CASE(name)                                                                                 \
    static void name();                                                                                 \
    static const ::vehicle_sil::unit::Registrar VEHICLE_SIL_CONCAT(registrar_, name){#name, &name};           \
    static void name()

#define CHECK(condition)                                                                                \
    do                                                                                                  \
    {                                                                                                   \
        if (!(condition))                                                                               \
        {                                                                                               \
            throw ::vehicle_sil::unit::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +    \
                                            ": CHECK(" #condition ") failed");                          \
        }                                                                                               \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                      \
    ::vehicle_sil::unit::check_equal((actual), (expected), #actual, #expected, __FILE__, __LINE__)
