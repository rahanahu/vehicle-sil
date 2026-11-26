// SPDX-License-Identifier: MIT
//
// vehicle_sil::can::RawChannel and VirtualCanBus (timing-less and timing-enabled) do not
// allocate memory in the steps, including active on_transmit() handlers and flush() rejections.
// This executable counts every allocation of the process (allocation_counter.hpp), so it is
// built on its own.
#include "allocation_counter.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/raw_channel.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimTime;
using vehicle_sil::can::FaultAction;
using vehicle_sil::can::Frame;
using vehicle_sil::can::Host;
using vehicle_sil::can::IdFormat;
using vehicle_sil::can::RawChannel;
using vehicle_sil::can::RawChannelOptions;
using vehicle_sil::can::RawSendStatus;
using vehicle_sil::can::StepDriver;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::TransmitEvent;
using vehicle_sil::can::VirtualCanBus;
using vehicle_sil::can::VirtualCanBusEndpointOptions;
using vehicle_sil::can::VirtualCanBusOptions;
using vehicle_sil::can::VirtualCanBusTiming;

namespace {

Frame make_frame(std::uint32_t id, std::uint8_t byte)
{
    Frame f;
    f.id = id;
    f.format = IdFormat::Standard;
    f.length = 8;
    f.data = {byte, 1, 2, 3, 4, 5, 6, 7};
    return f;
}

void run_allocation_scenario(bool with_timing)
{
    VirtualCanBusOptions busOptions;
    if (with_timing)
    {
        busOptions.timing = VirtualCanBusTiming{.bitrate = 500'000, .transmit_queue_capacity = 8};
    }
    VirtualCanBus bus(busOptions);

    // Sender has a small port transmit queue so every step triggers both Accepted completions
    // and flush() QueueFull rejections; a periodic FaultAction::failed() also exercises
    // single-frame flush() failures.
    auto& senderPort = bus.attach(VirtualCanBusEndpointOptions{.transmit_queue_capacity = 4});
    auto& receiverPort = bus.attach();

    std::uint64_t faultCounter = 0;
    senderPort.set_fault_injector([&faultCounter](const TimedFrame&) {
        if ((++faultCounter % 11) == 0)
        {
            return FaultAction::failed();
        }
        return FaultAction::deliver();
    });

    RawChannel sender(RawChannelOptions{.send_capacity = 8, .receive_capacity = 64});
    RawChannel receiver(RawChannelOptions{.send_capacity = 8, .receive_capacity = 64});

    Host senderHost;
    Host receiverHost;
    senderHost.add(sender, senderPort);
    receiverHost.add(receiver, receiverPort);

    std::uint64_t nextToken = 0; // starts at 0 to exercise token == 0
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t received = 0;
    std::uint64_t replies = 0;

    sender.on_transmit([&](const TransmitEvent& ev) {
        if (ev.kind == TransmitEvent::Kind::Completed)
        {
            ++completed;
        }
        else
        {
            ++failed;
        }
        // Reentrantly send from on_transmit() while capacity remains.
        if (sender.statistics().outstanding < 2)
        {
            static_cast<void>(sender.send(make_frame(0x101, static_cast<std::uint8_t>(ev.token)), nextToken++));
        }
    });
    sender.on_frame([&](const TimedFrame&) { ++replies; });

    senderHost.set_application([&](SimTime) {
        for (int i = 0; i < 6; ++i)
        {
            static_cast<void>(sender.send(make_frame(0x100, static_cast<std::uint8_t>(i)), nextToken++));
        }
    });

    receiver.on_frame([&](const TimedFrame& f) {
        ++received;
        if ((received % 4) == 0)
        {
            static_cast<void>(receiver.send(make_frame(0x200, f.frame.data[0]), received));
        }
    });
    receiver.on_transmit([](const TransmitEvent&) {});

    StepDriver driver;
    driver.add(senderHost, 1ms);
    driver.add(receiverHost, 1ms);

    // Warm up so every buffer is exercised once.
    driver.run_until(SimTime{} + 5ms);

    g_allocations.store(0);
    g_counting.store(true);
    driver.run_until(SimTime{} + 105ms);
    g_counting.store(false);

    CHECK_EQ(g_allocations.load(), 0u);
    CHECK(completed > 100u);
    CHECK(failed > 50u);
    CHECK(received > 100u);
    CHECK(replies > 20u);
    CHECK_EQ(sender.statistics().transmit_event_overflows, 0u);
    CHECK_EQ(sender.statistics().port_event_overflows, 0u);
    CHECK_EQ(sender.statistics().frames_sent + sender.statistics().frames_dropped,
             sender.statistics().transmit_completed + sender.statistics().transmit_failed +
                 sender.statistics().outstanding);
}

} // namespace

TEST_CASE(steps_do_not_allocate)
{
    run_allocation_scenario(/*with_timing=*/false);
    run_allocation_scenario(/*with_timing=*/true);
}
