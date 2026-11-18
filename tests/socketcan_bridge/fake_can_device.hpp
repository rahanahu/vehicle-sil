// SPDX-License-Identifier: MIT
//
// A CanDevice that plays what the kernel does, for the tests of SocketCanPort: frames and errors
// that the socket gives, the errno of a write, the interface as the kernel tells it.
#pragma once

#include "vehicle_sil/socketcan/can_device.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace vehicle_sil::unit {

class FakeCanDevice final : public socketcan::CanDevice
{
public:
    // --- test controls
    bool fail_open = false;
    /// Called inside write(), before it returns (the kernel gives a receipt back inside send() on vcan).
    std::function<void()> on_write;
    /// wait() says "readable" at once, though there is nothing to read (a flag of the socket that stays set).
    std::atomic<bool> spurious_readable{false};
    std::atomic<std::uint64_t> waits{0};

    void push_frame(const socketcan::RawFrame& frame)
    {
        socketcan::ReadResult result;
        result.status = socketcan::ReadStatus::Frame;
        result.frame = frame;
        push(result);
    }
    void push_error(int error)
    {
        socketcan::ReadResult result;
        result.status = socketcan::ReadStatus::Error;
        result.error = error;
        push(result);
    }
    /// The errno that write() returns from now on (0: success).
    void set_write_error(int error)
    {
        const std::lock_guard lock(mutex_);
        write_error_ = error;
    }
    void set_interface(std::optional<socketcan::CanInterfaceInfo> info)
    {
        const std::lock_guard lock(mutex_);
        info_ = std::move(info);
    }
    void set_up(bool up)
    {
        const std::lock_guard lock(mutex_);
        if (info_)
        {
            info_->up = up;
        }
    }
    std::vector<socketcan::RawFrame> written() const
    {
        const std::lock_guard lock(mutex_);
        return written_;
    }
    bool closed() const
    {
        const std::lock_guard lock(mutex_);
        return closed_;
    }

    // --- socketcan::CanDevice
    const std::string& name() const noexcept override { return name_; }
    void open() override
    {
        const std::lock_guard lock(mutex_);
        if (fail_open)
        {
            throw std::runtime_error("cannot open the fake interface");
        }
        closed_ = false;
    }
    socketcan::WaitResult wait(std::chrono::milliseconds timeout) override
    {
        ++waits;
        if (spurious_readable.load())
        {
            return socketcan::WaitResult::Readable;
        }
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return woken_ || !inbox_.empty(); });
        if (woken_)
        {
            woken_ = false;
            return socketcan::WaitResult::Woken;
        }
        return inbox_.empty() ? socketcan::WaitResult::Timeout : socketcan::WaitResult::Readable;
    }
    socketcan::ReadResult read() override
    {
        const std::lock_guard lock(mutex_);
        if (inbox_.empty())
        {
            return {};
        }
        auto result = inbox_.front();
        inbox_.pop_front();
        return result;
    }
    int write(const socketcan::RawFrame& frame) override
    {
        if (on_write)
        {
            on_write();
        }
        const std::lock_guard lock(mutex_);
        if (write_error_ == 0)
        {
            written_.push_back(frame);
        }
        return write_error_;
    }
    std::optional<socketcan::CanInterfaceInfo> interface() override
    {
        const std::lock_guard lock(mutex_);
        return info_;
    }
    void wake() noexcept override
    {
        {
            const std::lock_guard lock(mutex_);
            woken_ = true;
        }
        cv_.notify_all();
    }
    void close() noexcept override
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
    }

private:
    void push(const socketcan::ReadResult& result)
    {
        {
            const std::lock_guard lock(mutex_);
            inbox_.push_back(result);
        }
        cv_.notify_all();
    }

    const std::string name_ = "vcan0";
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<socketcan::ReadResult> inbox_;
    std::vector<socketcan::RawFrame> written_;
    int write_error_ = 0;
    bool woken_ = false;
    bool closed_ = true;
    std::optional<socketcan::CanInterfaceInfo> info_ = [] {
        socketcan::CanInterfaceInfo info;
        info.name = "vcan0";
        info.index = 3;
        info.kind = "vcan";
        info.up = true;
        return info;
    }();
};

} // namespace vehicle_sil::unit
