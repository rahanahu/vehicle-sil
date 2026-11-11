// SPDX-License-Identifier: MIT
//
// A LIN bus in this process: the ports of a master and its slaves, delivered synchronously
// (vehicle_sil::lin, no SIL Kit).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/lin/lin_port.hpp"
#include "vehicle_sil/lin/lin_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil::lin {

/// What happened on a VirtualLinBus, for its observers.
struct LinBusEvent
{
    enum class Kind
    {
        Frame,      ///< One response to a header: the master's own frame or a slave's response.
        NoResponse, ///< Nobody responded to a header.
        Collision,  ///< Two or more responses to one header.
        GoToSleep,  ///< The master's go-to-sleep command.
        Wakeup,     ///< A wakeup pulse.
    };
    Kind kind = Kind::Frame;
    SimTime time{};
    /// The name of the endpoint that sent it: for Frame the responder (the master for its own
    /// frame), for NoResponse, Collision and GoToSleep the master, for Wakeup the pulse's
    /// sender. Valid during the observer's call.
    std::string_view sender;
    /// Frame: the response as it was sent (also when its length or checksum model differs from
    /// the header's; the master reports that as an error). NoResponse and Collision: only `id`.
    /// GoToSleep and Wakeup: empty.
    LinFrame frame{};
};

/// A LIN bus in this process: one master and any number of slaves, each with an endpoint
/// (a LinPort). LocalBench makes one per LIN network name; it can also be used alone.
///
/// Everything is delivered synchronously, inside the sender's call, to the endpoints in the
/// order they were attached; the bus has no thread and no queue.
/// - send_header(now, id, length, checksum): every slave's on_header(now, id); the responses
///   they give with respond() go to the master: none is one on_response(NoResponse), each
///   response is one on_response(), Ok if its length and checksum model match the header's,
///   else Error. Several responses are several calls (LinMaster makes the collision an error).
///   The observers see one event: Frame, NoResponse or Collision.
/// - send_frame(now, frame): every slave's on_header(now, frame.id). Without a response the
///   master gets on_response(Ok, frame) and every slave on_response(now, frame, Ok); with one,
///   it is a collision: the master gets its own frame and each response (two or more calls),
///   the slaves get nothing.
/// - send_go_to_sleep(now): every slave's on_go_to_sleep(now). send_wakeup(now): on_wakeup(now)
///   of every endpoint but the sender. open() and resume() do nothing.
/// - Responses of slaves are not delivered to other slaves.
///
/// Determinism: vehicle_sil::can::StepDriver runs the steps in the order (start time,
/// registration order), so a step at T runs after every step before T of every host. LinMaster
/// and LinSlave use only the values set at a step strictly before a header's time and the events
/// stamped before the current step, so the order of the steps at one time does not change the
/// result. That is the same rule as on SIL Kit, so the results are the same.
///
/// Errors (std::logic_error, naming the endpoint and what to do instead): a second master, a
/// function of the other role (a slave's send_header(), a master's respond()), respond()
/// outside the slave's own on_header() or a second time in one on_header(), and a send_*()
/// from inside a listener or an observer (the bus delivers one thing at a time).
/// std::invalid_argument: send_header() or send_frame() with an identifier above 63 or a length
/// outside 1..8, a response with another identifier than the header's or an invalid length.
/// An exception from a listener or an observer reaches the sender's call; what the bus
/// delivered before it stays delivered (the endpoints and observers after it get nothing of
/// that event), and the bus can be used again.
///
/// Ownership: the bus owns its endpoints; references to them are valid as long as the bus.
/// Endpoints cannot be detached: a bus keeps its master for its lifetime, also after the
/// master's node is destroyed. Detach the listeners (destroy the nodes) before the bus.
///
/// Not thread safe: call it from one thread (LocalBench: the thread that runs the steps).
class VirtualLinBus
{
public:
    /// An endpoint of the bus: the port of one node.
    class Endpoint final : public LinPort
    {
    public:
        Endpoint(const Endpoint&) = delete;
        Endpoint& operator=(const Endpoint&) = delete;

        [[nodiscard]] const std::string& name() const noexcept { return name_; }
        [[nodiscard]] LinRole role() const noexcept { return role_; }

        void set_listener(LinPortListener* listener) override { listener_ = listener; }
        void open() override {}
        void send_header(SimTime now, LinId id, std::uint8_t data_length, LinChecksumModel checksum_model) override;
        void send_frame(SimTime now, const LinFrame& frame) override;
        void send_go_to_sleep(SimTime now) override;
        void respond(SimTime header_time, const LinFrame& frame) override;
        void send_wakeup(SimTime now) override;
        void resume() override {}

    private:
        friend class VirtualLinBus;
        Endpoint(VirtualLinBus& bus, LinRole role, std::string name) :
          bus_(bus), role_(role), name_(std::move(name))
        {
        }
        /// std::logic_error unless this endpoint has `role`.
        void require_role(LinRole role, const char* call) const;
        /// std::invalid_argument unless `id` and `data_length` make a LIN frame (for `call`).
        void require_frame(LinId id, std::size_t data_length, const char* call) const;

        VirtualLinBus& bus_;
        LinRole role_;
        std::string name_;
        LinPortListener* listener_ = nullptr;
    };

    VirtualLinBus() = default;
    ~VirtualLinBus();
    VirtualLinBus(const VirtualLinBus&) = delete;
    VirtualLinBus& operator=(const VirtualLinBus&) = delete;

    /// A new endpoint with `role`, named `name` (for messages and observers). std::logic_error
    /// for a second master (naming both) and during a delivery.
    Endpoint& attach(LinRole role, std::string name);
    /// `observer` sees every event after the endpoints got it. std::invalid_argument for an
    /// empty function, std::logic_error during a delivery.
    void add_observer(std::function<void(const LinBusEvent&)> observer);

private:
    struct Response
    {
        const Endpoint* slave;
        LinFrame frame;
    };
    /// Marks the bus busy for one delivery (std::logic_error if it is already).
    class Delivery;

    /// Every slave's on_header(now, id); returns their responses.
    std::vector<Response> collect_responses(SimTime now, LinId id);
    void notify(LinBusEvent::Kind kind, SimTime time, std::string_view sender, const LinFrame& frame);
    void deliver_header(const Endpoint& master, SimTime now, LinId id, std::uint8_t data_length,
                        LinChecksumModel checksum_model);
    void deliver_frame(const Endpoint& master, SimTime now, const LinFrame& frame);
    void deliver_go_to_sleep(const Endpoint& master, SimTime now);
    void deliver_wakeup(const Endpoint& sender, SimTime now);
    void accept_response(const Endpoint& slave, SimTime header_time, const LinFrame& frame);

    std::vector<std::unique_ptr<Endpoint>> endpoints_;
    const Endpoint* master_ = nullptr;
    std::vector<std::function<void(const LinBusEvent&)>> observers_;
    /// The call being delivered (nullptr: none); a second one is refused.
    const char* delivering_ = nullptr;
    /// During a slave's on_header(): that slave, the header and the responses so far.
    const Endpoint* responding_ = nullptr;
    SimTime header_time_{};
    LinId header_id_ = 0;
    std::vector<Response> responses_;
};

} // namespace vehicle_sil::lin
