// SPDX-License-Identifier: MIT
#include "vehicle_sil/ecu/detail/bus_connection.hpp"

#include <string>

namespace vehicle_sil::detail {

void BusConnection::begin_step(SimTime now)
{
    const std::lock_guard lock(mutex_);
    now_ = now;
}

bool BusConnection::connected() const
{
    const std::lock_guard lock(mutex_);
    return current_;
}

bool BusConnection::connected_for(SimTime time) const
{
    const std::lock_guard lock(mutex_);
    return changed_at_ && time <= *changed_at_ ? previous_ : current_;
}

void BusConnection::declare(control::Agent& agent)
{
    agent.input(
        std::string(kConnectedVariable), [this] { return connected(); },
        [this, &agent](bool value) {
            if (value == connected())
            {
                return;
            }
            set(value);
            agent.log(control::EventLevel::Info,
                      value ? "connected to its buses again (ecu.connected true)"
                            : "disconnected from its buses (ecu.connected false): what it sends does not leave, what the "
                              "buses carry does not reach it");
        },
        {.description = "on its buses; false: off all of them (CAN, LIN and Ethernet), its steps go on"});
}

void BusConnection::set(bool connected)
{
    bool came_back = false;
    {
        const std::lock_guard lock(mutex_);
        if (connected == current_)
        {
            return;
        }
        if (changed_at_ != now_)
        {
            // The first change in this step: what was before it stays for the events up to now.
            previous_ = current_;
            changed_at_ = now_;
        }
        current_ = connected;
        // Back from off (not off and on again within this step), once per step.
        came_back = connected && !previous_ && reconnected_at_ != now_;
        if (came_back)
        {
            reconnected_at_ = now_;
        }
    }
    if (came_back && on_reconnect_)
    {
        on_reconnect_();
    }
}

std::size_t GatedCanPort::release_before(SimTime now, can::FrameSink sink)
{
    std::size_t passed = 0;
    inner_.release_before(now, [&](const can::TimedFrame& frame) {
        if (connection_.connected_for(frame.time))
        {
            ++passed;
            sink(frame);
        }
    });
    return passed;
}

can::SendResult GatedCanPort::send(const can::TimedFrame& frame)
{
    return connection_.connected() ? inner_.send(frame) : can::SendResult::Failed;
}

std::size_t GatedEthPort::release_before(SimTime now, eth::FrameSink sink)
{
    std::size_t passed = 0;
    inner_.release_before(now, [&](const eth::TimedFrame& frame) {
        if (connection_.connected_for(frame.time))
        {
            ++passed;
            sink(frame);
        }
    });
    return passed;
}

eth::SendResult GatedEthPort::send(const eth::TimedFrame& frame)
{
    return connection_.connected() ? inner_.send(frame) : eth::SendResult::Failed;
}

void GatedLinPort::send_header(SimTime now, lin::LinId id, std::uint8_t data_length, lin::LinChecksumModel checksum_model)
{
    // Before the call: the virtual bus answers from inside it.
    const bool passes = connection_.connected();
    sent_.store(passes ? static_cast<int>(id) : kNone, std::memory_order_release);
    if (passes)
    {
        inner_.send_header(now, id, data_length, checksum_model);
    }
}

void GatedLinPort::send_frame(SimTime now, const lin::LinFrame& frame)
{
    const bool passes = connection_.connected();
    sent_.store(passes ? static_cast<int>(frame.id) : kNone, std::memory_order_release);
    if (passes)
    {
        inner_.send_frame(now, frame);
    }
}

void GatedLinPort::send_go_to_sleep(SimTime now)
{
    sent_.store(kNone, std::memory_order_release);
    if (connection_.connected())
    {
        inner_.send_go_to_sleep(now);
    }
}

void GatedLinPort::send_wakeup(SimTime now)
{
    if (connection_.connected())
    {
        inner_.send_wakeup(now);
    }
}

void GatedLinPort::on_header(SimTime time, lin::LinId id)
{
    if (listener_ != nullptr && connection_.connected_for(time))
    {
        listener_->on_header(time, id);
    }
}

void GatedLinPort::on_response(SimTime header_time, const lin::LinFrame& frame, lin::LinResultStatus status)
{
    // A master's results answer the header of its current slot: they pass if that header went
    // out, whatever the state is when they arrive (on SIL Kit possibly after a later step of the
    // master in the same slot). They arrive before the next header (LinMaster).
    const bool passes = role_ == lin::LinRole::Master ? sent_.load(std::memory_order_acquire) == static_cast<int>(frame.id)
                                                      : connection_.connected_for(header_time);
    if (listener_ != nullptr && passes)
    {
        listener_->on_response(header_time, frame, status);
    }
}

void GatedLinPort::on_go_to_sleep(SimTime time)
{
    if (listener_ != nullptr && connection_.connected_for(time))
    {
        listener_->on_go_to_sleep(time);
    }
}

void GatedLinPort::on_wakeup(SimTime time)
{
    if (listener_ != nullptr && connection_.connected_for(time))
    {
        listener_->on_wakeup(time);
    }
}

void announce_again(j1939::Network& network)
{
    if (network.on_reconnect() == j1939::OnReconnect::Nothing)
    {
        return;
    }
    for (std::size_t i = 0; i < network.node_count(); ++i)
    {
        j1939::Node& node = network.node(i);
        if (node.name() && node.address_state() != j1939::AddressState::NotStarted)
        {
            static_cast<void>(node.request(j1939::kPgnAddressClaimed, j1939::kGlobalAddress));
        }
    }
}

} // namespace vehicle_sil::detail
