// SPDX-License-Identifier: MIT
//
// vehicle_sil::lin port on a SIL Kit LIN controller (vehicle_sil::lin_silkit).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/lin/lin_port.hpp"
#include "vehicle_sil/lin/lin_types.hpp"

#include <silkit/services/lin/ILinController.hpp>

#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace vehicle_sil::lin {

/// A LinPort on a SIL Kit LIN controller (trivial simulation, virtual time), for a LinMaster or
/// a LinSlave of a SIL Kit participant. Peer makes one per Ecu::lin_master() and
/// Ecu::lin_slave().
///
/// - Master: open() initialises the controller as master. send_header() configures the
///   identifier for reception (once, with the length and checksum model of the header) and
///   sends the header; send_frame() sends a master response, send_go_to_sleep() the go-to-sleep
///   command (the controller sleeps), send_wakeup() a pulse (the controller is operational
///   again), resume() makes the controller operational without a pulse. The frame statuses of
///   the controller become on_response() (received and sent Ok, no response, errors).
/// - Slave: open() initialises the controller for dynamic responses (experimental API in SIL
///   Kit v5.0.8): every header becomes on_header(), and respond() answers it. The controller
///   stays operational: send_wakeup() sends the pulse (switching the controller to sleep just
///   before, as SIL Kit wants), resume() does nothing. Received frames become on_response(),
///   except the go-to-sleep command (on_go_to_sleep()) and the slave's own responses; SIL Kit
///   does not tell other slaves' responses apart from the master's frames, so they are passed
///   on too (not supported: see LinPortListener).
/// - Both: wakeup pulses of other nodes become on_wakeup(); the node's own pulse does not.
///
/// The `now` of the send functions is ignored: SIL Kit stamps the participant's time. Calling a
/// function of the other role is std::logic_error; send_header() and send_frame() with an
/// identifier above 63 or a length outside 1..8 are std::invalid_argument. Errors of SIL Kit
/// propagate.
///
/// Ownership: the port does not own the controller. It adds its handlers to the controller in
/// its constructor (and the slave's header handler in open()) and removes them in close() (called
/// by the destructor); close or destroy it before the participant.
///
/// Threading: SIL Kit calls the listener on its I/O thread, which also runs the steps; the LIN
/// controller is not thread safe, so call the send/respond functions only from the steps (and
/// respond() from on_header()). open() (from CommunicationReady) and close() may run at the same
/// time. close() may be called from any thread except a SIL Kit handler of this controller; after
/// close() returns, no SIL Kit callback runs into the port or its listener. set_listener() only
/// while no events arrive (before the run, after it).
class SilKitLinPort final : public LinPort
{
public:
    SilKitLinPort(SilKit::Services::Lin::ILinController& controller, LinRole role);
    ~SilKitLinPort() override;
    SilKitLinPort(const SilKitLinPort&) = delete;
    SilKitLinPort& operator=(const SilKitLinPort&) = delete;

    [[nodiscard]] LinRole role() const noexcept { return role_; }

    void set_listener(LinPortListener* listener) override;
    /// Initialises the controller (and, for a slave, adds the header handler). Does nothing after
    /// close().
    void open() override;
    /// Removes the handlers, detaches the listener and waits for any in-flight callback to finish.
    /// Idempotent. Send functions after close() do nothing (role and frame checks still apply).
    void close() noexcept;
    /// False from the constructor until close().
    [[nodiscard]] bool is_closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    void send_header(SimTime now, LinId id, std::uint8_t data_length, LinChecksumModel checksum_model) override;
    void send_frame(SimTime now, const LinFrame& frame) override;
    void send_go_to_sleep(SimTime now) override;
    void respond(SimTime header_time, const LinFrame& frame) override;
    void send_wakeup(SimTime now) override;
    void resume() override;

private:
    class CallbackScope;

    void require_role(LinRole role, const char* call) const;
    /// std::invalid_argument unless `id` and `data_length` make a LIN frame (for `call`).
    static void require_frame(LinId id, std::size_t data_length, const char* call);
    void close_locked() noexcept;
    void remove_handlers() noexcept;
    [[nodiscard]] bool enter_callback() noexcept;
    void leave_callback() noexcept;
    void on_frame_status(const SilKit::Services::Lin::LinFrameStatusEvent& event);

    SilKit::Services::Lin::ILinController& controller_;
    const LinRole role_;
    std::atomic<LinPortListener*> listener_{nullptr};
    /// Master: identifiers configured for reception in the controller.
    std::bitset<kMaxLinId + 1> configured_;
    /// Serialises constructor handler setup, open() and close(). Never taken by SIL Kit callbacks
    /// (to avoid lock-order inversion with SIL Kit's internal handler mutex).
    std::mutex lifecycle_mutex_;
    std::atomic<bool> closed_{false};
    /// Leaf lock tracking in-flight SIL Kit callbacks so close() waits until none remain.
    std::mutex callback_mutex_;
    std::condition_variable callback_cv_;
    std::size_t in_flight_callbacks_{0};
    std::optional<SilKit::Util::HandlerId> header_handler_;
    std::optional<SilKit::Util::HandlerId> status_handler_;
    std::optional<SilKit::Util::HandlerId> sleep_handler_;
    std::optional<SilKit::Util::HandlerId> wakeup_handler_;
};

} // namespace vehicle_sil::lin
