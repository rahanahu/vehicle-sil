// SPDX-License-Identifier: MIT
//
// Tests for the factor on the tests' upper bounds of waiting (test_timeout_scale.hpp).
#include "test_harness.hpp"
#include "test_timeout_scale.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::test::parse_timeout_scale;
using vehicle_sil::test::read_timeout_scale;
using vehicle_sil::test::scale_limit;
using vehicle_sil::test::timeout_scale_mismatch;

} // namespace

TEST_CASE(timeout_scale_reads_a_number_greater_than_zero)
{
    CHECK(parse_timeout_scale("3") == 3.0);
    CHECK(parse_timeout_scale("0.5") == 0.5);
    CHECK(parse_timeout_scale(".5") == 0.5);
    CHECK(parse_timeout_scale("5.") == 5.0);
    CHECK(parse_timeout_scale("007") == 7.0);
    CHECK(parse_timeout_scale("0.001") == 0.001);
}

TEST_CASE(timeout_scale_refuses_what_is_not_a_number_greater_than_zero)
{
    // Digits and one point only, as the readers in CMake, the GUI server's tests and the e2e.
    for (const char* text : {"", "0", "0.0", ".", "-1", "+3", "abc", "3x", " ", " 3", "3 ", "1e3", "1e-3", "0x10", "1_000",
                             "1.2.3", "inf", "nan"})
    {
        CHECK(!parse_timeout_scale(text).has_value());
    }
}

TEST_CASE(timeout_scale_comes_from_the_variable_or_else_the_build_default)
{
    CHECK(read_timeout_scale("3", "4").factor == 3.0);
    CHECK(read_timeout_scale("3", "4").source == "VEHICLE_SIL_TEST_TIMEOUT_SCALE=3");
    CHECK(read_timeout_scale("0.5", "4").factor == 0.5);
    CHECK(read_timeout_scale(nullptr, "4").factor == 4.0);
    CHECK(read_timeout_scale("", "4").factor == 4.0);
    CHECK(read_timeout_scale("", "4").source ==
          "VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=4 (the build's default; VEHICLE_SIL_TEST_TIMEOUT_SCALE is not set)");
    CHECK(read_timeout_scale(nullptr, "1").factor == 1.0);
    CHECK(vehicle_sil::unit::throws_with<std::invalid_argument>([] { (void)read_timeout_scale(" 3", "1"); },
                                                                "VEHICLE_SIL_TEST_TIMEOUT_SCALE=\" 3\" is not a number greater "
                                                                "than 0 in digits (such as 3 or 2.5): set it to a factor such "
                                                                "as 3 (every upper bound of waiting in the tests 3 times as "
                                                                "long), or unset it"));
    CHECK(vehicle_sil::unit::throws_with<std::invalid_argument>([] { (void)read_timeout_scale(nullptr, "x"); },
                                                                "VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=\"x\" is not a number "));
}

TEST_CASE(timeout_scale_larger_than_the_ctest_timeout_is_said)
{
    const auto scale = read_timeout_scale("8", "1");
    // Not run by CTest, or a TIMEOUT made for as much or more: nothing to say.
    CHECK(!timeout_scale_mismatch(scale, nullptr).has_value());
    CHECK(!timeout_scale_mismatch(scale, "").has_value());
    CHECK(!timeout_scale_mismatch(scale, "8").has_value());
    CHECK(!timeout_scale_mismatch(read_timeout_scale("2.5", "1"), "3").has_value());
    CHECK(!timeout_scale_mismatch(read_timeout_scale("0.5", "1"), "1").has_value());
    const auto said = timeout_scale_mismatch(scale, "4");
    CHECK(said.has_value());
    CHECK(*said == std::string("warning: VEHICLE_SIL_TEST_TIMEOUT_SCALE=8 makes the upper bounds of waiting 8 times as "
                                "long, but the CTest TIMEOUT of this test was made for 4 when the build was configured, "
                                "so CTest may stop the test before it says what failed: configure the build again with "
                                "VEHICLE_SIL_TEST_TIMEOUT_SCALE as it is now (cmake --preset <preset>)"));
    // 2.5 needs a TIMEOUT made for 3.
    CHECK(timeout_scale_mismatch(read_timeout_scale("2.5", "1"), "2").has_value());
    // The build's default counts as well.
    CHECK(timeout_scale_mismatch(read_timeout_scale(nullptr, "4"), "1").has_value());
}

TEST_CASE(timeout_scale_multiplies_and_rounds_up_to_milliseconds)
{
    CHECK_EQ(scale_limit(20s, 3.0).count(), 60000);
    CHECK_EQ(scale_limit(30s, 0.001).count(), 30);
    CHECK_EQ(scale_limit(1ms, 0.5).count(), 1);
    CHECK_EQ(scale_limit(1500us, 1.0).count(), 2);
    CHECK_EQ(scale_limit(0ms, 4.0).count(), 0);
    // Beyond any wait, but no overflow when added to now().
    CHECK(scale_limit(std::chrono::hours(1'000'000'000), 1e9) <= std::chrono::milliseconds(1'000'000'000'000));
}

TEST_CASE(timeout_scale_is_what_the_waits_use)
{
    // eventually() gives up after its scaled limit, not before.
    const auto begin = std::chrono::steady_clock::now();
    CHECK(!vehicle_sil::unit::eventually([] { return false; }, 20ms));
    CHECK(std::chrono::steady_clock::now() - begin >= vehicle_sil::unit::scaled_timeout(20ms));
}
