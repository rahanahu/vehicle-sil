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
#include <cstddef>
#include <cstdint>
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
/// its constructor and removes them in its destructor; destroy it before the participant.
///
/// Threading: SIL Kit calls the listener on its I/O thread, which also runs the steps; the LIN
/// controller is not thread safe, so call the port only from the steps (and respond() from
/// on_header()). set_listener() only while no events arrive (before the run, after it).
class SilKitLinPort final : public LinPort
{
public:
    SilKitLinPort(SilKit::Services::Lin::ILinController& controller, LinRole role);
    ~SilKitLinPort() override;
    SilKitLinPort(const SilKitLinPort&) = delete;
    SilKitLinPort& operator=(const SilKitLinPort&) = delete;

    [[nodiscard]] LinRole role() const noexcept { return role_; }

    void set_listener(LinPortListener* listener) override;
    void open() override;
    void send_header(SimTime now, LinId id, std::uint8_t data_length, LinChecksumModel checksum_model) override;
    void send_frame(SimTime now, const LinFrame& frame) override;
    void send_go_to_sleep(SimTime now) override;
    void respond(SimTime header_time, const LinFrame& frame) override;
    void send_wakeup(SimTime now) override;
    void resume() override;

private:
    void require_role(LinRole role, const char* call) const;
    /// std::invalid_argument unless `id` and `data_length` make a LIN frame (for `call`).
    static void require_frame(LinId id, std::size_t data_length, const char* call);
    void remove_handlers() noexcept;
    void on_frame_status(const SilKit::Services::Lin::LinFrameStatusEvent& event);

    SilKit::Services::Lin::ILinController& controller_;
    const LinRole role_;
    std::atomic<LinPortListener*> listener_{nullptr};
    /// Master: identifiers configured for reception in the controller.
    std::bitset<kMaxLinId + 1> configured_;
    std::optional<SilKit::Util::HandlerId> header_handler_;
    std::optional<SilKit::Util::HandlerId> status_handler_;
    std::optional<SilKit::Util::HandlerId> sleep_handler_;
    std::optional<SilKit::Util::HandlerId> wakeup_handler_;
};

} // namespace vehicle_sil::lin
