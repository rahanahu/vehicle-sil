// SPDX-License-Identifier: MIT
//
// J1939 value types of the J1939 stack: PGN, the 29-bit identifier (PDU1/PDU2), addresses.
#include "j1939_bench.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/j1939/timer_set.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::Identifier;
using vehicle_sil::j1939::is_unicast;
using vehicle_sil::j1939::Pgn;
using vehicle_sil::j1939::TimerId;
using vehicle_sil::j1939::TimerKind;
using vehicle_sil::j1939::TimerSet;
using vehicle_sil::j1939::TimerTarget;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::j1939::at;
using vehicle_sil::unit::j1939::j1939_id;
using namespace std::chrono_literals;

TEST_CASE(pgn_splits_into_its_fields)
{
    const Pgn dm1{0xFECA};
    CHECK(dm1.is_pdu2());
    CHECK(!dm1.is_pdu1());
    CHECK_EQ(dm1.pdu_format(), 0xFEu);
    CHECK_EQ(dm1.pdu_specific(), 0xCAu);
    CHECK(!dm1.data_page());
    CHECK(!dm1.extended_data_page());
    CHECK(dm1.is_valid());

    const Pgn dp{0x1FEF0};
    CHECK(dp.data_page());
    CHECK(!dp.extended_data_page());
    const Pgn edp{0x2EF00};
    CHECK(edp.extended_data_page());
    CHECK(edp.is_pdu1());
}

TEST_CASE(pgn_validity_at_the_pdu1_pdu2_boundary)
{
    // PF 239 is the last PDU1 format: the PS byte must be 0 in the PGN.
    CHECK(Pgn{0xEF00}.is_pdu1());
    CHECK(Pgn{0xEF00}.is_valid());
    CHECK(!Pgn{0xEF01}.is_valid());
    CHECK(!Pgn{0xEFFF}.is_valid());
    // PF 240 is the first PDU2 format: every PS is a group extension.
    CHECK(Pgn{0xF000}.is_pdu2());
    CHECK(Pgn{0xF000}.is_valid());
    CHECK(Pgn{0xF0FF}.is_valid());
    // Lowest and highest PGN values.
    CHECK(Pgn{0x0}.is_valid());
    CHECK(Pgn{0x3FFFF}.is_valid());
    CHECK(!Pgn{0x40000}.is_valid());
    CHECK(!Pgn{0xFFFFFFFF}.is_valid());
}

TEST_CASE(addresses_null_and_global_are_not_unicast)
{
    CHECK(is_unicast(0));
    CHECK(is_unicast(253));
    CHECK(!is_unicast(254));
    CHECK(!is_unicast(255));
}

TEST_CASE(identifier_encodes_pdu1_with_the_destination_in_ps)
{
    const Identifier id{3, Pgn{0xEF00}, 0x25, 0x80};
    CHECK_EQ(id.encode(), j1939_id(3, 0xEF00, 0x25, 0x80));
    CHECK_EQ(id.encode(), 0x0CEF2580u);
    // Global destination of a PDU1 group.
    CHECK_EQ((Identifier{6, Pgn{0xEA00}, 0xFF, 0x00}.encode()), 0x18EAFF00u);
}

TEST_CASE(identifier_encodes_pdu2_with_the_group_extension)
{
    CHECK_EQ((Identifier{6, Pgn{0xFECA}, 0xFF, 0x3D}.encode()), 0x18FECA3Du);
    CHECK_EQ((Identifier{7, Pgn{0x1FEF0}, 0xFF, 0x01}.encode()), 0x1DFEF001u);
    CHECK_EQ((Identifier{0, Pgn{0x3FFFF}, 0xFF, 0xFD}.encode()), 0x03FFFFFDu);
}

TEST_CASE(identifier_encode_rejects_invalid_fields)
{
    CHECK(throws<std::invalid_argument>([] { static_cast<void>(Identifier{6, Pgn{0xEF01}, 0x10, 0x80}.encode()); }));
    CHECK(throws<std::invalid_argument>([] { static_cast<void>(Identifier{6, Pgn{0x40000}, 0xFF, 0x80}.encode()); }));
    CHECK(throws<std::invalid_argument>([] { static_cast<void>(Identifier{8, Pgn{0xFECA}, 0xFF, 0x80}.encode()); }));
    // A PDU2 group cannot go to one destination.
    CHECK(throws<std::invalid_argument>([] { static_cast<void>(Identifier{6, Pgn{0xFECA}, 0x10, 0x80}.encode()); }));
}

TEST_CASE(identifier_decodes_pdu1_and_pdu2)
{
    const Identifier pdu1 = Identifier::decode(0x0CEF2580u);
    CHECK_EQ(pdu1.priority, 3u);
    CHECK_EQ(pdu1.pgn.value(), 0xEF00u);
    CHECK_EQ(pdu1.destination, 0x25u);
    CHECK_EQ(pdu1.source, 0x80u);

    const Identifier pdu2 = Identifier::decode(0x18FECA3Du);
    CHECK_EQ(pdu2.priority, 6u);
    CHECK_EQ(pdu2.pgn.value(), 0xFECAu);
    CHECK_EQ(pdu2.destination, 0xFFu);
    CHECK_EQ(pdu2.source, 0x3Du);

    // PF 239 / 240 boundary when decoding.
    CHECK_EQ(Identifier::decode(j1939_id(6, 0xEF00, 0x12, 0x01)).destination, 0x12u);
    CHECK_EQ(Identifier::decode(j1939_id(6, 0xF012, 0xFF, 0x01)).pgn.value(), 0xF012u);
    CHECK_EQ(Identifier::decode(j1939_id(6, 0xF012, 0xFF, 0x01)).destination, 0xFFu);
}

TEST_CASE(identifier_round_trips_every_priority_and_page)
{
    for (std::uint8_t priority = 0; priority <= 7; ++priority)
    {
        for (const std::uint32_t pgn : {0x00000u, 0x0EA00u, 0x0EF00u, 0x0F000u, 0x0FFFFu, 0x10000u, 0x1EF00u, 0x2EA00u, 0x3FFFFu})
        {
            const std::uint8_t da = Pgn{pgn}.is_pdu1() ? 0x42 : 0xFF;
            const Identifier id{priority, Pgn{pgn}, da, 0x17};
            CHECK(Identifier::decode(id.encode()) == id);
        }
    }
}

TEST_CASE(identifier_from_an_11_bit_frame_is_not_j1939)
{
    vehicle_sil::can::Frame f;
    f.id = 0x123;
    f.format = vehicle_sil::can::IdFormat::Standard;
    CHECK(!Identifier::decode(f).has_value());
    f.format = vehicle_sil::can::IdFormat::Extended;
    f.id = 0x18FECA3Du;
    CHECK(Identifier::decode(f).has_value());
}

namespace {

struct Recorder final : TimerTarget
{
    std::vector<std::uint32_t> fired;
    void on_timer(std::uint32_t tag) override { fired.push_back(tag); }
};

} // namespace

TEST_CASE(timer_set_fires_by_deadline_then_start_order)
{
    TimerSet timers(8);
    Recorder r;
    timers.start(TimerKind::Timeout, at(30ms), r, 1);
    timers.start(TimerKind::Timeout, at(10ms), r, 2);
    timers.start(TimerKind::Scheduled, at(10ms), r, 3);
    CHECK(timers.next_deadline() == at(10ms));
    while (timers.fire_one_before(at(31ms)))
    {
    }
    CHECK(r.fired == (std::vector<std::uint32_t>{2, 3, 1}));
    CHECK(!timers.next_deadline().has_value());
}

TEST_CASE(timer_set_due_rules_differ_for_timeouts_and_scheduled_timers)
{
    TimerSet timers(4);
    Recorder r;
    timers.start(TimerKind::Timeout, at(20ms), r, 1);
    timers.start(TimerKind::Scheduled, at(20ms), r, 2);
    // At now == deadline only the scheduled timer is due; the timeout waits for the frames at 20 ms.
    while (timers.fire_one_due(at(20ms)))
    {
    }
    CHECK(r.fired == (std::vector<std::uint32_t>{2}));
    while (timers.fire_one_due(at(20ms) + 1ns))
    {
    }
    CHECK(r.fired == (std::vector<std::uint32_t>{2, 1}));
    // A frame at the deadline comes before the timeout: fire_one_before(deadline) does not fire it.
    timers.start(TimerKind::Timeout, at(40ms), r, 3);
    CHECK(!timers.fire_one_before(at(40ms)));
    CHECK(timers.fire_one_before(at(40ms) + 1ns));
}

TEST_CASE(timer_set_cancel_capacity_and_ids)
{
    TimerSet timers(2);
    Recorder r;
    const TimerId a = timers.start(TimerKind::Timeout, at(10ms), r, 1);
    const TimerId b = timers.start(TimerKind::Timeout, at(20ms), r, 2);
    CHECK(throws<std::length_error>([&] { timers.start(TimerKind::Timeout, at(30ms), r, 3); }));
    CHECK(timers.cancel(a));
    CHECK(!timers.cancel(a));
    CHECK(!timers.is_running(a));
    // The slot is reused, but the old id stays stale.
    const TimerId c = timers.start(TimerKind::Timeout, at(5ms), r, 4);
    CHECK(c.slot == a.slot);
    CHECK(!timers.is_running(a));
    CHECK(timers.is_running(c));
    CHECK(timers.is_running(b));
    CHECK(!timers.cancel(TimerId{}));
    CHECK_EQ(timers.running(), 2u);
}

namespace {

struct Restarter final : TimerTarget
{
    TimerSet* timers = nullptr;
    std::vector<std::uint32_t> fired;
    SimDuration restart_after = 1ms;
    bool threw = false;
    void on_timer(std::uint32_t tag) override
    {
        fired.push_back(tag);
        if (tag < 3)
        {
            // Restart from inside the callback (the slot of the firing timer is free again).
            try
            {
                timers->start(TimerKind::Scheduled, at(10ms) + restart_after * tag, *this, tag + 1);
            }
            catch (const std::logic_error&)
            {
                threw = true;
            }
        }
    }
};

} // namespace

TEST_CASE(timer_set_target_may_start_timers_while_firing)
{
    TimerSet timers(1);
    Restarter r;
    r.timers = &timers;
    timers.start(TimerKind::Scheduled, at(10ms), r, 1);
    while (timers.fire_one_due(at(20ms)))
    {
    }
    CHECK(r.fired == (std::vector<std::uint32_t>{1, 2, 3}));
    CHECK(!r.threw);
}

TEST_CASE(timer_set_restart_at_a_due_deadline_waits_for_the_next_round)
{
    // Restarting at the firing deadline (or earlier) would keep the timer due for ever; a timer
    // started within a round fires in it only after the deadline that started it.
    for (const SimDuration offset : {SimDuration::zero(), SimDuration{-1ms}})
    {
        TimerSet timers(2);
        Restarter r;
        r.timers = &timers;
        r.restart_after = offset;
        timers.start(TimerKind::Scheduled, at(10ms), r, 1);
        timers.begin_round();
        while (timers.fire_one_due(at(20ms)))
        {
        }
        CHECK(!r.threw);
        CHECK(r.fired == (std::vector<std::uint32_t>{1}));
        CHECK_EQ(timers.running(), 1u);
        timers.begin_round();
        while (timers.fire_one_due(at(20ms)))
        {
        }
        CHECK(r.fired == (std::vector<std::uint32_t>{1, 2}));
    }
}
