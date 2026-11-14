// SPDX-License-Identifier: MIT
//
// vehicle_sil::can::Host::add() when the port refuses the host's wakeup: the pair is not added and
// nothing in the host points to it any more. Built without SIL Kit; under ASan a pointer left to
// the destroyed pair shows as a use after free.
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>

using namespace std::chrono_literals;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::can::FrameSink;
using vehicle_sil::can::Host;
using vehicle_sil::can::Port;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::Steppable;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::TimeDomain;
using vehicle_sil::can::Wakeup;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;

namespace {

SimTime at(std::chrono::milliseconds t)
{
    return SimTime{} + t;
}

/// Counts the calls of the host; a timer at 7 ms.
class Counter final : public Steppable
{
public:
    int starts = 0;
    int begins = 0;
    int flushes = 0;

    void start(std::optional<SimDuration>) override { ++starts; }
    void begin(SimTime) override { ++begins; }
    void receive(const TimedFrame&) override {}
    void process() override {}
    void flush(Port&) override { ++flushes; }
    [[nodiscard]] std::optional<SimTime> next_deadline() const noexcept override { return at(7ms); }
};

/// A port whose set_wakeup() throws for a function while `refuse` is set.
class RefusingPort final : public Port
{
public:
    bool refuse = true;
    int releases = 0;

    [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::Virtual; }
    std::size_t release_before(SimTime, FrameSink) override
    {
        ++releases;
        return 0;
    }
    SendResult send(const TimedFrame&) override { return SendResult::Accepted; }
    void set_wakeup(Wakeup wakeup) override
    {
        if (refuse && wakeup)
        {
            throw std::runtime_error("the port refuses the wakeup");
        }
    }
};

} // namespace

TEST_CASE(host_add_with_a_port_that_refuses_the_wakeup_adds_nothing)
{
    Counter steppable;
    RefusingPort port;
    Host host;
    host.set_wakeup([](SimTime) {});
    CHECK(throws_with<std::runtime_error>([&] { host.add(steppable, port); }, "the port refuses the wakeup"));
    CHECK(host.time_domain() == std::nullopt);
    CHECK(host.next_deadline() == std::nullopt);
    host.step(at(0ms));
    host.step(at(1ms));
    CHECK_EQ(steppable.starts, 0);
    CHECK_EQ(steppable.begins, 0);
    CHECK_EQ(port.releases, 0);
    host.set_wakeup({}); // reaches no port either
}

TEST_CASE(host_add_with_a_port_that_refuses_the_wakeup_can_be_repeated)
{
    Counter steppable;
    RefusingPort port;
    Host host;
    host.set_wakeup([](SimTime) {});
    CHECK(throws<std::runtime_error>([&] { host.add(steppable, port); }));
    port.refuse = false;
    host.add(steppable, port); // not "added only once": the failed add left nothing behind
    CHECK(host.next_deadline() == std::optional<SimTime>(at(7ms)));
    host.step(at(0ms));
    // The pair is in the host once.
    CHECK_EQ(steppable.starts, 1);
    CHECK_EQ(steppable.begins, 1);
    CHECK_EQ(steppable.flushes, 1);
    CHECK_EQ(port.releases, 1);
}
