// SPDX-License-Identifier: MIT
//
// The controls of a run (launch::RunControls) by themselves: what they are made of, the phase they
// keep and tell, and the controls a run makes for itself when it is given none (run_file() on a
// file that cannot be read: nothing starts).

#include "test_harness.hpp"

#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/run_controls.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::launch::Command;
using vehicle_sil::launch::EventStream;
using vehicle_sil::launch::EventStreamOptions;
using vehicle_sil::launch::EventWrite;
using vehicle_sil::launch::Phase;
using vehicle_sil::launch::RunControls;
using vehicle_sil::launch::RunOptions;
using vehicle_sil::launch::StopRequests;
using vehicle_sil::launch::TimeCommands;

Command command(const std::string& name)
{
    Command c;
    c.name = name;
    c.accepted = true;
    return c;
}

/// The events as written, in memory.
struct Written
{
    Written()
    {
        EventStreamOptions options;
        options.heartbeat = 0ms;
        stream = std::make_unique<EventStream>(
            [this](std::string_view bytes) {
                const std::lock_guard lock(mutex);
                text.append(bytes);
                EventWrite out;
                out.bytes = bytes.size();
                return out;
            },
            [](const std::string&) {}, options);
    }

    /// The values of "phase" of the phase events written so far, in their order.
    std::vector<std::string> phases()
    {
        CHECK(stream->wait_written(10s));
        const std::lock_guard lock(mutex);
        std::vector<std::string> out;
        const std::string_view key = "\"event\":\"phase\"";
        const std::string_view value = "\"phase\":\"";
        for (std::size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 1))
        {
            const std::size_t begin = text.find(value, at) + value.size();
            out.push_back(text.substr(begin, text.find('"', begin) - begin));
        }
        return out;
    }

    bool has(std::string_view part)
    {
        CHECK(stream->wait_written(10s));
        const std::lock_guard lock(mutex);
        return text.find(part) != std::string::npos;
    }

    std::mutex mutex;
    std::string text;
    std::unique_ptr<EventStream> stream; ///< last: its writer thread uses the members above
};

} // namespace

TEST_CASE(run_controls_lend_the_stop_requests_they_were_made_with)
{
    StopRequests stops;
    RunControls controls(stops);
    CHECK(&controls.stops() == &stops);
    stops.request(2);
    CHECK_EQ(controls.stops().count(), 1);
    controls.stops().request(15);
    CHECK_EQ(stops.count(), 2);
    CHECK_EQ(stops.first(), 2);
}

TEST_CASE(run_controls_keep_the_phase_before_they_tell_it)
{
    StopRequests stops;
    std::vector<Phase> told;
    RunControls* made = nullptr;
    RunControls::Observers observers;
    observers.on_phase = [&](Phase phase) {
        told.push_back(phase);
        CHECK(made->phase() == phase); // phase() has it when the observer hears of it
    };
    RunControls controls(stops, false, std::move(observers));
    made = &controls;
    CHECK(controls.phase() == Phase::Checking);
    CHECK(told.empty());
    for (const Phase phase : {Phase::Checking, Phase::Starting, Phase::Waiting, Phase::Running, Phase::Stopping, Phase::Ended})
    {
        controls.set_phase(phase);
        CHECK(controls.phase() == phase);
    }
    CHECK((told == std::vector<Phase>{Phase::Checking, Phase::Starting, Phase::Waiting, Phase::Running, Phase::Stopping,
                                     Phase::Ended}));
}

TEST_CASE(run_controls_without_observers_keep_the_phase)
{
    StopRequests stops;
    RunControls controls(stops);
    controls.set_phase(Phase::Stopping);
    CHECK(controls.phase() == Phase::Stopping);
    CHECK(controls.time().apply(command("pause"), "stdin").accepted);
}

TEST_CASE(run_controls_hand_the_time_commands_their_observer_and_the_start_paused)
{
    StopRequests stops;
    std::vector<std::string> seen;
    RunControls::Observers observers;
    observers.on_command = [&seen](const Command& c, std::string_view source) {
        seen.push_back(std::string(source) + " " + c.name.value_or("") + (c.accepted ? " accepted" : " refused"));
    };
    RunControls paused(stops, true, std::move(observers));
    CHECK(&paused.time() == &paused.time());
    CHECK(paused.time().apply(command("pause"), "stdin").accepted);
    CHECK(!paused.time().apply(command("step"), "control_server").accepted); // no controller is attached
    CHECK((seen == std::vector<std::string>{"stdin pause accepted", "control_server step refused"}));
    // Started paused, the run would hold before its first step; not otherwise.
    const auto give_up = [] { return true; };
    CHECK(paused.time().hold_before_start(give_up, 1ms) == TimeCommands::Hold::GaveUp);
    RunControls running(stops, false);
    CHECK(running.time().hold_before_start(give_up, 1ms) == TimeCommands::Hold::None);
    // A stop is no affair of the time commands: nothing here asks for one.
    CHECK_EQ(stops.count(), 0);
}

TEST_CASE(phase_names_are_those_of_the_phase_events)
{
    using vehicle_sil::launch::phase_name;
    CHECK(std::string(phase_name(Phase::Checking)) == "checking");
    CHECK(std::string(phase_name(Phase::Starting)) == "starting");
    CHECK(std::string(phase_name(Phase::Waiting)) == "waiting");
    CHECK(std::string(phase_name(Phase::Running)) == "running");
    CHECK(std::string(phase_name(Phase::Stopping)) == "stopping");
    CHECK(std::string(phase_name(Phase::Ended)) == "ended");
}

TEST_CASE(a_run_without_controls_makes_its_own_and_writes_its_phases)
{
    Written written;
    RunOptions options;
    options.events = written.stream.get();
    const auto report = vehicle_sil::launch::run_file("no-such-directory/sim.yaml", "sim.yaml", options);
    CHECK(!report.run.has_value());
    CHECK((written.phases() == std::vector<std::string>{"checking", "ended"}));
    // And again: each run has controls of its own, so the second begins as the first did.
    static_cast<void>(vehicle_sil::launch::run_file("no-such-directory/sim.yaml", "sim.yaml", options));
    CHECK((written.phases() == std::vector<std::string>{"checking", "ended", "checking", "ended"}));
}

TEST_CASE(a_run_with_the_callers_controls_sets_their_phase_and_leaves_the_phase_events_to_them)
{
    StopRequests stops;
    std::vector<Phase> told;
    RunControls::Observers observers;
    observers.on_phase = [&told](Phase phase) { told.push_back(phase); };
    RunControls controls(stops, false, std::move(observers));
    Written written;
    RunOptions options;
    options.controls = &controls;
    options.events = written.stream.get();
    const auto report = vehicle_sil::launch::run_file("no-such-directory/sim.yaml", "sim.yaml", options);
    CHECK(!report.run.has_value());
    CHECK(controls.phase() == Phase::Ended);
    CHECK((told == std::vector<Phase>{Phase::Checking, Phase::Ended}));
    // The observer wrote none, so there is none: the run does not write them a second time.
    CHECK(written.phases().empty());
    CHECK(written.has("\"event\":\"check\""));
}
