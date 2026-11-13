// SPDX-License-Identifier: MIT
//
// The connection of one ECU to its buses (the input "ecu.connected" every ECU has) and the ports
// that cut it (vehicle_sil::ecu, no SIL Kit). For the Ecu implementations (Peer, LocalBench).
#pragma once

#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/lin/lin_port.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>

namespace vehicle_sil::detail {

/// The input every ECU has: true while the ECU is on its buses.
inline constexpr std::string_view kConnectedVariable = "ecu.connected";

/// Whether one ECU is on its buses (all of them: CAN and LIN). The ECU's step writes it (the
/// input kConnectedVariable, applied with the other writes before the application); the ports of
/// the ECU read it (GatedCanPort, GatedLinPort).
///
/// - What the ECU sends in a step leaves only if it is connected in that step (after the writes
///   of the step).
/// - A bus event stamped `t` (a frame, a LIN header) reaches the ECU only if it was connected
///   after the last change made in a step before `t`: a change in the step at P applies to the
///   events stamped after P. So the result does not depend on whether an event stamped P arrives
///   before or after the ECU's own step at P (SIL Kit, the order of the ECUs on LocalBench).
///   This needs at most one change per step and the events stamped up to a change to have
///   arrived before the next change (true for virtual time: what is sent at T has arrived by
///   every step after T).
///
/// Thread safe (the LIN ports report on their own thread).
class BusConnection
{
public:
    BusConnection() = default;
    BusConnection(const BusConnection&) = delete;
    BusConnection& operator=(const BusConnection&) = delete;

    /// Called by the ECU before the writes of the step at `now` (the time a change is made at).
    void begin_step(SimTime now);
    /// true while connected (for what is sent now).
    [[nodiscard]] bool connected() const;
    /// For an event stamped `time` (see the class comment).
    [[nodiscard]] bool connected_for(SimTime time) const;
    /// What the ECU does when it comes back (its J1939 networks: announce_again()). Called once
    /// in the step it comes back in, if it was off before that step (not for a write of false and
    /// then true in one step). Set before the run.
    void set_on_reconnect(std::function<void()> fn) { on_reconnect_ = std::move(fn); }
    /// Registers kConnectedVariable on `agent` (a bool input, true at the start). Its setter
    /// calls set().
    void declare(control::Agent& agent);
    /// The write of the input in the current step.
    void set(bool connected);

private:
    mutable std::mutex mutex_;
    bool current_ = true;
    bool previous_ = true; ///< before the step changed_at_
    std::optional<SimTime> changed_at_;
    std::optional<SimTime> reconnected_at_; ///< the step on_reconnect_ last ran in
    SimTime now_{};
    std::function<void()> on_reconnect_;
};

/// A CAN port of an ECU that keeps the ECU off the bus while it is not connected: send() returns
/// SendResult::Failed (nothing goes to `inner`; the stacks count it as a failed send and drop the
/// frame), and the frames received while not connected are taken from `inner` and dropped. The
/// port's events (about the ECU's own controller) pass. Use it like `inner`; `inner` and
/// `connection` must outlive it.
class GatedCanPort final : public can::Port
{
public:
    GatedCanPort(can::Port& inner, const BusConnection& connection) noexcept : inner_(inner), connection_(connection) {}

    [[nodiscard]] can::TimeDomain time_domain() const noexcept override { return inner_.time_domain(); }
    std::size_t release_before(SimTime now, can::FrameSink sink) override;
    can::SendResult send(const can::TimedFrame& frame) override;
    std::size_t release_events_before(SimTime now, can::PortEventSink sink) override
    {
        return inner_.release_events_before(now, sink);
    }
    void set_wakeup(can::Wakeup wakeup) override { inner_.set_wakeup(std::move(wakeup)); }

private:
    can::Port& inner_;
    const BusConnection& connection_;
};

/// A LIN port of an ECU that keeps the ECU off the bus while it is not connected: what the node
/// sends (headers, frames, the go-to-sleep command, wakeup pulses) does not go to `inner`, and
/// the events of the bus do not reach the node. A master then gets no response to its headers
/// (LinResultStatus::NoResponse); a slave gets no headers, so it answers none. The results of a
/// master's header that went out pass, also if the master went off in the rest of the slot (they
/// are decided by the state the header was sent with). open(), resume() and respond() (only from
/// on_header(), which passed) always pass. `inner` and `connection` must outlive it.
class GatedLinPort final : public lin::LinPort, private lin::LinPortListener
{
public:
    GatedLinPort(lin::LinPort& inner, const BusConnection& connection, lin::LinRole role) noexcept :
      inner_(inner), connection_(connection), role_(role)
    {
    }
    GatedLinPort(const GatedLinPort&) = delete;
    GatedLinPort& operator=(const GatedLinPort&) = delete;
    ~GatedLinPort() override { inner_.set_listener(nullptr); }

    void set_listener(lin::LinPortListener* listener) override
    {
        listener_ = listener;
        inner_.set_listener(listener != nullptr ? this : nullptr);
    }
    void open() override { inner_.open(); }
    void send_header(SimTime now, lin::LinId id, std::uint8_t data_length, lin::LinChecksumModel checksum_model) override;
    void send_frame(SimTime now, const lin::LinFrame& frame) override;
    void send_go_to_sleep(SimTime now) override;
    void respond(SimTime header_time, const lin::LinFrame& frame) override { inner_.respond(header_time, frame); }
    void send_wakeup(SimTime now) override;
    void resume() override { inner_.resume(); }

private:
    void on_header(SimTime time, lin::LinId id) override;
    void on_response(SimTime header_time, const lin::LinFrame& frame, lin::LinResultStatus status) override;
    void on_go_to_sleep(SimTime time) override;
    void on_wakeup(SimTime time) override;

    static constexpr int kNone = -1;

    lin::LinPort& inner_;
    const BusConnection& connection_;
    lin::LinRole role_;
    lin::LinPortListener* listener_ = nullptr;
    /// Master: the identifier of the header (or own frame) of the current slot if it went out,
    /// else kNone. Set on the step thread; read where the port reports.
    std::atomic<int> sent_{kNone};
};

/// What a J1939 network does when its ECU comes back, as its NetworkOptions::on_reconnect says
/// (j1939::OnReconnect): by default each node with a NAME that has started address claiming
/// sends a Request for Address Claimed to all nodes, which it answers itself with its Address
/// Claimed (or Cannot Claim) and the other nodes answer with theirs. The node keeps its address
/// and state; a contention is then resolved as for any claim. Call it in a step.
void announce_again(j1939::Network& network);

} // namespace vehicle_sil::detail
