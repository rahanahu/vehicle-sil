// SPDX-License-Identifier: MIT
#include "vehicle_sil/testkit/recorders.hpp"

#include <algorithm>

namespace vehicle_sil::testkit {

J1939Events::J1939Events(j1939::Network& network, std::size_t capacity) : capacity_(capacity)
{
    network.set_observer(this);
}

void J1939Events::on_event(const j1939::Event& event)
{
    if (records_.size() >= capacity_)
    {
        ++dropped_;
        return;
    }
    Record record{event, std::vector<std::uint8_t>(event.data.begin(), event.data.end())};
    record.event.data = {};
    records_.push_back(std::move(record));
}

std::size_t J1939Events::count(j1939::EventKind kind) const
{
    return static_cast<std::size_t>(
        std::count_if(records_.begin(), records_.end(), [kind](const Record& r) { return r.event.kind == kind; }));
}

std::size_t J1939Events::count(j1939::EventKind kind, const Predicate& predicate) const
{
    return static_cast<std::size_t>(std::count_if(records_.begin(), records_.end(), [&](const Record& r) {
        return r.event.kind == kind && (!predicate || predicate(r));
    }));
}

std::vector<J1939Events::Record> J1939Events::of(j1939::EventKind kind, const Predicate& predicate) const
{
    std::vector<Record> out;
    for (const auto& r : records_)
    {
        if (r.event.kind == kind && (!predicate || predicate(r)))
        {
            out.push_back(r);
        }
    }
    return out;
}

const J1939Events::Record* J1939Events::first(j1939::EventKind kind, const Predicate& predicate) const
{
    for (const auto& r : records_)
    {
        if (r.event.kind == kind && (!predicate || predicate(r)))
        {
            return &r;
        }
    }
    return nullptr;
}

const J1939Events::Record* J1939Events::last(j1939::EventKind kind, const Predicate& predicate) const
{
    for (auto it = records_.rbegin(); it != records_.rend(); ++it)
    {
        if (it->event.kind == kind && (!predicate || predicate(*it)))
        {
            return &*it;
        }
    }
    return nullptr;
}

void J1939Events::clear() noexcept
{
    records_.clear();
    dropped_ = 0;
}

FrameLog::FrameLog(can::TapPort& tap, std::size_t capacity) : capacity_(capacity)
{
    tap.set_listener(this);
}

void FrameLog::on_tap(const can::TapRecord& record)
{
    if (records_.size() >= capacity_)
    {
        ++dropped_;
        return;
    }
    records_.push_back(record);
}

std::vector<can::TapRecord> FrameLog::of(Direction direction, const Predicate& predicate) const
{
    std::vector<can::TapRecord> out;
    for (const auto& r : records_)
    {
        if (r.direction == direction && (!predicate || predicate(r)))
        {
            out.push_back(r);
        }
    }
    return out;
}

std::size_t FrameLog::count(Direction direction, const Predicate& predicate) const
{
    return static_cast<std::size_t>(std::count_if(records_.begin(), records_.end(), [&](const can::TapRecord& r) {
        return r.direction == direction && (!predicate || predicate(r));
    }));
}

void FrameLog::clear() noexcept
{
    records_.clear();
    dropped_ = 0;
}

} // namespace vehicle_sil::testkit
