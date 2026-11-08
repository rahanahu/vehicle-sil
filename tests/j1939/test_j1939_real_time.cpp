// SPDX-License-Identifier: MIT
//
// The J1939 stack under the real-time driver, with a manual clock: the driver wakes for
// the network's timers, and frames that arrive are handled in the step after them.
#include "j1939_bench.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/can/real_time_driver.hpp"
#include "vehicle_sil/can/real_time_receiver.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <chrono>
#include <string_view>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimTime;
using vehicle_sil::can::FrameSink;
using vehicle_sil::can::ManualRealTimeClock;
using vehicle_sil::can::PortEventSink;
using vehicle_sil::can::RealTimeDriver;
using vehicle_sil::can::RealTimeDriverOptions;
using vehicle_sil::can::RealTimeReceiver;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::TimeDomain;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;

namespace {

constexpr auto kWait = std::chrono::seconds(10);

/// A real-time port: the test pushes received frames; sent frames are recorded.
class RealTimeTestPort final : public vehicle_sil::can::Port
{
public:
    explicit RealTimeTestPort(const vehicle_sil::can::RealTimeClock& clock) : receiver(clock, 64, 64) {}

    RealTimeReceiver receiver;
    std::vector<TimedFrame> sent; ///< driver's thread; read after wait_idle()

    [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::RealTime; }
    std::size_t release_before(SimTime now, FrameSink sink) override { return receiver.release_before(now, sink); }
    std::size_t release_events_before(SimTime now, PortEventSink sink) override
    {
        return receiver.release_events_before(now, sink);
    }
    SendResult send(const TimedFrame& f) override
    {
        sent.push_back(f);
        return SendResult::Accepted;
    }
    void set_wakeup(vehicle_sil::can::Wakeup wakeup) override { receiver.set_wakeup(std::move(wakeup)); }
};

NetworkOptions quiet()
{
    NetworkOptions options;
    options.warning = [](std::string_view) {};
    return options;
}

} // namespace

TEST_CASE(j1939_timers_wake_the_real_time_driver)
{
    ManualRealTimeClock clock;
    RealTimeTestPort port(clock);
    Network network(quiet());
    Node& node = network.add_node({0x80, Name{0x1000}}); // claims and waits 250 ms
    vehicle_sil::can::Host host;
    host.add(network, port);
    RealTimeDriver driver(host, clock, RealTimeDriverOptions{std::chrono::milliseconds(200)});
    driver.start();
    CHECK(driver.wait_idle(kWait));
    CHECK(node.address_state() == AddressState::Claiming);
    CHECK_EQ(port.sent.size(), 1u); // Address Claimed at 0 ms

    clock.advance_to(at(200ms)); // the idle step
    CHECK(driver.wait_idle(kWait));
    CHECK(node.address_state() == AddressState::Claiming);
    // The end of the wait is a timer: the driver wakes for it, long before the next idle step.
    clock.advance_to(at(250ms) + std::chrono::nanoseconds(1));
    CHECK(driver.wait_idle(kWait));
    CHECK(node.address_state() == AddressState::Claimed);
    CHECK(driver.last_step() == std::optional<SimTime>(at(250ms) + std::chrono::nanoseconds(1)));

    // A Request for Address Claimed arrives without the clock moving: answered in the step
    // right after it.
    CHECK(port.receiver.push_frame(to_can(frame(6, 0xEA00, 0x50, kGlobal, request(0xEE00)))));
    CHECK(driver.wait_idle(kWait));
    CHECK_EQ(port.sent.size(), 2u);
    CHECK(port.sent[1].time == at(250ms) + std::chrono::nanoseconds(2));
    CHECK_EQ((port.sent[1].frame.id >> 8) & 0xFFFFu, 0xEEFFu); // Address Claimed to global
    driver.stop();
    CHECK(!driver.failed());
}
