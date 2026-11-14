// SPDX-License-Identifier: MIT
//
// In-memory stand-in for SilKit::Services::Ethernet::IEthernetController. Like SIL Kit, it calls
// the handlers while holding a mutex, so removing a handler waits for a running call; frames
// reach only the handlers whose direction mask includes RX; a sent frame is acknowledged from
// inside SendFrame (as SIL Kit's trivial simulation does).
#pragma once

#include <silkit/services/ethernet/IEthernetController.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>

namespace vehicle_sil::unit {

class FakeEthernetController final : public SilKit::Services::Ethernet::IEthernetController
{
public:
    using Bytes = std::vector<std::uint8_t>;
    using EthernetFrameEvent = SilKit::Services::Ethernet::EthernetFrameEvent;
    using EthernetTransmitStatus = SilKit::Services::Ethernet::EthernetTransmitStatus;
    using HandlerId = SilKit::Util::HandlerId;

    // --- test controls (set before the bridge runs)
    bool throw_on_activate = false;
    bool throw_on_send = false;
    /// Status of the transmit acknowledgement SendFrame() reports.
    EthernetTransmitStatus ack_status = EthernetTransmitStatus::Transmitted;
    /// Called with every sent frame after it was recorded, outside the controller's lock (to
    /// wire controllers into a network).
    std::function<void(std::span<const std::uint8_t>)> on_send;

    /// A frame another participant sent: delivered to the RX handlers on the calling thread.
    void deliver(std::span<const std::uint8_t> frame)
    {
        const std::lock_guard lock(mutex_);
        EthernetFrameEvent event{};
        event.frame.raw = SilKit::Util::Span<const std::uint8_t>(frame.data(), frame.size());
        event.direction = SilKit::Services::TransmitDirection::RX;
        for (const auto& [id, entry] : frame_handlers_)
        {
            if ((entry.second & static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX)) != 0)
            {
                entry.first(this, event);
            }
        }
    }

    std::vector<Bytes> sent() const
    {
        const std::lock_guard lock(mutex_);
        return sent_;
    }

    std::size_t sent_count() const
    {
        const std::lock_guard lock(mutex_);
        return sent_.size();
    }

    bool active() const
    {
        const std::lock_guard lock(mutex_);
        return active_;
    }

    std::size_t frame_handler_count() const
    {
        const std::lock_guard lock(mutex_);
        return frame_handlers_.size();
    }

    std::size_t transmit_handler_count() const
    {
        const std::lock_guard lock(mutex_);
        return transmit_handlers_.size();
    }

    /// The direction mask of the only frame handler.
    SilKit::Services::DirectionMask frame_handler_mask() const
    {
        const std::lock_guard lock(mutex_);
        return frame_handlers_.empty() ? 0 : frame_handlers_.begin()->second.second;
    }

    // --- IEthernetController
    void Activate() override
    {
        if (throw_on_activate)
        {
            throw std::runtime_error("fake Activate failure");
        }
        const std::lock_guard lock(mutex_);
        active_ = true;
    }

    void Deactivate() override
    {
        const std::lock_guard lock(mutex_);
        active_ = false;
    }

    auto AddFrameHandler(FrameHandler handler, SilKit::Services::DirectionMask mask) -> HandlerId override
    {
        const std::lock_guard lock(mutex_);
        const auto id = next_id();
        frame_handlers_.emplace(id, std::make_pair(std::move(handler), mask));
        return id;
    }

    void RemoveFrameHandler(HandlerId id) override
    {
        const std::lock_guard lock(mutex_);
        frame_handlers_.erase(id);
    }

    auto AddFrameTransmitHandler(FrameTransmitHandler handler, SilKit::Services::Ethernet::EthernetTransmitStatusMask mask)
        -> HandlerId override
    {
        const std::lock_guard lock(mutex_);
        const auto id = next_id();
        transmit_handlers_.emplace(id, std::make_pair(std::move(handler), mask));
        return id;
    }

    void RemoveFrameTransmitHandler(HandlerId id) override
    {
        const std::lock_guard lock(mutex_);
        transmit_handlers_.erase(id);
    }

    auto AddStateChangeHandler(StateChangeHandler) -> HandlerId override { return next_id(); }
    void RemoveStateChangeHandler(HandlerId) override {}
    auto AddBitrateChangeHandler(BitrateChangeHandler) -> HandlerId override { return next_id(); }
    void RemoveBitrateChangeHandler(HandlerId) override {}

    void SendFrame(SilKit::Services::Ethernet::EthernetFrame msg, void* userContext = nullptr) override
    {
        if (throw_on_send)
        {
            throw std::runtime_error("fake SendFrame failure");
        }
        const Bytes frame(msg.raw.begin(), msg.raw.end());
        {
            const std::lock_guard lock(mutex_);
            sent_.push_back(frame);
            SilKit::Services::Ethernet::EthernetFrameTransmitEvent ack{};
            ack.status = ack_status;
            ack.userContext = userContext;
            for (const auto& [id, entry] : transmit_handlers_)
            {
                if ((static_cast<SilKit::Services::Ethernet::EthernetTransmitStatusMask>(ack_status) & entry.second) != 0)
                {
                    entry.first(this, ack);
                }
            }
        }
        if (on_send)
        {
            on_send(frame);
        }
    }

private:
    HandlerId next_id() { return static_cast<HandlerId>(++last_id_); }

    mutable std::mutex mutex_;
    std::map<HandlerId, std::pair<FrameHandler, SilKit::Services::DirectionMask>> frame_handlers_;
    std::map<HandlerId, std::pair<FrameTransmitHandler, SilKit::Services::Ethernet::EthernetTransmitStatusMask>>
        transmit_handlers_;
    std::vector<Bytes> sent_;
    bool active_ = false;
    std::uint64_t last_id_ = 0;
};

} // namespace vehicle_sil::unit
