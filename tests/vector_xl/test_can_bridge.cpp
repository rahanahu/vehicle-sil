// SPDX-License-Identifier: MIT
//
// CanBridge in isolation: fake SIL Kit controller on one side, fake external port on the other.
#include "fake_can_controller.hpp"
#include "test_harness.hpp"
#include "fake_external_port.hpp"
#include "frame_matrix.hpp"

#include "vehicle_sil/vector_xl/can_bridge.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace can = SilKit::Services::Can;
using vehicle_sil::test::frame_matrix;
using vehicle_sil::unit::FakeCanController;
using vehicle_sil::unit::FakeExternalPort;
using vehicle_sil::unit::record;
using vehicle_sil::unit::flag;
using vehicle_sil::unit::make_frame;
using vehicle_sil::unit::RecordedFrame;
using vehicle_sil::unit::throws;
using vehicle_sil::vector_xl::CanBridge;
using vehicle_sil::vector_xl::CanBridgeHealth;
using vehicle_sil::vector_xl::ExternalBusState;
using vehicle_sil::vector_xl::ExternalCanEvent;
using vehicle_sil::vector_xl::ExternalEventKind;
using vehicle_sil::vector_xl::format_health_change;
using vehicle_sil::vector_xl::problems_grew;
using vehicle_sil::vector_xl::TransmitResult;
using vehicle_sil::vector_xl::validate_classic_frame;

} // namespace

TEST_CASE(bridge_validate_classic_frame)
{
    const std::vector<std::uint8_t> eight(8, 1);
    const std::vector<std::uint8_t> three(3, 1);
    const std::vector<std::uint8_t> none;
    const auto ide = flag(can::CanFrameFlag::Ide);
    const auto rtr = flag(can::CanFrameFlag::Rtr);

    CHECK(validate_classic_frame(make_frame(0x7FF, 0, 3, three)).empty());
    CHECK(validate_classic_frame(make_frame(0x1FFFFFFF, ide, 8, eight)).empty());
    CHECK(validate_classic_frame(make_frame(0x100, rtr, 8, none)).empty());
    CHECK(validate_classic_frame(make_frame(0x100, 0, 15, eight)).empty()); // DLC 9..15 = 8 bytes

    CHECK(!validate_classic_frame(make_frame(0x800, 0, 3, three)).empty());
    CHECK(!validate_classic_frame(make_frame(0x20000000, ide, 3, three)).empty());
    CHECK(!validate_classic_frame(make_frame(0x100, 0, 4, three)).empty());
    CHECK(!validate_classic_frame(make_frame(0x100, rtr, 3, three)).empty());
    CHECK(!validate_classic_frame(make_frame(0x100, 0, 16, eight)).empty());
    CHECK(!validate_classic_frame(make_frame(0x100, flag(can::CanFrameFlag::Fdf), 8, eight)).empty());
    CHECK(!validate_classic_frame(make_frame(0x100, flag(can::CanFrameFlag::Xlf), 8, eight)).empty());
    CHECK(!validate_classic_frame(make_frame(0x100, flag(can::CanFrameFlag::Brs), 8, eight)).empty());
}

TEST_CASE(bridge_start_and_stop_manage_both_sides)
{
    FakeCanController controller;
    FakeExternalPort port;
    {
        CanBridge bridge(controller, port);
        CHECK(!bridge.running());
        bridge.start();
        CHECK(bridge.running());
        CHECK(port.started());
        CHECK_EQ(controller.frame_handler_count(), 1u);
        const std::vector<std::uint8_t> one{1};
        // The controller waits for the participant's CommunicationReady. Until then, a frame from
        // the external bus is not given to SIL Kit (it would drop it without telling).
        CHECK(!controller.started());
        port.deliver(ExternalEventKind::Frame, make_frame(0x123, 0, 1, one));
        CHECK(controller.sent().empty());
        CHECK_EQ(bridge.statistics().external_to_silkit, 0u);
        bridge.start_controller();
        CHECK(controller.started());
        bridge.start_controller(); // a second time: nothing
        port.deliver(ExternalEventKind::Frame, make_frame(0x123, 0, 1, one));
        CHECK_EQ(controller.sent().size(), 1u);
        CHECK_EQ(bridge.statistics().external_to_silkit, 1u);

        bridge.stop();
        CHECK(!bridge.running());
        CHECK(!port.started());
        CHECK(!controller.started());
        CHECK_EQ(controller.frame_handler_count(), 0u);
        bridge.stop(); // idempotent
        CHECK_EQ(port.stop_calls(), 1);

        bridge.start(); // restartable; after CommunicationReady it starts the controller itself
        CHECK(bridge.running());
        CHECK(controller.started());
    } // destructor stops
    CHECK(!port.started());
    CHECK(!controller.started());
    CHECK_EQ(controller.frame_handler_count(), 0u);

    // CommunicationReady before start(): start() starts the controller.
    FakeCanController early;
    FakeExternalPort earlyPort;
    CanBridge bridge(early, earlyPort);
    bridge.start_controller();
    CHECK(!early.started());
    bridge.start();
    CHECK(early.started());
}

TEST_CASE(bridge_start_failure_leaves_nothing_registered)
{
    FakeCanController controller;
    FakeExternalPort port;
    port.fail_start = true;
    CanBridge bridge(controller, port);
    CHECK(throws<std::runtime_error>([&] { bridge.start(); }));
    CHECK(!bridge.running());
    CHECK_EQ(controller.frame_handler_count(), 0u);
    CHECK(!controller.started());

    FakeCanController failingController;
    failingController.throw_on_start = true;
    FakeExternalPort port2;
    CanBridge bridge2(failingController, port2);
    bridge2.start();
    // A controller that cannot be started: a fault (the executable ends), not an exception out of
    // SIL Kit's handler.
    bridge2.start_controller();
    CHECK(bridge2.fault() == std::optional<std::string>("starting the SIL Kit CAN controller failed: fake Start failure"));
    CHECK(bridge2.last_error() == "starting the SIL Kit CAN controller failed: fake Start failure");
    bridge2.stop();
    // After CommunicationReady, start() starts the controller and throws if that fails.
    CHECK(throws<std::runtime_error>([&] { bridge2.start(); }));
    CHECK(!port2.started()); // external side rolled back
    CHECK_EQ(failingController.frame_handler_count(), 0u);
}

TEST_CASE(bridge_forwards_silkit_to_external_preserving_every_field)
{
    FakeCanController controller;
    FakeExternalPort port;
    CanBridge bridge(controller, port);
    bridge.start();
    bridge.start_controller();

    const auto matrix = frame_matrix();
    for (const auto& frame : matrix)
    {
        controller.deliver(frame.to_silkit());
    }
    const auto sent = port.transmitted();
    CHECK_EQ(sent.size(), matrix.size());
    for (std::size_t i = 0; i < matrix.size(); ++i)
    {
        CHECK(sent[i] == record(matrix[i].to_silkit()));
    }
    CHECK(controller.sent().empty());
    CHECK_EQ(bridge.statistics().silkit_to_external, matrix.size());
    CHECK_EQ(bridge.statistics().external_to_silkit, 0u);
}

TEST_CASE(bridge_forwards_external_to_silkit_preserving_every_field)
{
    FakeCanController controller;
    FakeExternalPort port;
    CanBridge bridge(controller, port);
    bridge.start();
    bridge.start_controller();

    const auto matrix = frame_matrix();
    for (const auto& frame : matrix)
    {
        port.deliver(ExternalEventKind::Frame, frame.to_silkit());
    }
    const auto sent = controller.sent();
    CHECK_EQ(sent.size(), matrix.size());
    for (std::size_t i = 0; i < matrix.size(); ++i)
    {
        CHECK(sent[i] == record(matrix[i].to_silkit()));
    }
    CHECK(port.transmitted().empty());
    CHECK_EQ(bridge.statistics().external_to_silkit, matrix.size());
}

TEST_CASE(bridge_never_forwards_own_external_transmissions)
{
    FakeCanController controller;
    FakeExternalPort port;
    port.report_own_transmissions = true; // driver echoes our frames as TX receipts
    CanBridge bridge(controller, port);
    bridge.start();
    bridge.start_controller();

    const auto matrix = frame_matrix();
    for (const auto& frame : matrix)
    {
        controller.deliver(frame.to_silkit());
    }
    CHECK_EQ(port.transmitted().size(), matrix.size());
    CHECK(controller.sent().empty()); // nothing came back into SIL Kit
    const auto stats = bridge.statistics();
    CHECK_EQ(stats.own_transmissions_dropped, matrix.size());
    CHECK_EQ(stats.external_to_silkit, 0u);
}

TEST_CASE(bridge_rejects_frames_it_cannot_carry)
{
    FakeCanController controller;
    FakeExternalPort port;
    CanBridge bridge(controller, port);
    bridge.start();
    bridge.start_controller();

    const std::vector<std::uint8_t> eight(8, 0x11);
    const std::vector<std::uint8_t> twelve(12, 0x22);
    controller.deliver(make_frame(0x100, flag(can::CanFrameFlag::Fdf), 12, twelve));
    controller.deliver(make_frame(0x100, flag(can::CanFrameFlag::Xlf), 8, eight));
    controller.deliver(make_frame(0x900, 0, 8, eight));
    CHECK(port.transmitted().empty());
    CHECK_EQ(bridge.statistics().silkit_rejected, 3u);
    CHECK(!bridge.last_error().empty());

    port.deliver(ExternalEventKind::Frame, make_frame(0x100, 0, 4, eight)); // DLC/data mismatch
    CHECK(controller.sent().empty());
    CHECK_EQ(bridge.statistics().external_rejected, 1u);
}

TEST_CASE(bridge_counts_error_frames_and_overruns_without_forwarding)
{
    FakeCanController controller;
    FakeExternalPort port;
    CanBridge bridge(controller, port);
    bridge.start();
    bridge.start_controller();
    port.deliver(ExternalEventKind::ErrorFrame);
    port.deliver(ExternalEventKind::ErrorFrame);
    port.deliver(ExternalEventKind::Overrun);
    CHECK(controller.sent().empty());
    const auto stats = bridge.statistics();
    CHECK_EQ(stats.external_error_frames, 2u);
    CHECK_EQ(stats.external_overruns, 1u);
}

TEST_CASE(bridge_keeps_the_bus_state_and_the_fault)
{
    FakeCanController controller;
    FakeExternalPort port;
    CanBridge bridge(controller, port);
    bridge.start();
    bridge.start_controller();
    CHECK(bridge.health().bus_state.level == ExternalBusState::Level::Unknown);
    CHECK(!bridge.fault());

    ExternalCanEvent state{ExternalEventKind::BusState, {}, {}};
    state.bus_state = {ExternalBusState::Level::ErrorPassive, 128, 3};
    port.deliver_event(state);
    CHECK(bridge.health().bus_state == state.bus_state);
    CHECK(!bridge.fault());

    // The reason comes with the event, not from the port's last_error().
    ExternalCanEvent fault{ExternalEventKind::Fault, {}, {}};
    fault.detail = "channel gone";
    port.deliver_event(fault);
    CHECK(bridge.fault() == std::optional<std::string>("channel gone"));
    CHECK(bridge.last_error() == "channel gone");
    CHECK(bridge.running()); // the caller decides to stop

    // Without a reason the port's last_error() is not used either.
    port.deliver(ExternalEventKind::Fault);
    CHECK(bridge.fault() == std::optional<std::string>("the external port stopped working"));

    ExternalCanEvent warning{ExternalEventKind::Warning, {}, {}};
    warning.detail = "not monitored";
    port.deliver_event(warning);
    CHECK(bridge.take_warnings() == std::vector<std::string>{"not monitored"});
    CHECK(bridge.take_warnings().empty());

    // A new start forgets the old run.
    bridge.stop();
    bridge.start();
    CHECK(!bridge.fault());
    CHECK(bridge.health().bus_state.level == ExternalBusState::Level::Unknown);
}

TEST_CASE(bridge_health_change_line)
{
    using Level = ExternalBusState::Level;
    CanBridgeHealth before;
    before.bus_state = {Level::ErrorActive, 0, 0};
    CHECK(!format_health_change(before, before));

    // Forwarded and own-transmission counts are not problems.
    CanBridgeHealth now = before;
    now.statistics.silkit_to_external = 10;
    now.statistics.external_to_silkit = 10;
    now.statistics.own_transmissions_dropped = 10;
    CHECK(!format_health_change(before, now));

    now.statistics.external_error_frames = 3650;
    now.statistics.external_transmit_failures = 2;
    now.bus_state = {Level::ErrorPassive, 128, 0};
    CHECK(format_health_change(before, now) ==
          std::optional<std::string>("XL bus: error passive (tx errors 128, rx errors 0); since the last report: "
                                     "error_frames +3650, xl_tx_failures +2"));

    CHECK(problems_grew(before.statistics, now.statistics));
    CHECK(!problems_grew(now.statistics, now.statistics));

    // A state change alone is reported, without counters that did not grow.
    CanBridgeHealth recovered = now;
    recovered.bus_state = {Level::ErrorActive, 5, 0};
    CHECK(format_health_change(now, recovered) == std::optional<std::string>("XL bus: error active (tx errors 5, rx errors 0)"));
    CHECK(!format_health_change(recovered, recovered));

    // The problems stopped while the state stays: reported once, after a sample that had grown.
    CHECK(format_health_change(now, now, true) ==
          std::optional<std::string>("XL bus: error passive (tx errors 128, rx errors 0); no new problems since the last report"));
    CHECK(!format_health_change(now, now, false));

    // The first answer of a healthy bus is not news; any other first answer is.
    CanBridgeHealth unknown;
    CanBridgeHealth healthy;
    healthy.bus_state = {Level::ErrorActive, 0, 0};
    CHECK(!format_health_change(unknown, healthy));
    CanBridgeHealth passive;
    passive.bus_state = {Level::ErrorPassive, 0, 128};
    CHECK(format_health_change(unknown, passive) == std::optional<std::string>("XL bus: error passive (tx errors 0, rx errors 128)"));

    // Every problem counter is named; an unknown state has no counters.
    CanBridgeHealth grown;
    grown.statistics.external_overruns = 1;
    grown.statistics.silkit_rejected = 1;
    grown.statistics.external_rejected = 1;
    grown.statistics.external_queue_full = 1;
    grown.statistics.silkit_transmit_failures = 1;
    CHECK(format_health_change(unknown, grown) ==
          std::optional<std::string>("XL bus: state unknown; since the last report: overruns +1, silkit_rejected +1, "
                                     "xl_rejected +1, xl_queue_full +1, silkit_tx_failures +1"));
}

TEST_CASE(bridge_counts_dropped_and_unsent_transmissions)
{
    FakeCanController controller;
    FakeExternalPort port;
    CanBridge bridge(controller, port);
    bridge.start();
    bridge.start_controller();
    port.result = TransmitResult::Dropped;
    const std::vector<std::uint8_t> data{1, 2};
    for (int i = 0; i < 7; ++i)
    {
        controller.deliver(make_frame(0x10, 0, 2, data));
    }
    ExternalCanEvent unsent{ExternalEventKind::TransmitUnsent, {}, {}};
    unsent.count = 3;
    port.deliver_event(unsent);
    const auto stats = bridge.statistics();
    CHECK_EQ(stats.external_transmit_dropped, 7u);
    CHECK_EQ(stats.external_transmit_unsent, 3u);
    CHECK_EQ(stats.silkit_to_external, 0u);
    CHECK_EQ(stats.external_transmit_failures, 0u);

    // Dropping is a problem the running bridge reports.
    CanBridgeHealth before;
    CanBridgeHealth now;
    now.statistics.external_transmit_dropped = 7;
    CHECK(format_health_change(before, now) ==
          std::optional<std::string>("XL bus: state unknown; since the last report: xl_tx_dropped +7"));
}

TEST_CASE(bridge_reports_transmit_problems)
{
    FakeCanController controller;
    FakeExternalPort port;
    CanBridge bridge(controller, port);
    bridge.start();
    bridge.start_controller();
    const std::vector<std::uint8_t> data{1, 2};

    port.result = TransmitResult::QueueFull;
    controller.deliver(make_frame(0x10, 0, 2, data));
    port.result = TransmitResult::Failed;
    controller.deliver(make_frame(0x10, 0, 2, data));
    controller.throw_on_send = true;
    port.deliver(ExternalEventKind::Frame, make_frame(0x10, 0, 2, data));

    const auto stats = bridge.statistics();
    CHECK_EQ(stats.external_queue_full, 1u);
    CHECK_EQ(stats.external_transmit_failures, 1u);
    CHECK_EQ(stats.silkit_transmit_failures, 1u);
    CHECK_EQ(stats.silkit_to_external, 0u);
    CHECK_EQ(stats.external_to_silkit, 0u);
}

TEST_CASE(bridge_forwards_nothing_after_stop)
{
    FakeCanController controller;
    FakeExternalPort port;
    CanBridge bridge(controller, port);
    bridge.start();
    bridge.start_controller();
    bridge.stop();
    const std::vector<std::uint8_t> data{1};
    controller.deliver(make_frame(0x10, 0, 1, data)); // handler removed
    port.deliver(ExternalEventKind::Frame, make_frame(0x10, 0, 1, data)); // handler cleared
    CHECK(port.transmitted().empty());
    CHECK(controller.sent().empty());
}

TEST_CASE(bridge_statistics_line_names_every_counter)
{
    // The key names are documented output that the launcher parses.
    vehicle_sil::vector_xl::CanBridgeStatistics s;
    s.silkit_to_external = 1;
    s.external_to_silkit = 2;
    s.own_transmissions_dropped = 3;
    s.external_error_frames = 4;
    s.external_overruns = 5;
    s.silkit_rejected = 6;
    s.external_rejected = 7;
    s.external_queue_full = 8;
    s.external_transmit_failures = 9;
    s.silkit_transmit_failures = 10;
    s.external_transmit_dropped = 11;
    s.external_transmit_unsent = 12;
    CHECK(vehicle_sil::vector_xl::format_statistics(s) ==
          "stats silkit_to_xl=1 xl_to_silkit=2 own_tx_dropped=3 error_frames=4 overruns=5 silkit_rejected=6 "
          "xl_rejected=7 xl_queue_full=8 xl_tx_failures=9 silkit_tx_failures=10 xl_tx_dropped=11 xl_tx_unsent=12");
}
