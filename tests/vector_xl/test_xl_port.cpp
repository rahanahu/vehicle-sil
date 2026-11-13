// SPDX-License-Identifier: MIT
//
// The real XL backend logic (bridges/vector-xl/src/xl_driver.cpp) against the fake XL backend.
#include "can_test_util.hpp"
#include "fake_xl_frames.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/vector_xl/xl_driver.hpp"
#include "xl_events.hpp" // internal: classify_xl_event, to_transmit_message

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace can = SilKit::Services::Can;
using namespace vehicle_sil::vector_xl;
using namespace std::chrono_literals;
using vehicle_sil::test::frame_matrix;
using vehicle_sil::test::MatrixFrame;
using vehicle_sil::test::to_fake;
using vehicle_sil::unit::flag;
using vehicle_sil::unit::record;
using vehicle_sil::unit::RecordedFrame;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_if;
using vehicle_sil::unit::throws_with;

constexpr unsigned kVirtual1 = 0; // fake channel indices, see fake_xl::default_channels()
constexpr unsigned kHardware1 = 2;
constexpr unsigned kLinOnly = 4;

/// Collects events delivered by an XlCanPort. The periodic BusState events are kept apart, so
/// that the other events can be checked in order.
class EventSink
{
public:
    struct Event
    {
        ExternalEventKind kind;
        RecordedFrame frame;
        std::uint64_t count = 0;
    };

    ExternalCanPort::EventHandler handler()
    {
        return [this](const ExternalCanEvent& event) {
            {
                const std::lock_guard lock(mutex_);
                if (event.kind == ExternalEventKind::BusState)
                {
                    bus_states_.push_back(event.bus_state);
                }
                else
                {
                    events_.push_back({event.kind, record(event.frame), event.count});
                }
            }
            cv_.notify_all();
        };
    }

    std::vector<Event> wait_for(std::size_t count, std::chrono::milliseconds timeout = 2000ms)
    {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return events_.size() >= count; });
        return events_;
    }

    std::vector<ExternalBusState> wait_for_bus_states(std::size_t count, std::chrono::milliseconds timeout = 2000ms)
    {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return bus_states_.size() >= count; });
        return bus_states_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Event> events_;
    std::vector<ExternalBusState> bus_states_;
};

XlChannelInfo channel_at(XlDriver& driver, unsigned index)
{
    return driver.channels().at(index);
}

using detail::backend::ReceivedEvent;

ReceivedEvent receive_event(std::uint32_t id, bool extended, std::uint8_t dlc)
{
    ReceivedEvent event;
    event.is_can_message = true;
    event.message.id = id;
    event.message.extended = extended;
    event.message.dlc = dlc;
    for (std::uint8_t i = 0; i < 8; ++i)
    {
        event.message.data[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    return event;
}

template <typename Change>
ReceivedEvent receive_event_with(std::uint32_t id, std::uint8_t dlc, Change change)
{
    auto event = receive_event(id, false, dlc);
    change(event);
    return event;
}

} // namespace

TEST_CASE(xl_classify_receive_events)
{
    using detail::classify_xl_event;
    using detail::XlEventClass;

    // classify_xl_event's frame.dataField points into the event, so keep each event alive.
    const ReceivedEvent extEvent = receive_event(0x18FF1080u, true, 8);
    const auto ext = classify_xl_event(extEvent);
    CHECK(ext.event_class == XlEventClass::Frame);
    CHECK_EQ(ext.frame.canId, 0x18FF1080u);
    CHECK_EQ(ext.frame.flags, flag(can::CanFrameFlag::Ide));
    CHECK_EQ(ext.frame.dlc, 8u);
    CHECK_EQ(ext.frame.dataField.size(), 8u);
    CHECK_EQ(ext.frame.dataField[7], 0xA7u);
    CHECK(!ext.overrun);

    const ReceivedEvent stdEvent = receive_event(0x7FF, false, 3);
    const auto std11 = classify_xl_event(stdEvent);
    CHECK_EQ(std11.frame.canId, 0x7FFu);
    CHECK_EQ(std11.frame.flags, 0u);
    CHECK_EQ(std11.frame.dataField.size(), 3u);

    const ReceivedEvent remoteEvent = receive_event_with(0x123, 6, [](ReceivedEvent& e) { e.message.remote = true; });
    const auto remote = classify_xl_event(remoteEvent);
    CHECK_EQ(remote.frame.flags, flag(can::CanFrameFlag::Rtr));
    CHECK_EQ(remote.frame.dlc, 6u);
    CHECK_EQ(remote.frame.dataField.size(), 0u);

    const ReceivedEvent dlc15Event = receive_event(0x123, false, 15);
    const auto dlc15 = classify_xl_event(dlc15Event);
    CHECK_EQ(dlc15.frame.dlc, 15u);
    CHECK_EQ(dlc15.frame.dataField.size(), 8u);

    CHECK(classify_xl_event(receive_event_with(0x10, 1, [](ReceivedEvent& e) { e.transmit_receipt = true; })).event_class ==
          XlEventClass::OwnTransmission);
    CHECK(classify_xl_event(receive_event_with(0x10, 1, [](ReceivedEvent& e) { e.transmit_request = true; })).event_class ==
          XlEventClass::OwnTransmission);
    CHECK(classify_xl_event(receive_event_with(0x10, 0, [](ReceivedEvent& e) { e.error_frame = true; })).event_class ==
          XlEventClass::ErrorFrame);
    CHECK(classify_xl_event(receive_event_with(0x10, 1, [](ReceivedEvent& e) { e.controller_overrun = true; })).overrun);
    CHECK(classify_xl_event(receive_event_with(0x10, 1, [](ReceivedEvent& e) { e.queue_overrun = true; })).overrun);
    CHECK(!classify_xl_event(receive_event(0x10, false, 1)).overrun);

    // Anything that is not a CAN message is ignored, but a lost-events mark on it still counts.
    ReceivedEvent other = receive_event(0, false, 0);
    other.is_can_message = false;
    other.transmit_receipt = true; // meaningless on such an event
    CHECK(classify_xl_event(other).event_class == XlEventClass::Ignored);
    CHECK(!classify_xl_event(other).overrun);
    other.queue_overrun = true;
    CHECK(classify_xl_event(other).event_class == XlEventClass::Ignored);
    CHECK(classify_xl_event(other).overrun);
}

TEST_CASE(xl_transmit_event_round_trip_preserves_every_field)
{
    for (const auto& frame : frame_matrix())
    {
        const auto message = detail::to_transmit_message(frame.to_silkit());
        CHECK_EQ(message.extended, frame.extended);
        CHECK(message.dlc <= 8u); // the XL driver transmits DLC 0..8
        // Pretend the driver received it: the same message as a received CAN message.
        ReceivedEvent event;
        event.is_can_message = true;
        event.message = message;
        const auto back = detail::classify_xl_event(event);
        CHECK(back.event_class == detail::XlEventClass::Frame);
        const auto original = frame.to_silkit();
        CHECK_EQ(back.frame.canId, original.canId);
        CHECK_EQ(back.frame.flags, original.flags);
        CHECK_EQ(back.frame.dlc, std::min<std::uint16_t>(original.dlc, 8)); // xlCanTransmit takes DLC 0..8
        CHECK(std::vector<std::uint8_t>(back.frame.dataField.begin(), back.frame.dataField.end()) == frame.data);
    }
}

TEST_CASE(xl_driver_lists_channels)
{
    fake_xl::reset();
    XlDriver driver;
    const auto channels = driver.channels();
    CHECK_EQ(channels.size(), 5u);
    CHECK(channels[0].name == "Virtual Channel 1");
    CHECK(channels[0].is_virtual);
    CHECK(channels[0].can_capable);
    CHECK_EQ(channels[0].channel_mask, 1u);
    CHECK(channels[2].name == "Test CAN Device Channel 1");
    CHECK(!channels[2].is_virtual);
    CHECK_EQ(channels[2].hw_type, fake_xl::kDeviceHwType); // passed through unchanged
    CHECK_EQ(channels[2].serial_number, 12345u);
    CHECK_EQ(channels[2].channel_mask, 4u);
    CHECK(!channels[4].can_capable);

    const auto physical = selectable_channels(channels, XlTarget::Physical);
    CHECK_EQ(physical.size(), 2u);
}

TEST_CASE(xl_driver_open_failure_is_reported)
{
    fake_xl::reset();
    fake_xl::set_fail_open_driver(true);
    CHECK(throws_if<XlError>([] { XlDriver driver; },
                             [](const XlError& e) {
                                 return e.status() == fake_xl::kStatusDriverUnavailable &&
                                        std::string(e.what()).find("xlOpenDriver") != std::string::npos;
                             }));
}

TEST_CASE(xl_port_requests_no_init_access_by_default)
{
    fake_xl::reset();
    XlDriver driver;
    XlCanPort port(driver, channel_at(driver, kVirtual1));
    auto state = fake_xl::port_state(kVirtual1);
    CHECK(state.open);
    // Switching TX receipts off needs init access, so without it they keep the driver default (on).
    CHECK(state.tx_receipts);
    CHECK(!state.init_access);
    CHECK(!port.has_init_access());
    CHECK(!state.bitrate.has_value());

    EventSink sink;
    port.start(sink.handler());
    state = fake_xl::port_state(kVirtual1);
    CHECK(state.active);
    port.stop();
}

TEST_CASE(xl_port_configures_bitrate_with_init_access)
{
    fake_xl::reset();
    XlDriver driver;
    XlCanPortOptions options;
    options.bitrate = 250000;
    XlCanPort first(driver, channel_at(driver, kHardware1), options);
    CHECK(first.has_init_access());
    CHECK(fake_xl::port_state(kHardware1).bitrate == std::optional<unsigned long>(250000));
    // With init access the port switches receipts on (and requests off) itself: they count the
    // frames that reached the bus.
    CHECK(fake_xl::port_state(kHardware1).tx_receipts);

    // A second application cannot get init access, so an explicit bit rate must fail loudly. The
    // open itself succeeded, so the message names no status number.
    CHECK(throws_if<XlError>([&] { XlCanPort second(driver, channel_at(driver, kHardware1), options); },
                             [](const XlError& e) {
                                 const std::string what = e.what();
                                 return what.find("no init access") != std::string::npos &&
                                        what.find("XL status") == std::string::npos;
                             }));
    // Without a bit rate it can still join the bus.
    XlCanPort third(driver, channel_at(driver, kHardware1));
    CHECK(!third.has_init_access());
}

TEST_CASE(xl_port_rejects_channel_without_can)
{
    fake_xl::reset();
    XlDriver driver;
    CHECK(throws_if<XlError>([&] { XlCanPort port(driver, channel_at(driver, kLinOnly)); },
                             [](const XlError& e) { return e.status() == fake_xl::kStatusNoAccess; }));
}

TEST_CASE(xl_port_transmits_onto_the_bus)
{
    fake_xl::reset();
    XlDriver driver;
    XlCanPort port(driver, channel_at(driver, kVirtual1));
    fake_xl::TestNode node(kVirtual1);
    EventSink sink;

    const auto matrix = frame_matrix();
    CHECK(port.transmit(matrix[0].to_silkit()) == TransmitResult::Failed); // not on bus yet
    port.start(sink.handler());
    for (const auto& frame : matrix)
    {
        CHECK(port.transmit(frame.to_silkit()) == TransmitResult::Ok);
    }
    for (const auto& frame : matrix)
    {
        const auto received = node.receive(1000ms);
        CHECK(received.has_value());
        CHECK(*received == as_transmitted_on_xl(frame));
    }
    // Without init access the port cannot switch TX receipts off: they come back, but only ever
    // as own transmissions, never as received frames.
    const auto events = sink.wait_for(matrix.size(), 2000ms);
    CHECK_EQ(events.size(), matrix.size());
    for (const auto& event : events)
    {
        CHECK(event.kind == ExternalEventKind::OwnTransmission);
    }
    port.stop();
}

TEST_CASE(xl_port_receives_from_the_bus)
{
    fake_xl::reset();
    XlDriver driver;
    XlCanPort port(driver, channel_at(driver, kVirtual1));
    fake_xl::TestNode node(kVirtual1);
    EventSink sink;
    port.start(sink.handler());

    const auto matrix = frame_matrix();
    for (const auto& frame : matrix)
    {
        node.send(to_fake(frame));
    }
    const auto events = sink.wait_for(matrix.size());
    CHECK_EQ(events.size(), matrix.size());
    for (std::size_t i = 0; i < matrix.size() && i < events.size(); ++i)
    {
        const auto expected = matrix[i].to_silkit();
        CHECK(events[i].kind == ExternalEventKind::Frame);
        CHECK_EQ(events[i].frame.canId, expected.canId);
        CHECK_EQ(events[i].frame.flags, expected.flags);
        CHECK_EQ(events[i].frame.dlc, expected.dlc);
        CHECK(events[i].frame.data == matrix[i].data);
    }
    port.stop();
    CHECK(!fake_xl::port_state(kVirtual1).active);
}

TEST_CASE(xl_port_flags_receipts_when_the_driver_ignores_the_channel_mode)
{
    fake_xl::reset();
    fake_xl::set_ignore_channel_mode(true);
    XlDriver driver;
    XlCanPort port(driver, channel_at(driver, kVirtual1));
    EventSink sink;
    port.start(sink.handler());
    const auto frame = frame_matrix()[5];
    CHECK(port.transmit(frame.to_silkit()) == TransmitResult::Ok);
    const auto events = sink.wait_for(1);
    CHECK_EQ(events.size(), 1u);
    CHECK(events[0].kind == ExternalEventKind::OwnTransmission);
    port.stop();
}

TEST_CASE(xl_port_reports_error_frames_overruns_and_full_queue)
{
    fake_xl::reset();
    XlDriver driver;
    XlCanPort port(driver, channel_at(driver, kVirtual1));
    fake_xl::TestNode node(kVirtual1);
    EventSink sink;
    port.start(sink.handler());

    node.send_error_frame();
    fake_xl::inject_overrun(kVirtual1);
    node.send({0x55, false, false, 1, {0x01}});
    const auto events = sink.wait_for(3);
    CHECK_EQ(events.size(), 3u);
    CHECK(events[0].kind == ExternalEventKind::ErrorFrame);
    CHECK(events[1].kind == ExternalEventKind::Overrun);
    CHECK(events[2].kind == ExternalEventKind::Frame);
    CHECK_EQ(events[2].frame.canId, 0x55u);

    fake_xl::set_transmit_queue_full(true);
    CHECK(port.transmit(frame_matrix()[1].to_silkit()) == TransmitResult::QueueFull);
    port.stop();
}

TEST_CASE(xl_port_reports_the_controller_state_periodically)
{
    fake_xl::reset();
    XlDriver driver;
    XlCanPortOptions options;
    options.bus_state_interval = 20ms;
    XlCanPort port(driver, channel_at(driver, kHardware1), options);
    EventSink sink;
    port.start(sink.handler());

    // The first query goes out when the port starts: error active by default.
    auto states = sink.wait_for_bus_states(1);
    CHECK(!states.empty() && states.front() == (ExternalBusState{ExternalBusState::Level::ErrorActive, 0, 0}));

    using detail::backend::ChipLevel;
    fake_xl::set_chip_state(kHardware1, {ChipLevel::ErrorPassive, 128, 7});
    states = sink.wait_for_bus_states(states.size() + 2);
    CHECK(!states.empty() && states.back() == (ExternalBusState{ExternalBusState::Level::ErrorPassive, 128, 7}));
    fake_xl::set_chip_state(kHardware1, {ChipLevel::BusOff, 255, 0});
    states = sink.wait_for_bus_states(states.size() + 2);
    CHECK(!states.empty() && states.back().level == ExternalBusState::Level::BusOff);
    fake_xl::set_chip_state(kHardware1, {ChipLevel::ErrorWarning, 96, 0});
    states = sink.wait_for_bus_states(states.size() + 2);
    CHECK(!states.empty() && states.back().level == ExternalBusState::Level::ErrorWarning);
    CHECK(sink.wait_for(1, 0ms).empty()); // nothing else was reported
    port.stop();
}

TEST_CASE(xl_port_reports_a_fault_once_when_the_device_is_lost)
{
    fake_xl::reset();
    XlDriver driver;
    XlCanPortOptions options;
    options.bus_state_interval = 20ms;
    XlCanPort port(driver, channel_at(driver, kHardware1), options);
    EventSink sink;
    port.start(sink.handler());
    CHECK(!sink.wait_for_bus_states(1).empty());

    fake_xl::set_device_lost(true);
    const auto events = sink.wait_for(1);
    CHECK_EQ(events.size(), 1u);
    CHECK(!events.empty() && events.front().kind == ExternalEventKind::Fault);
    const std::string why = port.last_error();
    CHECK(why.find("'Test CAN Device Channel 1' stopped working") != std::string::npos);
    CHECK(why.find("device lost (fake)") != std::string::npos);

    // No more queries and no more events after the fault; sending fails.
    const unsigned requests = fake_xl::chip_state_requests();
    std::this_thread::sleep_for(100ms);
    CHECK_EQ(fake_xl::chip_state_requests(), requests);
    CHECK_EQ(sink.wait_for(2, 0ms).size(), 1u);
    CHECK(port.transmit(frame_matrix()[1].to_silkit()) == TransmitResult::Failed);
    port.stop();
}

TEST_CASE(xl_port_bus_state_queries_can_be_switched_off)
{
    fake_xl::reset();
    XlDriver driver;
    XlCanPortOptions options;
    options.bus_state_interval = 0ms;
    XlCanPort port(driver, channel_at(driver, kVirtual1), options);
    EventSink sink;
    port.start(sink.handler());
    fake_xl::set_device_lost(true);
    std::this_thread::sleep_for(150ms);
    CHECK_EQ(fake_xl::chip_state_requests(), 0u);
    CHECK(sink.wait_for_bus_states(1, 0ms).empty());
    CHECK(sink.wait_for(1, 0ms).empty()); // without queries a lost device goes unnoticed
    port.stop();
}

namespace {

/// Port with init access on the hardware channel and a test node on the same bus.
struct BlockedBusFixture
{
    explicit BlockedBusFixture(XlCanPortOptions options)
    {
        options.bitrate = 250000;
        port.emplace(driver, channel_at(driver, kHardware1), options);
        port->start(sink.handler());
    }

    struct ResetFake
    {
        ResetFake() { fake_xl::reset(); }
    } resetFirst; // before the members below, which register with the fake
    XlDriver driver;
    fake_xl::TestNode node{kHardware1};
    EventSink sink;
    std::optional<XlCanPort> port;
};

std::vector<EventSink::Event> of_kind(const std::vector<EventSink::Event>& events, ExternalEventKind kind)
{
    std::vector<EventSink::Event> result;
    for (const auto& event : events)
    {
        if (event.kind == kind)
        {
            result.push_back(event);
        }
    }
    return result;
}

} // namespace

TEST_CASE(xl_port_stop_sends_what_it_accepted_before_going_off_the_bus)
{
    XlCanPortOptions options;
    options.drain_timeout = 2000ms;
    BlockedBusFixture f(options);
    fake_xl::set_bus_blocked(kHardware1, true);
    for (int i = 0; i < 3; ++i)
    {
        CHECK(f.port->transmit(frame_matrix()[1].to_silkit()) == TransmitResult::Ok);
    }
    CHECK_EQ(fake_xl::queued_transmissions(kHardware1), 3u);

    // The bus recovers while stop() waits: everything goes out and nothing is reported unsent.
    std::thread unblock([] {
        std::this_thread::sleep_for(200ms);
        fake_xl::set_bus_blocked(kHardware1, false);
    });
    const auto begin = std::chrono::steady_clock::now();
    f.port->stop();
    unblock.join();
    CHECK(std::chrono::steady_clock::now() - begin >= 150ms);
    for (int i = 0; i < 3; ++i)
    {
        CHECK(f.node.receive(1000ms).has_value());
    }
    CHECK(of_kind(f.sink.wait_for(3, 0ms), ExternalEventKind::TransmitUnsent).empty());
}

TEST_CASE(xl_port_stop_reports_what_it_could_not_send)
{
    XlCanPortOptions options;
    options.drain_timeout = 100ms;
    BlockedBusFixture f(options);
    fake_xl::set_bus_blocked(kHardware1, true);
    for (int i = 0; i < 4; ++i)
    {
        CHECK(f.port->transmit(frame_matrix()[1].to_silkit()) == TransmitResult::Ok);
    }
    const auto begin = std::chrono::steady_clock::now();
    f.port->stop();
    CHECK(std::chrono::steady_clock::now() - begin < 1000ms); // bounded by drain_timeout
    const auto unsent = of_kind(f.sink.wait_for(1, 0ms), ExternalEventKind::TransmitUnsent);
    CHECK_EQ(unsent.size(), 1u);
    CHECK(!unsent.empty() && unsent.front().count == 4u);
    CHECK_EQ(fake_xl::queued_transmissions(kHardware1), 0u); // gone with the channel
}

TEST_CASE(xl_port_refuses_frames_while_the_bus_is_stuck_longer_than_the_transmit_timeout)
{
    XlCanPortOptions options;
    options.transmit_timeout = 100ms;
    options.drain_timeout = 0ms;
    BlockedBusFixture f(options);
    fake_xl::set_bus_blocked(kHardware1, true);
    // Within the timeout frames are accepted and wait.
    for (int i = 0; i < 3; ++i)
    {
        CHECK(f.port->transmit(frame_matrix()[1].to_silkit()) == TransmitResult::Ok);
    }
    std::this_thread::sleep_for(150ms);
    // After it nothing new is accepted, so nothing more piles up.
    for (int i = 0; i < 5; ++i)
    {
        CHECK(f.port->transmit(frame_matrix()[1].to_silkit()) == TransmitResult::Dropped);
    }
    CHECK_EQ(fake_xl::queued_transmissions(kHardware1), 3u);

    // The bus works again: the 3 accepted frames go out, their receipts unblock the port.
    fake_xl::set_bus_blocked(kHardware1, false);
    for (int i = 0; i < 3; ++i)
    {
        CHECK(f.node.receive(1000ms).has_value());
    }
    CHECK(!f.node.receive(100ms).has_value());
    CHECK_EQ(of_kind(f.sink.wait_for(3, 1000ms), ExternalEventKind::OwnTransmission).size(), 3u);
    CHECK(f.port->transmit(frame_matrix()[2].to_silkit()) == TransmitResult::Ok);
    CHECK(f.node.receive(1000ms).has_value());
    CHECK_EQ(of_kind(f.sink.wait_for(4, 1000ms), ExternalEventKind::OwnTransmission).size(), 4u); // its receipt
    f.port->stop();
    CHECK(of_kind(f.sink.wait_for(100, 0ms), ExternalEventKind::TransmitUnsent).empty());
}

TEST_CASE(xl_port_stop_is_bounded_by_the_wait_timeout_and_restartable)
{
    fake_xl::reset();
    XlDriver driver;
    XlCanPortOptions options;
    options.wait_timeout = 1000ms; // stop() waits at most about one wait_timeout for the receive thread
    XlCanPort port(driver, channel_at(driver, kVirtual1), options);
    EventSink sink;
    port.start(sink.handler());
    CHECK(fake_xl::port_state(kVirtual1).active);
    const auto begin = std::chrono::steady_clock::now();
    port.stop();
    CHECK(std::chrono::steady_clock::now() - begin < 1500ms);
    CHECK(!fake_xl::port_state(kVirtual1).active);
    port.stop(); // idempotent

    port.start(sink.handler());
    fake_xl::TestNode node(kVirtual1);
    node.send({0x66, false, false, 0, {}});
    CHECK_EQ(sink.wait_for(1).size(), 1u);
}
