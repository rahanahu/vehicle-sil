// SPDX-License-Identifier: MIT
//
// NothingCameHint: when a bridge says that nothing came from a device on the bus it sends to. The
// time is given with the counts, so nothing here waits.
#include "test_harness.hpp"

#include "vehicle_sil/bridge_runtime/nothing_came_hint.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::bridge_runtime::kNothingCameAfter;
using vehicle_sil::bridge_runtime::NothingCameHint;
using Time = std::chrono::steady_clock::time_point;

const Time kStart{};

NothingCameHint make_hint(int& asked)
{
    return NothingCameHint([&asked] {
        ++asked;
        return std::string("nothing came");
    });
}

} // namespace

TEST_CASE(nothing_came_hint_is_due_ten_seconds_after_the_first_frame_sent)
{
    CHECK(kNothingCameAfter == 10s);
    int asked = 0;
    NothingCameHint hint = make_hint(asked);
    CHECK(!hint.look(0, 0, kStart)); // nothing sent: nothing to say, however long
    CHECK(!hint.look(0, 0, kStart + 1h));
    const Time sent = kStart + 1h + 5s;
    CHECK(!hint.look(3, 0, sent)); // the first look that sees a frame sent starts the time
    CHECK(!hint.look(40, 0, sent + 9999ms));
    CHECK_EQ(asked, 0); // the text is made only when it is due
    CHECK(hint.look(41, 0, sent + 10s) == std::optional<std::string>("nothing came"));
    CHECK_EQ(asked, 1);
    CHECK(!hint.look(42, 0, sent + 20s)); // once
    CHECK(!hint.look(42, 0, sent + 1h));
    CHECK_EQ(asked, 1);
}

TEST_CASE(nothing_came_hint_says_nothing_once_a_device_answered)
{
    int asked = 0;
    NothingCameHint hint = make_hint(asked);
    CHECK(!hint.look(5, 1, kStart));
    CHECK(!hint.look(5, 1, kStart + 1h));
    // Counts from the start: received cannot go back to 0, but even then the bus was there.
    CHECK(!hint.look(9, 0, kStart + 2h));
    CHECK(!hint.look(9, 0, kStart + 3h));
    CHECK_EQ(asked, 0);
}

TEST_CASE(nothing_came_hint_says_nothing_when_an_answer_came_before_it_was_due)
{
    int asked = 0;
    NothingCameHint hint = make_hint(asked);
    CHECK(!hint.look(1, 0, kStart));
    CHECK(!hint.look(2, 1, kStart + 9s));
    CHECK(!hint.look(3, 1, kStart + 10s));
    CHECK(!hint.look(3, 1, kStart + 1h));
    CHECK_EQ(asked, 0);
}

TEST_CASE(nothing_came_hint_with_a_frame_received_before_any_was_sent_stays_quiet)
{
    // A device that speaks first (a CAN node that sends on its own) shows the bus is there.
    int asked = 0;
    NothingCameHint hint = make_hint(asked);
    CHECK(!hint.look(0, 4, kStart));
    CHECK(!hint.look(10, 4, kStart + 1s));
    CHECK(!hint.look(10, 4, kStart + 1h));
    CHECK_EQ(asked, 0);
}

TEST_CASE(nothing_came_hint_takes_its_own_time)
{
    int asked = 0;
    NothingCameHint hint([&asked] {
        ++asked;
        return std::string("quick");
    }, 250ms);
    CHECK(!hint.look(1, 0, kStart));
    CHECK(!hint.look(1, 0, kStart + 249ms));
    CHECK(hint.look(1, 0, kStart + 250ms) == std::optional<std::string>("quick"));
}
