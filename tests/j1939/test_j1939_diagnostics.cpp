// SPDX-License-Identifier: MIT
//
// The diagnostics part of the J1939 stack (DM1, DM2, DM3, DM11, DM22): when DM1 goes out,
// the DTC encoding, answers to requests, clearing (also deferred by the application), DM22, the
// broadcasts competing for the node's transfer, and the claim; reading a DTC list
// (parse_dtc_list) back.
#include "j1939_parts_util.hpp"
#include "network_test_access.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/diagnostics.hpp"
#include "vehicle_sil/j1939/name.hpp"

#include <array>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::BroadcastSuppression;
using vehicle_sil::j1939::ClearDecision;
using vehicle_sil::j1939::Diagnostics;
using vehicle_sil::j1939::DiagnosticsOptions;
using vehicle_sil::j1939::Dtc;
using vehicle_sil::j1939::DtcClear;
using vehicle_sil::j1939::DtcListView;
using vehicle_sil::j1939::Lamp;
using vehicle_sil::j1939::LampFlash;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::parse_dtc_list;
using vehicle_sil::j1939::ReadDtc;
using vehicle_sil::j1939::Timer;
using vehicle_sil::unit::throws;

namespace {

constexpr Address kUs = 0x20;
constexpr Address kTool = 0x90;
constexpr Address kOtherTool = 0x91;

J1939Frame req(Address from, Address to, std::uint32_t pgn) { return frame(6, kPgnRequest, from, to, request(pgn)); }
J1939Frame ack_frame(std::uint8_t control, Address requester, std::uint32_t pgn)
{
    return frame(6, kPgnAck, kUs, kGlobal, acknowledgement(control, requester, pgn));
}
J1939Frame dm1_frame(Bytes data) { return frame(6, kPgnDm1, kUs, kGlobal, std::move(data)); }

/// A node without a NAME (it owns its address from the first step) with the diagnostics part.
struct Ecu
{
    explicit Ecu(DiagnosticsOptions options = {}, SimDuration step = 10ms, NetworkOptions networkOptions = {})
      : bench(std::move(networkOptions), step), node(bench.add_node(kUs)), diag(node.attach<Diagnostics>(std::move(options)))
    {
    }
    Bench bench;
    Node& node;
    Diagnostics& diag;
};

[[nodiscard]] std::vector<long long> times_of(const std::vector<Sent>& frames)
{
    std::vector<long long> out;
    for (const auto& s : frames)
    {
        out.push_back(s.ms);
    }
    return out;
}

} // namespace

// --- DM1

TEST_CASE(dm1_is_broadcast_every_second_from_the_first_step_without_dtcs)
{
    Ecu ecu;
    const auto frames = record(ecu.bench, 350);
    CHECK_EQ(frames.size(), only(frames, kPgnDm1).size());
    CHECK(times_of(frames) == (std::vector<long long>{0, 1000, 2000, 3000}));
    for (const auto& s : frames)
    {
        CHECK(s.frame == dm1_frame(kNoDtcs));
    }
}

TEST_CASE(dm1_starts_when_the_node_has_claimed_its_address_and_stops_when_it_loses_it)
{
    Bench bench;
    NodeOptions options;
    options.address = 0x80;
    options.name = Name{0x20008200FE001234u};
    Node& us = bench.network.add_node(options);
    us.attach<Diagnostics>();
    auto frames = record(bench, 150);
    // The claim at 0, Claimed at 250: the first DM1 in the step after (the timer started by the
    // change waits for the next round), then every second from 250.
    const auto dm1 = only(frames, kPgnDm1);
    CHECK_EQ(dm1.size(), 2u);
    CHECK_EQ(dm1[0].ms, 260);
    CHECK_EQ(dm1[1].ms, 1250);
    // A lower NAME takes 0x80: the node cannot move (not arbitrary address capable) and stops.
    bench.inject(frame(6, kPgnAddressClaim, 0x80, kGlobal, {0x01, 0, 0, 0, 0, 0, 0, 0}));
    frames = record(bench, 300);
    CHECK(us.address_state() == AddressState::CannotClaim);
    CHECK(only(frames, kPgnDm1).empty());
}

TEST_CASE(dm1_with_one_dtc_is_a_single_frame_with_its_lamp)
{
    Ecu ecu;
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    // A 19-bit SPN: the top 3 bits go into byte 5.
    CHECK(ecu.diag.set_active(Dtc{0x6D2B1, 3, Lamp::AmberWarning}, true));
    CHECK(!ecu.diag.set_active(Dtc{0x6D2B1, 3, Lamp::AmberWarning}, true)); // already active
    const auto sent = record(ecu.bench, 1); // at the next chance
    CHECK_EQ(sent.size(), 1u);
    CHECK(sent[0].frame == dm1_frame(dm_payload(0x04, 0xFF, {dtc_bytes(0x6D2B1, 3, 1)})));
    CHECK(throws<std::invalid_argument>([&] { ecu.diag.set_active(Dtc{0x80000, 1}, true); }));
    CHECK(throws<std::invalid_argument>([&] { ecu.diag.set_active(Dtc{1, 32}, true); }));
}

TEST_CASE(dm1_is_sent_at_the_next_chance_when_a_dtc_changes_and_the_period_counts_from_the_change)
{
    Ecu ecu;
    ecu.bench.steps(151); // 0 .. 1500
    (void)ecu.bench.take_sent();
    const Dtc dtc{100, 2};
    CHECK(ecu.diag.set_active(dtc, true)); // between the steps at 1500 and 1510
    auto dm1 = only(record(ecu.bench, 250), kPgnDm1);
    CHECK(times_of(dm1) == (std::vector<long long>{1510, 2500, 3500}));
    for (const auto& s : dm1)
    {
        CHECK(s.frame.data == dm_payload(0x00, 0xFF, {dtc_bytes(100, 2, 1)}));
    }
    // Inactive and active again (the occurrence count rises), each a second apart.
    CHECK(ecu.diag.set_active(dtc, false));
    dm1 = only(record(ecu.bench, 1), kPgnDm1);
    CHECK_EQ(dm1.size(), 1u);
    CHECK(dm1[0].frame.data == kNoDtcs);
    ecu.bench.steps(100);
    (void)ecu.bench.take_sent();
    CHECK(ecu.diag.set_active(dtc, true));
    dm1 = only(record(ecu.bench, 1), kPgnDm1);
    CHECK_EQ(dm1.size(), 1u);
    CHECK(dm1[0].frame.data == dm_payload(0x00, 0xFF, {dtc_bytes(100, 2, 2)}));
    CHECK(ecu.diag.occurrence_count(dtc) == std::optional<std::uint8_t>(2));
}

TEST_CASE(dm1_reports_one_change_per_dtc_and_second_and_another_dtc_at_once)
{
    Ecu ecu;
    ecu.bench.steps(151); // 0 .. 1500
    (void)ecu.bench.take_sent();
    const Dtc first{100, 2};
    const Dtc second{200, 2, Lamp::AmberWarning};
    CHECK(ecu.diag.set_active(first, true));
    auto dm1 = only(record(ecu.bench, 30), kPgnDm1); // 1510 .. 1800
    CHECK(times_of(dm1) == (std::vector<long long>{1510}));
    // Inactive again within its second: not reported now.
    CHECK(ecu.diag.set_active(first, false));
    dm1 = only(record(ecu.bench, 10), kPgnDm1);
    CHECK(dm1.empty());
    // Another DTC: reported at once, and the DM1 shows the first one's change too.
    CHECK(ecu.diag.set_active(second, true));
    dm1 = only(record(ecu.bench, 1), kPgnDm1);
    CHECK_EQ(dm1.size(), 1u);
    CHECK(dm1[0].frame.data == dm_payload(0x04, 0xFF, {dtc_bytes(200, 2, 1)}));
}

TEST_CASE(dm1_lamp_status_combines_the_listed_dtcs_and_more_than_8_bytes_is_a_bam)
{
    Ecu ecu;
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    const std::vector<Dtc> dtcs = {Dtc{1, 2, Lamp::MalfunctionIndicator, LampFlash::Slow}, Dtc{2, 2, Lamp::RedStop, LampFlash::Fast},
                                   Dtc{3, 2, Lamp::AmberWarning, LampFlash::Solid}, Dtc{4, 2, Lamp::Protect, LampFlash::Fast},
                                   Dtc{5, 2, Lamp::Protect, LampFlash::Slow}}; // fast wins, listed first or not
    std::vector<Bytes> encoded;
    for (const auto& dtc : dtcs)
    {
        CHECK(ecu.diag.set_active(dtc, true));
        encoded.push_back(dtc_bytes(dtc.spn, 2, 1));
    }
    // All four lamps on; flash: MIL slow (00), RSL fast (01), AWL solid (11), protect fast (01).
    const Bytes expected = dm_payload(0x55, 0x1D, encoded);
    const auto frames = record(ecu.bench, 30);
    const auto messages = bam_messages(frames, kUs, kPgnDm1);
    CHECK_EQ(messages.size(), 1u);
    CHECK(messages[0] == expected);
}

TEST_CASE(dm1_inactive_dtc_leaves_dm1_and_its_occurrence_count_stops_at_126)
{
    Ecu ecu;
    const Dtc dtc{523, 6, Lamp::RedStop};
    for (int i = 0; i < 130; ++i)
    {
        CHECK(ecu.diag.set_active(dtc, true));
        CHECK(ecu.diag.set_active(dtc, false));
    }
    CHECK(ecu.diag.is_previously_active(dtc));
    CHECK(!ecu.diag.is_active(dtc));
    CHECK(ecu.diag.occurrence_count(dtc) == std::optional<std::uint8_t>(126));
    CHECK(!ecu.diag.occurrence_count(Dtc{524, 6}).has_value());
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {dm1_frame(kNoDtcs)});
}

TEST_CASE(dm1_with_the_most_dtcs_fits_one_bam_and_the_capacity_is_checked)
{
    DiagnosticsOptions options;
    options.dtc_capacity = vehicle_sil::j1939::kMaxDtcs;
    Ecu ecu(options);
    std::vector<Bytes> encoded;
    for (std::uint32_t spn = 1; spn <= 445; ++spn)
    {
        CHECK(ecu.diag.set_active(Dtc{spn, 2}, true));
        encoded.push_back(dtc_bytes(spn, 2, 1));
    }
    CHECK(throws<std::length_error>([&] { ecu.diag.set_active(Dtc{446, 2}, true); }));
    const Bytes expected = dm_payload(0x00, 0xFF, encoded);
    CHECK_EQ(expected.size(), 1782u);
    // 255 packets, 50 ms apart: about 12.8 s. No other DM1 starts while it is under way.
    const auto frames = record(ecu.bench, 2000);
    const auto messages = bam_messages(frames, kUs, kPgnDm1);
    CHECK(!messages.empty());
    CHECK(messages[0] == expected);
    CHECK_EQ(ecu.diag.statistics().send_failures, 0u);
    std::size_t announcements = 0;
    for (const auto& s : frames)
    {
        announcements += (s.frame.pgn == kPgnTpCm && s.frame.data[0] == 32) ? 1u : 0u;
    }
    // A new broadcast starts only after the one before ended (255 packets, 50 ms apart).
    std::vector<long long> announced;
    for (const auto& s : frames)
    {
        if (s.frame.pgn == kPgnTpCm && s.frame.data[0] == 32)
        {
            announced.push_back(s.ms);
        }
    }
    CHECK(announcements == announced.size());
    for (std::size_t i = 1; i < announced.size(); ++i)
    {
        CHECK(announced[i] - announced[i - 1] >= 254 * 50);
    }
    Bench other;
    Node& node = other.add_node(kUs);
    DiagnosticsOptions tooMany;
    tooMany.dtc_capacity = 446;
    CHECK(throws<std::invalid_argument>([&] { node.attach<Diagnostics>(tooMany); }));
    CHECK(node.find<BroadcastSuppression>() == nullptr); // checked before anything is attached
}

TEST_CASE(dm1_period_does_not_drift_with_the_step_size)
{
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{3ms}, SimDuration{7ms}, SimDuration{30ms}})
    {
        Ecu ecu({}, step);
        const auto dm1 = only(record(ecu.bench, static_cast<int>(5100ms / step)), kPgnDm1);
        std::vector<long long> expected;
        const long long s = step.count() / 1000000;
        for (long long k = 0; k <= 5; ++k)
        {
            expected.push_back((k * 1000 + s - 1) / s * s); // the first step at or after k s
        }
        CHECK(times_of(dm1) == expected);
    }
}

// --- requests for DM1 and DM2

TEST_CASE(dm1_and_dm2_are_answered_on_request_once_per_request_time)
{
    Ecu ecu;
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    // PDU2: the answer to a destination-specific request goes to all nodes; no ACK.
    ecu.bench.inject(req(kTool, kUs, kPgnDm1));
    ecu.bench.inject(req(kOtherTool, kGlobal, kPgnDm1));
    ecu.bench.inject(req(kTool, kUs, kPgnDm2));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {dm1_frame(kNoDtcs), frame(6, kPgnDm2, kUs, kGlobal, kNoDtcs)});
    ecu.bench.inject(req(kTool, kUs, kPgnDm1));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {dm1_frame(kNoDtcs)});
    // Requests at different times in one step each get their answer.
    ecu.bench.inject(ecu.bench.last_time() + 1ms, req(kTool, kUs, kPgnDm1));
    ecu.bench.inject(ecu.bench.last_time() + 9ms, req(kOtherTool, kUs, kPgnDm1));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {dm1_frame(kNoDtcs), dm1_frame(kNoDtcs)});
}

TEST_CASE(dm2_lists_the_previously_active_dtcs_with_their_lamps)
{
    Ecu ecu;
    const Dtc dtc{0x7FFFF, 31, Lamp::AmberWarning, LampFlash::Slow};
    for (int i = 0; i < 3; ++i)
    {
        CHECK(ecu.diag.set_active(dtc, true));
        CHECK(ecu.diag.set_active(dtc, false));
    }
    CHECK(ecu.diag.set_active(Dtc{7, 1}, true)); // active: not in DM2
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    ecu.bench.inject(req(kTool, kGlobal, kPgnDm2));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {frame(6, kPgnDm2, kUs, kGlobal, dm_payload(0x04, 0xF3, {dtc_bytes(0x7FFFF, 31, 3)}))});
}

TEST_CASE(long_answers_go_to_all_nodes_or_by_rts_cts_to_the_requester)
{
    DiagnosticsOptions options;
    options.dtc_capacity = 64;
    Ecu ecu(options);
    std::vector<Bytes> encoded;
    for (std::uint32_t spn = 1; spn <= 64; ++spn)
    {
        CHECK(ecu.diag.set_active(Dtc{spn, 2}, true));
        CHECK(ecu.diag.set_active(Dtc{spn, 2}, false));
        encoded.push_back(dtc_bytes(spn, 2, 1));
    }
    const Bytes expected = dm_payload(0x00, 0xFF, encoded); // 258 bytes: not cut at 256
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    // Destination specific: RTS/CTS to the requester.
    ecu.bench.inject(req(kTool, kUs, kPgnDm2));
    CHECK(receive_rts_cts(ecu.bench, kUs, kTool, kPgnDm2) == expected);
    // To all nodes: one BAM, also for two requests while it is under way.
    ecu.bench.inject(req(kTool, kGlobal, kPgnDm2));
    auto frames = record(ecu.bench, 20);
    ecu.bench.inject(req(kOtherTool, kGlobal, kPgnDm2));
    const auto more = record(ecu.bench, 500);
    frames.insert(frames.end(), more.begin(), more.end());
    const auto messages = bam_messages(frames, kUs, kPgnDm2);
    CHECK_EQ(messages.size(), 1u);
    CHECK(messages[0] == expected);
    std::size_t announced = 0;
    for (const auto& f : frames)
    {
        announced += (f.frame.pgn == kPgnTpCm && f.frame.data[0] == 32) ? 1u : 0u;
    }
    CHECK_EQ(announced, 1u); // the second request did not start (or queue) another one
}

TEST_CASE(the_periodic_dm1_bam_waits_for_the_dm2_bam_of_the_node)
{
    DiagnosticsOptions options;
    options.dtc_capacity = 8;
    Ecu ecu(options);
    for (std::uint32_t spn = 1; spn <= 3; ++spn)
    {
        CHECK(ecu.diag.set_active(Dtc{spn, 2}, true));
        CHECK(ecu.diag.set_active(Dtc{spn, 2}, false)); // DM2: 14 bytes
    }
    CHECK(ecu.diag.set_active(Dtc{10, 2}, true));
    CHECK(ecu.diag.set_active(Dtc{11, 2}, true)); // DM1: 10 bytes
    ecu.bench.steps(95);                            // .. 940; the DM1 BAM of the change is over
    (void)ecu.bench.take_sent();
    // The DM2 BAM starts at 950 and runs past 1000, when the periodic DM1 is due.
    ecu.bench.inject(req(kTool, kGlobal, kPgnDm2));
    const auto frames = record(ecu.bench, 40);
    const auto dm2 = bam_messages(frames, kUs, kPgnDm2);
    const auto dm1 = bam_messages(frames, kUs, kPgnDm1);
    CHECK_EQ(dm2.size(), 1u);
    CHECK_EQ(dm1.size(), 1u);
    CHECK_EQ(ecu.diag.statistics().send_failures, 0u);
    std::vector<long long> announced;
    for (const auto& s : frames)
    {
        if (s.frame.pgn == kPgnTpCm && s.frame.data[0] == 32)
        {
            announced.push_back(s.ms);
        }
    }
    // DM2 at 950 (2 packets: 1000, 1050; it ends in the step after its last packet), the DM1
    // that was due at 1000 waits in the response queue and starts when it ended (1060).
    CHECK(announced == (std::vector<long long>{950, 1060}));
}

// --- DM3, DM11

TEST_CASE(dm3_clears_the_previously_active_dtcs_and_acknowledges_a_destination_specific_request)
{
    Ecu ecu;
    const Dtc old{91, 2};
    const Dtc now{92, 2};
    CHECK(ecu.diag.set_active(old, true));
    CHECK(ecu.diag.set_active(old, false));
    CHECK(ecu.diag.set_active(now, true));
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    ecu.bench.inject(req(kTool, kUs, kPgnDm3));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {ack_frame(kAck, kTool, kPgnDm3)});
    CHECK(!ecu.diag.is_previously_active(old));
    CHECK(ecu.diag.is_active(now)); // DM3 leaves the active ones
    // To all nodes: cleared, not acknowledged (also with nothing to clear).
    ecu.bench.inject(req(kTool, kGlobal, kPgnDm3));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {});
}

TEST_CASE(dm11_makes_the_active_dtcs_previously_active_and_sends_the_dm1_at_once)
{
    Ecu ecu;
    const Dtc dtc{91, 2, Lamp::AmberWarning};
    CHECK(ecu.diag.set_active(dtc, true));
    ecu.bench.steps(150);
    (void)ecu.bench.take_sent();
    ecu.bench.inject(req(kTool, kUs, kPgnDm11));
    ecu.bench.step();
    // The DM1 of the change goes in the same step (the timer fires at the end of it).
    CHECK_J1939(ecu.bench.take_sent(), {ack_frame(kAck, kTool, kPgnDm11), dm1_frame(kNoDtcs)});
    CHECK(ecu.diag.is_previously_active(dtc));
    ecu.bench.inject(req(kTool, kUs, kPgnDm2));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {frame(6, kPgnDm2, kUs, kGlobal, dm_payload(0x04, 0xFF, {dtc_bytes(91, 2, 1)}))});
}

TEST_CASE(a_clear_the_application_refuses_or_defers_is_acknowledged_accordingly)
{
    std::vector<DtcClear> asked;
    ClearDecision decision = ClearDecision::Refuse;
    DiagnosticsOptions options;
    options.deferred_capacity = 2;
    options.on_clear = [&](DtcClear kind) {
        asked.push_back(kind);
        return decision;
    };
    Ecu ecu(options);
    const Dtc dtc{91, 2};
    CHECK(ecu.diag.set_active(dtc, true));
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();

    ecu.bench.inject(req(kTool, kUs, kPgnDm11));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {ack_frame(kNack, kTool, kPgnDm11)});
    CHECK(ecu.diag.is_active(dtc));

    // Deferred: nothing until complete_clear(); requests meanwhile wait for the same answer
    // (the application is asked once); beyond deferred_capacity "cannot respond".
    decision = ClearDecision::Defer;
    ecu.bench.inject(req(kTool, kUs, kPgnDm11));
    ecu.bench.inject(req(kTool, kGlobal, kPgnDm11));
    ecu.bench.inject(req(kOtherTool, kUs, kPgnDm11));
    ecu.bench.inject(req(0x92, kUs, kPgnDm11));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {ack_frame(kCannotRespond, 0x92, kPgnDm11)});
    CHECK_EQ(ecu.diag.statistics().requests_refused, 1u);
    CHECK(asked == (std::vector<DtcClear>{DtcClear::Active, DtcClear::Active}));
    ecu.bench.steps(10);
    CHECK_J1939(ecu.bench.take_sent(), {});
    CHECK(!ecu.diag.complete_clear(DtcClear::PreviouslyActive, true)); // none deferred
    CHECK(ecu.diag.complete_clear(DtcClear::Active, true));            // between steps
    CHECK(!ecu.diag.is_active(dtc));
    ecu.bench.step();
    // No DM1 now: the DTC's change was reported less than a second ago (its activation).
    CHECK_J1939(ecu.bench.take_sent(), {ack_frame(kAck, kTool, kPgnDm11), ack_frame(kAck, kOtherTool, kPgnDm11)});
    CHECK(!ecu.diag.complete_clear(DtcClear::Active, true));

    // Deferred and failed: negative acknowledgement, nothing cleared.
    CHECK(ecu.diag.set_active(dtc, true));
    ecu.bench.inject(req(kTool, kUs, kPgnDm3));
    ecu.bench.step();
    (void)ecu.bench.take_sent();
    CHECK(ecu.diag.complete_clear(DtcClear::PreviouslyActive, false));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {ack_frame(kNack, kTool, kPgnDm3)});
}

TEST_CASE(complete_clear_called_inside_on_clear_answers_the_request)
{
    Diagnostics* diag = nullptr;
    DiagnosticsOptions options;
    options.on_clear = [&](DtcClear kind) {
        CHECK(diag->complete_clear(kind, true)); // done at once, but reported as deferred
        return ClearDecision::Defer;
    };
    Ecu ecu(options);
    diag = &ecu.diag;
    const Dtc dtc{91, 2};
    CHECK(ecu.diag.set_active(dtc, true));
    CHECK(ecu.diag.set_active(dtc, false));
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    ecu.bench.inject(req(kTool, kUs, kPgnDm3));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {ack_frame(kAck, kTool, kPgnDm3)});
    CHECK(!ecu.diag.is_previously_active(dtc));
    // Not stuck: the next request is asked about again and answered.
    ecu.bench.inject(req(kOtherTool, kUs, kPgnDm3));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {ack_frame(kAck, kOtherTool, kPgnDm3)});
}

TEST_CASE(deferred_clears_are_not_acknowledged_after_the_node_changed_its_address)
{
    Bench bench;
    NodeOptions options;
    options.address = 0x80;
    options.name = Name{0x80008200FE001234u}; // arbitrary address capable
    Node& us = bench.network.add_node(options);
    DiagnosticsOptions diagOptions;
    diagOptions.on_clear = [](DtcClear) { return ClearDecision::Defer; };
    auto& diag = us.attach<Diagnostics>(diagOptions);
    bench.steps(30);
    bench.inject(frame(6, kPgnRequest, kTool, 0x80, request(kPgnDm3)));
    bench.step();
    bench.inject(frame(6, kPgnAddressClaim, 0x80, kGlobal, {0x01, 0, 0, 0, 0, 0, 0, 0}));
    bench.step();
    CHECK_EQ(us.address(), 0x81);
    (void)bench.take_sent();
    CHECK(diag.complete_clear(DtcClear::PreviouslyActive, true));
    const auto frames = record(bench, 2);
    CHECK(only(frames, kPgnAck).empty());
    CHECK(!bench.network.failed());
}

// --- DM22

namespace {

Bytes dm22(std::uint8_t control, std::uint8_t reason, std::uint32_t spn, std::uint8_t fmi)
{
    return {control, reason, 0xFF, 0xFF, 0xFF, static_cast<std::uint8_t>(spn), static_cast<std::uint8_t>(spn >> 8),
            static_cast<std::uint8_t>((((spn >> 16) & 0x7u) << 5) | (fmi & 0x1Fu))};
}
J1939Frame dm22_from(Address sa, Address da, Bytes data) { return frame(6, kPgnDm22, sa, da, std::move(data)); }

} // namespace

TEST_CASE(dm22_clears_one_dtc_and_answers_the_requester)
{
    Ecu ecu;
    const Dtc kept{0x40001, 2};
    const Dtc cleared{0x40002, 4};
    CHECK(ecu.diag.set_active(kept, true));
    CHECK(ecu.diag.set_active(cleared, true));
    ecu.bench.steps(150); // past the first second: the changes are reported at once
    (void)ecu.bench.take_sent();
    ecu.bench.inject(dm22_from(kTool, kUs, dm22(0x11, 0xFF, 0x40002, 4)));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {dm22_from(kUs, kTool, dm22(0x12, 0xFF, 0x40002, 4)),
                                        dm1_frame(dm_payload(0x00, 0xFF, {dtc_bytes(0x40001, 2, 1)}))});
    CHECK(ecu.diag.is_active(kept));
    CHECK(ecu.diag.is_previously_active(cleared));
    ecu.bench.inject(dm22_from(kTool, kUs, dm22(0x01, 0xFF, 0x40002, 4)));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {dm22_from(kUs, kTool, dm22(0x02, 0xFF, 0x40002, 4))});
    CHECK(!ecu.diag.occurrence_count(cleared).has_value());
}

TEST_CASE(dm22_refuses_with_the_reason)
{
    Ecu ecu;
    CHECK(ecu.diag.set_active(Dtc{10, 2}, true));
    CHECK(ecu.diag.set_active(Dtc{11, 2}, true));
    CHECK(ecu.diag.set_active(Dtc{11, 2}, false));
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    const struct
    {
        Bytes request;
        Bytes answer;
    } cases[] = {
        {dm22(0x11, 0xFF, 12, 2), dm22(0x13, 0x02, 12, 2)}, // unknown
        {dm22(0x11, 0xFF, 11, 2), dm22(0x13, 0x04, 11, 2)}, // previously active
        {dm22(0x11, 0xFF, 10, 3), dm22(0x13, 0x02, 10, 3)}, // other FMI
        {dm22(0x01, 0xFF, 12, 2), dm22(0x03, 0x02, 12, 2)},
        {dm22(0x01, 0xFF, 10, 2), dm22(0x03, 0x03, 10, 2)}, // active
    };
    for (const auto& c : cases)
    {
        ecu.bench.inject(dm22_from(kTool, kUs, c.request));
        ecu.bench.step();
        CHECK_J1939(ecu.bench.take_sent(), {dm22_from(kUs, kTool, c.answer)});
    }
}

TEST_CASE(dm22_malformed_to_all_nodes_or_to_another_node_is_ignored_and_several_are_answered_in_order)
{
    Ecu ecu;
    CHECK(ecu.diag.set_active(Dtc{10, 2}, true));
    CHECK(ecu.diag.set_active(Dtc{11, 2}, true));
    ecu.bench.steps(150); // past the first second and its DM1 broadcast
    (void)ecu.bench.take_sent();
    Bytes shortOne = dm22(0x11, 0xFF, 10, 2);
    shortOne.resize(7);
    ecu.bench.inject(dm22_from(kTool, kUs, shortOne));
    ecu.bench.inject(dm22_from(kTool, kGlobal, dm22(0x11, 0xFF, 10, 2)));
    ecu.bench.inject(dm22_from(kTool, 0x91, dm22(0x11, 0xFF, 10, 2)));
    ecu.bench.inject(dm22_from(kTool, kUs, dm22(0x20, 0xFF, 10, 2))); // other control byte
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {});
    CHECK(ecu.diag.is_active(Dtc{10, 2}));

    // In the order the step handles them (by time, then sender): 0x90, 0x90 (a duplicate that
    // finds the DTC previously active), 0x91.
    ecu.bench.inject(dm22_from(kTool, kUs, dm22(0x11, 0xFF, 10, 2)));
    ecu.bench.inject(dm22_from(kOtherTool, kUs, dm22(0x11, 0xFF, 11, 2)));
    ecu.bench.inject(dm22_from(kTool, kUs, dm22(0x11, 0xFF, 10, 2)));
    ecu.bench.step();
    const auto sent = ecu.bench.take_sent();
    CHECK_EQ(sent.size(), 4u);
    CHECK(sent[0] == dm22_from(kUs, kTool, dm22(0x12, 0xFF, 10, 2)));
    CHECK(sent[1] == dm22_from(kUs, kTool, dm22(0x13, 0x04, 10, 2)));
    CHECK(sent[2] == dm22_from(kUs, kOtherTool, dm22(0x12, 0xFF, 11, 2)));
    CHECK(sent[3].pgn == kPgnDm1);
}

// --- requests and the claim

TEST_CASE(requests_before_the_wait_after_the_claim_ended_are_answered_when_it_ends)
{
    Bench bench;
    NodeOptions options;
    options.address = 0x80;
    options.name = Name{0x20008200FE001234u};
    Node& us = bench.network.add_node(options);
    us.attach<Diagnostics>();
    bench.inject(at(100ms), req(kTool, kGlobal, kPgnDm1));
    bench.inject(at(100ms), req(kTool, 0x80, kPgnDm3));
    const auto frames = record(bench, 30);
    // Held during the 250 ms after the claim, sent when it ends; the periodic DM1 follows.
    std::vector<Sent> after;
    for (const auto& s : frames)
    {
        if (s.frame.pgn != kPgnAddressClaim)
        {
            after.push_back(s);
        }
    }
    CHECK_EQ(after.size(), 3u);
    CHECK_EQ(after[0].ms, 250);
    CHECK(after[0].frame == frame(6, kPgnAck, 0x80, kGlobal, acknowledgement(kAck, kTool, kPgnDm3)));
    CHECK(after[1].frame == frame(6, kPgnDm1, 0x80, kGlobal, kNoDtcs));
    CHECK_EQ(after[1].ms, 250);
    CHECK_EQ(after[2].ms, 260);
}

TEST_CASE(dm1_requests_from_many_nodes_in_one_step_get_one_dm1)
{
    Ecu ecu;
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    std::vector<J1939Frame> expected;
    for (std::uint8_t i = 0; i < 50; ++i)
    {
        ecu.bench.inject(req(static_cast<Address>(0x10 + i), kUs, kPgnDm1));
        ecu.bench.inject(req(static_cast<Address>(0x10 + i), kUs, kPgnDm3));
    }
    ecu.bench.step();
    const auto sent = ecu.bench.take_sent();
    std::size_t acks = 0;
    std::size_t dm1 = 0;
    for (const auto& f : sent)
    {
        acks += f.pgn == kPgnAck ? 1u : 0u;
        dm1 += f.pgn == kPgnDm1 ? 1u : 0u;
    }
    CHECK_EQ(acks, 50u);
    CHECK_EQ(dm1, 1u);
}

// --- determinism

namespace {

/// DTC changes from Timers at odd times, DM1 requests at frame times, DM13 stop and start: what
/// the node sends, each frame with its time rounded up to 50 ms (every step size used divides
/// 50 ms, so the rounded times do not depend on it).
std::vector<std::string> diagnostics_trace(SimDuration step)
{
    Ecu ecu({}, step);
    const Dtc dtc{100, 2, Lamp::RedStop};
    Timer on(ecu.node, vehicle_sil::j1939::TimerKind::Scheduled, [&] { CHECK(ecu.diag.set_active(dtc, true)); });
    Timer off(ecu.node, vehicle_sil::j1939::TimerKind::Scheduled, [&] { CHECK(ecu.diag.set_active(dtc, false)); });
    Timer second(ecu.node, vehicle_sil::j1939::TimerKind::Scheduled, [&] { CHECK(ecu.diag.set_active(Dtc{200, 3}, true)); });
    on.start_at(at(109500us));
    second.start_at(at(600ms)); // moves the period away from the deactivation
    off.start_at(at(1100500us)); // 0.991 s after the activation: no DM1 of its own
    auto& b = ecu.bench;
    b.inject(at(2001ms), req(kTool, kUs, kPgnDm1));
    b.inject(at(2009ms), req(kOtherTool, kGlobal, kPgnDm1));
    b.inject(at(2009ms), req(kTool, kUs, kPgnDm1)); // the same time: one answer
    b.inject(at(2500ms), frame(6, kPgnDm13, kTool, kGlobal, dm13(kNetwork1, kStop)));
    b.inject(at(4200ms), frame(6, kPgnDm13, kTool, kGlobal, dm13(kNetwork1, kStart)));
    std::vector<std::string> out;
    for (const auto& s : record(b, static_cast<int>(6s / step)))
    {
        out.push_back(to_string(s.frame) + " @" + std::to_string((s.ms + 49) / 50 * 50));
    }
    return out;
}

} // namespace

TEST_CASE(what_the_diagnostics_send_does_not_depend_on_the_step_size)
{
    const auto reference = diagnostics_trace(1ms);
    // DM1 at 0, 109.5 (change), 600 (another DTC: two DTCs, a BAM of 3 frames), 1600 (period), the answers at 2.001 and
    // 2.009 s, nothing while stopped (2.5 .. 4.2 s), 4600, 5600. Not at 1100.5: within a second
    // of the DTC's activation (it shows in the DM1 at 1600).
    CHECK_EQ(reference.size(), 10u);
    for (const SimDuration step : {SimDuration{2ms}, SimDuration{5ms}, SimDuration{10ms}, SimDuration{25ms}})
    {
        const auto trace = diagnostics_trace(step);
        if (trace != reference)
        {
            std::string text = "step " + std::to_string(step.count()) + " ns:";
            for (std::size_t i = 0; i < trace.size(); ++i)
            {
                text += "\n  " + trace[i] + (i < reference.size() && trace[i] == reference[i] ? "" : "   <- differs");
            }
            throw vehicle_sil::unit::Failure(text);
        }
    }
}

TEST_CASE(dm1_broadcasts_again_after_its_bam_was_cancelled_by_an_address_change)
{
    Bench bench;
    NodeOptions options;
    options.address = 0x80;
    options.name = Name{0x80008200FE001234u}; // arbitrary address capable
    Node& us = bench.network.add_node(options);
    auto& diag = us.attach<Diagnostics>();
    CHECK(diag.set_active(Dtc{1, 2}, true));
    CHECK(diag.set_active(Dtc{2, 2}, true)); // 10 bytes: BAM
    auto frames = record(bench, 27);         // Claimed at 250, the BAM starts at 260
    CHECK_EQ(frames.back().ms, 260);
    CHECK(frames.back().frame == bam(0x80, 10, kPgnDm1));
    // A lower NAME takes 0x80 while the BAM runs: it ends Cancelled, the node moves to 0x81.
    bench.inject(frame(6, kPgnAddressClaim, 0x80, kGlobal, {0x01, 0, 0, 0, 0, 0, 0, 0}));
    const auto more = record(bench, 200);
    frames.insert(frames.end(), more.begin(), more.end());
    CHECK_EQ(us.address(), 0x81);
    CHECK(bam_messages(frames, 0x80, kPgnDm1).empty()); // the first one never completed
    const auto messages = bam_messages(frames, 0x81, kPgnDm1);
    CHECK(messages.size() >= 1u); // from the new address once it is claimed
    CHECK_EQ(diag.statistics().send_failures, 0u);
}

TEST_CASE(a_dtc_changed_from_a_timer_with_an_old_deadline_does_not_flood_dm1)
{
    Ecu ecu;
    ecu.bench.steps(1001); // .. 10 s
    (void)ecu.bench.take_sent();
    Timer late(ecu.node, vehicle_sil::j1939::TimerKind::Scheduled, [&] { CHECK(ecu.diag.set_active(Dtc{7, 1}, true)); });
    late.start_at(at(0ms)); // long past: its event time is 0
    const auto dm1 = only(record(ecu.bench, 300), kPgnDm1);
    // The timer fires at 10010, its change at the next chance (10020), then one DM1 a second
    // from there (not one per step to catch up from 0).
    CHECK(times_of(dm1) == (std::vector<long long>{10020, 11020, 12020}));
}

// --- Reading a DTC list (parse_dtc_list)

namespace {

/// The DM1s the node sent in `frames`: single frames and broadcasts (BAM), in the order sent.
[[nodiscard]] std::vector<Bytes> dm1_data(const std::vector<Sent>& frames)
{
    std::vector<Bytes> out;
    for (const auto& s : only(frames, kPgnDm1))
    {
        out.push_back(s.frame.data);
    }
    for (auto& m : bam_messages(frames, kUs, kPgnDm1))
    {
        out.push_back(std::move(m));
    }
    return out;
}

[[nodiscard]] std::vector<ReadDtc> dtcs_of(const DtcListView& list)
{
    std::vector<ReadDtc> out;
    for (const ReadDtc& dtc : list)
    {
        out.push_back(dtc);
    }
    return out;
}

[[nodiscard]] ReadDtc read_dtc(std::uint32_t spn, std::uint8_t fmi, std::uint8_t occurrences)
{
    const Bytes b = dtc_bytes(spn, fmi, occurrences);
    return ReadDtc{spn, fmi, occurrences, false, {b[0], b[1], b[2], b[3]}};
}

constexpr std::array kLamps = {Lamp::MalfunctionIndicator, Lamp::RedStop, Lamp::AmberWarning, Lamp::Protect};

} // namespace

// A view: it holds the span it reads (and what it found), copies for free, and iterates
// without a container.
static_assert(std::is_trivially_copyable_v<DtcListView>);
static_assert(std::is_trivially_destructible_v<DtcListView>);
static_assert(std::is_nothrow_default_constructible_v<DtcListView>);
static_assert(std::forward_iterator<DtcListView::iterator>);
static_assert(std::ranges::forward_range<const DtcListView>);
static_assert(std::is_same_v<std::iter_value_t<DtcListView::iterator>, ReadDtc>);
static_assert(noexcept(parse_dtc_list(std::span<const std::uint8_t>{})));

TEST_CASE(a_dtc_list_without_dtcs_reads_back_as_valid_and_empty)
{
    Ecu ecu;
    const auto sent = dm1_data(record(ecu.bench, 1));
    CHECK_EQ(sent.size(), 1u);
    CHECK(sent[0] == kNoDtcs);
    const DtcListView list = parse_dtc_list(sent[0]);
    CHECK(list.valid());
    CHECK_EQ(list.size(), 0u);
    CHECK(list.empty());
    CHECK(list.begin() == list.end());
    CHECK(!list.has(0, 0));
    for (const Lamp lamp : kLamps)
    {
        CHECK(!list.lamp(lamp).has_value());
    }
    CHECK_EQ(list.data().size(), 8u);
}

TEST_CASE(a_dtc_list_with_one_dtc_reads_back_what_diagnostics_sent)
{
    Ecu ecu;
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    // Its occurrence count is 2: active, inactive, active again.
    CHECK(ecu.diag.set_active(Dtc{0x6D2B1, 3, Lamp::AmberWarning, LampFlash::Slow}, true));
    CHECK(ecu.diag.set_active(Dtc{0x6D2B1, 3}, false));
    CHECK(ecu.diag.set_active(Dtc{0x6D2B1, 3, Lamp::AmberWarning, LampFlash::Slow}, true));
    const auto sent = dm1_data(record(ecu.bench, 1));
    CHECK_EQ(sent.size(), 1u);
    CHECK_EQ(sent[0].size(), 8u); // one frame, padded
    const DtcListView list = parse_dtc_list(sent[0]);
    CHECK(list.valid());
    CHECK_EQ(list.size(), 1u);
    CHECK(dtcs_of(list) == std::vector<ReadDtc>{read_dtc(0x6D2B1, 3, 2)});
    CHECK(list.has(0x6D2B1, 3));
    CHECK(!list.has(0x6D2B1, 4));
    CHECK(!list.has(0x0D2B1, 3)); // the top 3 bits of the SPN count
    CHECK(list.lamp(Lamp::AmberWarning) == std::optional<LampFlash>(LampFlash::Slow));
    CHECK(!list.lamp(Lamp::MalfunctionIndicator).has_value());
    CHECK(!list.lamp(Lamp::RedStop).has_value());
    CHECK(!list.lamp(Lamp::Protect).has_value());
    CHECK(!list.lamp(Lamp::None).has_value());
}

TEST_CASE(a_dtc_list_with_several_dtcs_and_all_lamps_reads_back_what_diagnostics_sent)
{
    Ecu ecu;
    ecu.bench.steps(5);
    (void)ecu.bench.take_sent();
    const std::vector<Dtc> dtcs = {Dtc{0x7F001, 2, Lamp::MalfunctionIndicator, LampFlash::Slow},
                                   Dtc{0x7F002, 31, Lamp::RedStop, LampFlash::Fast}, Dtc{0x7F003, 0, Lamp::AmberWarning, LampFlash::Solid},
                                   Dtc{0x7F004, 9, Lamp::Protect, LampFlash::Fast}, Dtc{0x7F005, 9, Lamp::Protect, LampFlash::Slow},
                                   Dtc{0x7FFFF, 17, Lamp::None}};
    std::vector<ReadDtc> expected;
    for (const auto& dtc : dtcs)
    {
        CHECK(ecu.diag.set_active(dtc, true));
        expected.push_back(read_dtc(dtc.spn, dtc.fmi, 1));
    }
    const auto sent = dm1_data(record(ecu.bench, 30));
    CHECK_EQ(sent.size(), 1u);
    CHECK_EQ(sent[0].size(), 2u + 4u * dtcs.size()); // a broadcast (BAM), not padded
    const DtcListView list = parse_dtc_list(sent[0]);
    CHECK(list.valid());
    CHECK_EQ(list.size(), dtcs.size());
    CHECK(dtcs_of(list) == expected);
    CHECK_EQ(static_cast<std::size_t>(std::ranges::distance(list)), dtcs.size());
    for (const auto& dtc : dtcs)
    {
        CHECK(list.has(dtc.spn, dtc.fmi));
    }
    CHECK(!list.has(0x7F006, 2));
    // Each lamp as Diagnostics combined it (fast > slow > solid).
    CHECK(list.lamp(Lamp::MalfunctionIndicator) == std::optional<LampFlash>(LampFlash::Slow));
    CHECK(list.lamp(Lamp::RedStop) == std::optional<LampFlash>(LampFlash::Fast));
    CHECK(list.lamp(Lamp::AmberWarning) == std::optional<LampFlash>(LampFlash::Solid));
    CHECK(list.lamp(Lamp::Protect) == std::optional<LampFlash>(LampFlash::Fast));
    CHECK(!list.lamp(Lamp::None).has_value());
}

TEST_CASE(a_dm2_and_the_longest_list_read_back)
{
    Ecu ecu;
    ecu.bench.steps(5);
    CHECK(ecu.diag.set_active(Dtc{0x7F0, 4, Lamp::RedStop, LampFlash::Fast}, true));
    CHECK(ecu.diag.set_active(Dtc{0x7F0, 4}, false)); // previously active
    (void)ecu.bench.take_sent();
    ecu.bench.inject(req(kTool, kUs, kPgnDm2));
    const auto dm2 = only(record(ecu.bench, 5), kPgnDm2);
    CHECK_EQ(dm2.size(), 1u);
    const DtcListView list = parse_dtc_list(dm2[0].frame.data);
    CHECK(dtcs_of(list) == std::vector<ReadDtc>{read_dtc(0x7F0, 4, 1)});
    CHECK(list.lamp(Lamp::RedStop) == std::optional<LampFlash>(LampFlash::Fast));

    std::vector<Bytes> encoded;
    std::vector<ReadDtc> expected;
    for (std::uint32_t spn = 1; spn <= vehicle_sil::j1939::kMaxDtcs; ++spn)
    {
        encoded.push_back(dtc_bytes(spn * 1171u, static_cast<std::uint8_t>(spn % 32), static_cast<std::uint8_t>(spn % 127)));
        expected.push_back(read_dtc(spn * 1171u, static_cast<std::uint8_t>(spn % 32), static_cast<std::uint8_t>(spn % 127)));
    }
    const Bytes longest = dm_payload(0x00, 0xFF, encoded);
    const DtcListView all = parse_dtc_list(longest);
    CHECK(all.valid());
    CHECK(dtcs_of(all) == expected);
}

TEST_CASE(a_dtc_list_of_any_length_is_read_without_failing)
{
    // Every length up to 41 bytes, with zeros, 0xFF and a mixed pattern: the list is valid for
    // 6 .. 8 bytes (one DTC) and 2 + 4 * n bytes, and then reads exactly that many DTCs.
    for (std::size_t length = 0; length <= 41; ++length)
    {
        for (const int fill : {0x00, 0xFF, -1})
        {
            Bytes data(length);
            for (std::size_t i = 0; i < length; ++i)
            {
                data[i] = static_cast<std::uint8_t>(fill >= 0 ? fill : static_cast<int>(i * 37 + 11));
            }
            const DtcListView list = parse_dtc_list(data);
            const bool shaped = length >= 6 && (length <= 8 || (length - 2) % 4 == 0);
            CHECK_EQ(list.valid(), shaped);
            const std::size_t dtcs = !shaped ? 0 : (fill == 0x00 && length <= 8) ? 0 : (length - 2) / 4;
            CHECK_EQ(list.size(), dtcs);
            CHECK_EQ(dtcs_of(list).size(), dtcs);
            CHECK_EQ(list.data().size(), length);
            if (!shaped)
            {
                CHECK(list.begin() == list.end());
                CHECK(!list.has(0, 0));
                for (const Lamp lamp : kLamps)
                {
                    CHECK(!list.lamp(lamp).has_value());
                }
            }
        }
    }
    // Zero DTCs among others are DTCs; only a list whose single DTC is 4 zero bytes is empty.
    const Bytes twoZeros = dm_payload(0x00, 0xFF, {Bytes{0, 0, 0, 0}, Bytes{0, 0, 0, 0}});
    CHECK_EQ(parse_dtc_list(twoZeros).size(), 2u);
    const DtcListView none;
    CHECK(!none.valid());
    CHECK_EQ(none.size(), 0u);
    CHECK(none.begin() == none.end());
}

TEST_CASE(the_top_bit_of_the_fourth_byte_is_passed_on_raw)
{
    // Not interpreted: the occurrence count is the low 7 bits, the top bit and the four bytes
    // come as they were.
    Bytes data = dm_payload(0x04, 0xFF, {dtc_bytes(0x6D2B1, 3, 5)});
    data[5] = 0x85;
    const DtcListView list = parse_dtc_list(data);
    CHECK(list.valid());
    const auto dtcs = dtcs_of(list);
    CHECK_EQ(dtcs.size(), 1u);
    CHECK_EQ(dtcs[0].spn, 0x6D2B1u);
    CHECK_EQ(dtcs[0].fmi, 3);
    CHECK_EQ(dtcs[0].occurrences, 5);
    CHECK(dtcs[0].byte4_high_bit);
    CHECK(dtcs[0].raw == (std::array<std::uint8_t, 4>{0xB1, 0xD2, 0xC3, 0x85}));
    // With only the top bit set the DTC is not the empty list's 4 zero bytes.
    const Bytes topBitOnly = dm_payload(0x00, 0xFF, {Bytes{0, 0, 0, 0x80}});
    const auto topBit = dtcs_of(parse_dtc_list(topBitOnly));
    CHECK_EQ(topBit.size(), 1u);
    CHECK_EQ(topBit[0].spn, 0u);
    CHECK_EQ(topBit[0].occurrences, 0);
    CHECK(topBit[0].byte4_high_bit);
    // What Diagnostics sends has it clear (also at the most occurrences).
    Ecu ecu;
    for (int i = 0; i < 130; ++i)
    {
        CHECK(ecu.diag.set_active(Dtc{523, 6}, true));
        CHECK(ecu.diag.set_active(Dtc{523, 6}, false));
    }
    CHECK(ecu.diag.set_active(Dtc{523, 6}, true));
    const auto sent = dm1_data(record(ecu.bench, 1));
    CHECK_EQ(sent.size(), 1u);
    const auto read = dtcs_of(parse_dtc_list(sent[0]));
    CHECK_EQ(read.size(), 1u);
    CHECK_EQ(read[0].occurrences, 126);
    CHECK(!read[0].byte4_high_bit);
}

TEST_CASE(lamp_values_diagnostics_does_not_send_read_as_off_or_solid)
{
    // Status: MIL 1 (on), red stop 2, amber 3 (both off), protect 1 (on).
    // Flash: MIL 2 (solid), protect 0 (slow).
    const Bytes data = dm_payload(0x6D, 0xBC, {dtc_bytes(1, 1, 1)});
    const DtcListView list = parse_dtc_list(data);
    CHECK(list.lamp(Lamp::MalfunctionIndicator) == std::optional<LampFlash>(LampFlash::Solid));
    CHECK(!list.lamp(Lamp::RedStop).has_value());
    CHECK(!list.lamp(Lamp::AmberWarning).has_value());
    CHECK(list.lamp(Lamp::Protect) == std::optional<LampFlash>(LampFlash::Slow));
    CHECK_EQ(list.data()[0], 0x6D); // the raw bytes for another reading
    CHECK_EQ(list.data()[1], 0xBC);
}
