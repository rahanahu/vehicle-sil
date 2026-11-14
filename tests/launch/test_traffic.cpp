// SPDX-License-Identifier: MIT
//
// The traffic of a run's result without a process: the count (run_traffic.hpp) fed with samples
// directly, what it reads of a control agent's hello, who is on which network, and the run's
// samples (run_samples.hpp) between a hub, the scenario's sink and the count. The same with real
// ECUs: launch_run.traffic (launch_run_launcher.cpp).

#include "launch_test_util.hpp"

#include "check_format.hpp"
#include "run_samples.hpp"
#include "run_traffic.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using namespace std::chrono_literals;
using vehicle_sil::launch::test::check_text;
using launch::detail::RunSamples;
using launch::detail::TrafficColumns;
using launch::detail::TrafficCount;
using launch::detail::TrafficPlan;
using launch::detail::TrafficSchema;

namespace {

SimTime at(std::chrono::milliseconds ms)
{
    return SimTime{} + ms;
}

/// A sample of an agent whose variables are [frames_received, frames_sent] of one network.
std::shared_ptr<const control::Snapshot> sample(std::chrono::milliseconds time, std::int64_t received, std::int64_t sent)
{
    control::Snapshot s;
    s.time = at(time);
    s.sequence = static_cast<std::uint64_t>(time.count()) + 1;
    s.values = {control::Value{received}, control::Value{sent}};
    return std::make_shared<const control::Snapshot>(std::move(s));
}

TrafficSchema counters(const std::string& network, bool virtual_time = true, bool mapped = false)
{
    return TrafficSchema{virtual_time, {TrafficColumns{network, 1, 0, mapped}}};
}

/// Two described ECUs on the CAN network "BUS".
TrafficPlan pair_plan()
{
    TrafficPlan plan;
    plan.buses = {{"BUS", "can"}};
    plan.participants = {{"A", true, true, {"BUS"}}, {"B", true, true, {"BUS"}}};
    return plan;
}

/// What a participant of a result says, as one text.
std::string said(const TrafficParticipant& p)
{
    return p.name + " " + (p.counted() ? "counted" : traffic_not_counted_name(p.not_counted)) + " at " +
           (p.at ? std::to_string((*p.at - SimTime{}) / 1ms) : "-") + " sent " +
           (p.frames_sent ? std::to_string(*p.frames_sent) : "-") + " received " +
           (p.frames_received ? std::to_string(*p.frames_received) : "-");
}

std::string said(const RunTraffic& traffic)
{
    std::string out = "before " + std::to_string(traffic.before_ms) + "\n";
    for (const TrafficNetwork& network : traffic.per_network)
    {
        out += network.name + " (" + network.kind + ") " + std::to_string(network.received()) + " of " +
               std::to_string(network.counted()) + "\n";
        for (const TrafficParticipant& p : network.participants)
        {
            out += "  " + said(p) + "\n";
        }
    }
    return out;
}

/// A runs every `step` from 0 and has sent one frame per step before; B has received them. The
/// samples up to `last` are given, and the count is settled after every `takes` samples of A with
/// the time the controller (a step of 10 ms) would have by then.
RunTraffic run_of(std::chrono::milliseconds step, std::chrono::milliseconds last, std::int64_t end_ms, int takes)
{
    TrafficCount count(pair_plan());
    count.hello(0, counters("BUS"));
    count.hello(1, counters("BUS"));
    int given = 0;
    for (std::chrono::milliseconds t = 0ms; t <= last; t += step)
    {
        const std::int64_t frames = t / step;
        count.sample(0, 1, sample(t, 0, frames));
        count.sample(1, 1, sample(t, frames, 0));
        if (++given % takes == 0)
        {
            // The controller is in the step of 10 ms that the ECU's step falls in, and not beyond the end.
            count.settle(at(std::min(t / 10ms * 10ms, std::chrono::milliseconds(end_ms))));
        }
    }
    return count.finish(end_ms);
}

} // namespace

TEST_CASE(the_numbers_are_those_of_the_last_step_before_the_end_whatever_came_after_it)
{
    // The run ends at 1000 ms. Of the steps from 1000 ms on, none, one or several samples came
    // before the run was over: the numbers are those of the step at 990 ms each time.
    const std::string expected = "before 1000\nBUS (can) 1 of 2\n  A counted at 990 sent 99 received 0\n"
                                 "  B counted at 990 sent 0 received 99\n";
    for (const std::chrono::milliseconds last : {990ms, 1000ms, 1010ms, 1050ms})
    {
        check_text(said(run_of(10ms, last, 1000, 1)), expected);
    }
}

TEST_CASE(where_the_takes_fall_does_not_change_the_numbers)
{
    const std::string every = said(run_of(10ms, 1030ms, 1000, 1));
    for (const int takes : {2, 3, 7, 50, 1000})
    {
        check_text(said(run_of(10ms, 1030ms, 1000, takes)), every);
    }
    CHECK(every.find("A counted at 990 sent 99") != std::string::npos);
}

TEST_CASE(an_ecu_with_finer_or_coarser_steps_than_the_controller_is_counted_by_its_own_steps)
{
    // 5 ms under a controller of 10 ms: its step at 995 ms began before the end, the one at 1000 ms did not.
    for (const std::chrono::milliseconds last : {995ms, 1000ms, 1005ms})
    {
        CHECK(said(run_of(5ms, last, 1000, 1)).find("A counted at 995 sent 199 received 0") != std::string::npos);
    }
    // 100 ms: the step at 900 ms is the last before the end, also when the one at 1000 ms came.
    for (const std::chrono::milliseconds last : {900ms, 1000ms})
    {
        CHECK(said(run_of(100ms, last, 1000, 1)).find("A counted at 900 sent 9 received 0") != std::string::npos);
    }
}

TEST_CASE(a_run_that_ends_while_it_stands_still_counts_the_steps_before_where_it_stood)
{
    // Paused at 500 ms: the sample of the step at 500 ms may have come; the controller's time
    // stays 500 ms however often the samples are taken, and the run ends there.
    for (const bool came : {false, true})
    {
        TrafficCount count(pair_plan());
        count.hello(0, counters("BUS"));
        for (std::chrono::milliseconds t = 0ms; t < 500ms; t += 10ms)
        {
            count.sample(0, 1, sample(t, 2, t / 10ms));
            count.settle(at(t));
        }
        if (came)
        {
            count.sample(0, 1, sample(500ms, 3, 50));
        }
        count.settle(at(500ms));
        count.settle(at(500ms));
        const RunTraffic traffic = count.finish(500);
        check_text(said(traffic.per_network.at(0).participants.at(0)), std::string("A counted at 490 sent 49 received 2"));
    }
    // It went on instead: the step at 500 ms is settled once the controller is beyond it.
    TrafficCount count(pair_plan());
    count.hello(0, counters("BUS"));
    count.sample(0, 1, sample(490ms, 2, 49));
    count.sample(0, 1, sample(500ms, 3, 50));
    count.settle(at(500ms));
    count.settle(at(510ms));
    check_text(said(count.finish(510).per_network.at(0).participants.at(0)), std::string("A counted at 500 sent 50 received 3"));
}

TEST_CASE(a_participant_that_is_not_counted_says_why)
{
    TrafficPlan plan;
    plan.buses = {{"BUS", "can"}};
    for (const char* name : {"Silent", "Restarted", "Later", "RealTime", "Raw", "Mapped", "Dropped", "Flooding", "Early", "Ok"})
    {
        plan.participants.push_back({name, true, true, {"BUS"}});
    }
    TrafficCount count(plan);
    // Silent: no hello.
    // Restarted: two processes, seen by their samples.
    count.hello(1, counters("BUS"));
    count.sample(1, 1, sample(10ms, 1, 1));
    count.sample(1, 2, sample(20ms, 0, 0));
    // Later: the hub had the hello of a later process when its samples were taken.
    count.restarted(2);
    count.hello(2, counters("BUS"));
    count.sample(2, 1, sample(10ms, 1, 1));
    // RealTime.
    count.hello(3, counters("BUS", false));
    count.sample(3, 1, sample(10ms, 1, 1));
    // Raw: an agent, and no counters of this network.
    count.hello(4, counters("OTHER"));
    count.sample(4, 1, sample(10ms, 1, 1));
    // Mapped.
    count.hello(5, counters("BUS", true, true));
    count.sample(5, 1, sample(10ms, 1, 1));
    // Dropped: the queue was full.
    count.hello(6, counters("BUS"));
    count.sample(6, 1, sample(10ms, 1, 1));
    count.dropped(6);
    // Flooding: more samples pending than the count keeps.
    count.hello(7, counters("BUS"));
    for (std::size_t i = 0; i <= TrafficCount::kMaxPending; ++i)
    {
        count.sample(7, 1, sample(1000ms + std::chrono::milliseconds(i), 1, 1));
    }
    count.settle(at(50ms));
    // Early: a hello, and only a sample of a step at the end.
    count.hello(8, counters("BUS"));
    count.sample(8, 1, sample(100ms, 1, 1));
    // Ok.
    count.hello(9, counters("BUS"));
    count.sample(9, 1, sample(90ms, 4, 5));
    const RunTraffic traffic = count.finish(100);
    check_text(said(traffic),
             std::string("before 100\nBUS (can) 1 of 1\n"
                         "  Silent no-control-link at - sent - received -\n"
                         "  Restarted restarted at - sent - received -\n"
                         "  Later restarted at - sent - received -\n"
                         "  RealTime real-time at - sent - received -\n"
                         "  Raw no-counters at - sent - received -\n"
                         "  Mapped mapped at - sent - received -\n"
                         "  Dropped queue-full at - sent - received -\n"
                         "  Flooding queue-full at - sent - received -\n"
                         "  Early no-sample at - sent - received -\n"
                         "  Ok counted at 90 sent 5 received 4\n"));
    const auto& rows = traffic.per_network.at(0).participants;
    CHECK(!rows.at(0).virtual_time.has_value());
    CHECK(rows.at(3).virtual_time == std::optional<bool>(false));
    CHECK(rows.at(9).virtual_time == std::optional<bool>(true));
    // A sample without the two integers (not this agent's schema) is no sample.
    TrafficCount odd(pair_plan());
    odd.hello(0, counters("BUS"));
    control::Snapshot shorter;
    shorter.time = at(10ms);
    shorter.values = {control::Value{true}};
    odd.sample(0, 1, std::make_shared<const control::Snapshot>(shorter));
    check_text(said(odd.finish(100).per_network.at(0).participants.at(0)), std::string("A no-sample at - sent - received -"));
}

TEST_CASE(received_counts_the_counted_participants_that_received_a_frame)
{
    TrafficCount count(pair_plan());
    count.hello(0, counters("BUS"));
    count.hello(1, counters("BUS"));
    count.sample(0, 1, sample(10ms, 0, 7));
    count.sample(1, 1, sample(10ms, 1, 0));
    const RunTraffic traffic = count.finish(20);
    CHECK_EQ(traffic.per_network.at(0).counted(), std::size_t{2});
    CHECK_EQ(traffic.per_network.at(0).received(), std::size_t{1});
}

TEST_CASE(a_participant_is_on_the_networks_describe_says_or_those_its_agent_counts)
{
    TrafficPlan plan;
    plan.buses = {{"FIRST", "can"}, {"UNUSED", "lin"}, {"SECOND", "can"}};
    plan.participants = {{"Described", true, true, {"SECOND", "FIRST", "NOT_IN_THE_FILE"}},
                         {"Tool", true, false, {}},
                         {"Disabled", false, true, {"FIRST"}},
                         {"Quiet", true, false, {}}};
    TrafficCount count(plan);
    count.hello(0, TrafficSchema{true, {TrafficColumns{"FIRST", 1, 0, false}}});
    count.sample(0, 1, sample(0ms, 1, 2));
    // Without --describe: where its agent has counters (also of a network that is not in the file).
    count.hello(1, TrafficSchema{true, {TrafficColumns{"SECOND", 1, 0, false}, TrafficColumns{"ELSEWHERE", 1, 0, false}}});
    count.sample(1, 1, sample(0ms, 3, 4));
    check_text(said(count.finish(10)),
             std::string("before 10\nFIRST (can) 1 of 1\n  Described counted at 0 sent 2 received 1\n"
                         "SECOND (can) 1 of 1\n  Described no-counters at - sent - received -\n"
                         "  Tool counted at 0 sent 4 received 3\n"));
    // Nobody is known to be on a network: no network at all.
    TrafficCount none(TrafficPlan{{{"BUS", "can"}}, {{"Tool", true, false, {}}}});
    CHECK(none.finish(10).per_network.empty());
}

TEST_CASE(the_counters_of_a_hello_are_found_by_the_names_the_agent_announced)
{
    const auto variable = [](std::string name, control::ValueType type = control::ValueType::Integer) {
        control::VariableInfo v;
        v.name = std::move(name);
        v.type = type;
        v.causality = control::Causality::Output;
        return v;
    };
    control::PeerInfo peer;
    peer.participant = "Engine";
    peer.virtual_time = false;
    peer.schema = {variable("engine.speed", control::ValueType::Real), variable("j1939.BUS.frames_received"),
                   variable("j1939.BUS.frames_sent"),                  variable("j1939.A.B.frames_sent"),
                   variable("j1939.A.B.frames_received"),              variable("j1939.BUS.send_failures"),
                   variable("j1939..frames_sent"),                     variable("j1939.REAL.frames_sent", control::ValueType::Real),
                   variable("j1939.HALF.frames_sent")};
    TrafficSchema schema = launch::detail::traffic_schema(peer);
    CHECK(!schema.virtual_time);
    CHECK_EQ(schema.counters.size(), std::size_t{3});
    CHECK(schema.counters.at(0).network == "BUS" && schema.counters.at(0).received == std::optional<std::size_t>(1) &&
          schema.counters.at(0).sent == std::optional<std::size_t>(2) && !schema.counters.at(0).mapped &&
          schema.counters.at(0).complete());
    // A name of a network may have dots.
    CHECK(schema.counters.at(1).network == "A.B" && schema.counters.at(1).sent == std::optional<std::size_t>(3) &&
          schema.counters.at(1).received == std::optional<std::size_t>(4));
    CHECK(schema.counters.at(2).network == "HALF" && !schema.counters.at(2).complete());

    // With a variable map: the clients see other names; the agent's are beside them, and a
    // counter that a mapping applies to is marked.
    peer.schema.at(1).name = "received_here";
    peer.schema.at(0).name = "j1939.FAKE.frames_sent"; // a name a mapping made up: not a counter
    peer.schema.at(0).type = control::ValueType::Integer;
    peer.sources.assign(peer.schema.size(), std::string{});
    peer.sources.at(0) = "engine.speed";
    peer.sources.at(1) = "j1939.BUS.frames_received";
    schema = launch::detail::traffic_schema(peer);
    CHECK_EQ(schema.counters.size(), std::size_t{3});
    CHECK(schema.counters.at(0).network == "BUS" && schema.counters.at(0).mapped && schema.counters.at(0).complete());
    CHECK(!schema.counters.at(1).mapped);
}

TEST_CASE(a_described_participants_networks_are_under_the_simulations_names)
{
    SimulationConfig config;
    config.networks = {NetworkConfig{"VehicleCAN", NetworkKind::Can, 3, true}, NetworkConfig{"Broken", NetworkKind::Can, 4, false},
                       NetworkConfig{"BodyLIN", NetworkKind::Lin, 5, true}};
    ParticipantConfig engine;
    engine.name = "Engine";
    engine.network_names = {NetworkName{"VehicleCAN", "J1939_CAN", 8}};
    ParticipantConfig tool;
    tool.name = "Tool";
    tool.describe = false;
    ParticipantConfig off;
    off.name = "Off";
    off.enabled = false;
    config.participants = {engine, tool, off};
    CheckReport check;
    check.participants.resize(3);
    ReportedEcu ecu;
    ecu.description.networks = {NetworkDescription{"J1939_CAN"}, NetworkDescription{"BodyLIN", NetworkUse::LinMaster}};
    check.participants[0].ecu = ecu;
    check.participants[2].ecu = ecu;
    const TrafficPlan plan = launch::detail::traffic_plan(config, check);
    CHECK_EQ(plan.buses.size(), std::size_t{2});
    CHECK(plan.buses.at(0).name == "VehicleCAN" && plan.buses.at(0).kind == "can" && plan.buses.at(1).name == "BodyLIN" &&
          plan.buses.at(1).kind == "lin");
    CHECK_EQ(plan.participants.size(), std::size_t{3});
    CHECK(plan.participants.at(0).name == "Engine" && plan.participants.at(0).in_run && plan.participants.at(0).described &&
          plan.participants.at(0).on == (std::vector<std::string>{"VehicleCAN", "BodyLIN"}));
    CHECK(plan.participants.at(1).in_run && !plan.participants.at(1).described && plan.participants.at(1).on.empty());
    CHECK(!plan.participants.at(2).in_run);
}

TEST_CASE(the_traffic_for_people_is_a_line_and_a_table)
{
    RunTraffic traffic;
    traffic.before_ms = 1000;
    check_text(launch::detail::traffic_text(traffic), std::string("traffic: no network in use\n"));
    TrafficParticipant engine{"Engine", TrafficNotCounted::None, true, at(990ms), 1234, 1};
    TrafficParticipant dashboard{"Dashboard", TrafficNotCounted::None, true, at(995ms), 1, 0};
    traffic.per_network = {TrafficNetwork{"J1939_CAN", "can", {engine, dashboard}}};
    check_text(launch::detail::traffic_text(traffic),
             std::string("traffic: J1939_CAN: 1 of 2 participants received (steps before 1000 ms)\n\n"
                         "network    participant  sent  received\n"
                         "J1939_CAN  Engine       1234         1\n"
                         "J1939_CAN  Dashboard       1         0\n"));
    const auto not_counted = [](const char* name, TrafficNotCounted why) {
        return TrafficParticipant{name, why, why == TrafficNotCounted::NoControlLink ? std::nullopt : std::optional<bool>(true)};
    };
    traffic.per_network.push_back(TrafficNetwork{"B",
                                                 "can",
                                                 {not_counted("P1", TrafficNotCounted::NoControlLink),
                                                  not_counted("P2", TrafficNotCounted::Restarted),
                                                  not_counted("P3", TrafficNotCounted::RealTime),
                                                  not_counted("P4", TrafficNotCounted::NoCounters),
                                                  not_counted("P5", TrafficNotCounted::Mapped),
                                                  not_counted("P6", TrafficNotCounted::QueueFull),
                                                  not_counted("P7", TrafficNotCounted::NoSample)}});
    check_text(launch::detail::traffic_text(traffic),
             std::string("traffic: J1939_CAN: 1 of 2 participants received; B: 0 of 0 participants received (7 not counted) "
                         "(steps before 1000 ms)\n\n"
                         "network    participant  sent  received\n"
                         "J1939_CAN  Engine       1234         1\n"
                         "J1939_CAN  Dashboard       1         0\n"
                         "B          P1              -         -  not counted: no control link\n"
                         "B          P2              -         -  not counted: restarted\n"
                         "B          P3              -         -  not counted: real time\n"
                         "B          P4              -         -  not counted: no J1939 counters\n"
                         "B          P5              -         -  not counted: counters mapped by variables\n"
                         "B          P6              -         -  not counted: samples dropped\n"
                         "B          P7              -         -  not counted: no sample before the end\n"));
}

namespace {

/// A control agent at a hub, as the hub's transport would bring it: variables
/// [j1939.BUS.frames_received, j1939.BUS.frames_sent].
struct FakeAgent
{
    struct Link final : control::AgentLink
    {
        bool send(const control::Command&) override { return true; }
    };
    control::Hub& hub;
    std::string name;
    control::ConnectionId connection = 0;
    std::uint64_t sequence = 0;

    FakeAgent(control::Hub& h, std::string participant, const std::string& incarnation) : hub(h), name(std::move(participant))
    {
        control::PeerHello hello;
        hello.participant = name;
        hello.incarnation = incarnation;
        for (const char* field : {"j1939.BUS.frames_received", "j1939.BUS.frames_sent"})
        {
            control::VariableInfo v;
            v.name = field;
            v.type = control::ValueType::Integer;
            v.causality = control::Causality::Output;
            hello.schema.push_back(std::move(v));
        }
        connection = hub.agent_connected(std::move(hello), std::make_shared<Link>());
    }
    void step(std::chrono::milliseconds time, std::int64_t received, std::int64_t sent)
    {
        control::Snapshot s;
        s.time = at(time);
        s.sequence = ++sequence;
        s.values = {control::Value{received}, control::Value{sent}};
        hub.agent_values(connection, std::move(s));
    }
};

} // namespace

TEST_CASE(the_samples_of_a_hub_go_to_the_count_and_from_its_beginning_to_the_scenario)
{
    control::Hub hub;
    RunSamples samples({"A", "B"}, pair_plan());
    samples.tap(hub);
    FakeAgent a(hub, "A", "a1");
    FakeAgent b(hub, "B", "b1");
    FakeAgent stranger(hub, "Stranger", "s1");
    std::vector<std::string> scenario;
    const auto sink = [&scenario](std::size_t participant, const std::string& incarnation,
                                  const std::shared_ptr<const control::Snapshot>& taken) {
        scenario.push_back(std::to_string(participant) + "/" + incarnation + "@" + std::to_string((taken->time - SimTime{}) / 1ms));
    };
    // Before the scenario begins (a participant that does not wait for the start): for the count only.
    a.step(0ms, 0, 0);
    stranger.step(0ms, 9, 9);
    samples.scenario_begins(sink);
    a.step(10ms, 0, 1);
    b.step(10ms, 1, 0);
    samples.take(at(10ms));
    CHECK(scenario == (std::vector<std::string>{"0/a1@10", "1/b1@10"}));
    a.step(20ms, 0, 2);
    b.step(20ms, 2, 0);
    samples.take(at(20ms));
    a.step(30ms, 0, 3);
    samples.take(at(30ms));
    CHECK(scenario == (std::vector<std::string>{"0/a1@10", "1/b1@10", "0/a1@20", "1/b1@20", "0/a1@30"}));
    // The scenario is over: the last take is the count's alone.
    samples.scenario_ends();
    b.step(30ms, 3, 0);
    a.step(40ms, 0, 4);
    const RunTraffic traffic = samples.finish(40);
    CHECK_EQ(scenario.size(), std::size_t{5});
    check_text(said(traffic), std::string("before 40\nBUS (can) 1 of 2\n  A counted at 30 sent 3 received 0\n"
                                        "  B counted at 30 sent 0 received 3\n"));
    // The tap is gone, and a take after the end does nothing.
    a.step(50ms, 0, 5);
    samples.take(at(60ms));
    samples.untap();
}

TEST_CASE(a_participant_whose_samples_did_not_come_is_told_from_one_without_an_agent)
{
    control::Hub hub;
    RunSamples samples({"A", "B"}, pair_plan());
    samples.tap(hub);
    FakeAgent a(hub, "A", "a1");
    check_text(said(samples.finish(100)), std::string("before 100\nBUS (can) 0 of 0\n  A no-sample at - sent - received -\n"
                                                    "  B no-control-link at - sent - received -\n"));
}

TEST_CASE(a_participant_that_was_started_again_is_not_counted)
{
    // Seen by its samples.
    {
        control::Hub hub;
        RunSamples samples({"A", "B"}, pair_plan());
        samples.tap(hub);
        FakeAgent first(hub, "A", "a1");
        first.step(0ms, 0, 5);
        samples.take(at(10ms));
        FakeAgent second(hub, "A", "a2");
        second.step(20ms, 0, 1);
        CHECK(said(samples.finish(100)).find("A restarted at") != std::string::npos);
    }
    // Seen by the hub's hello only: the samples taken are the first process's, the hello is the second's.
    {
        control::Hub hub;
        RunSamples samples({"A", "B"}, pair_plan());
        samples.tap(hub);
        FakeAgent first(hub, "A", "a1");
        first.step(0ms, 0, 5);
        FakeAgent second(hub, "A", "a2");
        CHECK(said(samples.finish(100)).find("A restarted at") != std::string::npos);
    }
}

TEST_CASE(a_queue_that_nobody_takes_from_is_bounded_and_says_so)
{
    control::Hub hub;
    RunSamples samples({"A", "B"}, pair_plan());
    samples.tap(hub);
    FakeAgent a(hub, "A", "a1");
    FakeAgent b(hub, "B", "b1");
    b.step(0ms, 1, 1);
    for (std::size_t i = 0; i < RunSamples::kMaxQueued; ++i)
    {
        a.step(std::chrono::milliseconds(i), 0, static_cast<std::int64_t>(i));
    }
    const std::string full = said(samples.finish(1'000'000'000));
    CHECK(full.find("A queue-full at") != std::string::npos);
    CHECK(full.find("B counted at 0 sent 1 received 1") != std::string::npos);
}
