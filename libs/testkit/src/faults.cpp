// SPDX-License-Identifier: MIT
#include "vehicle_sil/testkit/faults.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::testkit::faults {

namespace {

[[nodiscard]] bool selects(const FramePredicate& predicate, const can::TimedFrame& frame)
{
    return !predicate || predicate(frame);
}

} // namespace

FramePredicate j1939_pgn(j1939::Pgn pgn)
{
    return [pgn](const can::TimedFrame& f) {
        const auto id = j1939::Identifier::decode(f.frame);
        return id.has_value() && id->pgn == pgn;
    };
}

FramePredicate j1939_source(j1939::Address source)
{
    return [source](const can::TimedFrame& f) {
        const auto id = j1939::Identifier::decode(f.frame);
        return id.has_value() && id->source == source;
    };
}

FramePredicate can_id(std::uint32_t id, can::IdFormat format)
{
    return [id, format](const can::TimedFrame& f) { return f.frame.id == id && f.frame.format == format; };
}

can::FaultInjector when(FramePredicate predicate, can::FaultAction action)
{
    return [predicate = std::move(predicate), action](const can::TimedFrame& f) {
        return selects(predicate, f) ? action : can::FaultAction::deliver();
    };
}

can::FaultInjector between(SimTime from, SimTime until, can::FaultAction action, FramePredicate predicate)
{
    if (until <= from)
    {
        throw std::invalid_argument("vehicle_sil::testkit::faults::between: until must be after from");
    }
    return [from, until, action, predicate = std::move(predicate)](const can::TimedFrame& f) {
        return f.time >= from && f.time < until && selects(predicate, f) ? action : can::FaultAction::deliver();
    };
}

can::FaultInjector nth(std::size_t n, can::FaultAction action, FramePredicate predicate)
{
    if (n == 0)
    {
        throw std::invalid_argument("vehicle_sil::testkit::faults::nth: n starts at 1");
    }
    auto seen = std::make_shared<std::size_t>(0);
    return [n, action, predicate = std::move(predicate), seen](const can::TimedFrame& f) {
        if (!selects(predicate, f))
        {
            return can::FaultAction::deliver();
        }
        return ++*seen == n ? action : can::FaultAction::deliver();
    };
}

can::FaultInjector sequence(std::vector<can::FaultAction> actions, FramePredicate predicate)
{
    auto seen = std::make_shared<std::size_t>(0);
    return [actions = std::move(actions), predicate = std::move(predicate), seen](const can::TimedFrame& f) {
        if (!selects(predicate, f) || *seen >= actions.size())
        {
            return can::FaultAction::deliver();
        }
        return actions[(*seen)++];
    };
}

can::FaultInjector first_of(std::vector<can::FaultInjector> injectors)
{
    return [injectors = std::move(injectors)](const can::TimedFrame& f) {
        for (const auto& injector : injectors)
        {
            if (!injector)
            {
                continue;
            }
            const can::FaultAction action = injector(f);
            if (action.kind != can::FaultAction::Kind::Deliver)
            {
                return action;
            }
        }
        return can::FaultAction::deliver();
    };
}

} // namespace vehicle_sil::testkit::faults
