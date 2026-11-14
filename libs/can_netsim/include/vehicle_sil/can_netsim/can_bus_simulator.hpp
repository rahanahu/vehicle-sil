// SPDX-License-Identifier: MIT
//
// A simple CAN network simulator on SIL Kit's network simulator API (experimental):
// arbitration, transmission time and bus occupancy with CanBusModel (vehicle_sil::can_netsim).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace SilKit {
class IParticipant;
} // namespace SilKit

namespace vehicle_sil::can {

struct CanBusSimulatorOptions
{
    /// Bit rate of a network until a controller sets one with SetBaudRate() (> 0).
    std::uint32_t default_bitrate = 500'000;
    /// Frames each controller can have waiting for the bus (> 0). A request beyond it is
    /// answered with a TransmitQueueFull acknowledgement.
    std::size_t transmit_queue_capacity = 64;
    /// Requests of one network waiting for the next step (> 0). A request beyond it is dropped
    /// and counted (an error: the run is no longer deterministic).
    std::size_t inbox_capacity = 4096;
    /// A send time further than this (or than the simulator's step size, if larger) ahead of
    /// the simulator's last step is not trusted; the request is handled like one without a send
    /// time.
    SimDuration future_limit{std::chrono::seconds(1)};
};

/// What the simulator has done on one network. Counters only grow.
struct CanBusSimulatorStatistics
{
    std::uint32_t bitrate = 0;          ///< The bit rate in use.
    std::size_t controllers = 0;        ///< Controllers connected now.
    std::uint64_t requests = 0;         ///< Frames the controllers asked to send.
    std::uint64_t transmissions = 0;    ///< Frames transmitted.
    std::uint64_t untagged_requests = 0; ///< Requests without a usable send time (not deterministic).
    std::uint64_t rejected_requests = 0; ///< Requests dropped: not a classic CAN data frame, from a
                                         ///< controller that is not started or has disconnected,
                                         ///< or cancelled by the controller (Reset()).
    std::uint64_t transmit_queue_full = 0; ///< Requests refused because the controller's queue was full.
    std::uint64_t inbox_overflows = 0;     ///< Requests dropped because the inbox was full.
    std::uint64_t bitrate_conflicts = 0;   ///< SetBaudRate() calls with another rate than the one in use.
};

/// Simulates CAN networks for a SIL Kit participant with virtual time, in place of SIL Kit's
/// trivial simulation. Every controller on a simulated network switches to it (SIL Kit's
/// detailed simulation); the participants need no change.
///
/// On each network: arbitration by identifier (11-bit and 29-bit), transmission time from the
/// bit rate (frame_bits(), no stuff bits), and a transmit FIFO per controller (CanBusModel). A
/// received frame carries the end of its transmission as its timestamp; the sender gets a
/// Transmitted acknowledgement with the same timestamp and its own userContext.
///
/// Time: call step(now, step) from the participant's step handler. The step at T takes the
/// requests sent before T and decides the transmissions that start in [T, T + step). A request
/// sent at t therefore starts no earlier than the first step after t. The result depends on
/// the send times and the step size, not on the step sizes of the other participants or on
/// thread timing. The send time comes from the userContext of a vehicle-sil port
/// (encode_send_time()); a request without it (another kind of participant) gets the time of
/// the simulator's last step and is counted in untagged_requests (not deterministic).
///
/// Bit rate: the first SetBaudRate() of a controller on the network, else
/// CanBusSimulatorOptions::default_bitrate. A later, different rate is counted and reported,
/// and not used.
///
/// Controller modes, as in SIL Kit's trivial simulation: a mode change (Start(), Stop(), Sleep(),
/// Reset()) is acknowledged with a state change event at the next step. What a controller sends
/// after it told a mode other than Started is dropped and counted in rejected_requests, without
/// an acknowledgement; what it sent before still goes out. A controller the simulator heard no
/// mode of counts as started: it may have called Start() before it found the simulator, and
/// SIL Kit does not send that again. Frames reach every connected controller except the sender,
/// whatever the receiver's mode.
///
/// Reset() cancels the controller's transmit requests: its frames that were not transmitted
/// yet never reach the bus, are counted in rejected_requests, and are acknowledged as Canceled
/// (at the step that applies the cancellation: the first one that begins after the Reset()
/// arrived). The request to reset the error handling has nothing to reset.
///
/// Not modelled: stuff bits, CAN FD and remote frames (dropped and counted), error frames,
/// ACK errors (also when a controller is alone on the bus), error states, the order of a
/// controller's transmit buffers by priority (FIFO).
///
/// Lifetime: construct after the participant and before its lifecycle starts; it registers
/// the networks with SIL Kit and starts SIL Kit's network simulator. SIL Kit keeps calling into
/// the networks until the participant is destroyed; they stay valid for that time even if the
/// CanBusSimulator is destroyed first. Stop stepping it before destroying it.
///
/// A network that no controller has joined yet does nothing (SIL Kit hands the simulator the
/// means to deliver frames when the first controller appears), so a configuration may list
/// networks that are not used.
///
/// Threading: SIL Kit calls the network callbacks on its own threads. Call step() only from the
/// participant's step handler (ParticipantRuntime's): it delivers through SIL Kit's network
/// simulator, which expects to be used from that thread. statistics() may be called from any
/// thread.
class CanBusSimulator
{
public:
    enum class LogLevel
    {
        Info,    ///< The bit rate in use.
        Warning, ///< Bit rate conflicts, dropped requests, the first untagged request.
    };
    /// Receives the simulator's messages, on the step handler's thread.
    using Log = std::function<void(LogLevel level, std::string_view message)>;

    /// Simulates `networks` (CAN network names) on `participant`. Throws std::invalid_argument
    /// for an empty or repeated network name or invalid options.
    CanBusSimulator(SilKit::IParticipant& participant, const std::vector<std::string>& networks,
                    CanBusSimulatorOptions options = {}, Log log = {});
    ~CanBusSimulator();
    CanBusSimulator(const CanBusSimulator&) = delete;
    CanBusSimulator& operator=(const CanBusSimulator&) = delete;

    /// One step of the simulator at `now` with step size `step` (> 0).
    void step(SimTime now, SimDuration step);

    /// The networks, in the order given.
    [[nodiscard]] std::vector<std::string> networks() const;
    /// Statistics of `network` (std::invalid_argument if it is not simulated).
    [[nodiscard]] CanBusSimulatorStatistics statistics(std::string_view network) const;

    class Network; // implementation

private:
    std::vector<std::shared_ptr<Network>> networks_;
};

} // namespace vehicle_sil::can
