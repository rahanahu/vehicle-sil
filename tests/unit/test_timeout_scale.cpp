// SPDX-License-Identifier: MIT
//
// Tests for the factor on the tests' upper bounds of waiting (test_timeout_scale.hpp).
#include "test_harness.hpp"
#include "test_timeout_scale.hpp"

#include <chrono>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::test::parse_timeout_scale;
using vehicle_sil::test::scale_limit;

} // namespace

TEST_CASE(timeout_scale_reads_a_number_greater_than_zero)
{
    CHECK(parse_timeout_scale(nullptr) == 1.0);
    CHECK(parse_timeout_scale("") == 1.0);
    CHECK(parse_timeout_scale("3") == 3.0);
    CHECK(parse_timeout_scale("0.5") == 0.5);
    CHECK(parse_timeout_scale("1e-3") == 1e-3);
}

TEST_CASE(timeout_scale_refuses_what_is_not_a_number_greater_than_zero)
{
    for (const char* text : {"0", "-1", "abc", "3x", " ", "inf", "nan"})
    {
        CHECK(!parse_timeout_scale(text).has_value());
    }
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
