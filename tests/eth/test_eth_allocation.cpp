// SPDX-License-Identifier: MIT
//
// vehicle_sil::eth::Channel and the virtual network do not allocate memory in the steps:
// receiving and ordering frames from several senders with equal times, the handlers,
// receive(), and sending. This executable counts every allocation of the process
// (allocation_counter.hpp), so it is built on its own.
#include "allocation_counter.hpp"
#include "eth_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/eth/channel.hpp"
#include "vehicle_sil/eth/virtual_ethernet_bus.hpp"
#include "vehicle_sil/step/host.hpp"
#include "vehicle_sil/step/step_driver.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimTime;
using vehicle_sil::eth::Channel;
using vehicle_sil::eth::kBroadcast;
using vehicle_sil::eth::SendStatus;
using vehicle_sil::eth::TimedFrame;
using vehicle_sil::eth::VirtualEthernetBus;
using vehicle_sil::unit::mac;
using vehicle_sil::unit::make_frame;

TEST_CASE(steps_do_not_allocate)
{
    // Three senders with the same step size (their frames have equal times) and addresses in
    // the opposite order of their registration, so the receiver has to reorder them. The
    // receiver takes half of the frames with its handler's count and the other half with
    // receive(), and answers each one.
    constexpr std::size_t kSenders = 3;
    VirtualEthernetBus bus;
    std::vector<std::unique_ptr<Channel>> senders;
    std::vector<std::unique_ptr<vehicle_sil::step::Host>> hosts;
    for (std::size_t i = 0; i < kSenders; ++i)
    {
        senders.push_back(std::make_unique<Channel>(bus.attach()));
        hosts.push_back(std::make_unique<vehicle_sil::step::Host>());
        hosts.back()->add(*senders.back());
    }
    Channel receiver(bus.attach());
    vehicle_sil::step::Host receiverHost;
    receiverHost.add(receiver);

    std::array<std::uint8_t, kSenders> seq{};
    for (std::size_t i = 0; i < kSenders; ++i)
    {
        Channel* channel = senders[i].get();
        const auto address = mac(static_cast<std::uint8_t>(kSenders - i));
        std::uint8_t* counter = &seq[i];
        hosts[i]->set_application([channel, address, counter](SimTime) {
            for (int n = 0; n < 4; ++n)
            {
                CHECK(channel->send(make_frame(kBroadcast, address, 1, (*counter)++, n % 2 == 0 ? 20 : 300)) ==
                      SendStatus::Ok);
            }
        });
    }
    std::size_t handled = 0;
    std::size_t taken = 0;
    receiver.on_frame([&](const TimedFrame& f) {
        ++handled;
        CHECK(receiver.send(make_frame(f.frame.source(), mac(9), 2, f.frame.bytes()[15])) == SendStatus::Ok);
    });
    receiverHost.set_application([&](SimTime) {
        while (const auto f = receiver.receive())
        {
            ++taken;
            if (taken % 2 == 0)
            {
                break; // leave the rest of the step's frames
            }
        }
    });

    vehicle_sil::step::StepDriver driver;
    for (auto& host : hosts)
    {
        driver.add(*host, 1ms);
    }
    driver.add(receiverHost, 1ms);

    driver.run_until(SimTime{} + 5ms); // warm up: every buffer is in use once
    g_allocations.store(0);
    g_counting.store(true);
    driver.run_until(SimTime{} + 105ms);
    g_counting.store(false);
    CHECK_EQ(g_allocations.load(), 0u);
    CHECK(handled > 1000u);
    CHECK(taken > 100u);
    CHECK(receiver.statistics().frames_not_taken > 0u);
    CHECK_EQ(receiver.statistics().receive_overflows, 0u);
}
