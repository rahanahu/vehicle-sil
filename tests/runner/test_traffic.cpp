// SPDX-License-Identifier: MIT
//
// The traffic of a run's result without a process: which sample of a participant counts
// (run_traffic.hpp, fed with samples directly), what is read of a control agent's hello, who is on
// which network, what a network says of itself, and the run's samples (run_samples.hpp) between a
// hub, the scenario and the count. The same with real ECUs: launch_run.traffic
// (launch_run_launcher.cpp). How a network's traffic is worded in the outputs is in
// tests/launch/test_traffic.cpp.

#include "traffic_support.hpp"
#include "launch_test_util.hpp"

#include "vehicle_sil/runner/detail/run_samples.hpp"
#include "vehicle_sil/runner/detail/run_traffic.hpp"

#include "vehicle_sil/control/hub.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace vehicle_sil;
using namespace vehicle_sil::runner;
using namespace vehicle_sil::config;
using namespace std::chrono_literals;
using vehicle_sil::config::test::check_text;
using vehicle_sil::runner::detail::RunSamples;
using vehicle_sil::runner::detail::TrafficColumns;
using vehicle_sil::runner::detail::TrafficLatest;
using vehicle_sil::runner::detail::TrafficSeen;
using vehicle_sil::runner::detail::TrafficPlan;
using vehicle_sil::runner::detail::TrafficSchema;
using namespace vehicle_sil::runner::test::traffic_support;

namespace {

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
    for (const vehicle_sil::runner::TrafficUnplaced& p : traffic.unplaced)
    {
        out += "unplaced " + p.name + " " + vehicle_sil::runner::traffic_unplaced_reason_name(p.reason) + "\n";
    }
    for (const TrafficNetwork& network : traffic.per_network)
    {
        const std::optional<bool> delivered = network.delivered();
        out += network.name + " (" + network.kind + ")" +
               (!network.unplaced.empty() ? " unplaced " + std::to_string(network.unplaced.size()) : std::string{}) + " counted " +
               std::to_string(network.counted()) + " senders " +
               std::to_string(network.senders()) + " receivers " + std::to_string(network.receivers()) + " delivered " +
               (!delivered ? "?" : *delivered ? "yes" : "no") + "\n";
        for (const TrafficParticipant& p : network.participants)
        {
            out += "  " + said(p) + "\n";
        }
        for (const vehicle_sil::runner::TrafficUnplaced& p : network.unplaced)
        {
            out += "  " + p.name + " unplaced " + vehicle_sil::runner::traffic_unplaced_reason_name(p.reason) + "\n";
        }
    }
    return out;
}

constexpr SimDuration kStep = 10ms; // the controller's

/// The participants of a plan as the run's samples keep them: each sample is put with the
/// controller's time when it came.
struct Fed
{
    TrafficPlan plan;
    std::vector<TrafficLatest> latest;
    std::vector<TrafficSeen> seen;
    explicit Fed(TrafficPlan p) : plan(std::move(p)), latest(plan.participants.size()), seen(plan.participants.size()) {}
    void hello(std::size_t i, TrafficSchema schema)
    {
        seen[i].hello = true;
        seen[i].schema = std::move(schema);
    }
    void put(std::size_t i, std::shared_ptr<const control::Snapshot> s, std::chrono::milliseconds now)
    {
        latest[i].put(std::move(s), at(now), kStep);
    }
    RunTraffic finish(std::int64_t end_ms)
    {
        for (std::size_t i = 0; i < latest.size(); ++i)
        {
            seen[i].off_time = latest[i].off_time();
            seen[i].last = latest[i].last_before(at(std::chrono::milliseconds(end_ms)), kStep);
        }
        return vehicle_sil::runner::detail::count_traffic(plan, seen, end_ms);
    }
};

/// A runs every `step` from 0 and has sent one frame per step before; B has received them. The
/// samples up to `last` come. `lag`: how far the controller is behind when a sample comes (0: it
/// is in the step of 10 ms the ECU's step falls in; more: the samples come early, as while the
/// controller's step is held).
RunTraffic run_of(std::chrono::milliseconds step, std::chrono::milliseconds last, std::int64_t end_ms,
                  std::chrono::milliseconds lag = 0ms)
{
    Fed fed(pair_plan());
    fed.hello(0, counters("BUS"));
    fed.hello(1, counters("BUS"));
    for (std::chrono::milliseconds t = 0ms; t <= last; t += step)
    {
        const std::int64_t frames = t / step;
        // Never beyond the end: the run's end is the controller's last step.
        const std::chrono::milliseconds now = std::min(std::max(t / 10ms * 10ms - lag, 0ms), std::chrono::milliseconds(end_ms));
        fed.put(0, sample(t, 0, frames), now);
        fed.put(1, sample(t, frames, 0), now);
    }
    return fed.finish(end_ms);
}

} // namespace

TEST_CASE(the_numbers_are_those_of_the_last_step_before_the_end_whatever_came_after_it)
{
    // The run ends at 1000 ms. Of the steps from 1000 ms on, none, one or several samples came
    // before the run was over: the numbers are those of the step at 990 ms each time.
    const std::string expected = "before 1000\nBUS (can) counted 2 senders 1 receivers 1 delivered yes\n"
                                 "  A counted at 990 sent 99 received 0\n  B counted at 990 sent 0 received 99\n";
    for (const std::chrono::milliseconds last : {990ms, 1000ms, 1010ms, 1050ms})
    {
        check_text(said(run_of(10ms, last, 1000)), expected);
    }
}

TEST_CASE(when_the_samples_come_does_not_change_the_numbers)
{
    // The same samples, come while the controller was in the same step, a step behind, far behind.
    const std::string every = said(run_of(10ms, 1030ms, 1000));
    for (const std::chrono::milliseconds lag : {10ms, 20ms, 300ms})
    {
        check_text(said(run_of(10ms, 1030ms, 1000, lag)), every);
        check_text(said(run_of(5ms, 1030ms, 1000, lag)), said(run_of(5ms, 1030ms, 1000)));
    }
    CHECK(every.find("A counted at 990 sent 99") != std::string::npos);
}

TEST_CASE(an_ecu_with_finer_or_coarser_steps_than_the_controller_is_counted_by_its_own_steps)
{
    // 5 ms under a controller of 10 ms: its step at 995 ms began before the end, the one at 1000 ms did not.
    for (const std::chrono::milliseconds last : {995ms, 1000ms, 1005ms})
    {
        CHECK(said(run_of(5ms, last, 1000)).find("A counted at 995 sent 199 received 0") != std::string::npos);
    }
    // 100 ms: the step at 900 ms is the last before the end, also when the one at 1000 ms came.
    for (const std::chrono::milliseconds last : {900ms, 1000ms})
    {
        CHECK(said(run_of(100ms, last, 1000)).find("A counted at 900 sent 9 received 0") != std::string::npos);
    }
}

TEST_CASE(a_run_that_ends_while_it_stands_still_counts_the_steps_before_where_it_stood)
{
    // Paused at 500 ms: the sample of the step at 500 ms may have come; the controller's time
    // stays 500 ms, and the run ends there.
    for (const bool came : {false, true})
    {
        Fed fed(pair_plan());
        fed.hello(0, counters("BUS"));
        for (std::chrono::milliseconds t = 0ms; t < 500ms; t += 10ms)
        {
            fed.put(0, sample(t, 2, t / 10ms), t);
        }
        if (came)
        {
            fed.put(0, sample(500ms, 3, 50), 500ms);
        }
        check_text(said(fed.finish(500).per_network.at(0).participants.at(0)), "A counted at 490 sent 49 received 2");
    }
    // It went on instead: the step at 500 ms counts once the run ends beyond it.
    Fed fed(pair_plan());
    fed.hello(0, counters("BUS"));
    fed.put(0, sample(490ms, 2, 49), 490ms);
    fed.put(0, sample(500ms, 3, 50), 500ms);
    check_text(said(fed.finish(510).per_network.at(0).participants.at(0)), "A counted at 500 sent 50 received 3");
}

TEST_CASE(only_a_few_samples_are_kept_however_many_come)
{
    // An hour of steps of 5 ms: what is kept is the last sample of each step of the controller
    // from the one that runs on, and the last one before.
    TrafficLatest latest;
    std::shared_ptr<const control::Snapshot> first = sample(0ms, 0, 0);
    const std::weak_ptr<const control::Snapshot> gone = first;
    latest.put(std::move(first), at(0ms), kStep);
    std::vector<std::weak_ptr<const control::Snapshot>> all;
    for (std::chrono::milliseconds t = 5ms; t <= 3'600'000ms; t += 5ms)
    {
        std::shared_ptr<const control::Snapshot> s = sample(t, 0, t / 5ms);
        all.push_back(s);
        latest.put(std::move(s), at(t / 10ms * 10ms), kStep);
    }
    CHECK(gone.expired());
    const auto alive = std::count_if(all.begin(), all.end(), [](const auto& s) { return !s.expired(); });
    CHECK(alive <= 3);
    CHECK(!latest.off_time());
    CHECK(latest.last_before(at(3'600'000ms), kStep)->time == at(3'599'995ms));
    // A late sample (of a step before the controller's) still counts if it is the latest of its kind.
    latest.put(sample(3'599'996ms, 0, 7), at(3'600'000ms), kStep);
    CHECK(latest.last_before(at(3'600'000ms), kStep)->time == at(3'599'996ms));
    latest.put(sample(100ms, 0, 7), at(3'600'000ms), kStep);
    CHECK(latest.last_before(at(3'600'000ms), kStep)->time == at(3'599'996ms));
}

TEST_CASE(a_participant_far_ahead_of_the_controller_is_not_on_the_runs_time)
{
    // The simulation stands still at 100 ms; a participant in real time goes on.
    Fed fed(pair_plan());
    fed.hello(0, counters("BUS"));
    fed.hello(1, counters("BUS"));
    fed.put(1, sample(90ms, 1, 1), 100ms);
    for (std::size_t i = 0; i < TrafficLatest::kMaxAhead; ++i)
    {
        fed.put(0, sample(100ms + 10ms * static_cast<int>(i), 1, 1), 100ms);
    }
    CHECK(!fed.latest[0].off_time());
    fed.put(0, sample(100ms + 10ms * static_cast<int>(TrafficLatest::kMaxAhead), 1, 1), 100ms);
    CHECK(fed.latest[0].off_time());
    // Nothing of it is kept from then on, and the others are as they were.
    std::shared_ptr<const control::Snapshot> more = sample(5000ms, 1, 1);
    const std::weak_ptr<const control::Snapshot> kept = more;
    fed.put(0, std::move(more), 100ms);
    CHECK(kept.expired());
    check_text(said(fed.finish(100)), "before 100\nBUS (can) counted 1 senders 1 receivers 1 delivered ?\n"
                                      "  A real_time at - sent - received -\n  B counted at 90 sent 1 received 1\n");
    // One that runs in virtual time is a step or two ahead, never that far.
    Fed synced(pair_plan());
    synced.hello(0, counters("BUS"));
    for (std::chrono::milliseconds t = 0ms; t < 5000ms; t += 1ms)
    {
        synced.put(0, sample(t, 1, 1), std::max(t / 10ms * 10ms - 10ms, 0ms));
    }
    CHECK(!synced.latest[0].off_time());
}

TEST_CASE(a_participant_that_is_not_counted_says_why)
{
    TrafficPlan plan;
    plan.buses = {{"BUS", "can"}};
    for (const char* name : {"Silent", "Restarted", "RealTime", "Raw", "Mapped", "Early", "Odd", "Ok"})
    {
        plan.participants.push_back({name, true, true, {"BUS"}});
    }
    Fed fed(plan);
    // Silent: no hello.
    // Restarted: more than one process ran under its name.
    fed.hello(1, counters("BUS"));
    fed.put(1, sample(10ms, 1, 1), 10ms);
    fed.seen[1].restarted = true;
    // RealTime.
    fed.hello(2, counters("BUS", false));
    fed.put(2, sample(10ms, 1, 1), 10ms);
    // Raw: an agent, and no counters of this network.
    fed.hello(3, counters("OTHER"));
    fed.put(3, sample(10ms, 1, 1), 10ms);
    // Mapped.
    fed.hello(4, counters("BUS", true, true));
    fed.put(4, sample(10ms, 1, 1), 10ms);
    // Early: a hello, and only a sample of a step at the end.
    fed.hello(5, counters("BUS"));
    fed.put(5, sample(100ms, 1, 1), 100ms);
    // Odd: a sample without the two integers (not this agent's schema) is no sample.
    fed.hello(6, counters("BUS"));
    control::Snapshot shorter;
    shorter.time = at(10ms);
    shorter.values = {control::Value{true}};
    fed.put(6, std::make_shared<const control::Snapshot>(shorter), 10ms);
    // Ok.
    fed.hello(7, counters("BUS"));
    fed.put(7, sample(90ms, 4, 5), 90ms);
    const RunTraffic traffic = fed.finish(100);
    check_text(said(traffic), "before 100\nBUS (can) counted 1 senders 1 receivers 1 delivered ?\n"
                              "  Silent no_control_link at - sent - received -\n"
                              "  Restarted restarted at - sent - received -\n"
                              "  RealTime real_time at - sent - received -\n"
                              "  Raw no_counters at - sent - received -\n"
                              "  Mapped mapped at - sent - received -\n"
                              "  Early no_sample at - sent - received -\n"
                              "  Odd no_sample at - sent - received -\n"
                              "  Ok counted at 90 sent 5 received 4\n");
    const auto& rows = traffic.per_network.at(0).participants;
    CHECK(!rows.at(0).virtual_time.has_value());
    CHECK(rows.at(2).virtual_time == std::optional<bool>(false));
    CHECK(rows.at(7).virtual_time == std::optional<bool>(true));
}

TEST_CASE(a_network_says_whether_what_was_sent_arrived_as_far_as_the_counters_tell)
{
    const auto counted = [](const char* name, std::int64_t sent, std::int64_t received) {
        return TrafficParticipant{name, TrafficNotCounted::None, true, at(90ms), sent, received};
    };
    const TrafficParticipant other{"Tool", TrafficNotCounted::NoCounters, true};
    const auto of = [](std::vector<TrafficParticipant> participants, std::size_t unplaced = 0) {
        const TrafficNetwork network{"BUS", "can", std::move(participants),
                                     std::vector<vehicle_sil::runner::TrafficUnplaced>(unplaced, vehicle_sil::runner::TrafficUnplaced{"Bridge"})};
        const std::optional<bool> delivered = network.delivered();
        return std::to_string(network.counted()) + " " + std::to_string(network.senders()) + " " +
               std::to_string(network.receivers()) + " " + (!delivered ? "?" : *delivered ? "yes" : "no");
    };
    // One sends, one only listens: as it should be.
    check_text(of({counted("Engine", 11, 0), counted("Dashboard", 0, 11)}), "2 1 1 yes");
    check_text(of({counted("A", 5, 5), counted("B", 5, 5)}), "2 2 2 yes");
    check_text(of({counted("A", 5, 5), counted("B", 5, 0)}), "2 2 1 yes");
    // Sent, and nobody else received.
    check_text(of({counted("Engine", 11, 0)}), "1 1 0 no");
    check_text(of({counted("Engine", 11, 0), counted("Dashboard", 0, 0)}), "2 1 0 no");
    // Cannot be said: nobody sent, nobody is counted, nobody is known, or one that is not counted may have received.
    check_text(of({counted("A", 0, 0), counted("B", 0, 4)}), "2 0 1 ?");
    check_text(of({other}), "0 0 0 ?");
    check_text(of({}), "0 0 0 ?");
    check_text(of({counted("Engine", 11, 0), other}), "1 1 0 ?");
    check_text(of({counted("Engine", 11, 3), other}), "1 1 1 ?");
    // A participant's own frames are not in what it received: the one sender that is the one
    // receiver heard someone the launcher does not count, who may have heard it.
    check_text(of({counted("Engine", 11, 3), counted("Dashboard", 0, 0)}), "2 1 1 ?");
    check_text(of({counted("Engine", 11, 3)}), "1 1 1 ?");
    // A participant of the run that may be on this network without the launcher knowing (a bridge).
    check_text(of({counted("Engine", 11, 0)}, 1), "1 1 0 ?");
    check_text(of({counted("Engine", 11, 0), counted("Dashboard", 0, 0)}, 2), "2 1 0 ?");
    // One that is not counted, or unplaced, does not take away what the counted ones show.
    check_text(of({counted("Engine", 11, 0), counted("Dashboard", 0, 11), other}), "2 1 1 yes");
    check_text(of({counted("Engine", 11, 0), counted("Dashboard", 0, 11)}, 1), "2 1 1 yes");
}

TEST_CASE(every_network_of_the_file_is_there_with_who_is_known_to_be_on_it)
{
    TrafficPlan plan;
    plan.buses = {{"FIRST", "can"}, {"UNUSED", "lin"}, {"SECOND", "can"}};
    plan.participants = {{"Described", true, true, {"SECOND", "FIRST", "NOT_IN_THE_FILE"}},
                         {"Tool", true, false, {}},
                         {"Disabled", false, true, {"FIRST"}},
                         {"Quiet", true, false, {}}};
    Fed fed(plan);
    fed.hello(0, TrafficSchema{true, {TrafficColumns{"FIRST", 1, 0, false}}});
    fed.put(0, sample(0ms, 1, 2), 0ms);
    // Without --describe: where its agent has counters (also of a network that is not in the file).
    fed.hello(1, TrafficSchema{true, {TrafficColumns{"SECOND", 1, 0, false}, TrafficColumns{"ELSEWHERE", 1, 0, false}}});
    fed.put(1, sample(0ms, 3, 4), 0ms);
    // Quiet (without --describe and without a control agent) may be on any of them, and Tool on
    // those its agent has no counters for.
    check_text(said(fed.finish(10)), "before 10\nunplaced Quiet no_control_link\n"
                                     "FIRST (can) unplaced 2 counted 1 senders 1 receivers 1 delivered ?\n"
                                     "  Described counted at 0 sent 2 received 1\n"
                                     "  Tool unplaced no_counters\n"
                                     "  Quiet unplaced no_control_link\n"
                                     "UNUSED (lin) unplaced 2 counted 0 senders 0 receivers 0 delivered ?\n"
                                     "  Tool unplaced no_counters\n"
                                     "  Quiet unplaced no_control_link\n"
                                     "SECOND (can) unplaced 1 counted 1 senders 1 receivers 1 delivered ?\n"
                                     "  Described no_counters at - sent - received -\n"
                                     "  Tool counted at 0 sent 4 received 3\n"
                                     "  Quiet unplaced no_control_link\n");
    // Nobody is known to be on a network of the file (participants without --describe and
    // without a control agent): it is there all the same. No network in the file: none.
    Fed unknown(TrafficPlan{{{"BUS", "can"}}, {{"Tool", true, false, {}}}});
    check_text(said(unknown.finish(10)), "before 10\nunplaced Tool no_control_link\n"
                                         "BUS (can) unplaced 1 counted 0 senders 0 receivers 0 delivered ?\n"
                                         "  Tool unplaced no_control_link\n");
    Fed none(TrafficPlan{{}, {{"Tool", true, false, {}}}});
    CHECK(none.finish(10).per_network.empty());
}

TEST_CASE(a_participant_known_on_no_network_keeps_a_network_from_saying_nothing_arrived)
{
    // A counted sender that nobody counted heard, and a bridge without --describe and without a
    // control agent: the frames may have reached the bridge.
    TrafficPlan plan;
    plan.buses = {{"BUS", "can"}};
    plan.participants = {{"Engine", true, true, {"BUS"}}, {"Bridge", true, false, {}}};
    Fed bridged(plan);
    bridged.hello(0, counters("BUS"));
    bridged.put(0, sample(0ms, 0, 11), 0ms);
    check_text(said(bridged.finish(10)), "before 10\nunplaced Bridge no_control_link\n"
                                         "BUS (can) unplaced 1 counted 1 senders 1 receivers 0 delivered ?\n"
                                         "  Engine counted at 0 sent 11 received 0\n"
                                         "  Bridge unplaced no_control_link\n");
    // Without the bridge in the run (disabled), nothing arrived.
    plan.participants.at(1).in_run = false;
    Fed alone(plan);
    alone.hello(0, counters("BUS"));
    alone.put(0, sample(0ms, 0, 11), 0ms);
    check_text(said(alone.finish(10)), "before 10\nBUS (can) counted 1 senders 1 receivers 0 delivered no\n"
                                       "  Engine counted at 0 sent 11 received 0\n");
    // A control agent with counters only of a network that is not in the file places it nowhere,
    // and the reason says that this is so in every run (not: its hello did not come this time);
    // with counters of a network of the file, it is placed there and counted.
    plan.participants.at(1).in_run = true;
    Fed elsewhere(plan);
    elsewhere.hello(0, counters("BUS"));
    elsewhere.put(0, sample(0ms, 0, 11), 0ms);
    elsewhere.hello(1, counters("ELSEWHERE"));
    check_text(said(elsewhere.finish(10)), "before 10\nunplaced Bridge no_counters\n"
                                           "BUS (can) unplaced 1 counted 1 senders 1 receivers 0 delivered ?\n"
                                           "  Engine counted at 0 sent 11 received 0\n"
                                           "  Bridge unplaced no_counters\n");
    Fed placed(plan);
    placed.hello(0, counters("BUS"));
    placed.put(0, sample(0ms, 0, 11), 0ms);
    placed.hello(1, counters("BUS"));
    placed.put(1, sample(0ms, 0, 0), 0ms);
    check_text(said(placed.finish(10)), "before 10\nBUS (can) counted 2 senders 1 receivers 0 delivered no\n"
                                        "  Engine counted at 0 sent 11 received 0\n"
                                        "  Bridge counted at 0 sent 0 received 0\n");
}

TEST_CASE(a_participant_placed_on_one_network_by_its_counters_may_be_on_another_one_too)
{
    // A gateway without --describe: J1939 counters for CAN_A, raw CAN (no counters) on CAN_B.
    // The engine's frames on CAN_B may have reached it.
    TrafficPlan plan;
    plan.buses = {{"CAN_A", "can"}, {"CAN_B", "can"}};
    plan.participants = {{"Gateway", true, false, {}}, {"Engine", true, true, {"CAN_B"}}};
    Fed fed(plan);
    fed.hello(0, counters("CAN_A"));
    fed.put(0, sample(0ms, 0, 0), 0ms);
    fed.hello(1, counters("CAN_B"));
    fed.put(1, sample(0ms, 0, 11), 0ms);
    check_text(said(fed.finish(10)), "before 10\n"
                                     "CAN_A (can) counted 1 senders 0 receivers 0 delivered ?\n"
                                     "  Gateway counted at 0 sent 0 received 0\n"
                                     "CAN_B (can) unplaced 1 counted 1 senders 1 receivers 0 delivered ?\n"
                                     "  Engine counted at 0 sent 11 received 0\n"
                                     "  Gateway unplaced no_counters\n");
    // A described participant is where --describe says, and nowhere else: with the gateway
    // described (on CAN_A only), nothing arrived on CAN_B.
    plan.participants.at(0) = {"Gateway", true, true, {"CAN_A"}};
    Fed described(plan);
    described.hello(0, counters("CAN_A"));
    described.put(0, sample(0ms, 0, 0), 0ms);
    described.hello(1, counters("CAN_B"));
    described.put(1, sample(0ms, 0, 11), 0ms);
    check_text(said(described.finish(10)), "before 10\n"
                                           "CAN_A (can) counted 1 senders 0 receivers 0 delivered ?\n"
                                           "  Gateway counted at 0 sent 0 received 0\n"
                                           "CAN_B (can) counted 1 senders 1 receivers 0 delivered no\n"
                                           "  Engine counted at 0 sent 11 received 0\n");
    // Its hello did not come: it is known on no network, and the reason is another one.
    plan.participants.at(0) = {"Gateway", true, false, {}};
    Fed late(plan);
    late.hello(1, counters("CAN_B"));
    late.put(1, sample(0ms, 0, 11), 0ms);
    check_text(said(late.finish(10)), "before 10\nunplaced Gateway no_control_link\n"
                                      "CAN_A (can) unplaced 1 counted 0 senders 0 receivers 0 delivered ?\n"
                                      "  Gateway unplaced no_control_link\n"
                                      "CAN_B (can) unplaced 1 counted 1 senders 1 receivers 0 delivered ?\n"
                                      "  Engine counted at 0 sent 11 received 0\n"
                                      "  Gateway unplaced no_control_link\n");
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
    TrafficSchema schema = vehicle_sil::runner::detail::traffic_schema(peer);
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
    schema = vehicle_sil::runner::detail::traffic_schema(peer);
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
    std::vector<vehicle_sil::runner::RunMember> members(3); // participants of the file: none is a bridge of --connect
    const TrafficPlan plan = vehicle_sil::runner::detail::traffic_plan(config, check, members);
    CHECK_EQ(plan.buses.size(), std::size_t{2});
    CHECK(plan.buses.at(0).name == "VehicleCAN" && plan.buses.at(0).kind == "can" && plan.buses.at(1).name == "BodyLIN" &&
          plan.buses.at(1).kind == "lin");
    CHECK_EQ(plan.participants.size(), std::size_t{3});
    CHECK(plan.participants.at(0).name == "Engine" && plan.participants.at(0).in_run && plan.participants.at(0).described &&
          plan.participants.at(0).on == (std::vector<std::string>{"VehicleCAN", "BodyLIN"}));
    CHECK(plan.participants.at(1).in_run && !plan.participants.at(1).described && plan.participants.at(1).on.empty());
    CHECK(!plan.participants.at(2).in_run);
    // A bridge of --connect (the plan's mark) carries the frames of the others: known, on no network of its own.
    members[1].outside = vehicle_sil::config::NetworkOutside{"VehicleCAN", "vehicle_sil_can_bridge", "/t/can", "can0", "bridge.VehicleCAN", std::nullopt};
    const TrafficPlan connected = vehicle_sil::runner::detail::traffic_plan(config, check, members);
    CHECK(connected.participants.at(1).described && connected.participants.at(1).on.empty());
    CHECK(connected.participants.at(0).described && !connected.participants.at(0).on.empty());
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

    FakeAgent(control::Hub& h, std::string participant, const std::string& incarnation, bool virtual_time = true) :
      hub(h), name(std::move(participant))
    {
        control::PeerHello hello;
        hello.participant = name;
        hello.incarnation = incarnation;
        hello.virtual_time = virtual_time;
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

/// The samples of a run with A and B on BUS, at a hub, with a controller whose time the test sets.
struct Tapped
{
    control::Hub hub;
    SimTime now{};
    RunSamples samples{{"A", "B"}, pair_plan()};
    Tapped()
    {
        samples.tap(hub, kStep, [this] { return now; });
    }
    /// What the scenario takes, as "place/time".
    std::vector<std::string> taken()
    {
        std::vector<std::string> out;
        for (const auto& [participant, s] : samples.take())
        {
            out.push_back(std::to_string(participant) + "@" + std::to_string((s->time - SimTime{}) / 1ms));
        }
        return out;
    }
};

} // namespace

TEST_CASE(the_samples_of_a_hub_are_counted_and_from_its_beginning_go_to_the_scenario)
{
    Tapped t;
    FakeAgent a(t.hub, "A", "a1");
    FakeAgent b(t.hub, "B", "b1");
    FakeAgent stranger(t.hub, "Stranger", "s1");
    // Before the scenario begins (a participant that does not wait for the start): counted only.
    a.step(0ms, 0, 0);
    stranger.step(0ms, 9, 9);
    CHECK(t.taken().empty());
    t.samples.scenario_begins({"a1", "b1"});
    t.now = at(10ms);
    a.step(10ms, 0, 1);
    b.step(10ms, 1, 0);
    stranger.step(10ms, 9, 9);
    CHECK(t.taken() == (std::vector<std::string>{"0@10", "1@10"}));
    t.now = at(20ms);
    a.step(20ms, 0, 2);
    b.step(20ms, 2, 0);
    t.now = at(30ms);
    a.step(30ms, 0, 3);
    CHECK(t.taken() == (std::vector<std::string>{"0@20", "1@20", "0@30"}));
    CHECK(t.taken().empty());
    // The scenario is over: nothing is kept for it any more.
    b.step(30ms, 3, 0);
    t.samples.scenario_ends();
    t.now = at(40ms);
    a.step(40ms, 0, 4);
    CHECK(t.taken().empty());
    check_text(said(t.samples.finish(40)), "before 40\nBUS (can) counted 2 senders 1 receivers 1 delivered yes\n"
                                           "  A counted at 30 sent 3 received 0\n  B counted at 30 sent 0 received 3\n");
    // The tap is gone.
    a.step(50ms, 0, 5);
    t.samples.untap();
}

TEST_CASE(the_scenario_gets_only_the_samples_of_the_agents_that_were_there_when_it_began)
{
    Tapped t;
    FakeAgent a(t.hub, "A", "a1");
    FakeAgent b(t.hub, "B", "b1");
    t.samples.scenario_begins({"a1", ""}); // the scenario does not use B
    a.step(10ms, 0, 1);
    b.step(10ms, 1, 0);
    FakeAgent again(t.hub, "A", "a2"); // started again: not the agent the scenario began with
    again.step(20ms, 0, 1);
    CHECK(t.taken() == (std::vector<std::string>{"0@10"}));
}

TEST_CASE(a_flood_while_nobody_looks_loses_nothing_of_the_scenario_and_of_the_count)
{
    // The simulation stands still at 100 ms for a long time (nobody takes samples: the scenario
    // holds no step, a run without one never takes). B runs in real time and goes on sending; then
    // the simulation goes on and A's samples come. Every sample of A reaches the scenario, and A
    // is counted by its last step.
    Tapped t;
    FakeAgent a(t.hub, "A", "a1");
    FakeAgent b(t.hub, "B", "b1", false);
    t.samples.scenario_begins({"a1", ""});
    t.now = at(90ms);
    a.step(90ms, 0, 9);
    t.now = at(100ms);
    for (int i = 0; i < 200'000; ++i)
    {
        b.step(100ms + std::chrono::milliseconds(i), 0, 0);
    }
    a.step(100ms, 0, 10);
    a.step(105ms, 0, 11);
    t.now = at(110ms);
    a.step(110ms, 0, 12);
    CHECK(t.taken() == (std::vector<std::string>{"0@90", "0@100", "0@105", "0@110"}));
    check_text(said(t.samples.finish(110)), "before 110\nBUS (can) counted 1 senders 1 receivers 0 delivered ?\n"
                                            "  A counted at 105 sent 11 received 0\n  B real_time at - sent - received -\n");
}

TEST_CASE(a_participant_whose_samples_did_not_come_is_told_from_one_without_an_agent)
{
    Tapped t;
    FakeAgent a(t.hub, "A", "a1");
    check_text(said(t.samples.finish(100)), "before 100\nBUS (can) counted 0 senders 0 receivers 0 delivered ?\n"
                                            "  A no_sample at - sent - received -\n  B no_control_link at - sent - received -\n");
}

TEST_CASE(a_participant_that_was_started_again_is_not_counted)
{
    // Seen by its samples.
    {
        Tapped t;
        FakeAgent first(t.hub, "A", "a1");
        first.step(0ms, 0, 5);
        FakeAgent second(t.hub, "A", "a2");
        t.now = at(20ms);
        second.step(20ms, 0, 1);
        CHECK(said(t.samples.finish(100)).find("A restarted at") != std::string::npos);
    }
    // Seen by the hub's hello only: the samples are the first process's, the hello is the second's.
    {
        Tapped t;
        FakeAgent first(t.hub, "A", "a1");
        first.step(0ms, 0, 5);
        FakeAgent second(t.hub, "A", "a2");
        CHECK(said(t.samples.finish(100)).find("A restarted at") != std::string::npos);
    }
}
