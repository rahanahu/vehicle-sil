// SPDX-License-Identifier: MIT
//
// The hub's end of the control agents' connections over SIL Kit's publish/subscribe
// (vehicle_sil::control_silkit).
#pragma once

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vehicle_sil::control_silkit {

/// Where the connection to one participant's agent stands.
enum class LinkState
{
    Connected,       ///< its hello arrived: it is a peer of the hub
    VersionMismatch, ///< its hello is of another version of the format (LinkInfo::version): not a peer
    Ended,           ///< it was connected and has said its last word, or left the simulation
};

struct LinkInfo
{
    std::string participant;
    LinkState state = LinkState::Connected;
    /// The version of the format its hello has.
    std::uint16_t version = 0;
    /// What its hello said (Connected and Ended).
    bool virtual_time = false;
    SimDuration step{};
    SimDuration sample_period{};
    std::size_t variables = 0;
    /// It has answered that it receives this side's commands: a write to it is sent. Until then
    /// (a moment after its hello, for a participant that joined a running simulation) a write is
    /// refused at once.
    bool ready = false;
    /// Step messages of it that arrived although the hub's participant had begun a step that
    /// starts after them (see HubTransportStats::late_messages).
    std::uint64_t late_messages = 0;
};

struct HubTransportStats
{
    std::uint64_t hellos = 0;        ///< accepted
    std::uint64_t messages = 0;      ///< step and submitted messages given to the hub
    std::uint64_t bytes = 0;         ///< of all received messages
    std::uint64_t undecodable = 0;   ///< not a message of this version (a hello of another version is counted here too)
    std::uint64_t unknown = 0;       ///< of a session without a hello (not: after its end), or a sample that does not fit its schema (count, order, types)
    /// Hellos that were not taken: of more participants than HubTransportOptions::max_links, or
    /// with the session number of another participant's connection.
    std::uint64_t refused_hellos = 0;
    /// Step messages of virtual-time agents that arrived after the hub's participant had begun a
    /// step that starts after the message's step. Zero as long as every agent publishes within
    /// its steps and the hub's participant takes part in the time synchronisation: anything else
    /// means that "the values before P" were not complete at some P.
    std::uint64_t late_messages = 0;
    /// The receive handlers run on the SIL Kit thread of the hub's participant, which also runs
    /// its steps: the simulation waits for them. How often they ran and how long in all, and the
    /// longest call.
    std::uint64_t handler_calls = 0;
    std::chrono::nanoseconds handler_time{0};
    std::chrono::nanoseconds max_handler_time{0};
};

struct HubTransportOptions
{
    /// The most participants whose hello is taken in the life of the transport (connected, ended
    /// and of another version together): a bound on what it and the hub keep. Hellos beyond it
    /// are counted (HubTransportStats::refused_hellos) and dropped. Must be > 0.
    std::size_t max_links = 256;
};

/// What wait_for_hellos() found, each list in the order asked.
struct HelloReport
{
    /// Said hello and answered that it receives commands: a write to it now is delivered.
    std::vector<std::string> connected;
    std::vector<std::string> mismatched; ///< a hello of another version (links() has the version)
    std::vector<std::string> ended;      ///< left the simulation (or ended its agent) without being awaited any longer
    std::vector<std::string> missing;    ///< no hello (or no answer to the probe) by the deadline, or when the wait was cancelled
    /// wait_for_announced() only: had not said hello when it looked, and was not waited for.
    std::vector<std::string> not_announced;
    bool cancelled = false;
};

/// Connects the control agents of the simulation (AgentTransport, each on its ECU's participant)
/// to a hub, through subscribers and one publisher on `runtime`'s participant.
///
/// - An agent becomes a peer of the hub when its hello arrives (control::Hub::agent_connected();
///   PeerHello::incarnation is the session number of its transport). A hello of another version
///   of the format makes no peer; links() says so, with the participant's name. A participant
///   started anew under its name (also after it failed) says hello with a new session, which
///   replaces the old connection.
/// - Commands go to an agent once it has answered a probe (LinkInfo::ready): the hello does not
///   say that the agent receives this side's commands yet. The probe is sent with the hello, again
///   with every step message until answered, and by wait_for_hellos(). Until the answer, and once
///   the participant's lifecycle is in a stopping or terminal state, AgentLink::send() refuses
///   (the hub settles the write as Invalid at once) instead of sending what may reach nobody.
/// - What is remembered is bounded: HubTransportOptions::max_links participants; a hello may
///   describe control::wire::kMaxVariables variables in control::wire::kMaxHelloBytes.
/// - The receive handlers decode a message and give it to the hub, on the SIL Kit thread of the
///   participant, nothing more: the hub's subscribers are served by their own threads. Nothing is
///   dropped and nothing is queued here, so a slow handler slows the simulation down
///   (HubTransportStats::handler_time) instead of losing or delaying values.
/// - With `runtime` in the time synchronisation (a SimulationController with a time gate), and
///   agents that step in it: when a step of `runtime` at P has begun, and so when the gate stands
///   still at P, the hub has every result and sample of the agents' steps that start before P.
///   What their steps at P itself produce may or may not have arrived: it does arrive while the
///   simulation stands still. Nothing of a later step arrives before the simulation goes on.
/// - Commands (the hub's AgentLink::send()) are published from the calling thread.
/// - An agent's connection ends with its last word (AgentTransport's destructor), when its
///   participant's lifecycle goes to Error (a failed step, or a lost connection: the process
///   ended), and when this transport is destroyed. The commands still open for it then become
///   final (control::Hub::agent_ended()): none waits for ever. A participant that shuts down in
///   order and never says its last word keeps its connection until this transport is destroyed.
///
/// There is no authentication on this path: whoever can join the registry can publish commands,
/// as it can send frames on the simulated buses.
///
/// Make it before `runtime` starts (std::logic_error afterwards), destroy it before the runtime
/// (the SimulationController) and before the hub. It takes the runtime's peer status observer
/// (ParticipantRuntime::set_peer_status_observer()) and makes SIL Kit services named
/// "vehicle_sil.control.*" on the participant.
class HubTransport
{
public:
    /// std::invalid_argument for options out of range.
    HubTransport(control::Hub& hub, ParticipantRuntime& runtime, HubTransportOptions options = {});
    ~HubTransport();
    HubTransport(const HubTransport&) = delete;
    HubTransport& operator=(const HubTransport&) = delete;

    /// Waits until each of `participants` is connected and ready, has sent a hello of another version, or
    /// has left the simulation (its lifecycle is in a stopping or terminal state); at most until
    /// `deadline`, and not once `cancelled` (looked at every few milliseconds; may be empty) says
    /// true. For the window before the start (SimulationController::set_start_window()): an agent
    /// publishes its hello before its participant starts, so it is normally there when the window
    /// opens. Not from a SIL Kit handler.
    HelloReport wait_for_hellos(const std::vector<std::string>& participants, std::chrono::steady_clock::time_point deadline,
                                const std::function<bool()>& cancelled = {});

    /// wait_for_hellos() for those of `participants` that have said hello already; the others
    /// are HelloReport::not_announced. For the window before the start of a controller that does
    /// not know which participants offer their variables (vehicle_sil_control_server): in the
    /// window, a required participant that published its hello before it started has been heard
    /// (docs/dependencies.md), so one that has not offers no agent, and waiting for it would only
    /// delay the start.
    HelloReport wait_for_announced(const std::vector<std::string>& participants, std::chrono::steady_clock::time_point deadline,
                                   const std::function<bool()>& cancelled = {});

    /// One entry per participant that has sent a hello, by name.
    [[nodiscard]] std::vector<LinkInfo> links() const;
    [[nodiscard]] HubTransportStats stats() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace vehicle_sil::control_silkit
