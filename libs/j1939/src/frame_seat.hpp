// SPDX-License-Identifier: MIT
//
// The listener's seat: the one internal place in a Network's receive path from which a
// Listener hears what the network receives. Not installed and not an extension point for
// users; see docs/internals/j1939.md ("聞き役の席（内部の口）").
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/j1939/types.hpp"

namespace vehicle_sil::j1939 {

class Network;

namespace detail {

/// What sits in a Network's seat. A network has one seat (Network::set_seat()); the functions
/// are called synchronously inside Network::process() (on_message() also when a message is
/// delivered in the application stage), in the order the network handles things.
///
/// Read only: calling into the network or its nodes from one of these functions (sending,
/// registering a handler, adding a node, starting a Timer, ...) throws std::logic_error and
/// fails the network. An exception that leaves one of them fails the network too. A function
/// that makes the network fail without letting an exception out (it caught the std::logic_error,
/// or it removed a handler or cancelled or destroyed a Timer, which cannot throw) stops the step
/// in the same way: the network throws std::logic_error when the function returns, and handles
/// nothing more of the frame, the message or the step. References and spans are valid only
/// during the call.
class FrameSeat
{
public:
    FrameSeat() = default;
    FrameSeat(const FrameSeat&) = delete;
    FrameSeat& operator=(const FrameSeat&) = delete;

    /// A frame the network took (29-bit, valid, from a source it accepts, within
    /// NetworkOptions::receive_capacity), in the order the network handles the step's frames and
    /// before it handles this one: TP.CM and TP.DT and frames to other addresses included.
    /// `time`: the frame's time. `local`: sent by a node of this network (the loopback, which
    /// exists only when the network has more than one node).
    virtual void on_frame(SimTime time, const can::Frame& frame, bool local) = 0;

    /// A message handed to the nodes (a single frame, or a message a transfer delivered), before
    /// any handler runs: the same moment and content as the Observer's MessageReceived, after
    /// the observer. `local`: sent by a node of this network.
    virtual void on_message(const Message& message, bool local) = 0;

    /// Once per step that reaches this point without failing: the step's frames are handled
    /// (all on_frame() and their on_message() calls are done), the timers due at the step time
    /// have not fired yet. `now`: the step time.
    virtual void on_step_end(SimTime now) = 0;

protected:
    /// Takes the seat of `network`: Network::set_seat(this), with its conditions (only outside a
    /// step; std::logic_error if the seat is taken or this one sits in another network).
    void sit(Network& network);
    /// Leaves the seat it is in, if any: only outside a step (std::logic_error), as sit().
    void leave();
    /// Leaves the seat. Unlike leave() also inside a step (a destructor cannot refuse), from a
    /// node's handler, a timer or the application stage: the network calls nothing afterwards.
    /// Not from inside one of this object's own functions: that function would go on running in
    /// a destroyed object, which is undefined behaviour and which the network cannot notice.
    virtual ~FrameSeat();

private:
    friend class vehicle_sil::j1939::Network;
    Network* network_ = nullptr; ///< The network whose seat this is in.
};

} // namespace detail
} // namespace vehicle_sil::j1939
