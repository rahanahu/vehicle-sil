// SPDX-License-Identifier: MIT
//
// Recorders for tests: the events of a J1939 network (Observer) and the raw frames of a port
// (TapListener), kept in memory up to a limit (vehicle_sil::testkit).
#pragma once

#include "vehicle_sil/can/tap_port.hpp"
#include "vehicle_sil/j1939/observer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace vehicle_sil::testkit {

/// Records every event of one J1939 network (an Observer; read only). The constructor sets it
/// as the network's observer: only outside a step (std::logic_error otherwise); a network has
/// one observer, so this replaces another one. Records beyond `capacity` are not kept, only
/// counted (dropped()): a test that sees dropped() > 0 looked at an incomplete record.
class J1939Events final : public j1939::Observer
{
public:
    /// An event with a copy of its data. `event.data` is empty: use `data`.
    struct Record
    {
        j1939::Event event;
        std::vector<std::uint8_t> data;
    };
    using Predicate = std::function<bool(const Record&)>;

    explicit J1939Events(j1939::Network& network, std::size_t capacity = 100000);

    void on_event(const j1939::Event& event) override;

    [[nodiscard]] const std::vector<Record>& all() const noexcept { return records_; }
    [[nodiscard]] std::size_t count(j1939::EventKind kind) const;
    [[nodiscard]] std::size_t count(j1939::EventKind kind, const Predicate& predicate) const;
    [[nodiscard]] bool has(j1939::EventKind kind) const { return count(kind) != 0; }
    /// The records of `kind` (and `predicate`, if given), in order.
    [[nodiscard]] std::vector<Record> of(j1939::EventKind kind, const Predicate& predicate = {}) const;
    /// The first or last record of `kind` (and `predicate`), nullptr if none.
    [[nodiscard]] const Record* first(j1939::EventKind kind, const Predicate& predicate = {}) const;
    [[nodiscard]] const Record* last(j1939::EventKind kind, const Predicate& predicate = {}) const;
    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }
    void clear() noexcept;

private:
    std::size_t capacity_;
    std::vector<Record> records_;
    std::size_t dropped_ = 0;
};

/// Records what passes through one TapPort (a TapListener; read only). The constructor sets
/// it as the tap's listener (replacing another one). Records beyond `capacity` are only
/// counted (dropped()).
class FrameLog final : public can::TapListener
{
public:
    using Direction = can::TapRecord::Direction;
    using Predicate = std::function<bool(const can::TapRecord&)>;

    explicit FrameLog(can::TapPort& tap, std::size_t capacity = 100000);

    void on_tap(const can::TapRecord& record) override;

    [[nodiscard]] const std::vector<can::TapRecord>& all() const noexcept { return records_; }
    /// The records of `direction` (and `predicate`, if given), in order.
    [[nodiscard]] std::vector<can::TapRecord> of(Direction direction, const Predicate& predicate = {}) const;
    [[nodiscard]] std::size_t count(Direction direction, const Predicate& predicate = {}) const;
    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }
    void clear() noexcept;

private:
    std::size_t capacity_;
    std::vector<can::TapRecord> records_;
    std::size_t dropped_ = 0;
};

} // namespace vehicle_sil::testkit
