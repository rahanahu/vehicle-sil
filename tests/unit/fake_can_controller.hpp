// SPDX-License-Identifier: MIT
//
// In-memory stand-in for SilKit::Services::Can::ICanController. Like SIL Kit, it invokes
// handlers while holding a (recursive) mutex, so removing a handler waits for a running call.
#pragma once

#include "can_test_util.hpp"

#include <silkit/services/can/ICanController.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace vehicle_sil::unit {

class FakeCanController final : public SilKit::Services::Can::ICanController
{
public:
    using CanFrame = SilKit::Services::Can::CanFrame;
    using CanFrameEvent = SilKit::Services::Can::CanFrameEvent;
    using HandlerId = SilKit::Util::HandlerId;

    // --- test controls
    bool throw_on_start = false;
    bool throw_on_send = false;
    /// Status of the transmit acknowledgement SendFrame() reports.
    SilKit::Services::Can::CanTransmitStatus ack_status = SilKit::Services::Can::CanTransmitStatus::Transmitted;
    /// Timestamp of the transmit acknowledgement SendFrame() reports.
    std::chrono::nanoseconds ack_timestamp{0};

    void deliver(const CanFrame& frame, std::chrono::nanoseconds timestamp = std::chrono::nanoseconds{0})
    {
        const std::lock_guard lock(mutex_);
        CanFrameEvent event{};
        event.timestamp = timestamp;
        event.frame = frame;
        event.direction = SilKit::Services::TransmitDirection::RX;
        for (const auto& [id, handler] : frame_handlers_)
        {
            handler(this, event);
        }
    }

    /// Calls the error state handlers, like a network simulator that changes the state.
    void deliver_error_state(SilKit::Services::Can::CanErrorState state,
                             std::chrono::nanoseconds timestamp = std::chrono::nanoseconds{0})
    {
        const std::lock_guard lock(mutex_);
        const SilKit::Services::Can::CanErrorStateChangeEvent event{timestamp, state};
        for (const auto& [id, handler] : error_state_handlers_)
        {
            handler(this, event);
        }
    }

    std::size_t error_state_handler_count() const
    {
        const std::lock_guard lock(mutex_);
        return error_state_handlers_.size();
    }

    std::vector<RecordedFrame> sent() const
    {
        const std::lock_guard lock(mutex_);
        return sent_;
    }

    /// The frames sent after the first `first` ones (cheaper than sent() for long recordings).
    std::vector<RecordedFrame> sent_after(std::size_t first) const
    {
        const std::lock_guard lock(mutex_);
        return first < sent_.size() ? std::vector<RecordedFrame>(sent_.begin() + static_cast<std::ptrdiff_t>(first), sent_.end())
                                    : std::vector<RecordedFrame>{};
    }

    std::size_t frame_handler_count() const
    {
        const std::lock_guard lock(mutex_);
        return frame_handlers_.size();
    }

    /// Direction masks the frame handlers were added with, in order of addition.
    std::vector<SilKit::Services::DirectionMask> frame_handler_masks() const
    {
        const std::lock_guard lock(mutex_);
        return frame_handler_masks_;
    }

    std::size_t transmit_handler_count() const
    {
        const std::lock_guard lock(mutex_);
        return transmit_handlers_.size();
    }

    bool started() const
    {
        const std::lock_guard lock(mutex_);
        return started_;
    }

    // --- ICanController
    void SetBaudRate(uint32_t, uint32_t, uint32_t) override {}
    void Reset() override {}
    void Start() override
    {
        if (throw_on_start)
        {
            throw std::runtime_error("fake Start failure");
        }
        const std::lock_guard lock(mutex_);
        started_ = true;
    }
    void Stop() override
    {
        const std::lock_guard lock(mutex_);
        started_ = false;
    }
    void Sleep() override {}

    void SendFrame(const CanFrame& msg, void* userContext = nullptr) override
    {
        if (throw_on_send)
        {
            throw std::runtime_error("fake SendFrame failure");
        }
        const std::lock_guard lock(mutex_);
        sent_.push_back(record(msg));
        SilKit::Services::Can::CanFrameTransmitEvent ack{};
        ack.canId = msg.canId;
        ack.timestamp = ack_timestamp;
        ack.status = ack_status;
        ack.userContext = userContext;
        for (const auto& [id, entry] : transmit_handlers_)
        {
            if ((entry.mask & static_cast<SilKit::Services::Can::CanTransmitStatusMask>(ack.status)) != 0)
            {
                entry.handler(this, ack);
            }
        }
    }

    auto AddFrameHandler(FrameHandler handler, SilKit::Services::DirectionMask mask) -> HandlerId override
    {
        const std::lock_guard lock(mutex_);
        frame_handler_masks_.push_back(mask);
        const auto id = next_id();
        frame_handlers_.emplace(id, std::move(handler));
        return id;
    }
    void RemoveFrameHandler(HandlerId handlerId) override
    {
        const std::lock_guard lock(mutex_);
        frame_handlers_.erase(handlerId);
    }
    auto AddStateChangeHandler(StateChangeHandler) -> HandlerId override { return next_id(); }
    void RemoveStateChangeHandler(HandlerId) override {}
    auto AddErrorStateChangeHandler(ErrorStateChangeHandler handler) -> HandlerId override
    {
        const std::lock_guard lock(mutex_);
        const auto id = next_id();
        error_state_handlers_.emplace(id, std::move(handler));
        return id;
    }
    void RemoveErrorStateChangeHandler(HandlerId handlerId) override
    {
        const std::lock_guard lock(mutex_);
        error_state_handlers_.erase(handlerId);
    }
    auto AddFrameTransmitHandler(FrameTransmitHandler handler, SilKit::Services::Can::CanTransmitStatusMask mask)
        -> HandlerId override
    {
        const std::lock_guard lock(mutex_);
        const auto id = next_id();
        transmit_handlers_.emplace(id, TransmitHandler{std::move(handler), mask});
        return id;
    }
    void RemoveFrameTransmitHandler(HandlerId handlerId) override
    {
        const std::lock_guard lock(mutex_);
        transmit_handlers_.erase(handlerId);
    }

private:
    HandlerId next_id() { return static_cast<HandlerId>(next_id_++); }

    mutable std::recursive_mutex mutex_;
    std::map<HandlerId, FrameHandler> frame_handlers_;
    std::vector<SilKit::Services::DirectionMask> frame_handler_masks_;
    struct TransmitHandler
    {
        FrameTransmitHandler handler;
        SilKit::Services::Can::CanTransmitStatusMask mask;
    };
    std::map<HandlerId, TransmitHandler> transmit_handlers_;
    std::map<HandlerId, ErrorStateChangeHandler> error_state_handlers_;
    std::vector<RecordedFrame> sent_;
    bool started_ = false;
    std::uint64_t next_id_ = 1;
};

} // namespace vehicle_sil::unit
