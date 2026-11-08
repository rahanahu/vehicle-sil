// SPDX-License-Identifier: MIT
#include "vehicle_sil/lin/lin_slave.hpp"

#include "lin_internal.hpp"

#include <silkit/experimental/services/lin/LinControllerExtensions.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::lin {
namespace {

namespace silkit_lin = SilKit::Services::Lin;
namespace experimental_lin = SilKit::Experimental::Services::Lin;

bool is_go_to_sleep_frame(const silkit_lin::LinFrame& frame)
{
    const auto command = silkit_lin::GoToSleepFrame();
    return frame.id == command.id && frame.data == command.data;
}

} // namespace

LinSlave::LinSlave(silkit_lin::ILinController& controller, LinSlaveOptions options, FrameHandler on_frame,
                   StateHandler on_state) :
  controller_(controller),
  bus_idle_timeout_(options.bus_idle_timeout),
  on_frame_(std::move(on_frame)),
  on_state_(std::move(on_state))
{
    for (const LinId id : options.subscriptions)
    {
        if (id > kMaxLinId)
        {
            throw std::invalid_argument("LinSlave subscription " + std::to_string(id) + ": id must be 0..63");
        }
        subscriptions_.set(id);
    }
    if (bus_idle_timeout_ && *bus_idle_timeout_ <= SimDuration::zero())
    {
        throw std::invalid_argument("LinSlave bus_idle_timeout must be > 0");
    }
    header_handler_ = experimental_lin::AddFrameHeaderHandler(
        &controller_, [this](silkit_lin::ILinController*, const experimental_lin::LinFrameHeaderEvent& event) {
            on_header(from_silkit_time(event.timestamp), event.id);
        });
    status_handler_ = controller_.AddFrameStatusHandler(
        [this](silkit_lin::ILinController*, const silkit_lin::LinFrameStatusEvent& event) { on_frame_status(event); });
    sleep_handler_ = controller_.AddGoToSleepHandler(
        [this](silkit_lin::ILinController*, const silkit_lin::LinGoToSleepEvent& event) {
            on_go_to_sleep(from_silkit_time(event.timestamp));
        });
    wakeup_handler_ =
        controller_.AddWakeupHandler([this](silkit_lin::ILinController*, const silkit_lin::LinWakeupEvent& event) {
            on_wakeup(from_silkit_time(event.timestamp), event.direction);
        });
}

LinSlave::~LinSlave()
{
    detail::remove_handler(header_handler_,
                           [this](auto id) { experimental_lin::RemoveFrameHeaderHandler(&controller_, id); });
    detail::remove_handler(status_handler_, [this](auto id) { controller_.RemoveFrameStatusHandler(id); });
    detail::remove_handler(sleep_handler_, [this](auto id) { controller_.RemoveGoToSleepHandler(id); });
    detail::remove_handler(wakeup_handler_, [this](auto id) { controller_.RemoveWakeupHandler(id); });
}

void LinSlave::init()
{
    experimental_lin::LinControllerDynamicConfig config;
    config.controllerMode = silkit_lin::LinControllerMode::Slave;
    config.baudRate = detail::kBaudRate;
    experimental_lin::InitDynamic(&controller_, config);
}

void LinSlave::set_response(SimTime now, LinId id, std::span<const std::uint8_t> data, LinChecksumModel checksum_model)
{
    if (!valid_lin_frame(id, data.size()))
    {
        throw std::invalid_argument("LinSlave::set_response: id must be 0..63 and data 1..8 bytes");
    }
    if (id == kMasterRequestId || id == kSlaveResponseId)
    {
        throw std::invalid_argument("LinSlave::set_response: diagnostic frames go through set_slave_response()");
    }
    if (checksum_model != LinChecksumModel::Classic && checksum_model != LinChecksumModel::Enhanced)
    {
        throw std::invalid_argument("LinSlave::set_response: checksum_model must be Classic or Enhanced");
    }
    silkit_lin::LinFrame frame;
    frame.id = id;
    frame.checksumModel = checksum_model;
    frame.dataLength = static_cast<silkit_lin::LinDataLength>(data.size());
    std::copy(data.begin(), data.end(), frame.data.begin());
    store(now, id, frame);
}

void LinSlave::set_slave_response(SimTime now, std::span<const std::uint8_t> data)
{
    if (data.size() != kDiagnosticFrameLength)
    {
        throw std::invalid_argument("LinSlave::set_slave_response: a slave response has 8 bytes");
    }
    silkit_lin::LinFrame frame;
    frame.id = kSlaveResponseId;
    frame.checksumModel = LinChecksumModel::Classic;
    frame.dataLength = static_cast<silkit_lin::LinDataLength>(data.size());
    std::copy(data.begin(), data.end(), frame.data.begin());
    store(now, kSlaveResponseId, frame, true);
}

void LinSlave::clear_response(SimTime now, LinId id)
{
    if (id > kMaxLinId)
    {
        throw std::invalid_argument("LinSlave::clear_response: id must be 0..63");
    }
    store(now, id, std::nullopt);
}

void LinSlave::store(SimTime now, LinId id, std::optional<silkit_lin::LinFrame> frame, bool once)
{
    const std::lock_guard lock(mutex_);
    auto& values = values_[id];
    if (values.current && now < values.current->since)
    {
        throw std::logic_error("LinSlave: response time went backwards");
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

    std::optional<silkit_lin::LinFrame> response;
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
        experimental_lin::SendDynamicResponse(&controller_, *response);
    }
}

void LinSlave::on_frame_status(const silkit_lin::LinFrameStatusEvent& event)
{
    // TX statuses are this slave's own responses; error statuses carry no valid data.
    const auto& frame = event.frame;
    if (event.status != silkit_lin::LinFrameStatus::LIN_RX_OK || frame.id > kMaxLinId || !subscriptions_.test(frame.id) ||
        is_go_to_sleep_frame(frame))
    {
        return;
    }
    BusEvent received{BusEvent::Kind::Frame, frame.id,
                      static_cast<std::uint8_t>(std::min<std::size_t>(frame.dataLength, kMaxLinDataLength)), frame.data};
    push(from_silkit_time(event.timestamp), received);
}

void LinSlave::on_go_to_sleep(SimTime time)
{
    push(time, BusEvent{BusEvent::Kind::GoToSleep, 0, 0, {}});
}

void LinSlave::on_wakeup(SimTime time, SilKit::Services::TransmitDirection direction)
{
    // TX: the pulse this slave sent itself (request_wakeup()).
    if (direction != SilKit::Services::TransmitDirection::RX)
    {
        return;
    }
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
        // The controller is always operational (see the class comment). Wakeup() sends the pulse
        // and switches the controller to operational, and warns if it already is: switch it to
        // sleep first. Nothing is received in between: SIL Kit runs this step on the thread that
        // delivers messages.
        controller_.GoToSleepInternal();
        controller_.Wakeup();
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
