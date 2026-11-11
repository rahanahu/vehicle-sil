// SPDX-License-Identifier: MIT
#include "vehicle_sil/lin/lin_slave.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::lin {
LinSlave::LinSlave(LinPort& port, LinSlaveOptions options) : port_(port), bus_idle_timeout_(options.bus_idle_timeout)
{
    validate(options);
    for (const LinId id : options.subscriptions)
    {
        subscriptions_.set(id);
    }
    // Last: nothing above may throw once the port knows the slave.
    port_.set_listener(&listener_);
}

void LinSlave::validate(const LinSlaveOptions& options)
{
    for (const LinId id : options.subscriptions)
    {
        if (id > kMaxLinId)
        {
            throw std::invalid_argument("vehicle_sil::lin::LinSlave: subscription " + std::to_string(id) +
                                        ": a LIN id is 0..63");
        }
    }
    if (options.bus_idle_timeout && *options.bus_idle_timeout <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::lin::LinSlave: bus_idle_timeout is " +
                                    std::to_string(options.bus_idle_timeout->count()) +
                                    " ns; it must be > 0 (std::nullopt disables it)");
    }
}

LinSlave::~LinSlave()
{
    port_.set_listener(nullptr);
}

void LinSlave::init()
{
    port_.open();
}

void LinSlave::set_response(SimTime now, LinId id, std::span<const std::uint8_t> data, LinChecksumModel checksum_model)
{
    if (!valid_lin_frame(id, data.size()))
    {
        throw std::invalid_argument("vehicle_sil::lin::LinSlave: set_response(id " + std::to_string(id) + ", " +
                                    std::to_string(data.size()) + " bytes): the id must be 0..63 and the data 1..8 bytes");
    }
    if (id == kMasterRequestId || id == kSlaveResponseId)
    {
        throw std::invalid_argument("vehicle_sil::lin::LinSlave: set_response(id " + std::to_string(id) +
                                    "): diagnostic frames go through set_slave_response()");
    }
    LinFrame frame;
    frame.id = id;
    frame.checksum_model = checksum_model;
    frame.data_length = static_cast<std::uint8_t>(data.size());
    std::copy(data.begin(), data.end(), frame.data.begin());
    store(now, id, frame);
}

void LinSlave::set_slave_response(SimTime now, std::span<const std::uint8_t> data)
{
    if (data.size() != kDiagnosticFrameLength)
    {
        throw std::invalid_argument("vehicle_sil::lin::LinSlave: set_slave_response() got " + std::to_string(data.size()) +
                                    " bytes; a slave response has 8 bytes");
    }
    LinFrame frame;
    frame.id = kSlaveResponseId;
    frame.checksum_model = LinChecksumModel::Classic;
    frame.data_length = static_cast<std::uint8_t>(data.size());
    std::copy(data.begin(), data.end(), frame.data.begin());
    store(now, kSlaveResponseId, frame, true);
}

void LinSlave::clear_response(SimTime now, LinId id)
{
    if (id > kMaxLinId)
    {
        throw std::invalid_argument("vehicle_sil::lin::LinSlave: clear_response(id " + std::to_string(id) +
                                    "): a LIN id is 0..63");
    }
    store(now, id, std::nullopt);
}

void LinSlave::store(SimTime now, LinId id, std::optional<LinFrame> frame, bool once)
{
    const std::lock_guard lock(mutex_);
    auto& values = values_[id];
    if (values.current && now < values.current->since)
    {
        throw std::logic_error("vehicle_sil::lin::LinSlave: the response of id " + std::to_string(id) + " was set at " +
                               std::to_string(values.current->since.time_since_epoch().count()) +
                               " ns and now at an earlier time (" + std::to_string(now.time_since_epoch().count()) +
                               " ns); pass the time of the current step");
    }
    // A value set again at the same `now` replaces the one set before; headers stamped after
    // `now` see only the new one, which is a new one-shot response (sent cleared).
    if (values.current && values.current->since == now)
    {
        values.current = Value{now, frame, once};
    }
    else
    {
        values.previous = std::move(values.current);
        values.current = Value{now, frame, once};
    }
}

void LinSlave::request_wakeup()
{
    wakeup_requested_ = true;
}

void LinSlave::push(SimTime time, const BusEvent& event)
{
    // A full inbox counts the loss in its overflows(), reported by statistics().
    static_cast<void>(events_.push(time, event));
}

void LinSlave::on_header(SimTime header_time, LinId id)
{
    push(header_time, BusEvent{BusEvent::Kind::Header, id, 0, {}});

    std::optional<LinFrame> response;
    {
        const std::lock_guard lock(mutex_);
        if (id <= kMaxLinId)
        {
            // The newest value set before the header; a `once` value answers one header only.
            auto& values = values_[id];
            for (auto* value : {&values.current, &values.previous})
            {
                if (*value && (*value)->since < header_time)
                {
                    if (!(*value)->sent)
                    {
                        response = (*value)->frame; // nullopt after clear_response()
                        (*value)->sent = (*value)->once;
                    }
                    break;
                }
            }
        }
        if (response)
        {
            ++stats_.responses_sent;
        }
        else
        {
            ++stats_.headers_ignored;
        }
    }
    if (response)
    {
        port_.respond(header_time, *response);
    }
}

void LinSlave::on_response(SimTime header_time, const LinFrame& frame, LinResultStatus status)
{
    // The port passes the master's frames only (not the go-to-sleep command, not this slave's
    // own responses); an error carries no valid data.
    if (status != LinResultStatus::Ok || frame.id > kMaxLinId || !subscriptions_.test(frame.id))
    {
        return;
    }
    BusEvent received{BusEvent::Kind::Frame, frame.id,
                      static_cast<std::uint8_t>(std::min<std::size_t>(frame.data_length, kMaxLinDataLength)), frame.data};
    push(header_time, received);
}

void LinSlave::on_go_to_sleep(SimTime time)
{
    push(time, BusEvent{BusEvent::Kind::GoToSleep, 0, 0, {}});
}

void LinSlave::on_wakeup(SimTime time)
{
    // The port reports only the pulses of other nodes (not request_wakeup()'s own).
    push(time, BusEvent{BusEvent::Kind::WakeupPulse, 0, 0, {}});
}

void LinSlave::set_state(SimTime time, LinNodeState state)
{
    if (state_ == state)
    {
        return;
    }
    state_ = state;
    if (on_state_)
    {
        on_state_(time, state);
    }
}

void LinSlave::step(SimTime now)
{
    events_.drain_before(now, [&](SimTime t, BusEvent& event) {
        last_activity_ = std::max(last_activity_, t);
        switch (event.kind)
        {
        case BusEvent::Kind::GoToSleep:
            set_state(t, LinNodeState::Sleep);
            break;
        case BusEvent::Kind::WakeupPulse:
        case BusEvent::Kind::Header:
            set_state(t, LinNodeState::Operational);
            break;
        case BusEvent::Kind::Frame:
        {
            {
                const std::lock_guard lock(mutex_);
                ++stats_.frames_received;
            }
            if (on_frame_)
            {
                on_frame_(LinReceivedFrame{t, event.id, event.data_length, event.data});
            }
            break;
        }
        }
    });

    if (state_ == LinNodeState::Operational && bus_idle_timeout_ && now - last_activity_ >= *bus_idle_timeout_)
    {
        set_state(now, LinNodeState::Sleep);
    }

    if (std::exchange(wakeup_requested_, false) && state_ == LinNodeState::Sleep)
    {
        // The port is always operational (see the class comment); it sends the pulse.
        port_.send_wakeup(now);
        {
            const std::lock_guard lock(mutex_);
            ++stats_.wakeups_sent;
        }
        last_activity_ = now;
        set_state(now, LinNodeState::Operational);
    }
}

LinSlaveStatistics LinSlave::statistics() const
{
    const std::lock_guard lock(mutex_);
    auto stats = stats_;
    stats.inbox_overflows = events_.overflows();
    return stats;
}

} // namespace vehicle_sil::lin
