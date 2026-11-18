// SPDX-License-Identifier: MIT
//
// In-memory ExternalCanPort for CanBridge unit tests.
#pragma once

#include "can_test_util.hpp"
#include "vehicle_sil/can_bridge/can_bridge.hpp"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace vehicle_sil::unit {

class FakeExternalPort final : public can_bridge::ExternalCanPort
{
public:
    // --- test controls
    bool fail_start = false;
    can_bridge::TransmitResult result = can_bridge::TransmitResult::Ok;
    /// Behave like a driver with TX receipts on: report every transmitted frame back as
    /// OwnTransmission.
    bool report_own_transmissions = false;

    void deliver(can_bridge::ExternalEventKind kind, const SilKit::Services::Can::CanFrame& frame = {})
    {
        deliver_event(can_bridge::ExternalCanEvent{kind, frame, {}});
    }

    void deliver_event(const can_bridge::ExternalCanEvent& event)
    {
        EventHandler handler;
        {
            const std::lock_guard lock(mutex_);
            handler = handler_;
        }
        if (handler)
        {
            handler(event);
        }
    }

    std::vector<RecordedFrame> transmitted() const
    {
        const std::lock_guard lock(mutex_);
        return transmitted_;
    }

    bool started() const
    {
        const std::lock_guard lock(mutex_);
        return static_cast<bool>(handler_);
    }

    int stop_calls() const
    {
        const std::lock_guard lock(mutex_);
        return stop_calls_;
    }

    // --- ExternalCanPort
    void start(EventHandler handler) override
    {
        if (fail_start)
        {
            throw std::runtime_error("fake external start failure");
        }
        const std::lock_guard lock(mutex_);
        handler_ = std::move(handler);
    }

    void stop() noexcept override
    {
        const std::lock_guard lock(mutex_);
        handler_ = nullptr;
        ++stop_calls_;
    }

    can_bridge::TransmitResult transmit(const SilKit::Services::Can::CanFrame& frame) override
    {
        {
            const std::lock_guard lock(mutex_);
            if (result != can_bridge::TransmitResult::Ok)
            {
                return result;
            }
            transmitted_.push_back(record(frame));
        }
        if (report_own_transmissions)
        {
            deliver(can_bridge::ExternalEventKind::OwnTransmission, frame);
        }
        return can_bridge::TransmitResult::Ok;
    }

    std::string last_error() const override { return "fake external error"; }

private:
    mutable std::mutex mutex_;
    EventHandler handler_;
    std::vector<RecordedFrame> transmitted_;
    int stop_calls_ = 0;
};

} // namespace vehicle_sil::unit
