// SPDX-License-Identifier: MIT
//
// In-memory ExternalCanPort for CanBridge unit tests.
#pragma once

#include "can_test_util.hpp"
#include "vehicle_sil/vector_xl/can_bridge.hpp"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace vehicle_sil::unit {

class FakeExternalPort final : public vector_xl::ExternalCanPort
{
public:
    // --- test controls
    bool fail_start = false;
    vector_xl::TransmitResult result = vector_xl::TransmitResult::Ok;
    /// Behave like a driver with TX receipts on: report every transmitted frame back as
    /// OwnTransmission.
    bool report_own_transmissions = false;

    void deliver(vector_xl::ExternalEventKind kind, const SilKit::Services::Can::CanFrame& frame = {})
    {
        EventHandler handler;
        {
            const std::lock_guard lock(mutex_);
            handler = handler_;
        }
        if (handler)
        {
            handler(vector_xl::ExternalCanEvent{kind, frame});
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

    vector_xl::TransmitResult transmit(const SilKit::Services::Can::CanFrame& frame) override
    {
        {
            const std::lock_guard lock(mutex_);
            if (result != vector_xl::TransmitResult::Ok)
            {
                return result;
            }
            transmitted_.push_back(record(frame));
        }
        if (report_own_transmissions)
        {
            deliver(vector_xl::ExternalEventKind::OwnTransmission, frame);
        }
        return vector_xl::TransmitResult::Ok;
    }

    std::string last_error() const override { return "fake external error"; }

private:
    mutable std::mutex mutex_;
    EventHandler handler_;
    std::vector<RecordedFrame> transmitted_;
    int stop_calls_ = 0;
};

} // namespace vehicle_sil::unit
