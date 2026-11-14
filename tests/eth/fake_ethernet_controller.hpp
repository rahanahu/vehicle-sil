// SPDX-License-Identifier: MIT
//
// In-memory stand-in for SilKit::Services::Ethernet::IEthernetController. Like SIL Kit, it
// invokes handlers while holding a (recursive) mutex, so removing a handler waits for a running
// call, and like SIL Kit without a network simulator it acknowledges a frame inside SendFrame().
#pragma once

#include <silkit/services/ethernet/IEthernetController.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace vehicle_sil::unit {

class FakeEthernetController final : public SilKit::Services::Ethernet::IEthernetController
{
public:
    using EthernetState = SilKit::Services::Ethernet::EthernetState;
    using EthernetTransmitStatus = SilKit::Services::Ethernet::EthernetTransmitStatus;
    using HandlerId = SilKit::Util::HandlerId;

    // --- test controls
    bool throw_on_activate = false;
    bool throw_on_send = false;
    /// Status of the transmit acknowledgement SendFrame() reports.
    EthernetTransmitStatus ack_status = EthernetTransmitStatus::Transmitted;
    /// Timestamp of the frames sent and of their acknowledgements.
    std::chrono::nanoseconds now{0};

    /// Calls the frame handlers with a received frame.
    void deliver(const std::vector<std::uint8_t>& bytes, std::chrono::nanoseconds timestamp)
    {
        const std::lock_guard lock(mutex_);
        SilKit::Services::Ethernet::EthernetFrameEvent event{};
        event.timestamp = timestamp;
        event.frame.raw = SilKit::Util::Span<const std::uint8_t>(bytes.data(), bytes.size());
        event.direction = SilKit::Services::TransmitDirection::RX;
        event.userContext = nullptr;
        for (const auto& [id, handler] : frame_handlers_)
        {
            handler(this, event);
        }
    }

    [[nodiscard]] bool active() const
    {
        const std::lock_guard lock(mutex_);
        return state_ != EthernetState::Inactive;
    }
    [[nodiscard]] std::size_t handler_count() const
    {
        const std::lock_guard lock(mutex_);
        return frame_handlers_.size() + transmit_handlers_.size() + state_handlers_.size();
    }
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> sent() const
    {
        const std::lock_guard lock(mutex_);
        return sent_;
    }

    // --- IEthernetController
    void Activate() override
    {
        const std::lock_guard lock(mutex_);
        if (throw_on_activate)
        {
            throw std::runtime_error("FakeEthernetController: Activate failed");
        }
        set_state(EthernetState::LinkUp);
    }
    void Deactivate() override
    {
        const std::lock_guard lock(mutex_);
        set_state(EthernetState::Inactive);
    }
    HandlerId AddFrameHandler(FrameHandler handler, SilKit::Services::DirectionMask directionMask) override
    {
        const std::lock_guard lock(mutex_);
        if (directionMask != static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX))
        {
            throw std::logic_error("FakeEthernetController: only RX handlers are expected");
        }
        const auto id = next_id();
        frame_handlers_[id] = std::move(handler);
        return id;
    }
    void RemoveFrameHandler(HandlerId id) override
    {
        const std::lock_guard lock(mutex_);
        frame_handlers_.erase(id);
    }
    HandlerId AddFrameTransmitHandler(FrameTransmitHandler handler,
                                      SilKit::Services::Ethernet::EthernetTransmitStatusMask) override
    {
        const std::lock_guard lock(mutex_);
        const auto id = next_id();
        transmit_handlers_[id] = std::move(handler);
        return id;
    }
    void RemoveFrameTransmitHandler(HandlerId id) override
    {
        const std::lock_guard lock(mutex_);
        transmit_handlers_.erase(id);
    }
    HandlerId AddStateChangeHandler(StateChangeHandler handler) override
    {
        const std::lock_guard lock(mutex_);
        const auto id = next_id();
        state_handlers_[id] = std::move(handler);
        return id;
    }
    void RemoveStateChangeHandler(HandlerId id) override
    {
        const std::lock_guard lock(mutex_);
        state_handlers_.erase(id);
    }
    HandlerId AddBitrateChangeHandler(BitrateChangeHandler) override { return next_id(); }
    void RemoveBitrateChangeHandler(HandlerId) override {}
    void SendFrame(SilKit::Services::Ethernet::EthernetFrame msg, void* userContext) override
    {
        const std::lock_guard lock(mutex_);
        if (throw_on_send)
        {
            throw std::runtime_error("FakeEthernetController: SendFrame failed");
        }
        sent_.emplace_back(msg.raw.begin(), msg.raw.end());
        const SilKit::Services::Ethernet::EthernetFrameTransmitEvent ack{now, ack_status, userContext};
        for (const auto& [id, handler] : transmit_handlers_)
        {
            handler(this, ack);
        }
    }

private:
    HandlerId next_id() { return static_cast<HandlerId>(++last_id_); }
    void set_state(EthernetState state)
    {
        if (state == state_)
        {
            return;
        }
        state_ = state;
        const SilKit::Services::Ethernet::EthernetStateChangeEvent event{now, state};
        for (const auto& [id, handler] : state_handlers_)
        {
            handler(this, event);
        }
    }

    mutable std::recursive_mutex mutex_;
    std::uint64_t last_id_ = 0;
    EthernetState state_ = EthernetState::Inactive;
    std::map<HandlerId, FrameHandler> frame_handlers_;
    std::map<HandlerId, FrameTransmitHandler> transmit_handlers_;
    std::map<HandlerId, StateChangeHandler> state_handlers_;
    std::vector<std::vector<std::uint8_t>> sent_;
};

} // namespace vehicle_sil::unit
