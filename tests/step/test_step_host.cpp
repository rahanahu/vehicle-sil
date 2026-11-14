// SPDX-License-Identifier: MIT
//
// vehicle_sil::step::Host, StepDriver and RealTimeDriver with members that are not CAN: the
// stages and their order, time domains that must not be mixed, post(), next_deadline() and the
// wakeup. Links only vehicle_sil::step (no CAN, no SIL Kit). The real-time cases use
// ManualRealTimeClock and wait_idle(), not the wall clock.
#include "test_harness.hpp"

#include "vehicle_sil/step/host.hpp"
#include "vehicle_sil/step/real_time_clock.hpp"
#include "vehicle_sil/step/real_time_driver.hpp"
#include "vehicle_sil/step/step_driver.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::step::Host;
using vehicle_sil::step::ManualRealTimeClock;
using vehicle_sil::step::Member;
using vehicle_sil::step::RealTimeDriver;
using vehicle_sil::step::RealTimeDriverOptions;
using vehicle_sil::step::StepDriver;
using vehicle_sil::step::TimeDomain;
using vehicle_sil::step::Wakeup;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;

namespace {

SimTime at(std::chrono::milliseconds t)
{
    return SimTime{} + t;
}

constexpr SimDuration kNs{1};
constexpr auto kWait = 10s; // upper bound for wait_idle(); a pass never waits that long

/// A member with no bus behind it: it logs every call (with the step time where there is one)
/// and can throw at a chosen stage.
class Recorder final : public Member
{
public:
    Recorder(std::string name, std::vector<std::string>& log, TimeDomain domain = TimeDomain::Virtual)
      : name_(std::move(name)), log_(log), domain_(domain)
    {
    }

    std::string throw_at;                 ///< a stage name; empty: never throws
    std::optional<SimTime> deadline;      ///< next_deadline()
    std::optional<SimDuration> step_size; ///< what start() was given
    int starts = 0;

    [[nodiscard]] TimeDomain time_domain() const noexcept override { return domain_; }
    void start(std::optional<SimDuration> size) override
    {
        ++starts;
        step_size = size;
        stage("start");
    }
    void begin(SimTime now) override { stage("begin@" + std::to_string(to_ns(now) / 1000000) + "ms"); }
    void receive(SimTime now) override { stage("receive@" + std::to_string(to_ns(now) / 1000000) + "ms"); }
    void process() override { stage("process"); }
    void flush() override { stage("flush"); }
    [[nodiscard]] std::optional<SimTime> next_deadline() const noexcept override { return deadline; }

private:
    void stage(const std::string& what)
    {
        log_.push_back(name_ + "." + what);
        if (!throw_at.empty() && what.rfind(throw_at, 0) == 0)
        {
            throw std::runtime_error(name_ + " failed in " + what);
        }
    }

    std::string name_;
    std::vector<std::string>& log_;
    TimeDomain domain_;
};

/// A real-time member that a test feeds from another thread, like a port that receives on its
/// own thread: arrive() stamps the arrival with the clock and calls the wakeup. It records the
/// steps it runs in and what it took in each.
class Inbox final : public Member
{
public:
    explicit Inbox(const ManualRealTimeClock& clock) : clock_(clock) {}

    [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::RealTime; }
    void set_wakeup(Wakeup wakeup) override
    {
        const std::lock_guard lock(mutex_);
        wakeup_ = std::move(wakeup);
    }
    void begin(SimTime now) override { steps.push_back(now); }
    void receive(SimTime now) override
    {
        const std::lock_guard lock(mutex_);
        std::vector<SimTime> later;
        for (const SimTime arrival : pending_)
        {
            (arrival < now ? taken : later).push_back(arrival);
        }
        pending_ = std::move(later);
    }
    void process() override {}
    void flush() override {}

    /// Called from the test thread.
    void arrive()
    {
        const std::lock_guard lock(mutex_);
        const SimTime arrival = clock_.now();
        pending_.push_back(arrival);
        if (wakeup_)
        {
            wakeup_(arrival);
        }
    }
    [[nodiscard]] bool has_wakeup()
    {
        const std::lock_guard lock(mutex_);
        return static_cast<bool>(wakeup_);
    }

    std::vector<SimTime> steps; ///< driver's thread; read after wait_idle()
    std::vector<SimTime> taken; ///< arrivals released to a step, in order

private:
    const ManualRealTimeClock& clock_;
    std::mutex mutex_;
    Wakeup wakeup_;
    std::vector<SimTime> pending_;
};

} // namespace

TEST_CASE(host_runs_the_stages_of_members_in_registration_order)
{
    std::vector<std::string> log;
    Recorder a("a", log);
    Recorder b("b", log);
    Host host;
    host.add(a);
    host.add(b);
    host.set_step_size(5ms);
    host.set_application([&log](SimTime now) { log.push_back("app@" + std::to_string(to_ns(now) / 1000000) + "ms"); });
    host.post([&log] { log.push_back("posted"); });
    host.step(at(0ms));
    host.step(at(5ms));
    const std::vector<std::string> expected{
        "a.start", "b.start",                                         // once, before the first step
        "a.begin@0ms", "b.begin@0ms", "a.receive@0ms", "b.receive@0ms", //
        "a.process", "b.process", "posted", "app@0ms", "a.flush", "b.flush", //
        "a.begin@5ms", "b.begin@5ms", "a.receive@5ms", "b.receive@5ms", //
        "a.process", "b.process", "app@5ms", "a.flush", "b.flush"};
    CHECK(log == expected);
    CHECK(a.step_size == std::optional<SimDuration>(5ms));
    CHECK_EQ(a.starts, 1);
    CHECK(host.last_step() == std::optional<SimTime>(at(5ms)));
}

TEST_CASE(host_rejects_a_member_added_twice_or_after_the_first_step)
{
    std::vector<std::string> log;
    Recorder a("a", log);
    Recorder b("b", log);
    Host host;
    host.add(a);
    CHECK(throws_with<std::logic_error>([&] { host.add(a); }, "added only once"));
    host.step(at(0ms));
    CHECK(throws_with<std::logic_error>([&] { host.add(b); }, "only allowed before the first step"));
}

TEST_CASE(host_rejects_members_of_mixed_time_domains)
{
    std::vector<std::string> log;
    Recorder simulated("simulated", log, TimeDomain::Virtual);
    Recorder wall("wall", log, TimeDomain::RealTime);
    {
        Host host;
        host.add(simulated);
        CHECK(throws_with<std::logic_error>([&] { host.add(wall); }, "share a time domain"));
        CHECK(host.time_domain() == std::optional<TimeDomain>(TimeDomain::Virtual));
    }
    {
        // The step driver requires virtual time, before or after the members are added.
        Host host;
        host.add(wall);
        StepDriver driver;
        CHECK(throws<std::logic_error>([&] { driver.add(host, 1ms); }));
        Host other;
        StepDriver second;
        second.add(other, 1ms);
        CHECK(throws<std::logic_error>([&] { other.add(wall); }));
    }
    {
        // The real-time driver requires real time.
        Host host;
        host.add(simulated);
        ManualRealTimeClock clock;
        CHECK(throws<std::logic_error>([&] { RealTimeDriver driver(host, clock); }));
    }
}

TEST_CASE(host_fails_when_a_member_throws_and_stays_failed)
{
    std::vector<std::string> log;
    Recorder a("a", log);
    Recorder b("b", log);
    b.throw_at = "process";
    Host host;
    host.add(a);
    host.add(b);
    bool applicationRan = false;
    host.set_application([&applicationRan](SimTime) { applicationRan = true; });
    CHECK(throws_with<std::runtime_error>([&] { host.step(at(0ms)); }, "b failed in process"));
    CHECK(host.failed());
    CHECK(host.error() == "b failed in process");
    CHECK(!applicationRan); // the rest of the step does not run
    CHECK(log.back() == "b.process");
    CHECK(throws_with<std::logic_error>([&] { host.step(at(1ms)); }, "the host has failed"));
}

TEST_CASE(host_post_runs_in_the_next_step_and_wakes_the_driver)
{
    std::vector<std::string> log;
    Recorder a("a", log);
    Host host;
    host.add(a);
    std::vector<SimTime> wakeups;
    host.set_wakeup([&wakeups](SimTime arrival) { wakeups.push_back(arrival); });
    host.set_application([&](SimTime now) {
        if (now == at(0ms))
        {
            host.post([&log] { log.push_back("posted in step 0"); }); // runs in the next step
        }
    });
    host.post([&log] { log.push_back("posted before"); });
    CHECK_EQ(wakeups.size(), 1u);
    CHECK(wakeups[0] == SimTime{});
    host.step(at(0ms));
    host.step(at(1ms));
    const std::vector<std::string> expected{"a.start", "a.begin@0ms", "a.receive@0ms", "a.process", "posted before", "a.flush",
                                            "a.begin@1ms", "a.receive@1ms", "a.process", "posted in step 0", "a.flush"};
    CHECK(log == expected);
    CHECK_EQ(wakeups.size(), 2u);
    CHECK(throws<std::invalid_argument>([&] { host.post({}); }));
}

TEST_CASE(host_next_deadline_is_the_earliest_of_its_members)
{
    std::vector<std::string> log;
    Recorder a("a", log);
    Recorder b("b", log);
    Recorder c("c", log);
    Host host;
    CHECK(host.next_deadline() == std::nullopt);
    host.add(a);
    host.add(b);
    host.add(c);
    CHECK(host.next_deadline() == std::nullopt);
    a.deadline = at(9ms);
    c.deadline = at(4ms);
    CHECK(host.next_deadline() == std::optional<SimTime>(at(4ms)));
    c.deadline.reset();
    CHECK(host.next_deadline() == std::optional<SimTime>(at(9ms)));
}

TEST_CASE(step_driver_steps_members_on_virtual_time)
{
    std::vector<std::string> log;
    Recorder a("a", log);
    Host host;
    host.add(a);
    StepDriver driver;
    driver.add(host, 2ms);
    driver.run_until(at(5ms));
    CHECK(a.step_size == std::optional<SimDuration>(2ms));
    const std::vector<std::string> begins{log[1], log[5], log[9]};
    CHECK((begins == std::vector<std::string>{"a.begin@0ms", "a.begin@2ms", "a.begin@4ms"}));
    CHECK_EQ(log.size(), 13u); // start, then 3 steps of 4 stages
    CHECK(driver.next_step_time() == at(6ms));
}

TEST_CASE(real_time_driver_steps_a_member_after_its_deadline_and_after_an_arrival)
{
    ManualRealTimeClock clock;
    Inbox inbox(clock);
    Host host;
    host.add(inbox);
    RealTimeDriver driver(host, clock, RealTimeDriverOptions{10ms});
    CHECK(!inbox.has_wakeup());
    driver.start();
    CHECK(driver.wait_idle(kWait));
    CHECK(inbox.has_wakeup()); // the host passed the driver's wakeup to the member
    CHECK((inbox.steps == std::vector<SimTime>{at(0ms)}));

    // Nothing arrives: the idle limit.
    clock.advance_to(at(10ms));
    CHECK(driver.wait_idle(kWait));
    CHECK((inbox.steps == std::vector<SimTime>{at(0ms), at(10ms)}));

    // An arrival wakes the driver; the clock does not move, so it steps 1 ns after the arrival
    // and the member releases it there.
    clock.advance_to(at(12ms));
    CHECK(driver.wait_idle(kWait));
    inbox.arrive();
    CHECK(driver.wait_idle(kWait));
    CHECK((inbox.steps == std::vector<SimTime>{at(0ms), at(10ms), at(12ms) + kNs}));
    CHECK((inbox.taken == std::vector<SimTime>{at(12ms)}));

    driver.stop();
    CHECK(!inbox.has_wakeup()); // stop() removes the wakeup from the members
    CHECK(!driver.failed());
}
