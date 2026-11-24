// SPDX-License-Identifier: MIT
//
// The control server's audit log (internal).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace vehicle_sil::control_grpc::detail {

/// A text for the audit log: no line breaks or other control characters from the caller.
std::string clean(std::string text);

/// At most `max` bytes of `text`, not cut in the middle of a UTF-8 sequence.
std::string cut_utf8(std::string text, std::size_t max);

/// The audit log (ServerOptions::audit): cleaned lines of bounded length, and refused calls
/// thinned out, since anyone who reaches the port can make those (a flood must not fill the disk
/// or slow the other calls down). `prefix` comes before every line ("[run <ID>] ": one file can
/// hold the lines of several runs).
class AuditLog
{
public:
    static constexpr std::size_t kMaxLine = 1000;
    static constexpr int kRefusedPerSecond = 20;

    explicit AuditLog(std::function<void(const std::string&)> sink, std::string prefix = {},
                      std::function<std::chrono::steady_clock::time_point()> clock = {}) :
      sink_(std::move(sink)), prefix_(std::move(prefix)), clock_(std::move(clock))
    {
    }
    AuditLog(const AuditLog&) = delete;
    AuditLog& operator=(const AuditLog&) = delete;

    void operator()(std::string text) const
    {
        if (sink_)
        {
            write(std::move(text));
        }
    }

    void refused(std::string text) const
    {
        if (!sink_)
        {
            return;
        }
        std::uint64_t left_out = 0;
        {
            const std::lock_guard lock(mutex_);
            const auto now = clock_ ? clock_() : std::chrono::steady_clock::now();
            if (now - window_ >= std::chrono::seconds(1))
            {
                window_ = now;
                in_window_ = 0;
                left_out = std::exchange(left_out_, 0);
            }
            if (in_window_ >= kRefusedPerSecond)
            {
                ++left_out_;
                return;
            }
            ++in_window_;
        }
        if (left_out > 0)
        {
            write("(" + std::to_string(left_out) + " more refused calls not logged)");
        }
        write(std::move(text));
    }

private:
    void write(std::string text) const
    {
        text = clean(prefix_ + text);
        if (text.size() > kMaxLine)
        {
            text = cut_utf8(std::move(text), kMaxLine) + "...";
        }
        try
        {
            sink_(text);
        }
        catch (const std::exception&)
        {
            // A failing log must not end a call (or a gRPC callback) halfway.
        }
    }

    std::function<void(const std::string&)> sink_;
    std::string prefix_;
    std::function<std::chrono::steady_clock::time_point()> clock_;
    mutable std::mutex mutex_;
    mutable std::chrono::steady_clock::time_point window_{};
    mutable int in_window_ = 0;
    mutable std::uint64_t left_out_ = 0;
};

} // namespace vehicle_sil::control_grpc::detail
