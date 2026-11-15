// SPDX-License-Identifier: MIT
//
// The run's side of the ECUs' control agents: the hub, what --describe says about each
// participant's variables, and what the run's result says about each link. Internal.
#pragma once

#include "run_context.hpp"
#include "run_output.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::launch::detail {

/// What the start window found: who answered, and every link as it was then.
struct ControlStart
{
    control_silkit::HelloReport report;
    std::vector<control_silkit::LinkInfo> links;
};

/// The hub of the run and what is known about the participants' control links.
///
/// Threads and locks: no lock of its own.
/// - hub(): the run's own hub is made at the first call. That call is the main thread's, in the
///   setup, before the controller is started and so before any other thread of the run exists
///   (Runner::run(): the control server, or the hub's transport); every later call (the start
///   window's thread, the main thread) only reads the pointer. The Hub keeps itself.
/// - described_link(), offers_control(), expected_agents(): any thread; they read the RunContext only.
/// - note_links(), note_late_values(), note_late_maps(): the main thread (they write the RunOutcome).
class RunControl
{
public:
    RunControl(const RunContext& ctx, RunOutput& out) : ctx_(ctx), out_(out) {}

    /// RunOptions::hub, or the run's own. The first call gives it the variable maps of the
    /// enabled participants (control::Hub::set_variable_map()), before any agent connects.
    [[nodiscard]] control::Hub& hub();

    /// What --describe says about participant `i`'s variables (ControlLink::None, NotOffered,
    /// VersionMismatch), or Connected for "it offers them in this launcher's version".
    [[nodiscard]] ControlLink described_link(std::size_t i) const;

    /// --describe says that participant `i` has a control agent that speaks this launcher's
    /// version of the messages (with variables or without).
    [[nodiscard]] bool offers_control(std::size_t i) const;

    /// The enabled, required ECUs whose --describe offers a control agent in this launcher's
    /// version of the messages: their hello is waited for. Not a participant with describe: false
    /// (the launcher does not know whether it has an agent): note_links() looks again for it.
    [[nodiscard]] std::vector<std::string> expected_agents() const;

    /// After the start: where each participant's control agent stands (the result's
    /// "control_link", the event "control_link"): what its hello says (`start`, what the start
    /// window found; for a participant the window did not wait for and had no link of that takes
    /// commands, `now`, the links after the start), else what --describe said. With it, what of
    /// its variable map does not fit (a line of the run too). For the participants of the run
    /// (`in_run(i)`).
    void note_links(RunOutcome& outcome, const std::optional<ControlStart>& start,
                    const std::vector<control_silkit::LinkInfo>& now, const std::function<bool(std::size_t)>& in_run) const;

    /// After the simulation: the agents whose messages came late (RunOutcome's
    /// control_late_messages), and what the transport dropped.
    void note_late_values(RunOutcome& outcome, control_silkit::HubTransport& transport) const;

    /// After the simulation: what does not fit of the variable map of a participant whose agent
    /// connected after note_links() had looked (one that joined during the run, or whose hello
    /// came late), so that the result has it whenever the hello came; a line of the run too.
    void note_late_maps(RunOutcome& outcome) const;

private:
    /// The line of the run about `run.variable_map_problems`; `late`: said after the simulation.
    void say_map_problems(const ParticipantRun& run, bool late) const;

    const RunContext& ctx_;
    RunOutput& out_;
    std::unique_ptr<control::Hub> own_hub_; ///< without RunOptions::hub
    bool maps_set_ = false;                 ///< the first hub() gave the hub the participants' variable maps
};

} // namespace vehicle_sil::launch::detail
