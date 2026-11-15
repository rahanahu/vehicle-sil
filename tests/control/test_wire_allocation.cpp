// SPDX-License-Identifier: MIT
//
// Building the step messages of a step allocates nothing once the buffer they are built in has
// grown, in either version of the format: an agent's transport does it in the step
// (control/wire.hpp, encode_step()). This executable counts every allocation of the process
// (allocation_counter.hpp), so it is built on its own.
#include "allocation_counter.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/control/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::control;
namespace wire = vehicle_sil::control::wire;

TEST_CASE(wire_building_the_step_messages_does_not_allocate_once_the_buffer_has_grown)
{
    const SimTime now = SimTime{} + 5ms;
    // More than one message holds, with texts beyond the limit of a text (cut, not copied).
    std::vector<CommandResult> results;
    for (std::uint64_t i = 0; i < 40; ++i)
    {
        results.push_back({i, i % 2 == 0 ? CommandStatus::Applied : CommandStatus::Rejected,
                           i % 2 == 0 ? std::optional<SimTime>(now) : std::nullopt, i % 3 == 0, std::string(i * 60, 'r'),
                           static_cast<std::uint32_t>(i)});
    }
    std::vector<Event> events;
    for (std::size_t i = 0; i < 40; ++i)
    {
        events.push_back({now, EventLevel::Warning, std::string(i * 60, 'e')});
    }
    Snapshot sample{now, 1, {}};
    for (int i = 0; i < 2000; ++i)
    {
        sample.values.emplace_back(i % 2 == 0 ? Value{1.5 * i} : Value{i % 3 == 0});
    }

    for (const std::uint16_t version : {std::uint16_t{1}, std::uint16_t{2}})
    {
        for (const std::size_t limit : {wire::kMinMessageBytes, wire::kMaxMessageBytes})
        {
            wire::Bytes scratch;
            std::size_t messages = 0;
            std::size_t bytes = 0;
            const std::function<void(const wire::Bytes&)> emit = [&](const wire::Bytes& message) {
                ++messages;
                bytes += message.size();
            };
            // The first step grows the buffer.
            wire::encode_step(1, now, false, results, events, &sample, limit, scratch, emit, version);
            CHECK(messages > 1);
            const std::size_t first = bytes;
            messages = 0;
            bytes = 0;
            g_allocations.store(0);
            g_counting.store(true);
            wire::encode_step(1, now, false, results, events, &sample, limit, scratch, emit, version);
            g_counting.store(false);
            CHECK_EQ(g_allocations.load(), std::size_t{0});
            CHECK_EQ(bytes, first);
        }
    }
}
