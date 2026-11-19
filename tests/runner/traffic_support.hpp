// SPDX-License-Identifier: MIT
//
// The runs and samples that the tests of a run's traffic build. Shared by the tests of the count
// (tests/runner) and by the test of the traffic lines of the launcher's library (tests/launch).

#pragma once

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

namespace vehicle_sil::launch::test::traffic_support {

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

inline SimTime at(std::chrono::milliseconds ms)
{
    return SimTime{} + ms;
}

/// A sample of an agent whose variables are [frames_received, frames_sent] of one network.
inline std::shared_ptr<const control::Snapshot> sample(std::chrono::milliseconds time, std::int64_t received, std::int64_t sent)
{
    control::Snapshot s;
    s.time = at(time);
    s.sequence = static_cast<std::uint64_t>(time.count()) + 1;
    s.values = {control::Value{received}, control::Value{sent}};
    return std::make_shared<const control::Snapshot>(std::move(s));
}

inline TrafficSchema counters(const std::string& network, bool virtual_time = true, bool mapped = false)
{
    return TrafficSchema{virtual_time, {TrafficColumns{network, 1, 0, mapped}}};
}

} // namespace vehicle_sil::launch::test::traffic_support
