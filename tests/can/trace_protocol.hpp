// SPDX-License-Identifier: MIT
//
// A small test protocol on vehicle_sil::can::Host that records a byte-exact trace: it sends
// frames on a period and answers received frames by a fixed rule. Running the same scenario on
// the virtual bus and on SIL Kit must give the same traces (backend independence).
#pragma once

#include "vehicle_sil/can/host.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace vehicle_sil::unit {

/// Node `self` (1..15) of the test protocol. Frame layout:
///   id     = 0x18000000 | kind << 8 | sender   (kind 0x20 = periodic A, 0x10 = periodic B,
///                                               0x30 + hop = answer)
///   data   = [hop, origin, sequence, checksum]
/// - Every `period` (first at T = 0) it sends two frames in one step: periodic A, then B with a
///   lower id. Their order must survive (same sender, same time).
/// - Each received frame with hop < 2 is answered in the same step with hop + 1.
/// - Frames received in one step are handled in (time, sender) order; frames of one sender keep
///   their release order (stable sort). That is the deterministic same-time order of this
///   protocol; the port only keeps arrival order.
class TraceProtocol final : public can::Steppable
{
public:
    TraceProtocol(std::uint8_t self, SimDuration period) : self_(self), period_(period)
    {
        if (self == 0 || self > 15 || period <= SimDuration::zero())
        {
            throw std::invalid_argument("TraceProtocol: bad node or period");
        }
    }

    void begin(SimTime now) override
    {
        now_ = now;
        received_.clear();
        outgoing_.clear();
    }

    void receive(const can::TimedFrame& frame) override { received_.push_back(frame); }

    void process() override
    {
        std::stable_sort(received_.begin(), received_.end(), [](const can::TimedFrame& a, const can::TimedFrame& b) {
            if (a.time != b.time)
            {
                return a.time < b.time;
            }
            return sender(a.frame) < sender(b.frame);
        });
        for (const auto& rx : received_)
        {
            if (sender(rx.frame) == self_)
            {
                throw std::logic_error("TraceProtocol: received an own frame");
            }
            if (rx.time >= now_)
            {
                throw std::logic_error("TraceProtocol: received a frame stamped at or after the step");
            }
            record("rx", rx.time, rx.frame);
            const std::uint8_t hop = rx.frame.data[0];
            if (hop < 2)
            {
                queue(static_cast<std::uint8_t>(0x30 + hop + 1), static_cast<std::uint8_t>(hop + 1), rx.frame.data[1],
                      rx.frame.data[2]);
            }
        }
        while (next_periodic_ <= now_)
        {
            queue(0x20, 0, self_, sequence_); // A
            queue(0x10, 0, self_, sequence_); // B: lower id, sent after A
            ++sequence_;
            next_periodic_ += period_;
        }
    }

    void flush(can::Port& port) override
    {
        for (const auto& frame : outgoing_)
        {
            const auto result = port.send(can::TimedFrame{now_, frame});
            if (result != can::SendResult::Accepted)
            {
                throw std::runtime_error("TraceProtocol: send was not accepted");
            }
            record("tx", now_, frame);
        }
        outgoing_.clear();
    }

    [[nodiscard]] const std::vector<std::string>& trace() const noexcept { return trace_; }

    [[nodiscard]] static std::uint8_t sender(const can::Frame& frame) noexcept { return static_cast<std::uint8_t>(frame.id & 0xFFu); }

private:
    void queue(std::uint8_t kind, std::uint8_t hop, std::uint8_t origin, std::uint8_t sequence)
    {
        can::Frame frame;
        frame.id = 0x1800'0000u | (static_cast<std::uint32_t>(kind) << 8) | self_;
        frame.length = 4;
        frame.data[0] = hop;
        frame.data[1] = origin;
        frame.data[2] = sequence;
        frame.data[3] = static_cast<std::uint8_t>(hop * 31 + origin * 7 + sequence + self_);
        outgoing_.push_back(frame);
    }

    void record(const char* what, SimTime time, const can::Frame& frame)
    {
        char line[96];
        std::snprintf(line, sizeof line, "%lld %s %lld %08X %02X%02X%02X%02X", static_cast<long long>(to_ns(now_)), what,
                      static_cast<long long>(to_ns(time)), static_cast<unsigned>(frame.id), frame.data[0], frame.data[1],
                      frame.data[2], frame.data[3]);
        trace_.emplace_back(line);
    }

    std::uint8_t self_;
    SimDuration period_;
    SimTime now_{};
    SimTime next_periodic_{};
    std::uint8_t sequence_ = 0;
    std::vector<can::TimedFrame> received_;
    std::vector<can::Frame> outgoing_;
    std::vector<std::string> trace_;
};

/// Counts how many trace lines are receptions of frames stamped at the same time from different
/// senders in one step (to check that a scenario exercises the same-time order).
[[nodiscard]] inline std::size_t same_time_receptions(const std::vector<std::string>& trace)
{
    std::size_t count = 0;
    for (std::size_t i = 1; i < trace.size(); ++i)
    {
        long long stepA = 0, timeA = 0, stepB = 0, timeB = 0;
        unsigned idA = 0, idB = 0;
        char whatA[3] = {}, whatB[3] = {};
        if (std::sscanf(trace[i - 1].c_str(), "%lld %2s %lld %x", &stepA, whatA, &timeA, &idA) == 4 &&
            std::sscanf(trace[i].c_str(), "%lld %2s %lld %x", &stepB, whatB, &timeB, &idB) == 4 &&
            std::string(whatA) == "rx" && std::string(whatB) == "rx" && stepA == stepB && timeA == timeB &&
            (idA & 0xFFu) != (idB & 0xFFu))
        {
            ++count;
        }
    }
    return count;
}

} // namespace vehicle_sil::unit
