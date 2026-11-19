// SPDX-License-Identifier: MIT
//
// The traffic of a run's result as the launcher's outputs word it (the traffic lines of the text
// for people). The counting itself is in tests/runner/test_traffic.cpp.

#include "traffic_support.hpp"
#include "launch_test_util.hpp"

#include "format/check_format.hpp"
#include "vehicle_sil/runner/detail/run_samples.hpp"
#include "vehicle_sil/runner/detail/run_traffic.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/launch/run.hpp"

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
using namespace vehicle_sil::launch;
using namespace std::chrono_literals;
using vehicle_sil::launch::test::check_text;
using launch::detail::RunSamples;
using launch::detail::TrafficColumns;
using launch::detail::TrafficLatest;
using launch::detail::TrafficSeen;
using launch::detail::TrafficPlan;
using launch::detail::TrafficSchema;
using namespace vehicle_sil::launch::test::traffic_support;

TEST_CASE(the_traffic_for_people_is_a_line_and_a_table)
{
    RunTraffic traffic;
    traffic.before_ms = 1000;
    check_text(launch::detail::traffic_text(traffic), "traffic: no network in use\n");
    TrafficParticipant engine{"Engine", TrafficNotCounted::None, true, at(990ms), 1234, 0};
    TrafficParticipant dashboard{"Dashboard", TrafficNotCounted::None, true, at(995ms), 0, 1234};
    traffic.per_network = {TrafficNetwork{"J1939_CAN", "can", {engine, dashboard}}};
    check_text(launch::detail::traffic_text(traffic), "traffic: J1939_CAN: 1 participant sent, 1 received (2 counted) (steps before 1000 ms)\n\n"
                                                      "network    participant  sent  received\n"
                                                      "J1939_CAN  Engine       1234         0\n"
                                                      "J1939_CAN  Dashboard       0      1234\n");
    const auto not_counted = [](const char* name, TrafficNotCounted why) {
        return TrafficParticipant{name, why, why == TrafficNotCounted::NoControlLink ? std::nullopt : std::optional<bool>(true)};
    };
    TrafficParticipant both{"Both", TrafficNotCounted::None, true, at(990ms), 7, 8};
    traffic.per_network = {TrafficNetwork{"A", "can", {engine, both, not_counted("P0", TrafficNotCounted::NoCounters)}},
                           TrafficNetwork{"Quiet", "can", {TrafficParticipant{"Q", TrafficNotCounted::None, true, at(990ms), 0, 0}}},
                           TrafficNetwork{"B",
                                          "lin",
                                          {not_counted("P1", TrafficNotCounted::NoControlLink),
                                           not_counted("P2", TrafficNotCounted::Restarted),
                                           not_counted("P3", TrafficNotCounted::RealTime),
                                           not_counted("P4", TrafficNotCounted::NoCounters),
                                           not_counted("P5", TrafficNotCounted::Mapped),
                                           not_counted("P6", TrafficNotCounted::NoSample)}},
                           TrafficNetwork{"One", "lin", {not_counted("P7", TrafficNotCounted::NoCounters)}},
                           TrafficNetwork{"Nobody", "ethernet", {}}};
    check_text(launch::detail::traffic_text(traffic),
               "traffic: A: 2 participants sent, 1 received (2 counted, 1 not counted); Quiet: nothing sent (1 counted); "
               "B: not counted (6 participants); One: not counted (1 participant); "
               "Nobody: not counted (no participant known to be on it) (steps before 1000 ms)\n\n"
               "network  participant  sent  received\n"
               "A        Engine       1234         0\n"
               "A        Both            7         8\n"
               "A        P0              -         -  not counted: no J1939 counters\n"
               "Quiet    Q               0         0\n"
               "B        P1              -         -  not counted: no control link\n"
               "B        P2              -         -  not counted: restarted\n"
               "B        P3              -         -  not counted: real time\n"
               "B        P4              -         -  not counted: no J1939 counters\n"
               "B        P5              -         -  not counted: counters mapped by variables\n"
               "B        P6              -         -  not counted: no sample before the end\n"
               "One      P7              -         -  not counted: no J1939 counters\n"
               "Nobody   -               -         -  no participant known to be on it\n");

    // "nothing sent" only when everyone who may be on the network is counted and nobody received:
    // otherwise the numbers.
    const TrafficParticipant listener{"Listener", TrafficNotCounted::None, true, at(990ms), 0, 76};
    const TrafficParticipant quiet{"Q", TrafficNotCounted::None, true, at(990ms), 0, 0};
    traffic.per_network = {TrafficNetwork{"Raw", "can", {listener, not_counted("P0", TrafficNotCounted::NoCounters)}},
                           TrafficNetwork{"Heard", "can", {listener}}, TrafficNetwork{"Quiet", "can", {quiet}}};
    const std::string text = launch::detail::traffic_text(traffic);
    check_text(text.substr(0, text.find('\n') + 1),
               "traffic: Raw: 0 participants sent, 1 received (1 counted, 1 not counted); Heard: 0 participants sent, 1 received "
               "(1 counted); Quiet: nothing sent (1 counted) (steps before 1000 ms)\n");

    // A participant of the run that the launcher knows on no network: counted in every network's
    // parentheses, and one row of its own with the reason. One that its counters place on
    // another network: a row in each network it may be on as well.
    using launch::TrafficUnplaced;
    using launch::TrafficUnplacedReason;
    const TrafficUnplaced bridge{"Bridge", TrafficUnplacedReason::NoControlLink};
    const TrafficUnplaced tool{"Tool", TrafficUnplacedReason::NoCounters};
    const TrafficUnplaced gateway{"Gateway", TrafficUnplacedReason::NoCounters};
    traffic.unplaced = {bridge, tool};
    traffic.per_network = {TrafficNetwork{"J1939_CAN", "can", {engine}, {bridge, tool, gateway}},
                           TrafficNetwork{"Quiet", "can", {quiet}, {bridge, tool}},
                           TrafficNetwork{"Nobody", "lin", {}, {bridge, tool, gateway}}};
    check_text(launch::detail::traffic_text(traffic),
               "traffic: J1939_CAN: 1 participant sent, 0 received (1 counted, 3 unplaced); Quiet: 0 participants sent, "
               "0 received (1 counted, 2 unplaced); Nobody: not counted (no participant known to be on it, 3 unplaced) "
               "(steps before 1000 ms)\n\n"
               "network    participant  sent  received\n"
               "J1939_CAN  Engine       1234         0\n"
               "J1939_CAN  Gateway         -         -  unplaced: no J1939 counters\n"
               "Quiet      Q               0         0\n"
               "Nobody     -               -         -  no participant known to be on it\n"
               "Nobody     Gateway         -         -  unplaced: no J1939 counters\n"
               "-          Bridge          -         -  unplaced: no control link\n"
               "-          Tool            -         -  unplaced: no J1939 counters\n");
}
