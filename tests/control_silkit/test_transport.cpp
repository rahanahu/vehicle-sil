// SPDX-License-Identifier: MIT
//
// vehicle_sil::control_silkit with real SIL Kit participants (in-process registry): ECUs
// (vehicle_sil::Peer with an AgentTransport) and a SimulationController with a time gate whose
// participant carries the hub's end (HubTransport).
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "topics.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/control_silkit/agent_transport.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <silkit/services/orchestration/all.hpp>
#include <silkit/services/pubsub/all.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
using namespace vehicle_sil;
using namespace vehicle_sil::control;
using vehicle_sil::control_silkit::AgentTransport;
using vehicle_sil::control_silkit::AgentTransportOptions;
using vehicle_sil::control_silkit::HelloReport;
using vehicle_sil::control_silkit::HubTransport;
using vehicle_sil::control_silkit::LinkState;
using vehicle_sil::unit::eventually;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;
namespace orch = SilKit::Services::Orchestration;
namespace pubsub = SilKit::Services::PubSub;
namespace topics = vehicle_sil::control_silkit::detail;
using Wall = std::chrono::steady_clock;

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

/// The start of the first step of size `step` at or after `t`.
SimTime first_step_at_or_after(SimTime t, SimDuration step)
{
    const auto n = (t.time_since_epoch() + step - SimDuration{1}) / step;
    return SimTime{} + step * n;
}

/// The value of the variable `name` of `peer` in `snapshot` (a sample of it the hub gave).
std::optional<Value> value_in(const Hub& hub, const std::string& peer, const std::shared_ptr<const Snapshot>& snapshot,
                              const std::string& name)
{
    if (!snapshot)
    {
        return std::nullopt;
    }
    for (const auto& info : hub.peers())
    {
        if (info.participant != peer)
        {
            continue;
        }
        for (std::size_t i = 0; i < info.schema.size() && i < snapshot->values.size(); ++i)
        {
            if (info.schema[i].name == name)
            {
                return snapshot->values[i];
            }
        }
    }
    return std::nullopt;
}

/// The value of the variable `name` of `peer` in the hub's latest sample.
std::optional<Value> hub_value(const Hub& hub, const std::string& peer, const std::string& name)
{
    return value_in(hub, peer, hub.latest(peer), name);
}

struct EcuOptions
{
    SimDuration step = 1ms;
    SimDuration sample_period = SimDuration::zero(); ///< every step
    bool transport = true;
    bool virtual_time = true;
    std::size_t extra_outputs = 0;
    std::size_t max_message_bytes = wire::kMaxMessageBytes;
    /// The application throws in its step at this time.
    std::optional<SimTime> fail_at{};
    /// AgentTransportOptions::last_word_wait. A test that checks what the hub has right after the
    /// ECU ended gives it plenty: with a hub that answers, the wait ends with the answer.
    std::chrono::milliseconds last_word_wait = AgentTransportOptions{}.last_word_wait;
};

/// An ECU with the inputs `cmd` and `timed`, the parameter `gain` and the output `steps` (the
/// number of its steps so far, counted before the agent samples: in a sample taken every step it
/// is the sample's sequence number). The application records what it sees in each step.
struct TestEcu
{
    std::string name;
    EcuOptions options;
    std::int64_t cmd = 0;
    std::int64_t timed = 0;
    std::int64_t gain = 0;
    std::int64_t steps = 0;
    std::vector<std::int64_t> extra;
    std::atomic<std::int64_t> gain_at_step0{std::numeric_limits<std::int64_t>::min()};
    std::mutex mutex;
    std::vector<std::pair<SimTime, std::int64_t>> timed_seen; ///< guarded by mutex: (step, `timed` as the application saw it)
    std::unique_ptr<Peer> peer;
    std::unique_ptr<AgentTransport> transport;

    TestEcu(std::string ecu_name, const std::string& uri, EcuOptions ecu_options = {}) :
      name(std::move(ecu_name)), options(ecu_options)
    {
        PeerOptions o;
        o.participant = name;
        o.registry_uri = uri;
        if (options.virtual_time)
        {
            o.lifecycle = LifecycleMode::Coordinated;
            o.virtual_time = VirtualTimeOptions{options.step, std::nullopt, std::nullopt};
        }
        else
        {
            o.lifecycle = LifecycleMode::Autonomous;
            o.virtual_time = std::nullopt;
            o.max_idle = 1ms;
        }
        o.monitor = false;
        // A process started anew under the name of one that has just gone: the peer waits while the
        // registry still holds the name (on a slow machine that takes a while).
        o.name_in_use_wait = 30s;
        peer = std::make_unique<Peer>(o);
        AgentOptions agent;
        agent.sample_period = options.sample_period;
        peer->set_agent_options(agent);
        peer->input("cmd", &cmd);
        peer->input("timed", &timed);
        VariableOptions parameter;
        parameter.parameter = true;
        peer->input("gain", &gain, parameter);
        peer->output("steps", &steps);
        extra.resize(options.extra_outputs);
        for (std::size_t i = 0; i < extra.size(); ++i)
        {
            extra[i] = static_cast<std::int64_t>(i) * 3;
            peer->output("extra." + std::to_string(i), &extra[i]);
        }
        peer->on_step([this](SimTime now) {
            if (options.fail_at && now >= *options.fail_at)
            {
                throw std::runtime_error("the test ECU fails here");
            }
            ++steps;
            if (now == SimTime{})
            {
                gain_at_step0.store(gain);
            }
            const std::lock_guard lock(mutex);
            timed_seen.emplace_back(now, timed);
        });
        if (options.transport)
        {
            AgentTransportOptions t;
            t.max_message_bytes = options.max_message_bytes;
            t.step = options.virtual_time ? options.step : SimDuration::zero();
            t.last_word_wait = options.last_word_wait;
            transport = std::make_unique<AgentTransport>(peer->agent(), peer->runtime(), t);
        }
    }

    /// As an ECU's process ends: the steps stop, the transport says its last word (and waits for
    /// the hub's answer), and the participant goes right after it.
    ~TestEcu() { end(); }

    void end()
    {
        if (peer)
        {
            peer->stop("the test ECU ends");
        }
        transport.reset();
        peer.reset();
    }

    [[nodiscard]] std::optional<std::int64_t> timed_at(SimTime step)
    {
        const std::lock_guard lock(mutex);
        for (const auto& [time, value] : timed_seen)
        {
            if (time == step)
            {
                return value;
            }
        }
        return std::nullopt;
    }
};

/// The hub's side: a hub, a controller with a time gate, and the hub's transport on the
/// controller's participant.
struct HubSide
{
    Hub hub{HubOptions{4096, std::uint64_t{1000}}};
    std::unique_ptr<SimulationController> controller;
    std::unique_ptr<HubTransport> transport;

    HubSide(const std::string& uri, std::vector<std::string> required, SimDuration step = 10ms, bool start_paused = true,
            vehicle_sil::control_silkit::HubTransportOptions options = {})
    {
        SimulationControllerOptions c;
        c.registry_uri = uri;
        c.required_participants = std::move(required);
        TimeControlOptions time;
        time.step = step;
        time.start_paused = start_paused;
        c.time_control = time;
        c.shutdown_timeout = 2000ms;
        controller = std::make_unique<SimulationController>(std::move(c));
        transport = std::make_unique<HubTransport>(hub, controller->runtime(), options);
    }

    /// Waits until the agents of `names` have said that they receive commands (a moment after
    /// their hello): a write before that is refused.
    void wait_ready(const std::vector<std::string>& names)
    {
        const bool ready = eventually(
            [&] {
                std::size_t n = 0;
                for (const auto& link : transport->links())
                {
                    n += link.ready && std::find(names.begin(), names.end(), link.participant) != names.end() ? 1 : 0;
                }
                return n == names.size();
            },
            30s);
        if (!ready)
        {
            throw vehicle_sil::unit::Failure("the agents did not say that they receive commands");
        }
    }

    ~HubSide()
    {
        if (controller)
        {
            controller->stop("the test ends");
        }
        transport.reset();
        controller.reset();
    }
};

/// The target of `operation` for `peer`.
TargetResult target(const OperationResult& operation, const std::string& peer)
{
    for (const auto& t : operation.targets)
    {
        if (t.peer == peer)
        {
            return t;
        }
    }
    throw vehicle_sil::unit::Failure("the operation has no target " + peer);
}

std::string text_of(TargetStatus status)
{
    static const char* const kNames[] = {"Sent",     "Queued",    "Disconnected", "Applied", "Late",
                                         "Rejected", "Cancelled", "Invalid",      "Busy",    "Unknown"};
    return kNames[static_cast<std::size_t>(status)];
}

/// The result of operation `seq` (`what` it is, for the message), which has to settle: fails the
/// test, instead of reading an empty optional, when it did not settle within the wait.
OperationResult settled(const std::optional<OperationResult>& done, std::uint64_t seq, const std::string& what)
{
    if (!done)
    {
        throw std::runtime_error("operation " + std::to_string(seq) + " (" + what + ") did not settle within the wait");
    }
    return *done;
}

void check_status(const OperationResult& operation, const std::string& peer, TargetStatus expected)
{
    const TargetResult t = target(operation, peer);
    if (t.status != expected)
    {
        throw vehicle_sil::unit::Failure("operation " + std::to_string(operation.seq) + " for " + peer + " is " +
                                         text_of(t.status) + " (" + t.reason + "), expected " + text_of(expected));
    }
}

} // namespace

// When the gate stands still at P, the hub has every sample and result of the steps before
// P; a write made while it stands still takes effect in the next step; nothing arrives late.
TEST_CASE(values_before_a_pause_are_complete_and_a_write_while_paused_takes_effect_in_the_next_step)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"A", "B"});
    TestEcu a("A", registry.uri, {.step = 1ms});
    TestEcu b("B", registry.uri, {.step = 3ms});
    const std::vector<TestEcu*> ecus{&a, &b};
    start_together(*side.controller, {a.peer.get(), b.peer.get()});
    auto& controller = *side.controller;
    auto& hub = side.hub;

    const auto paused0 = controller.wait_until_paused(scaled_timeout(20s));
    CHECK(paused0.has_value());
    CHECK(*paused0 == SimTime{});
    CHECK_EQ(hub.peers().size(), std::size_t{2});
    side.wait_ready({"A", "B"});

    struct Pending
    {
        std::uint64_t untimed = 0;
        std::uint64_t timed = 0;
        SimTime paused_at{};
        std::int64_t value = 0;
    };
    std::optional<Pending> pending;
    std::vector<Pending> done;
    // Targets that are no multiples of the controller's step among them: the gate stands still
    // at its first step at or after the target.
    const SimDuration advances[] = {10ms, 20ms, 10ms, 30ms, 70ms, 10ms, 13ms, 10ms, 5ms, 40ms};
    SimTime target_time{};
    std::size_t before_p = 0;
    std::size_t at_p_arrived = 0;
    constexpr int kIterations = 150;
    for (int i = 0; i < kIterations; ++i)
    {
        target_time += advances[static_cast<std::size_t>(i) % std::size(advances)];
        controller.run_until(target_time);
        const auto paused = controller.wait_until_paused(scaled_timeout(30s));
        CHECK(paused.has_value());
        const SimTime p = *paused;
        CHECK(p >= target_time);
        CHECK(p < target_time + 10ms);
        target_time = p;

        // At once, without waiting: everything of the steps before P is in the hub.
        for (TestEcu* ecu : ecus)
        {
            const auto snapshot = hub.latest(ecu->name);
            CHECK(snapshot != nullptr);
            const SimTime first_at_or_after = first_step_at_or_after(p, ecu->options.step);
            const SimTime last_before = first_at_or_after - ecu->options.step;
            const auto steps_before = static_cast<std::uint64_t>(first_at_or_after.time_since_epoch() / ecu->options.step);
            // The step at P itself (if the ECU has one) may or may not have arrived; none after it.
            CHECK(snapshot->time >= last_before);
            CHECK(snapshot->time <= p);
            CHECK(snapshot->sequence >= steps_before);
            CHECK(snapshot->sequence <= steps_before + 1);
            // The sample is the one of that step: `steps` counts the steps, as the sequence does.
            const auto steps = value_in(hub, ecu->name, snapshot, "steps");
            CHECK(steps.has_value());
            CHECK_EQ(static_cast<std::uint64_t>(std::get<std::int64_t>(*steps)), snapshot->sequence);
            ++before_p;
            at_p_arrived += snapshot->time == p ? 1 : 0;
        }
        // And the results of the writes made at the previous pause: applied in a step before P.
        if (pending)
        {
            const auto untimed = hub.operation(pending->untimed);
            const auto timed = hub.operation(pending->timed);
            CHECK(untimed.has_value());
            CHECK(timed.has_value());
            CHECK(untimed->done());
            CHECK(timed->done());
            for (TestEcu* ecu : ecus)
            {
                check_status(*untimed, ecu->name, TargetStatus::Applied);
                check_status(*timed, ecu->name, TargetStatus::Applied);
                const SimTime next = first_step_at_or_after(pending->paused_at + SimDuration{1}, ecu->options.step);
                // With a time after the pause: the first step after the pause, exactly.
                CHECK(target(*timed, ecu->name).applied_at == next);
                CHECK(!target(*timed, ecu->name).late);
                // Without a time: the next step the ECU runs, which is the one at the pause if it
                // had not run yet, else the first after it.
                const auto applied = target(*untimed, ecu->name).applied_at;
                CHECK(applied.has_value());
                CHECK(*applied >= pending->paused_at);
                CHECK(*applied <= next);
            }
            done.push_back(*pending);
            pending.reset();
        }
        // Two writes while the simulation stands still.
        Pending now;
        now.paused_at = p;
        now.value = 100 + i;
        now.untimed = hub.write(OperationRequest{{{"A", "cmd", Value{now.value}}, {"B", "cmd", Value{now.value}}}});
        now.timed = hub.write(OperationRequest{{{"A", "timed", Value{now.value}}, {"B", "timed", Value{now.value}}},
                                               p + SimDuration{1}});
        // The agents answer while the simulation stands still.
        const auto queued = hub.wait_queued(now.timed, scaled_timeout(20s));
        CHECK(queued.has_value());
        for (TestEcu* ecu : ecus)
        {
            check_status(*queued, ecu->name, TargetStatus::Queued);
        }
        const auto accepted = hub.wait_queued(now.untimed, scaled_timeout(20s));
        CHECK(accepted.has_value());
        for (TestEcu* ecu : ecus)
        {
            const auto status = target(*accepted, ecu->name).status;
            CHECK(status == TargetStatus::Queued || status == TargetStatus::Applied);
        }
        pending = now;
    }
    CHECK_EQ(before_p, static_cast<std::size_t>(2 * kIterations));
    CHECK(at_p_arrived <= before_p); // how often the step at P had arrived already is not decided

    // While the simulation stands still at P, what the steps at P produced arrives too.
    const SimTime p = target_time;
    CHECK(eventually(
        [&] {
            for (TestEcu* ecu : ecus)
            {
                const auto snapshot = hub.latest(ecu->name);
                if (p.time_since_epoch() % ecu->options.step == SimDuration::zero() && snapshot->time != p)
                {
                    return false;
                }
            }
            return true;
        },
        20s));

    const auto stats = side.transport->stats();
    const auto links = side.transport->links();
    side.controller->stop("done");
    a.end();
    b.end();

    // The application saw each timed write in the step that applied it, and not before.
    for (const Pending& write : done)
    {
        for (TestEcu* ecu : ecus)
        {
            const SimTime applied = first_step_at_or_after(write.paused_at + SimDuration{1}, ecu->options.step);
            const auto seen = ecu->timed_at(applied);
            const auto before = ecu->timed_at(applied - ecu->options.step);
            CHECK(seen.has_value());
            CHECK(before.has_value());
            CHECK_EQ(*seen, write.value);
            CHECK(*before != write.value);
        }
    }
    CHECK_EQ(done.size(), static_cast<std::size_t>(kIterations - 1));

    // No message of a step arrived after the controller had begun a later step.
    CHECK_EQ(stats.late_messages, std::uint64_t{0});
    CHECK_EQ(stats.undecodable, std::uint64_t{0});
    CHECK_EQ(stats.unknown, std::uint64_t{0});
    CHECK_EQ(stats.hellos, std::uint64_t{2});
    CHECK(stats.messages > 2000);
    CHECK_EQ(links.size(), std::size_t{2});
    for (const auto& link : links)
    {
        CHECK(link.state == LinkState::Connected);
        CHECK(link.virtual_time);
        CHECK_EQ(link.late_messages, std::uint64_t{0});
        CHECK_EQ(link.variables, std::size_t{5}); // with ecu.connected
        CHECK(link.step == (link.participant == "A" ? SimDuration{1ms} : SimDuration{3ms}));
    }
    // The receive handlers are short: the simulation waits for them. A bound on the mean (wall
    // clock, but a thousand times what decoding a message and handing it to the hub takes; a
    // machine would have to stall for seconds in all to reach it).
    CHECK(stats.handler_calls >= stats.messages);
    CHECK(stats.handler_time / static_cast<std::int64_t>(stats.handler_calls) < 2ms);
}

// The read a Control Server makes for a snapshot: the gate's time first, then all the values in
// one look at the hub. Whatever the simulation does meanwhile (it runs, stops, goes on), the
// values are never older than the time: every step before the time is in them. And when the time
// stood still and no value is of a later step, they are the values of that moment.
TEST_CASE(the_time_and_then_all_values_are_never_older_than_the_time_while_the_simulation_runs_and_stops)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"A", "B"});
    TestEcu a("A", registry.uri, {.step = 1ms});
    TestEcu b("B", registry.uri, {.step = 3ms});
    start_together(*side.controller, {a.peer.get(), b.peer.get()});
    auto& controller = *side.controller;
    auto& hub = side.hub;
    CHECK(controller.wait_until_paused(scaled_timeout(20s)) == SimTime{});

    struct Look
    {
        TimeState time;
        std::vector<LatestValues> values;
    };
    const auto look = [&] {
        Look l;
        l.time = controller.time_state(); // first
        l.values = hub.latest_of({"A", "B"});
        return l;
    };
    // What is wrong with a look ("" if nothing).
    const auto wrong = [&](const Look& l) -> std::string {
        const SimDuration steps[] = {1ms, 3ms};
        for (std::size_t i = 0; i < l.values.size(); ++i)
        {
            const auto& sample = l.values[i].snapshot;
            if (l.time.now == SimTime{})
            {
                continue; // no step before 0
            }
            const SimTime last_before = first_step_at_or_after(l.time.now, steps[i]) - steps[i];
            if (!sample || sample->time < last_before)
            {
                return l.values[i].participant + " at " + std::to_string(sample ? sample->time.time_since_epoch().count() : -1)
                       + " ns is older than the last step before " + std::to_string(l.time.now.time_since_epoch().count()) + " ns";
            }
        }
        return {};
    };

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> looks{0};
    std::atomic<std::uint64_t> moving{0};     ///< looks at a time that went on
    std::atomic<std::uint64_t> still{0};      ///< looks at a time that stood still
    std::atomic<std::uint64_t> consistent{0}; ///< of those: no value after it
    std::mutex failure_mutex;
    std::string failure;
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r)
    {
        readers.emplace_back([&] {
            while (!stop.load())
            {
                const Look l = look();
                const std::string what = wrong(l);
                if (!what.empty())
                {
                    const std::lock_guard lock(failure_mutex);
                    failure = what;
                    return;
                }
                looks.fetch_add(1);
                moving.fetch_add(l.time.paused ? 0 : 1);
                if (l.time.paused)
                {
                    still.fetch_add(1);
                    consistent.fetch_add(none_after(l.values, l.time.now) ? 1 : 0);
                }
            }
        });
    }

    // Mostly it runs free and is stopped (a look sees a step begin between its two reads only
    // while it runs); now and then it steps. With a look at once after each stop.
    const SimDuration advances[] = {10ms, 13ms, 40ms, 5ms, 20ms, 70ms, 10ms};
    const auto give_up = Wall::now() + scaled_timeout(120s);
    constexpr std::uint64_t kMoving = 20000;
    int rounds = 0;
    for (; (rounds < 40 || moving.load() < kMoving) && Wall::now() < give_up; ++rounds)
    {
        if (rounds % 4 != 3)
        {
            controller.resume();
            std::this_thread::sleep_for(std::chrono::microseconds(1000 + 700 * (rounds % 5)));
            controller.pause();
        }
        else
        {
            controller.step(advances[static_cast<std::size_t>(rounds) % std::size(advances)]);
        }
        const auto paused = controller.wait_until_paused(scaled_timeout(30s));
        CHECK(paused.has_value());
        // Nobody lets it go on now: the look is of this moment.
        const Look l = look();
        CHECK(l.time.paused);
        CHECK(l.time.now == *paused);
        CHECK(wrong(l).empty());
        CHECK(none_after(l.values, l.time.now));
    }
    stop.store(true);
    for (auto& reader : readers)
    {
        reader.join();
    }
    {
        const std::lock_guard lock(failure_mutex);
        if (!failure.empty())
        {
            throw vehicle_sil::unit::Failure(failure);
        }
    }
    CHECK(moving.load() >= kMoving);
    CHECK(looks.load() > moving.load());
    CHECK(still.load() > 0);
    CHECK(consistent.load() > 0);
    CHECK_EQ(side.transport->stats().late_messages, std::uint64_t{0});
}

// A parameter written in the window before the start is what the application sees in its
// step at 0, whatever the order in which the ECUs and the controller come up.
TEST_CASE(a_write_in_the_start_window_is_seen_in_the_first_step_in_every_start_order)
{
    std::mt19937 rng(20261112);
    const auto jitter = [&rng] { std::this_thread::sleep_for(std::chrono::microseconds(rng() % 15000)); };
    constexpr int kRounds = 4;
    for (int round = 0; round < kRounds; ++round)
    {
        for (int order = 0; order < 5; ++order)
        {
            const InProcessRegistry registry;
            const std::int64_t base = 1000 + round * 100 + order * 10;
            std::unique_ptr<HubSide> side;
            std::unique_ptr<TestEcu> ecus[2];
            std::vector<std::thread> starters;
            std::thread controller_thread;
            std::string start_error;
            HelloReport hellos;
            std::optional<OperationResult> queued;
            std::uint64_t seq = 0;

            const auto make_hub = [&] {
                side = std::make_unique<HubSide>(registry.uri, std::vector<std::string>{"A", "B"});
                side->controller->set_start_window([&, s = side.get()](const StartWindow& window) {
                    hellos = s->transport->wait_for_hellos({"A", "B"}, std::min(window.deadline(), Wall::now() + 20s),
                                                           [&window] { return window.cancelled(); });
                    seq = s->hub.write(OperationRequest{{{"A", "gain", Value{base}}, {"B", "gain", Value{base + 1}}}});
                    queued = s->hub.wait_queued(seq, scaled_timeout(20s));
                });
            };
            const auto make_ecu = [&](int i) {
                ecus[i] = std::make_unique<TestEcu>(i == 0 ? "A" : "B", registry.uri, EcuOptions{.step = i == 0 ? 1ms : 3ms});
            };
            const auto start_ecu = [&](int i) {
                starters.emplace_back([&, i] {
                    try
                    {
                        ecus[i]->peer->start(60s);
                    }
                    catch (const std::exception&)
                    {
                        // Seen below: the controller's start fails too.
                    }
                });
            };
            const auto start_controller = [&] {
                controller_thread = std::thread([&] {
                    try
                    {
                        side->controller->start(60s);
                    }
                    catch (const std::exception& e)
                    {
                        start_error = e.what();
                    }
                });
            };
            switch (order)
            {
            case 0: // the hub's side first
                make_hub();
                jitter();
                make_ecu(0);
                make_ecu(1);
                start_ecu(0);
                start_ecu(1);
                jitter();
                start_controller();
                break;
            case 1: // the ECUs exist (their hello is published) before the controller's participant does
                make_ecu(0);
                make_ecu(1);
                jitter();
                make_hub();
                start_ecu(0);
                start_ecu(1);
                jitter();
                start_controller();
                break;
            case 2: // the ECUs wait in start() when the controller appears
                make_ecu(0);
                make_ecu(1);
                start_ecu(0);
                start_ecu(1);
                std::this_thread::sleep_for(30ms);
                jitter();
                make_hub();
                jitter();
                start_controller();
                break;
            case 3: // the controller waits in start() when the ECUs appear
                make_hub();
                start_controller();
                std::this_thread::sleep_for(30ms);
                jitter();
                make_ecu(0);
                start_ecu(0);
                jitter();
                make_ecu(1);
                start_ecu(1);
                break;
            default: // one ECU before the controller, one after
                make_ecu(0);
                start_ecu(0);
                jitter();
                make_hub();
                start_controller();
                std::this_thread::sleep_for(30ms);
                jitter();
                make_ecu(1);
                start_ecu(1);
                break;
            }
            controller_thread.join();
            for (auto& t : starters)
            {
                t.join();
            }
            CHECK(start_error.empty());
            CHECK_EQ(hellos.connected.size(), std::size_t{2});
            CHECK(hellos.missing.empty());
            // Both agents accepted the parameter before the start.
            CHECK(queued.has_value());
            check_status(*queued, "A", TargetStatus::Queued);
            check_status(*queued, "B", TargetStatus::Queued);

            auto& controller = *side->controller;
            const auto paused = controller.wait_until_paused(scaled_timeout(20s));
            CHECK(paused.has_value());
            CHECK(*paused == SimTime{});
            controller.run_until(at(30ms));
            CHECK(controller.wait_until_paused(scaled_timeout(20s)).has_value());
            // Applied in the step at 0, and reported by the time the gate stands at 30 ms.
            const auto result = side->hub.operation(seq);
            CHECK(result.has_value());
            for (const char* name : {"A", "B"})
            {
                check_status(*result, name, TargetStatus::Applied);
                CHECK(target(*result, name).applied_at == SimTime{});
            }
            CHECK(hub_value(side->hub, "A", "gain") == Value{base});
            CHECK(hub_value(side->hub, "B", "gain") == Value{base + 1});
            // A parameter is refused once the simulation has run.
            const std::uint64_t later = side->hub.write(OperationRequest{{{"A", "gain", Value{std::int64_t{5}}}}});
            const auto refused = side->hub.wait_done(later, scaled_timeout(20s));
            CHECK(refused.has_value());
            check_status(*refused, "A", TargetStatus::Invalid);
            CHECK_EQ(side->transport->stats().late_messages, std::uint64_t{0});

            controller.stop("done");
            ecus[0]->end();
            ecus[1]->end();
            // What the application saw in its step at 0.
            CHECK_EQ(ecus[0]->gain_at_step0.load(), base);
            CHECK_EQ(ecus[1]->gain_at_step0.load(), base + 1);
            side.reset();
        }
    }
}

TEST_CASE(a_missing_hello_ends_the_wait_at_its_deadline_and_a_stop_request_ends_it_at_once)
{
    {
        // An ECU with variables and no transport: the wait ends at its deadline, and the
        // simulation starts without it being a peer.
        const InProcessRegistry registry;
        HubSide side(registry.uri, {"A", "Silent"});
        TestEcu a("A", registry.uri);
        TestEcu silent("Silent", registry.uri, {.transport = false});
        HelloReport first;
        HelloReport hellos;
        Wall::duration waited{};
        side.controller->set_start_window([&](const StartWindow& window) {
            // A's hello comes (how soon depends on the machine): first that, within the window,
            // then the wait that only its deadline ends.
            first = side.transport->wait_for_hellos({"A"}, std::min(window.deadline(), Wall::now() + scaled_timeout(30s)),
                                                    [&window] { return window.cancelled(); });
            const auto begin = Wall::now();
            hellos = side.transport->wait_for_hellos({"A", "Silent"}, begin + 300ms, [&window] { return window.cancelled(); });
            waited = Wall::now() - begin;
        });
        start_together(*side.controller, {a.peer.get(), silent.peer.get()});
        CHECK(first.connected == std::vector<std::string>{"A"});
        CHECK(hellos.connected == std::vector<std::string>{"A"});
        CHECK(hellos.missing == std::vector<std::string>{"Silent"});
        CHECK(hellos.mismatched.empty());
        CHECK(hellos.ended.empty());
        CHECK(!hellos.cancelled);
        CHECK(waited >= 300ms);
        CHECK_EQ(side.hub.peers().size(), std::size_t{1});
        CHECK(side.controller->wait_until_paused(scaled_timeout(20s)).has_value());
    }
    {
        // A stop requested while the window waits: the wait ends, the start goes on (and the
        // stop is then carried out) instead of waiting for the deadline.
        const InProcessRegistry registry;
        HubSide side(registry.uri, {"Silent"});
        TestEcu silent("Silent", registry.uri, {.transport = false});
        HelloReport hellos;
        std::atomic<bool> waiting{false};
        side.controller->set_start_window([&](const StartWindow& window) {
            waiting.store(true);
            hellos = side.transport->wait_for_hellos({"Silent"}, Wall::now() + 120s, [&window] { return window.cancelled(); });
        });
        std::thread stopper([&] {
            if (eventually([&] { return waiting.load(); }, 60s))
            {
                std::this_thread::sleep_for(50ms);
                side.controller->runtime().request_stop("the test stops it in the window");
            }
        });
        const auto begin = Wall::now();
        start_together(*side.controller, {silent.peer.get()}, 60s);
        stopper.join();
        CHECK(Wall::now() - begin < 30s);
        CHECK(hellos.cancelled);
        CHECK(hellos.missing == std::vector<std::string>{"Silent"});
        // The stop that was requested in the window ends the simulation in order.
        const auto state = side.controller->wait_until_finished(scaled_timeout(20s));
        CHECK(state == orch::ParticipantState::Shutdown);
    }
}

TEST_CASE(a_hello_of_another_version_is_reported_and_makes_no_peer)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"A"});
    TestEcu a("A", registry.uri);
    // A participant outside the simulation's lifecycle that says hello in a later version.
    auto old = SilKit::CreateParticipant(
        SilKit::Config::ParticipantConfigurationFromString(vehicle_sil::default_participant_configuration(registry.uri)), "Newer",
        registry.uri);
    namespace detail = vehicle_sil::control_silkit::detail;
    auto* publisher = old->CreateDataPublisher(detail::kHelloTopic, detail::spec(detail::kHelloTopic), 1);
    wire::Hello hello;
    hello.participant = "Newer";
    hello.session = 5;
    wire::Bytes bytes;
    std::string why;
    CHECK(wire::encode_hello(hello, bytes, why));
    bytes[4] = static_cast<std::uint8_t>(wire::kVersion + 1);
    bytes.push_back(0x55); // whatever that version adds
    publisher->Publish(detail::span_of(bytes));
    // And something that is no message at all.
    auto* noise = old->CreateDataPublisher("noise", detail::spec(detail::kStepTopic), 0);

    HelloReport hellos;
    side.controller->set_start_window([&](const StartWindow& window) {
        // Returns as soon as each has answered one way or the other, long before the deadline.
        hellos = side.transport->wait_for_hellos({"Newer", "A"}, Wall::now() + 60s, [&window] { return window.cancelled(); });
    });
    const auto begin = Wall::now();
    start_together(*side.controller, {a.peer.get()}, 60s);
    CHECK(Wall::now() - begin < 30s);
    CHECK(hellos.mismatched == std::vector<std::string>{"Newer"});
    CHECK(hellos.connected == std::vector<std::string>{"A"});
    CHECK(hellos.missing.empty());
    bool found = false;
    for (const auto& link : side.transport->links())
    {
        if (link.participant == "Newer")
        {
            found = true;
            CHECK(link.state == LinkState::VersionMismatch);
            CHECK_EQ(link.version, static_cast<std::uint16_t>(wire::kVersion + 1));
        }
    }
    CHECK(found);
    CHECK_EQ(side.hub.peers().size(), std::size_t{1});

    const wire::Bytes junk{'n', 'o', 't', ' ', 'a', ' ', 'm', 'e', 's', 's', 'a', 'g', 'e'};
    noise->Publish(detail::span_of(junk));
    CHECK(eventually([&] { return side.transport->stats().undecodable >= 2; }, 20s));
    // The simulation is not disturbed.
    side.controller->run_until(at(20ms));
    CHECK(side.controller->wait_until_paused(scaled_timeout(20s)).has_value());
    CHECK(side.hub.latest("A") != nullptr);
    side.controller->stop("done");
    a.end();
}

TEST_CASE(a_participant_that_leaves_in_the_start_window_ends_the_wait)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"Gone"});
    // A coordinated participant that gets as far as the window and then disappears without a word.
    auto gone = SilKit::CreateParticipant(
        SilKit::Config::ParticipantConfigurationFromString(vehicle_sil::default_participant_configuration(registry.uri)), "Gone",
        registry.uri);
    auto* lifecycle = gone->CreateLifecycleService({orch::OperationMode::Coordinated});
    auto done = lifecycle->StartLifecycle();

    HelloReport hellos;
    std::atomic<bool> waiting{false};
    side.controller->set_start_window([&](const StartWindow& window) {
        waiting.store(true);
        hellos = side.transport->wait_for_hellos({"Gone"}, window.deadline(), [&window] { return window.cancelled(); });
    });
    std::thread leaver([&] {
        if (eventually([&] { return waiting.load(); }, 60s))
        {
            std::this_thread::sleep_for(50ms);
            gone.reset(); // the connection is lost
        }
    });
    const auto begin = Wall::now();
    // The start cannot succeed any more (a required participant is gone); it fails at its
    // timeout, and neither it nor the window hangs.
    CHECK(throws<std::runtime_error>([&] { side.controller->start(5s); }));
    leaver.join();
    CHECK(Wall::now() - begin < 30s);
    CHECK(hellos.ended == std::vector<std::string>{"Gone"});
    CHECK(hellos.missing.empty());
}

TEST_CASE(start_window_of_the_controller_is_checked_and_its_failure_fails_the_start)
{
    const InProcessRegistry registry;
    {
        HubSide side(registry.uri, {"A"});
        CHECK(throws<std::invalid_argument>([&] { side.controller->set_start_window({}); }));
        side.controller->set_start_window([](const StartWindow&) {});
        CHECK(throws<std::logic_error>([&] { side.controller->set_start_window([](const StartWindow&) {}); }));
    }
    {
        // The function throws: the simulation starts all the same (nobody is left waiting), and
        // start() says what happened.
        HubSide side(registry.uri, {"A"});
        TestEcu a("A", registry.uri);
        std::atomic<bool> cancelled_in_window{true};
        side.controller->set_start_window([&](const StartWindow& window) {
            cancelled_in_window.store(window.cancelled());
            throw std::runtime_error("the window's work failed");
        });
        std::string error;
        std::thread ecu([&] { a.peer->start(60s); });
        try
        {
            side.controller->start(60s);
        }
        catch (const std::runtime_error& e)
        {
            error = e.what();
        }
        ecu.join();
        CHECK(error.find("the start window failed: the window's work failed") != std::string::npos);
        CHECK(!cancelled_in_window.load());
        CHECK(side.controller->wait_until_paused(scaled_timeout(20s)).has_value());
    }
    {
        // The function does not return before start()'s timeout: start() throws at its timeout,
        // the function is told, and the destructor waits for it.
        HubSide side(registry.uri, {"A"});
        TestEcu a("A", registry.uri);
        std::atomic<bool> told{false};
        side.controller->set_start_window([&](const StartWindow& window) {
            while (!window.cancelled())
            {
                std::this_thread::sleep_for(5ms);
            }
            told.store(true);
        });
        std::thread ecu([&] {
            try
            {
                a.peer->start(60s);
            }
            catch (const std::exception&)
            {
            }
        });
        const auto begin = Wall::now();
        CHECK(throws<StartTimeoutError>([&] { side.controller->start(700ms); }));
        CHECK(Wall::now() - begin < 20s);
        CHECK(eventually([&] { return told.load(); }, 20s));
        ecu.join(); // the window was completed after all, so the ECU started
    }
}

TEST_CASE(the_end_of_an_agent_settles_the_operations_that_wait_for_it)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"A", "B", "Failing"});
    // The hub answers every last word here, so no ECU waits long: with plenty of time for the
    // answer, what the hub has when end() has returned does not depend on how busy the machine is.
    TestEcu a("A", registry.uri, {.last_word_wait = 60s});
    TestEcu b("B", registry.uri, {.last_word_wait = 60s});
    TestEcu failing("Failing", registry.uri, {.fail_at = at(15ms), .last_word_wait = 60s});
    start_together(*side.controller, {a.peer.get(), b.peer.get(), failing.peer.get()});
    auto& hub = side.hub;
    CHECK(side.controller->wait_until_paused(scaled_timeout(20s)).has_value());
    side.wait_ready({"A", "B", "Failing"});

    // Writes far in the future: queued on each agent.
    const SimTime far = at(std::chrono::hours(1));
    const std::uint64_t seq = hub.write(
        OperationRequest{{{"A", "cmd", Value{std::int64_t{1}}}, {"B", "cmd", Value{std::int64_t{1}}}, {"Failing", "cmd", Value{std::int64_t{1}}}},
                         far});
    const auto queued = hub.wait_queued(seq, scaled_timeout(20s));
    CHECK(queued.has_value());
    for (const char* name : {"A", "B", "Failing"})
    {
        check_status(*queued, name, TargetStatus::Queued);
    }

    // One ECU fails in a step: its lifecycle goes to Error, and what waits for it is settled
    // (nobody can say whether a later result would have come).
    side.controller->resume();
    CHECK(eventually(
        [&] {
            const auto op = hub.operation(seq);
            return op && target(*op, "Failing").status == TargetStatus::Unknown;
        },
        20s));
    const auto after_failure = hub.operation(seq);
    CHECK(target(*after_failure, "Failing").reason.find("lifecycle state Error") != std::string::npos);
    check_status(*after_failure, "A", TargetStatus::Queued);
    for (const auto& peer : hub.peers())
    {
        CHECK_EQ(peer.connected, peer.participant != "Failing");
    }
    // A write to the agent that has ended is refused at once.
    const std::uint64_t refused = hub.write(OperationRequest{{{"Failing", "cmd", Value{std::int64_t{2}}}}});
    const auto refused_result = hub.wait_done(refused, scaled_timeout(20s));
    CHECK(refused_result.has_value());
    check_status(*refused_result, "Failing", TargetStatus::Invalid);

    // The others end in order: their last word carries the cancelled writes. From the moment
    // their lifecycle stops, a write is refused at once (were their last word not to arrive,
    // nothing else would settle it).
    side.controller->abort();
    CHECK(eventually([&] { return has_finished(side.controller->runtime().peer_status("A")); }, 20s));
    const std::uint64_t too_late = hub.write(OperationRequest{{{"A", "cmd", Value{std::int64_t{3}}}}});
    const auto too_late_result = hub.wait_done(too_late, scaled_timeout(20s));
    CHECK(too_late_result.has_value());
    check_status(*too_late_result, "A", TargetStatus::Invalid);
    // The ECU ends as its process would: the transport, and right after it the participant. The
    // transport has waited for the hub's answer to its last word, and the hub answers after it
    // has ended the connection: nothing is left to wait for here.
    a.end();
    check_status(*hub.operation(seq), "A", TargetStatus::Cancelled);
    CHECK(!hub.wait_done(seq, 100ms).has_value()); // B is still open
    b.end();
    const auto all = hub.wait_done(seq, 0ms);
    CHECK(all.has_value());
    check_status(*all, "A", TargetStatus::Cancelled);
    check_status(*all, "B", TargetStatus::Cancelled);
    check_status(*all, "Failing", TargetStatus::Unknown);
    for (const auto& link : side.transport->links())
    {
        CHECK(link.state == LinkState::Ended);
    }
    for (const auto& peer : hub.peers())
    {
        CHECK(!peer.connected);
    }
    // The failed ECU's process ends too and its agent says its last word, for a connection that
    // has ended already: answered all the same (the ECU does not wait for nothing), and not
    // counted as a message nobody knows.
    const auto before = side.transport->stats();
    CHECK_EQ(before.last_words, std::uint64_t{2});
    const auto began = Wall::now();
    failing.end();
    CHECK(Wall::now() - began < 50s);
    CHECK_EQ(side.transport->stats().last_words, std::uint64_t{3});
    CHECK_EQ(side.transport->stats().unknown, std::uint64_t{0});
}

// An ECU's process destroys its participant right after the transport. SIL Kit drops what the
// participant has not written to the socket by then, and the last word waits behind every earlier
// send to the hub's participant: here behind megabytes of another topic. The transport therefore
// waits for the hub's answer before it lets the participant go.
TEST_CASE(the_last_word_arrives_though_the_participant_goes_right_after_it_with_a_large_send_under_way)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"A"});
    const char* const kNoise = "vehicle_sil.test.noise";
    std::atomic<std::uint64_t> noise_received{0};
    side.controller->runtime().participant().CreateDataSubscriber(
        kNoise, topics::spec(kNoise),
        [&noise_received](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) { noise_received += event.data.size(); });
    TestEcu a("A", registry.uri, {.last_word_wait = 60s});
    auto* const noise = a.peer->runtime().participant().CreateDataPublisher(kNoise, topics::spec(kNoise), 0);
    start_together(*side.controller, {a.peer.get()});
    CHECK(side.controller->wait_until_paused(scaled_timeout(20s)).has_value());
    side.wait_ready({"A"});
    const std::uint64_t seq = side.hub.write(OperationRequest{{{"A", "cmd", Value{std::int64_t{1}}}}, at(std::chrono::hours(1))});
    CHECK(side.hub.wait_queued(seq, scaled_timeout(20s)).has_value());
    side.controller->abort();
    CHECK(eventually([&] { return has_finished(side.controller->runtime().peer_status("A")); }, 20s));

    a.peer->stop("the test ECU ends");
    const std::vector<std::uint8_t> big(4 * 1024 * 1024, 0x5A);
    for (int i = 0; i < 4; ++i)
    {
        noise->Publish(topics::span_of(big));
    }
    a.transport.reset();
    a.peer.reset();
    const auto done = side.hub.wait_done(seq, 0ms);
    CHECK(done.has_value());
    check_status(*done, "A", TargetStatus::Cancelled);
    CHECK(side.transport->links().at(0).state == LinkState::Ended);
    CHECK(!side.hub.peers().at(0).connected);
}

TEST_CASE(closing_the_hub_transport_settles_what_is_open)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"A"});
    TestEcu a("A", registry.uri);
    start_together(*side.controller, {a.peer.get()});
    CHECK(side.controller->wait_until_paused(scaled_timeout(20s)).has_value());
    side.wait_ready({"A"});
    const std::uint64_t seq = side.hub.write(OperationRequest{{{"A", "cmd", Value{std::int64_t{1}}}}, at(std::chrono::hours(1))});
    CHECK(side.hub.wait_queued(seq, scaled_timeout(20s)).has_value());
    side.transport.reset();
    const auto closed = side.hub.wait_done(seq, scaled_timeout(20s));
    CHECK(closed.has_value());
    check_status(*closed, "A", TargetStatus::Unknown);
    CHECK(!side.hub.peers().at(0).connected);
    // A write now is not sent.
    const std::uint64_t later = side.hub.write(OperationRequest{{{"A", "cmd", Value{std::int64_t{2}}}}});
    check_status(settled(side.hub.wait_done(later, scaled_timeout(20s)), later, "a write after the hub transport was closed"), "A", TargetStatus::Invalid);
    side.controller->stop("done");
    a.end();
}

TEST_CASE(a_sample_larger_than_a_message_arrives_whole)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"Big"});
    TestEcu big("Big", registry.uri, {.extra_outputs = 700, .max_message_bytes = wire::kMinMessageBytes});
    start_together(*side.controller, {big.peer.get()});
    CHECK(side.controller->wait_until_paused(scaled_timeout(20s)).has_value());
    side.controller->run_until(at(50ms));
    const auto paused = side.controller->wait_until_paused(scaled_timeout(20s));
    CHECK(paused.has_value());
    const auto snapshot = side.hub.latest("Big");
    CHECK(snapshot != nullptr);
    CHECK_EQ(snapshot->values.size(), std::size_t{705}); // ecu.connected first
    CHECK(snapshot->time >= *paused - 1ms);
    for (std::size_t i = 0; i < 700; ++i)
    {
        CHECK(snapshot->values[5 + i] == Value{static_cast<std::int64_t>(i) * 3});
    }
    const auto agent_stats = big.transport->stats();
    const auto steps = static_cast<std::uint64_t>(std::get<std::int64_t>(*value_in(side.hub, "Big", snapshot, "steps")));
    // 700 integers of 9 bytes in messages of at most 2048 bytes: at least 4 messages a step.
    CHECK(agent_stats.step_messages >= 4 * steps);
    CHECK(agent_stats.step_bytes / agent_stats.step_messages <= wire::kMinMessageBytes);
    const auto stats = side.transport->stats();
    CHECK_EQ(stats.late_messages, std::uint64_t{0});
    CHECK_EQ(stats.unknown, std::uint64_t{0});
    side.controller->stop("done");
    big.end();
}

TEST_CASE(a_real_time_peer_is_a_peer_too_and_a_restarted_one_replaces_the_old)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"A"}, 10ms, false);
    TestEcu a("A", registry.uri);
    auto real = std::make_unique<TestEcu>("Real", registry.uri, EcuOptions{.virtual_time = false});
    real->peer->start(30s);
    start_together(*side.controller, {a.peer.get()});
    auto& hub = side.hub;

    // Its values arrive (without any promise about when), and a write takes effect.
    CHECK(eventually([&] { return hub.latest("Real") != nullptr && hub.latest("Real")->sequence >= 3; }, 30s));
    // It joined on its own: a write goes out once it has said that it receives commands.
    side.wait_ready({"Real"});
    const std::uint64_t seq = hub.write(OperationRequest{{{"Real", "cmd", Value{std::int64_t{9}}}}});
    const auto result = hub.wait_done(seq, scaled_timeout(30s));
    CHECK(result.has_value());
    check_status(*result, "Real", TargetStatus::Applied);
    CHECK(eventually([&] { return hub_value(hub, "Real", "cmd") == Value{std::int64_t{9}}; }, 30s));
    std::string first_incarnation;
    for (const auto& link : side.transport->links())
    {
        if (link.participant == "Real")
        {
            CHECK(!link.virtual_time);
            CHECK(link.state == LinkState::Connected);
        }
    }
    for (const auto& peer : hub.peers())
    {
        if (peer.participant == "Real")
        {
            first_incarnation = peer.incarnation;
            CHECK(!peer.virtual_time); // as its hello said
        }
        else
        {
            CHECK(peer.virtual_time);
        }
    }
    CHECK_EQ(first_incarnation.size(), std::size_t{16});

    // It ends, and is started anew under the same name: a new incarnation of the same peer.
    real->end();
    CHECK(eventually(
        [&] {
            for (const auto& peer : hub.peers())
            {
                if (peer.participant == "Real")
                {
                    return !peer.connected;
                }
            }
            return false;
        },
        30s));
    // The registry may still hold the name of the process that has just gone: the peer waits for it
    // (PeerOptions::name_in_use_wait).
    real = std::make_unique<TestEcu>("Real", registry.uri, EcuOptions{.virtual_time = false});
    real->peer->start(30s);
    CHECK(eventually(
        [&] {
            for (const auto& peer : hub.peers())
            {
                if (peer.participant == "Real")
                {
                    return peer.connected && peer.incarnation != first_incarnation;
                }
            }
            return false;
        },
        30s));
    CHECK(eventually(
        [&] {
            for (const auto& link : side.transport->links())
            {
                if (link.participant == "Real")
                {
                    return link.ready;
                }
            }
            return false;
        },
        30s));
    const std::uint64_t again = hub.write(OperationRequest{{{"Real", "cmd", Value{std::int64_t{11}}}}});
    check_status(settled(hub.wait_done(again, scaled_timeout(30s)), again, "a write to the restarted Real"), "Real", TargetStatus::Applied);
    // Real-time messages are not measured against the virtual time.
    CHECK_EQ(side.transport->stats().late_messages, std::uint64_t{0});

    side.controller->stop("done");
    real->end();
    a.end();
}

TEST_CASE(transports_are_made_before_the_start_and_one_per_agent)
{
    const InProcessRegistry registry;
    {
        TestEcu a("A", registry.uri, {.transport = false});
        AgentTransportOptions bad;
        bad.max_message_bytes = wire::kMinMessageBytes - 1;
        CHECK(throws<std::invalid_argument>([&] { AgentTransport t(a.peer->agent(), a.peer->runtime(), bad); }));
        AgentTransport first(a.peer->agent(), a.peer->runtime());
        CHECK(first.session() != 0);
        CHECK_EQ(a.peer->agent().transports(), std::size_t{1});
        // The agent has one step sink.
        CHECK(throws_with<std::logic_error>([&] { AgentTransport second(a.peer->agent(), a.peer->runtime()); }, "step sink"));
        CHECK_EQ(a.peer->agent().transports(), std::size_t{1});
    }
    {
        HubSide side(registry.uri, {"A"});
        TestEcu a("A", registry.uri, {.transport = false});
        start_together(*side.controller, {a.peer.get()});
        CHECK(throws_with<std::logic_error>([&] { AgentTransport t(a.peer->agent(), a.peer->runtime()); }, "before the ECU starts"));
        Hub other;
        CHECK(throws_with<std::logic_error>([&] { HubTransport t(other, side.controller->runtime()); }, "before it starts"));
        CHECK_EQ(a.peer->agent().transports(), std::size_t{0});
        side.controller->stop("done");
    }
}

namespace {

/// A participant that speaks the agent's side by hand: whatever bytes the test gives it. It
/// answers the hub's probes only if told to.
struct RawAgent
{
    std::string name;
    std::uint64_t session;
    std::atomic<bool> answers{false};
    std::atomic<int> probes{0};
    std::atomic<int> last_word_answers{0}; ///< received for its session; it takes no other notice of them
    /// Called for each of them before it is counted, on the participant's SIL Kit thread (set
    /// before the first can come): what a test wants to see at the moment the answer arrives.
    std::function<void()> on_last_word_answer;
    std::unique_ptr<SilKit::IParticipant> participant;
    pubsub::IDataPublisher* hello_out = nullptr;
    pubsub::IDataPublisher* step_out = nullptr;
    pubsub::IDataPublisher* submitted_out = nullptr;

    RawAgent(std::string agent_name, const std::string& uri, std::uint64_t agent_session) :
      name(std::move(agent_name)), session(agent_session)
    {
        // A process started anew under the name of one that has just gone: the registry may refuse
        // the name until it has seen the end of the old connection (on a slow machine that takes a
        // while); create_participant() tries again until then.
        participant = vehicle_sil::create_participant(
            SilKit::Config::ParticipantConfigurationFromString(vehicle_sil::default_participant_configuration(uri)), name, uri,
            30s);
        hello_out = participant->CreateDataPublisher(topics::kHelloTopic, topics::spec(topics::kHelloTopic), 1);
        step_out = participant->CreateDataPublisher(topics::kStepTopic, topics::spec(topics::kStepTopic), 0);
        submitted_out = participant->CreateDataPublisher(topics::kSubmittedTopic, topics::spec(topics::kSubmittedTopic), 0);
        participant->CreateDataSubscriber(topics::kCommandTopic, topics::spec(topics::kCommandTopic),
                                          [this](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) {
                                              const auto to = wire::addressed({event.data.data(), event.data.size()});
                                              if (to && to->kind == wire::Kind::Probe && to->session == session)
                                              {
                                                  ++probes;
                                                  if (answers.load())
                                                  {
                                                      ready();
                                                  }
                                              }
                                              if (to && to->kind == wire::Kind::LastWordReceived && to->session == session)
                                              {
                                                  if (on_last_word_answer)
                                                  {
                                                      on_last_word_answer();
                                                  }
                                                  ++last_word_answers;
                                              }
                                          });
    }
    ~RawAgent() { participant.reset(); } // first: its handler uses the members

    /// A hello with `variables` variables of the types Bool, Integer, Real, Bool, ... in turn.
    void hello(std::size_t variables, const std::string& as = {}, std::uint64_t with_session = 0)
    {
        wire::Hello h;
        h.participant = as.empty() ? name : as;
        h.session = with_session != 0 ? with_session : session;
        for (std::size_t i = 0; i < variables; ++i)
        {
            VariableInfo v;
            v.name = "v" + std::to_string(i);
            v.type = i % 3 == 0 ? ValueType::Bool : i % 3 == 1 ? ValueType::Integer : ValueType::Real;
            v.causality = Causality::Input;
            h.schema.push_back(std::move(v));
        }
        wire::Bytes bytes;
        std::string why;
        if (!wire::encode_hello(h, bytes, why))
        {
            throw vehicle_sil::unit::Failure(why);
        }
        hello_out->Publish(topics::span_of(bytes));
    }
    void ready()
    {
        wire::Bytes bytes;
        wire::encode_session(wire::Kind::Ready, session, bytes);
        submitted_out->Publish(topics::span_of(bytes));
    }
    void step(const wire::Bytes& bytes) { step_out->Publish(topics::span_of(bytes)); }
};

/// A participant that speaks the hub's side by hand, as a hub from before the answer to the last
/// word existed: it asks the agents that said hello whether they receive commands, notes their
/// last words, and answers none.
struct RawHub
{
    std::mutex mutex;
    std::map<std::string, std::uint64_t> sessions; ///< guarded by mutex: by participant, from its hello
    std::set<std::uint64_t> ready;                 ///< guarded by mutex
    std::set<std::uint64_t> last_words;            ///< guarded by mutex: the sessions that said theirs
    std::unique_ptr<SilKit::IParticipant> participant;
    pubsub::IDataPublisher* command_out = nullptr;

    explicit RawHub(const std::string& uri)
    {
        participant = vehicle_sil::create_participant(
            SilKit::Config::ParticipantConfigurationFromString(vehicle_sil::default_participant_configuration(uri)), "OldHub",
            uri, 30s);
        command_out = participant->CreateDataPublisher(topics::kCommandTopic, topics::spec(topics::kCommandTopic), 0);
        participant->CreateDataSubscriber(topics::kHelloTopic, topics::spec(topics::kHelloTopic),
                                          [this](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) {
                                              wire::Hello hello;
                                              if (!wire::decode_hello({event.data.data(), event.data.size()}, hello))
                                              {
                                                  const std::lock_guard lock(mutex);
                                                  sessions[hello.participant] = hello.session;
                                              }
                                          });
        participant->CreateDataSubscriber(topics::kSubmittedTopic, topics::spec(topics::kSubmittedTopic),
                                          [this](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) {
                                              const auto from = wire::addressed({event.data.data(), event.data.size()});
                                              if (from && from->kind == wire::Kind::Ready)
                                              {
                                                  const std::lock_guard lock(mutex);
                                                  ready.insert(from->session);
                                              }
                                          });
        participant->CreateDataSubscriber(topics::kStepTopic, topics::spec(topics::kStepTopic),
                                          [this](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) {
                                              wire::StepMessage step;
                                              if (!wire::decode_step({event.data.data(), event.data.size()}, step) && step.final)
                                              {
                                                  const std::lock_guard lock(mutex);
                                                  last_words.insert(step.session);
                                              }
                                          });
    }
    ~RawHub() { participant.reset(); } // first: its handlers use the members

    /// Asks `name` (again) whether it receives commands. @returns its session once it has said so.
    std::optional<std::uint64_t> probe(const std::string& name)
    {
        std::uint64_t session = 0;
        {
            const std::lock_guard lock(mutex);
            const auto it = sessions.find(name);
            if (it == sessions.end())
            {
                return std::nullopt;
            }
            session = it->second;
            if (ready.count(session) != 0)
            {
                return session;
            }
        }
        send(static_cast<std::uint8_t>(wire::Kind::Probe), session);
        return std::nullopt;
    }
    [[nodiscard]] bool heard_last_word_of(std::uint64_t session)
    {
        const std::lock_guard lock(mutex);
        return last_words.count(session) != 0;
    }
    /// A message of `kind` (also one that no version knows) that is `session` and nothing else.
    void send(std::uint8_t kind, std::uint64_t session)
    {
        wire::Bytes bytes;
        wire::encode_session(wire::Kind::Probe, session, bytes);
        bytes[6] = kind;
        command_out->Publish(topics::span_of(bytes));
    }
};

/// A sample of `variables` values of the types RawAgent::hello() announces.
Snapshot sample_of(std::size_t variables, std::uint64_t sequence)
{
    Snapshot s{at(1ms * static_cast<int>(sequence)), sequence, {}};
    for (std::size_t i = 0; i < variables; ++i)
    {
        if (i % 3 == 0)
        {
            s.values.emplace_back(i % 2 == 0);
        }
        else if (i % 3 == 1)
        {
            s.values.emplace_back(static_cast<std::int64_t>(i * sequence));
        }
        else
        {
            s.values.emplace_back(static_cast<double>(i) + 0.5);
        }
    }
    return s;
}

/// The step messages of `sample` for `session`, of at most 2048 bytes each.
std::vector<wire::Bytes> parts_of(std::uint64_t session, const Snapshot& sample)
{
    std::vector<wire::Bytes> out;
    wire::Bytes scratch;
    wire::encode_step(session, sample.time, false, {}, {}, &sample, wire::kMinMessageBytes, scratch,
                      [&](const wire::Bytes& m) { out.push_back(m); });
    return out;
}

} // namespace

TEST_CASE(samples_that_do_not_fit_are_dropped_and_counted)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"Unused"});
    static constexpr std::size_t kVariables = 600;
    RawAgent fake("Fake", registry.uri, 0x1111);
    fake.hello(kVariables);
    CHECK(eventually([&] { return side.hub.peers().size() == 1; }, 30s));
    auto& hub = side.hub;
    const auto unknown = [&] { return side.transport->stats().unknown; };
    const auto latest_sequence = [&] {
        const auto latest = hub.latest("Fake");
        return latest ? latest->sequence : std::uint64_t{0};
    };
    // Every sample of the test has the next sequence number (the hub takes only newer ones).
    std::uint64_t sequence = 0;
    const auto next_parts = [&](std::size_t variables) { return parts_of(fake.session, sample_of(variables, ++sequence)); };
    // Sends the messages and a good sample behind them, and waits for that one: what was sent
    // before it has been handled then, and none of it became the hub's sample.
    const auto refused = [&](const std::vector<wire::Bytes>& messages) {
        const std::uint64_t before = unknown();
        const std::uint64_t had = latest_sequence();
        for (const auto& m : messages)
        {
            fake.step(m);
        }
        const std::uint64_t bad = sequence;
        const Snapshot good = sample_of(kVariables, ++sequence);
        for (const auto& m : parts_of(fake.session, good))
        {
            fake.step(m);
        }
        // Never the bad one on the way (the latest goes from what it was straight to the good one).
        bool saw_bad = false;
        const bool arrived = eventually(
            [&] {
                const std::uint64_t now = latest_sequence();
                saw_bad = saw_bad || (now != had && now != good.sequence);
                return now == good.sequence;
            },
            30s);
        static_cast<void>(bad);
        return arrived && !saw_bad && hub.latest("Fake")->values == good.values && unknown() > before;
    };

    // A good sample in parts arrives whole.
    const Snapshot good = sample_of(kVariables, ++sequence);
    const auto good_parts = parts_of(fake.session, good);
    CHECK_EQ(good_parts.size(), std::size_t{3});
    for (const auto& m : good_parts)
    {
        fake.step(m);
    }
    CHECK(eventually([&] { return latest_sequence() == good.sequence; }, 30s));
    CHECK(hub.latest("Fake")->values == good.values);
    CHECK_EQ(unknown(), std::uint64_t{0});

    // Parts out of order.
    auto parts = next_parts(kVariables);
    CHECK(refused(std::vector<wire::Bytes>{parts[1], parts[0], parts[2]}));
    // A part missing in the middle.
    parts = next_parts(kVariables);
    CHECK(refused(std::vector<wire::Bytes>{parts[0], parts[2]}));
    // A part twice.
    parts = next_parts(kVariables);
    CHECK(refused(std::vector<wire::Bytes>{parts[0], parts[1], parts[1], parts[2]}));
    // Parts of two samples mixed.
    parts = next_parts(kVariables);
    const auto other = next_parts(kVariables);
    CHECK(refused(std::vector<wire::Bytes>{parts[0], other[1], parts[2]}));
    // Another number of values than the hello announced.
    CHECK(refused(next_parts(kVariables - 1)));
    CHECK(refused(next_parts(kVariables + 1)));
    // A value of another type than its variable has: a bool where a real goes (first part), a
    // real where an integer goes (last part).
    Snapshot wrong = sample_of(kVariables, ++sequence);
    wrong.values[2] = Value{true};
    CHECK(refused(parts_of(fake.session, wrong)));
    wrong = sample_of(kVariables, ++sequence);
    wrong.values[kVariables - 2] = Value{1.5};
    CHECK(refused(parts_of(fake.session, wrong)));
    // A message of a session nobody announced.
    CHECK(refused(std::vector<wire::Bytes>{parts_of(0x9999, sample_of(kVariables, ++sequence))[1]}));
    // ... also between the parts of a good sample, which it does not disturb.
    parts = next_parts(kVariables);
    const std::uint64_t undisturbed = sequence;
    const auto stranger = parts_of(0x9999, sample_of(kVariables, sequence));
    const std::uint64_t before = unknown();
    fake.step(parts[0]);
    fake.step(stranger[1]);
    fake.step(parts[1]);
    fake.step(parts[2]);
    CHECK(eventually([&] { return latest_sequence() == undisturbed; }, 30s));
    CHECK_EQ(unknown(), before + 1);
    CHECK_EQ(side.transport->stats().undecodable, std::uint64_t{0});
}

TEST_CASE(hellos_beyond_the_bounds_are_refused_and_counted)
{
    const InProcessRegistry registry;
    vehicle_sil::control_silkit::HubTransportOptions options;
    options.max_links = 4;
    HubSide side(registry.uri, {"Unused"}, 10ms, true, options);
    Hub other;
    CHECK(throws<std::invalid_argument>([&] {
        HubTransport t(other, side.controller->runtime(), vehicle_sil::control_silkit::HubTransportOptions{0});
    }));

    RawAgent flood("Flood", registry.uri, 1);
    // One participant after the other says hello under a new name and session. The first one
    // alone, until the hub has it: SIL Kit carries only what is published after the hub's
    // subscriber has found this publisher (and, with the history of 1, the last message before
    // that), and nothing waits for that when the publisher is made. Published at once, all but
    // the last of the ten could be lost, and then fewer than six are refused.
    flood.hello(3, "Name1", 101);
    CHECK(eventually([&] { return side.transport->stats().hellos == 1; }, 30s));
    for (std::uint64_t i = 2; i <= 10; ++i)
    {
        flood.hello(3, "Name" + std::to_string(i), 100 + i);
    }
    CHECK(eventually([&] { return side.transport->stats().refused_hellos == 6; }, 30s));
    CHECK_EQ(side.hub.peers().size(), std::size_t{4});
    CHECK_EQ(side.transport->links().size(), std::size_t{4});
    // Hellos of another version under more names: no more is remembered either.
    wire::Hello newer;
    newer.participant = "Newer";
    newer.session = 500;
    wire::Bytes bytes;
    std::string why;
    CHECK(wire::encode_hello(newer, bytes, why));
    bytes[4] = static_cast<std::uint8_t>(wire::kVersion + 1);
    flood.hello_out->Publish(topics::span_of(bytes));
    CHECK(eventually([&] { return side.transport->stats().refused_hellos == 7; }, 30s));
    CHECK_EQ(side.transport->links().size(), std::size_t{4});
    // A known name may come again with a new session (the participant was started anew).
    flood.hello(3, "Name2", 600);
    CHECK(eventually([&] { return side.transport->stats().hellos == 5; }, 30s));
    CHECK_EQ(side.hub.peers().size(), std::size_t{4});
    // The session number of a connected participant under another (known) name: not taken.
    flood.hello(3, "Name3", 600);
    CHECK(eventually([&] { return side.transport->stats().refused_hellos == 8; }, 30s));

    // The largest hello there is, announcing more variables than a hello may have, again and
    // again: each is refused when it is decoded, and nothing is kept.
    wire::Bytes huge = bytes;
    huge[4] = static_cast<std::uint8_t>(wire::kVersion);
    huge.resize(8 + 4 + 5 + 8 + 1 + 8 + 8 + 4);
    for (const std::uint8_t b : {0x40, 0x0D, 0x03, 0x00}) // 200000 variables
    {
        huge.push_back(b);
    }
    huge.resize(wire::kMaxHelloBytes, 0);
    const auto undecodable = side.transport->stats().undecodable;
    for (int i = 0; i < 20; ++i)
    {
        flood.hello_out->Publish(topics::span_of(huge));
    }
    CHECK(eventually([&] { return side.transport->stats().undecodable == undecodable + 20; }, 60s));
    CHECK_EQ(side.hub.peers().size(), std::size_t{4});
    for (const auto& peer : side.hub.peers())
    {
        CHECK_EQ(peer.schema.size(), std::size_t{3});
    }
}

TEST_CASE(a_participant_started_anew_after_it_failed_is_connected_again)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"Unused"});
    auto& hub = side.hub;
    const auto connected = [&] {
        const auto peers = hub.peers();
        return peers.size() == 1 && peers[0].connected;
    };
    for (int round = 0; round < 5; ++round)
    {
        // A process with a running lifecycle and an agent ...
        // From the second round on the name was in use a moment ago (RawAgent waits for it).
        auto first = std::make_unique<RawAgent>("Crash", registry.uri, 1000 + 2 * static_cast<std::uint64_t>(round));
        auto* lifecycle = first->participant->CreateLifecycleService({orch::OperationMode::Autonomous});
        first->hello(3);
        auto done = lifecycle->StartLifecycle();
        CHECK(eventually(connected, 30s));
        CHECK(eventually([&] { return is_running(side.controller->runtime().peer_status("Crash")); }, 30s));
        // ... disappears without a word: its connection ends (the hub saw the Error) ...
        first.reset();
        CHECK(eventually([&] { return !connected(); }, 30s));
        CHECK(side.controller->runtime().peer_status("Crash").state == orch::ParticipantState::Error);
        // ... and is started anew under its name. Its hello comes before any lifecycle state of
        // the new process: what the hub knows of "Crash" then is still the old Error.
        const auto anew = std::make_unique<RawAgent>("Crash", registry.uri, 1001 + 2 * static_cast<std::uint64_t>(round));
        RawAgent& second = *anew;
        second.answers.store(true);
        second.hello(3);
        CHECK(eventually(connected, 30s));
        std::this_thread::sleep_for(50ms);
        CHECK(connected()); // and stays
        auto* again = second.participant->CreateLifecycleService({orch::OperationMode::Autonomous});
        auto done_again = again->StartLifecycle();
        CHECK(eventually([&] { return is_running(side.controller->runtime().peer_status("Crash")); }, 30s));
        CHECK(connected());
        // A write reaches it once it is seen running and has answered the probe.
        CHECK(eventually(
            [&] {
                const auto links = side.transport->links();
                return links.size() == 1 && links[0].ready;
            },
            30s));
        const std::uint64_t seq = hub.write(OperationRequest{{{"Crash", "v1", Value{std::int64_t{1}}}}});
        CHECK(hub.wait_queued(seq, 200ms) == std::nullopt); // sent: the hand-made agent does not answer commands
        check_status(*hub.operation(seq), "Crash", TargetStatus::Sent);
        again->Stop("round over");
        CHECK(done_again.wait_for(scaled_timeout(30s)) == std::future_status::ready);
        // Its lifecycle has ended in order and it says no last word: writes are refused once the
        // controller has seen the end (the hub decides by what the controller saw, which comes a
        // moment after the participant's own end).
        CHECK(eventually([&] { return has_finished(side.controller->runtime().peer_status("Crash")); }, 30s));
        const std::uint64_t late = hub.write(OperationRequest{{{"Crash", "v1", Value{std::int64_t{2}}}}});
        check_status(settled(hub.wait_done(late, scaled_timeout(30s)), late, "a write after Crash ended"), "Crash", TargetStatus::Invalid);
    }
}

// What bounds the end of an ECU: no hub (no wait), a hub that does not answer (the whole wait,
// and no longer), and being told not to wait.
TEST_CASE(an_agent_waits_for_the_answer_to_its_last_word_no_longer_than_it_is_told)
{
    const InProcessRegistry registry;
    const auto real_time = [&](const std::string& name, std::chrono::milliseconds wait) {
        auto ecu = std::make_unique<TestEcu>(name, registry.uri, EcuOptions{.virtual_time = false, .last_word_wait = wait});
        ecu->peer->start(30s);
        return ecu;
    };
    /// How long the transport's destructor takes, after the ECU was stopped.
    const auto end_of = [](TestEcu& ecu) {
        ecu.peer->stop("the test ECU ends");
        const auto began = Wall::now();
        ecu.transport.reset();
        return Wall::now() - began;
    };
    CHECK(throws_with<std::invalid_argument>([&] { TestEcu bad("Bad", registry.uri, {.last_word_wait = -1ms}); },
                                             "last_word_wait must not be negative"));

    // No hub has ever spoken to it: nobody would answer, and nothing waits for it.
    {
        const auto alone = real_time("Alone", 60s);
        CHECK(end_of(*alone) < 30s);
    }

    // A hub that asks it whether it receives commands, as every hub does, and never answers a
    // last word, as a hub of an earlier version: the whole wait, then the end as before.
    RawHub old(registry.uri);
    {
        const auto a = real_time("A", 400ms);
        std::optional<std::uint64_t> session;
        CHECK(eventually([&] { return (session = old.probe("A")).has_value(); }, 30s));
        CHECK_EQ(*session, a->transport->session());
        // Before its last word, an answer to it means nothing (and does not end the wait later),
        // nor does a kind that this version does not know: counted, nothing else.
        const auto undecodable = a->transport->stats().undecodable;
        old.send(static_cast<std::uint8_t>(wire::Kind::LastWordReceived), *session);
        old.send(static_cast<std::uint8_t>(wire::Kind::LastWordReceived) + 1, *session);
        CHECK(eventually([&] { return a->transport->stats().undecodable == undecodable + 2; }, 30s));
        // Nor does the answer to another agent's last word, which every agent receives.
        std::thread other([&] {
            static_cast<void>(eventually([&] { return old.heard_last_word_of(*session); }, 30s));
            old.send(static_cast<std::uint8_t>(wire::Kind::LastWordReceived), *session + 1);
        });
        const auto took = end_of(*a);
        other.join();
        CHECK(took >= 400ms);
        CHECK(took < 30s);
        // The participant is still there: its last word arrives.
        CHECK(eventually([&] { return old.heard_last_word_of(*session); }, 30s));
    }
    // Told not to wait.
    {
        const auto b = real_time("B", 0ms);
        CHECK(eventually([&] { return old.probe("B").has_value(); }, 30s));
        CHECK(end_of(*b) < 30s);
    }
    // The answer ends the wait.
    {
        const auto c = real_time("C", 120s);
        std::optional<std::uint64_t> session;
        CHECK(eventually([&] { return (session = old.probe("C")).has_value(); }, 30s));
        std::thread answers([&] {
            // Once its last word is there (not before: an earlier answer is not one).
            static_cast<void>(eventually([&] { return old.heard_last_word_of(*session); }, 60s));
            old.send(static_cast<std::uint8_t>(wire::Kind::LastWordReceived), *session);
        });
        const auto took = end_of(*c);
        answers.join();
        CHECK(took < 100s);
    }
}

// The hub's side of the answer, with an agent made by hand that takes no notice of it (as an
// agent of an earlier version): the connection ends as it always did.
TEST_CASE(a_last_word_is_answered_also_for_a_session_that_has_ended_or_was_never_connected)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"Unused"});
    RawAgent agent("Old", registry.uri, 55);
    agent.answers.store(true);
    // What the hub's side shows at the moment the answer arrives (in the agent's receive handler:
    // an AgentTransport's destructor returns from there on): the connection has ended by then.
    std::atomic<int> ended_when_answered{0};
    std::atomic<int> connected_when_answered{0};
    agent.on_last_word_answer = [&] {
        ended_when_answered += side.transport->links().at(0).state == LinkState::Ended ? 1 : 0;
        connected_when_answered += side.hub.peers().at(0).connected ? 1 : 0;
    };
    agent.hello(3);
    side.wait_ready({"Old"});
    const auto last_word = [](std::uint64_t session) {
        wire::Bytes scratch;
        wire::Bytes out;
        wire::encode_step(session, at(0ms), true, {}, {}, nullptr, wire::kMaxMessageBytes, scratch,
                          [&](const wire::Bytes& m) { out = m; });
        return out;
    };
    agent.step(last_word(55));
    CHECK(eventually([&] { return agent.last_word_answers.load() == 1; }, 30s));
    // Answered after the connection was ended, not before.
    CHECK_EQ(ended_when_answered.load(), 1);
    CHECK_EQ(connected_when_answered.load(), 0);
    CHECK(side.transport->links().at(0).state == LinkState::Ended);
    CHECK(!side.hub.peers().at(0).connected);
    CHECK_EQ(side.transport->stats().last_words, std::uint64_t{1});
    // Said again, for the session that has ended: answered again.
    agent.step(last_word(55));
    CHECK(eventually([&] { return agent.last_word_answers.load() == 2; }, 30s));
    CHECK_EQ(side.transport->stats().unknown, std::uint64_t{0});
    // Of a session that never said hello: answered too (and counted as a message nobody knows).
    agent.step(last_word(56));
    CHECK(eventually([&] { return side.transport->stats().last_words == 3; }, 30s));
    CHECK_EQ(side.transport->stats().unknown, std::uint64_t{1});
    CHECK_EQ(agent.last_word_answers.load(), 2);
}

TEST_CASE(a_write_to_an_agent_that_has_not_said_it_receives_commands_is_refused_at_once)
{
    const InProcessRegistry registry;
    HubSide side(registry.uri, {"Unused"});
    auto& hub = side.hub;
    RawAgent quiet("Quiet", registry.uri, 77);
    quiet.hello(3);
    CHECK(eventually([&] { return hub.peers().size() == 1; }, 30s));
    // It is asked whether it receives commands (with its hello, and with what it sends).
    CHECK(eventually([&] { return quiet.probes.load() >= 1; }, 30s));
    for (std::uint64_t i = 1; i <= 20; ++i)
    {
        for (const auto& m : parts_of(quiet.session, sample_of(3, i)))
        {
            quiet.step(m);
        }
    }
    CHECK(eventually([&] { return quiet.probes.load() >= 3; }, 30s));
    CHECK(!side.transport->links().at(0).ready);
    // No answer yet: the write is not sent into the void, and nothing stays open.
    const std::uint64_t early = hub.write(OperationRequest{{{"Quiet", "v1", Value{std::int64_t{1}}}}});
    const auto refused = hub.wait_done(early, scaled_timeout(30s));
    CHECK(refused.has_value());
    check_status(*refused, "Quiet", TargetStatus::Invalid);
    // wait_for_hellos() does not count it as connected either.
    const HelloReport waiting = side.transport->wait_for_hellos({"Quiet"}, Wall::now() + 200ms);
    CHECK(waiting.missing == std::vector<std::string>{"Quiet"});
    CHECK(waiting.connected.empty());

    quiet.answers.store(true);
    const HelloReport there = side.transport->wait_for_hellos({"Quiet"}, Wall::now() + 30s);
    CHECK(there.connected == std::vector<std::string>{"Quiet"});
    CHECK(side.transport->links().at(0).ready);
    const std::uint64_t sent = hub.write(OperationRequest{{{"Quiet", "v1", Value{std::int64_t{1}}}}});
    CHECK(hub.wait_queued(sent, 200ms) == std::nullopt);
    check_status(*hub.operation(sent), "Quiet", TargetStatus::Sent);
    // A ready message of a session nobody announced is counted, not taken.
    const auto unknown = side.transport->stats().unknown;
    wire::Bytes bytes;
    wire::encode_session(wire::Kind::Ready, 4242, bytes);
    quiet.submitted_out->Publish(topics::span_of(bytes));
    CHECK(eventually([&] { return side.transport->stats().unknown == unknown + 1; }, 30s));
}
