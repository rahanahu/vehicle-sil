// SPDX-License-Identifier: MIT
#include "vehicle_sil/can/bus_model.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace vehicle_sil::can {

SimDuration frame_duration(const Frame& frame, std::uint32_t bitrate)
{
    if (bitrate == 0)
    {
        throw std::invalid_argument("vehicle_sil::can::frame_duration: the bitrate must be > 0");
    }
    const std::uint64_t bits = frame_bits(frame);
    const std::uint64_t ns = (bits * 1'000'000'000ull + bitrate - 1) / bitrate;
    return SimDuration{static_cast<SimDuration::rep>(ns)};
}

bool goes_first(const Frame& a, const Frame& b) noexcept
{
    const std::uint32_t ka = arbitration_key(a);
    const std::uint32_t kb = arbitration_key(b);
    if (ka != kb)
    {
        return ka < kb;
    }
    if (a.length != b.length)
    {
        return a.length < b.length;
    }
    const auto pa = a.payload();
    const auto pb = b.payload();
    return std::lexicographical_compare(pa.begin(), pa.end(), pb.begin(), pb.end());
}

CanBusModel::CanBusModel(CanBusModelOptions options) : bitrate_(options.bitrate), capacity_(options.queue_capacity)
{
    if (bitrate_ == 0)
    {
        throw std::invalid_argument("vehicle_sil::can::CanBusModel: the bitrate must be > 0");
    }
    if (capacity_ == 0)
    {
        throw std::invalid_argument("vehicle_sil::can::CanBusModel: the queue capacity must be > 0");
    }
}

std::size_t CanBusModel::add_sender()
{
    Queue queue;
    queue.slots.resize(capacity_);
    queues_.push_back(std::move(queue));
    return queues_.size() - 1;
}

bool CanBusModel::submit(std::size_t sender, const Frame& frame, SimTime ready, std::uint64_t context)
{
    if (sender >= queues_.size())
    {
        throw std::invalid_argument("vehicle_sil::can::CanBusModel: unknown sender");
    }
    if (!is_valid(frame))
    {
        throw std::invalid_argument("vehicle_sil::can::CanBusModel: invalid frame");
    }
    Queue& queue = queues_[sender];
    if (queue.size >= capacity_)
    {
        return false;
    }
    // Only the queue head competes (run()), so a frame with an earlier `ready` than the frame
    // ahead of it waits for that frame anyway.
    queue.slots[(queue.head + queue.size) % capacity_] = Pending{frame, ready, context};
    ++queue.size;
    ++total_queued_;
    return true;
}

void CanBusModel::pop(Queue& queue) noexcept
{
    queue.head = (queue.head + 1) % capacity_;
    --queue.size;
    --total_queued_;
}

std::size_t CanBusModel::run(SimTime from, SimTime until, TransmissionSink sink)
{
    std::size_t count = 0;
    SimTime t = std::max(bus_free_, from);
    while (total_queued_ > 0)
    {
        std::optional<SimTime> earliest;
        for (const auto& queue : queues_)
        {
            if (queue.size > 0 && (!earliest || queue.front().ready < *earliest))
            {
                earliest = queue.front().ready;
            }
        }
        t = std::max(t, *earliest);
        if (t >= until)
        {
            break;
        }
        std::size_t best = queues_.size();
        for (std::size_t i = 0; i < queues_.size(); ++i)
        {
            const Queue& queue = queues_[i];
            if (queue.size == 0 || queue.front().ready > t)
            {
                continue;
            }
            // Lower index first among equal frames: only a strictly earlier frame replaces best.
            if (best == queues_.size() || goes_first(queue.front().frame, queues_[best].front().frame))
            {
                best = i;
            }
        }
        Queue& queue = queues_[best];
        const Pending pending = queue.front();
        pop(queue);
        Transmission tx;
        tx.sender = best;
        tx.frame = pending.frame;
        tx.ready = pending.ready;
        tx.start = t;
        tx.end = t + frame_duration(pending.frame, bitrate_);
        tx.context = pending.context;
        bus_free_ = tx.end;
        t = tx.end;
        ++transmissions_;
        ++count;
        sink(tx);
    }
    return count;
}

std::size_t CanBusModel::clear(std::size_t sender)
{
    if (sender >= queues_.size())
    {
        throw std::invalid_argument("vehicle_sil::can::CanBusModel: unknown sender");
    }
    Queue& queue = queues_[sender];
    const std::size_t removed = queue.size;
    total_queued_ -= removed;
    queue.head = 0;
    queue.size = 0;
    return removed;
}

void CanBusModel::set_bitrate(std::uint32_t bitrate)
{
    if (bitrate == 0)
    {
        throw std::invalid_argument("vehicle_sil::can::CanBusModel: the bitrate must be > 0");
    }
    bitrate_ = bitrate;
}

std::size_t CanBusModel::queued(std::size_t sender) const
{
    if (sender >= queues_.size())
    {
        throw std::invalid_argument("vehicle_sil::can::CanBusModel: unknown sender");
    }
    return queues_[sender].size;
}

} // namespace vehicle_sil::can
