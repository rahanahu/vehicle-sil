// SPDX-License-Identifier: MIT
//
// The port through which a LIN master or slave reaches its bus (vehicle_sil::lin, no SIL Kit).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/lin/lin_types.hpp"

#include <cstdint>

namespace vehicle_sil::lin {

/// What a LIN port reports to its node (LinMaster and LinSlave implement it).
///
/// - on_header(): slave ports only. Called for every header the master sends, also for the
///   frames the master answers itself. The slave may call LinPort::respond() from inside it.
/// - on_response() at a master port: what became of a header of this master: its own frame went
///   out (Ok), a slave's response (Ok with the data), no response (NoResponse; may be called
///   from inside send_header()), or a response whose length or checksum model differs from the
///   header's (Error). One header may get two or more of them: several responses, which collide
///   (LinMaster decides).
/// - on_response() at a slave port: frames the master answered itself (Ok), stamped with the
///   header's time. Not the go-to-sleep command, not the slave's own responses. Responses of
///   other slaves are not supported: their arrival is not bound to a step of the sender. The
///   virtual bus never delivers them; the SIL Kit port cannot tell them apart from the master's
///   frames and passes them on.
/// - on_go_to_sleep(): slave ports only, for the master's go-to-sleep command.
/// - on_wakeup(): a wakeup pulse of another node (never the node's own).
///
/// Times are the bus times of the events. A port calls its listener on its own thread (SIL
/// Kit: the I/O thread that also runs the steps; virtual bus: inside the sender's call), so the
/// listener guards what it shares with its step.
class LinPortListener
{
public:
    virtual void on_header(SimTime time, LinId id) = 0;
    virtual void on_response(SimTime header_time, const LinFrame& frame, LinResultStatus status) = 0;
    virtual void on_go_to_sleep(SimTime time) = 0;
    virtual void on_wakeup(SimTime time) = 0;

protected:
    ~LinPortListener() = default;
};

/// A node's access to a LIN bus: the only way LinMaster and LinSlave reach it. The role
/// (master or slave) is fixed when the port is made; calling a function of the other role is
/// std::logic_error. Implementations: VirtualLinBus::Endpoint (in this process) and
/// SilKitLinPort (vehicle_sil::lin_silkit, a SIL Kit LIN controller).
///
/// `now` is the time of the step that calls the function. The SIL Kit port ignores it: SIL Kit
/// stamps the participant's time.
///
/// Arguments: send_header() and send_frame() with an identifier above 63 or a length outside
/// 1..8 are std::invalid_argument (both implementations; LinMaster never sends them).
///
/// Threading: a port is not thread safe. Call it from the node's step, and respond() from
/// on_header(). set_listener() only while no events can arrive (before the run, after it).
class LinPort
{
public:
    virtual ~LinPort() = default;

    /// The node that receives the port's events; nullptr detaches it. The listener must
    /// outlive the port or be detached first.
    virtual void set_listener(LinPortListener* listener) = 0;
    /// Makes the port ready for traffic (LinMaster::init(), LinSlave::init()).
    virtual void open() = 0;

    // Master.
    /// Sends the header of a frame a slave answers. Its responses arrive at on_response(),
    /// compared with `data_length` and `checksum_model`.
    virtual void send_header(SimTime now, LinId id, std::uint8_t data_length, LinChecksumModel checksum_model) = 0;
    /// Sends the header of `frame` and `frame` itself as the master's response.
    virtual void send_frame(SimTime now, const LinFrame& frame) = 0;
    /// Sends the go-to-sleep command.
    virtual void send_go_to_sleep(SimTime now) = 0;

    // Slave.
    /// Answers the header of `header_time` with `frame` (its identifier is the header's). Only
    /// from inside on_header().
    virtual void respond(SimTime header_time, const LinFrame& frame) = 0;

    // Both.
    /// Sends a wakeup pulse.
    virtual void send_wakeup(SimTime now) = 0;
    /// The node woke up from another node's pulse: makes the port operational again, puts
    /// nothing on the bus.
    virtual void resume() = 0;

protected:
    LinPort() = default;
    LinPort(const LinPort&) = default;
    LinPort& operator=(const LinPort&) = default;
};

} // namespace vehicle_sil::lin
