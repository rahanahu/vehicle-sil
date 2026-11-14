// SPDX-License-Identifier: MIT
//
// The ECU's end of the control agent's connection over SIL Kit's publish/subscribe
// (vehicle_sil::control_silkit).
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace vehicle_sil::control_silkit {

struct AgentTransportOptions
{
    /// The largest step message (control::wire::kMinMessageBytes..kMaxMessageBytes); what a step
    /// has to say beyond it goes out in several messages.
    std::size_t max_message_bytes = control::wire::kMaxMessageBytes;
    /// The ECU's step, for the hello (what the hub shows; nothing depends on it). Zero: not said.
    SimDuration step{};
    /// The longest the destructor waits for the hub's answer to the agent's last word (wall-clock
    /// time, once, after the steps have stopped; it has no effect on the virtual time). The answer
    /// usually comes at once; the whole time passes only if the hub has gone or does not answer
    /// (a version before the answer existed). 0: do not wait. Not negative.
    std::chrono::milliseconds last_word_wait{500};
};

struct AgentTransportStats
{
    std::uint64_t step_messages = 0; ///< published from the steps (and the last word)
    std::uint64_t step_bytes = 0;
    std::uint64_t commands = 0;      ///< received for this agent and given to Agent::submit()
    std::uint64_t ignored = 0;       ///< commands for another agent (every agent receives every command)
    /// Received on the command topic and not a command or probe of this version (a kind a later
    /// version added counts here and is otherwise ignored).
    std::uint64_t undecodable = 0;
};

/// Connects a control agent to the hub of the simulation (HubTransport, on another participant)
/// through publishers and a subscriber on the ECU's own SIL Kit participant.
///
/// What it does, and on which thread:
/// - The constructor completes the agent's schema (control::Agent::complete_schema(): a Peer adds
///   its monitoring outputs) and publishes the hello, which SIL Kit keeps for a hub that
///   subscribes later.
/// - What a step produces (the results of the commands it applied or refused, its events, its
///   sample) is published within that step, on the step thread, right after the agent sampled
///   (control::Agent::set_step_sink()). With virtual time SIL Kit stamps those messages with the
///   step's time and delivers them to a synchronised subscriber before that subscriber's first
///   step that starts after it. Nothing is sent from another thread: that would lose the order.
/// - A command is given to control::Agent::submit() in SIL Kit's receive handler, and what
///   submit() said goes back from there at once (outside the steps: in order, but without a time).
///   Every agent receives every command and looks first at whom it is for (the session); the
///   hub's probe ("do you receive my commands?") is answered from the same handler.
/// - The destructor publishes the agent's last word: the results not sent yet (after Peer::stop()
///   those of the writes it cancelled). The hub answers it after it has ended the connection, and
///   the destructor waits for that answer, at most AgentTransportOptions::last_word_wait: the
///   participant may be destroyed right after the transport, and SIL Kit drops what a participant
///   has not written to the socket when it is destroyed. So, when the destructor returns within
///   that time, the hub has the last word and has settled what waited for this agent.
///   It does not wait if no hub has ever asked this agent or sent it a command (a simulation
///   without a hub). It waits the whole time, and the last word may then not arrive, if the hub
///   has gone, is of a version that does not answer, or cannot answer in time; the hub's side
///   then settles what is open when its transport is destroyed, as Unknown. The answer comes on
///   SIL Kit's thread: a transport destroyed from a SIL Kit handler waits the whole time.
///
/// In real time (a Peer without virtual time) the same messages flow, without any promise about
/// when they arrive relative to the hub's time.
///
/// Make it after the ECU (a Peer; its runtime()) and before the ECU starts (std::logic_error
/// once the runtime has started: SIL Kit connects the services made before the start), on the
/// set-up thread; destroy it before the ECU, after stopping the ECU (Peer::stop()) so that the
/// cancelled writes are still reported. It holds a control::TransportLease on the agent. One per
/// agent (std::logic_error: the agent has one step sink). std::invalid_argument for options out of
/// range (max_message_bytes, a negative last_word_wait) and for a schema whose description does not fit a hello (control::wire::kMaxHelloBytes).
/// It makes SIL Kit services named "vehicle_sil.control.*" on the participant.
class AgentTransport
{
public:
    AgentTransport(control::Agent& agent, ParticipantRuntime& runtime, AgentTransportOptions options = {});
    ~AgentTransport();
    AgentTransport(const AgentTransport&) = delete;
    AgentTransport& operator=(const AgentTransport&) = delete;

    /// The number that names this transport in every message (new for every transport).
    [[nodiscard]] std::uint64_t session() const noexcept;
    [[nodiscard]] AgentTransportStats stats() const;

private:
    struct State;
    std::shared_ptr<State> state_;
    std::unique_ptr<control::TransportLease> lease_;
    std::chrono::milliseconds last_word_wait_{};
};

} // namespace vehicle_sil::control_silkit
