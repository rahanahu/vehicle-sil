// SPDX-License-Identifier: MIT
#include "vehicle_sil/lin/lin_master.hpp"

#include "lin_internal.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::lin {
namespace {

namespace silkit_lin = SilKit::Services::Lin;

LinResultStatus to_result_status(silkit_lin::LinFrameStatus status) noexcept
{
    switch (status)
    {
    case silkit_lin::LinFrameStatus::LIN_RX_OK:
    case silkit_lin::LinFrameStatus::LIN_TX_OK:
        return LinResultStatus::Ok;
    case silkit_lin::LinFrameStatus::LIN_RX_NO_RESPONSE:
        return LinResultStatus::NoResponse;
    default:
        return LinResultStatus::Error;
    }
}

/// The first time on the grid of `step` (multiples of `step` from T = 0) at or after `t`.
SimTime ceil_to_grid(SimTime t, SimDuration step) noexcept
{
    const auto since = t.time_since_epoch();
    const auto steps = (since + step - SimDuration(1)) / step;
    return SimTime{steps * step};
}

} // namespace

LinMaster::LinMaster(silkit_lin::ILinController& controller, LinScheduleTable schedule, LinMasterOptions options,
                     ResultHandler on_result, StateHandler on_state) :
  controller_(controller),
  options_(options),
  on_result_(std::move(on_result)),
  on_state_(std::move(on_state)),
  schedule_(std::move(schedule))
{
    validate_schedule(schedule_, options_.step);
    remember_frame_configs(schedule_);
    if (options_.wakeup_delay < SimDuration::zero())
    {
        throw std::invalid_argument("LinMaster wakeup_delay must be >= 0");
    }
    status_handler_ = controller_.AddFrameStatusHandler(
        [this](silkit_lin::ILinController*, const silkit_lin::LinFrameStatusEvent& event) { on_frame_status(event); });
    wakeup_handler_ =
        controller_.AddWakeupHandler([this](silkit_lin::ILinController*, const silkit_lin::LinWakeupEvent& event) {
            // TX: the pulse this master sent itself.
            if (event.direction == SilKit::Services::TransmitDirection::RX)
            {
                wakeups_.push(from_silkit_time(event.timestamp), WakeupPulse{});
            }
        });
}

LinMaster::~LinMaster()
{
    detail::remove_handler(status_handler_, [this](auto id) { controller_.RemoveFrameStatusHandler(id); });
    detail::remove_handler(wakeup_handler_, [this](auto id) { controller_.RemoveWakeupHandler(id); });
}

void LinMaster::init()
{
    silkit_lin::LinControllerConfig config;
    config.controllerMode = silkit_lin::LinControllerMode::Master;
    config.baudRate = detail::kBaudRate;
    for (const auto& entry : schedule_)
    {
        if (entry.publisher != LinPublisher::Slave || configured_.test(entry.id))
        {
            continue;
        }
        configured_.set(entry.id);
        silkit_lin::LinFrameResponse response;
        response.frame.id = entry.id;
        response.frame.checksumModel = entry.checksum_model;
        response.frame.dataLength = entry.data_length;
        response.responseMode = silkit_lin::LinFrameResponseMode::Rx;
        config.frameResponses.push_back(response);
    }
    controller_.Init(config);
}

void LinMaster::remember_frame_configs(const LinScheduleTable& table)
{
    std::array<std::optional<FrameConfig>, kMaxLinId + 1> merged = frame_configs_;
    for (const auto& entry : table)
    {
        const FrameConfig config{entry.publisher, entry.data_length, entry.checksum_model};
        auto& known = merged[entry.id];
        if (known && *known != config)
        {
            // SIL Kit does not let a controller reconfigure an identifier once configured.
            throw std::invalid_argument("LIN schedule entry for id " + std::to_string(entry.id) +
                                        ": an id keeps its length, checksum and publisher in every schedule");
        }
        known = config;
    }
    frame_configs_ = merged;
}

void LinMaster::configure(const LinScheduleEntry& entry)
{
    // A master frame configures itself in SendFrame(); a slave frame needs an Rx response once.
    if (entry.publisher == LinPublisher::Slave && !configured_.test(entry.id))
    {
        silkit_lin::LinFrameResponse response;
        response.frame.id = entry.id;
        response.frame.checksumModel = entry.checksum_model;
        response.frame.dataLength = entry.data_length;
        response.responseMode = silkit_lin::LinFrameResponseMode::Rx;
        controller_.SetFrameResponse(response);
        configured_.set(entry.id);
    }
}

void LinMaster::set_frame_data(LinId id, std::span<const std::uint8_t> data)
{
    if (!valid_lin_frame(id, data.size()))
    {
        throw std::invalid_argument("LinMaster::set_frame_data: id must be 0..63 and data 1..8 bytes");
    }
    if (id == kMasterRequestId)
    {
        throw std::invalid_argument("LinMaster::set_frame_data: the master request frame is sent with send_master_request()");
    }
    const auto& config = frame_configs_[id];
    if (!config || config->publisher != LinPublisher::Master || config->data_length != data.size())
    {
        throw std::invalid_argument("LinMaster::set_frame_data: id " + std::to_string(id) +
                                    " is not a master frame of this length in any schedule given to the master");
    }
    auto& stored = master_data_[id];
    stored.fill(0);
    std::copy(data.begin(), data.end(), stored.begin());
}

bool LinMaster::send_master_request(std::span<const std::uint8_t> data)
{
    if (data.size() != kDiagnosticFrameLength)
    {
        throw std::invalid_argument("LinMaster::send_master_request: a master request has 8 bytes");
    }
    if (data[0] == 0)
    {
        throw std::invalid_argument("LinMaster::send_master_request: NAD 0 is the go-to-sleep command (request_sleep())");
    }
    if (master_request_)
    {
        return false;
    }
    auto& request = master_request_.emplace();
    std::copy(data.begin(), data.end(), request.begin());
    return true;
}

void LinMaster::set_schedule(LinScheduleTable schedule)
{
    validate_schedule(schedule, options_.step);
    remember_frame_configs(schedule);
    pending_schedule_ = std::move(schedule);
}

void LinMaster::request_sleep()
{
    sleep_requested_ = true;
}

void LinMaster::request_wakeup()
{
    wakeup_requested_ = true;
}

void LinMaster::on_frame_status(const silkit_lin::LinFrameStatusEvent& event)
{
    const std::lock_guard lock(mutex_);
    if (!outstanding_ || event.frame.id != outstanding_->id)
    {
        return;
    }
    if (answered_)
    {
        // A second response to the same header: two nodes sent a response, which collide on a
        // real bus. Both have arrived by the end of the slot, so the result does not depend on
        // their arrival order.
        outstanding_->status = LinResultStatus::Error;
        outstanding_->data_length = 0;
        outstanding_->data = {};
        return;
    }
    answered_ = true;
    outstanding_->status = to_result_status(event.status);
    if (outstanding_->status == LinResultStatus::Ok)
    {
        outstanding_->data_length = std::min<std::uint8_t>(event.frame.dataLength, kMaxLinDataLength);
        outstanding_->data = event.frame.data;
    }
}

std::optional<LinScheduleResult> LinMaster::end_slot()
{
    const std::lock_guard lock(mutex_);
    auto done = std::exchange(outstanding_, std::nullopt);
    if (done)
    {
        switch (done->status)
        {
        case LinResultStatus::Ok:
            ++stats_.responses_ok;
            break;
        case LinResultStatus::NoResponse:
            ++stats_.no_responses;
            break;
        case LinResultStatus::Error:
            ++stats_.errors;
            break;
        }
    }
    return done;
}

void LinMaster::report(const std::optional<LinScheduleResult>& result)
{
    if (result && on_result_)
    {
        on_result_(*result);
    }
}

void LinMaster::notify(SimTime time, LinNodeState state)
{
    if (on_state_)
    {
        on_state_(time, state);
    }
}

void LinMaster::wake(SimTime now, SimTime pulse_time)
{
    state_ = LinNodeState::Operational;
    next_entry_ = 0;
    sleep_requested_ = false; // made while asleep: dropped
    next_header_time_ = std::max(now, ceil_to_grid(pulse_time + options_.wakeup_delay, options_.step));
    notify(now, LinNodeState::Operational);
}

void LinMaster::step(SimTime now)
{
    // A slave's wakeup pulse stamped T has arrived by every step after T.
    std::optional<SimTime> pulse;
    wakeups_.drain_before(now, [&](SimTime t, WakeupPulse&) {
        // A pulse at the time of the go-to-sleep command does not wake the master: the command
        // wins, as it does at the slaves (LinSlave orders it after a pulse of the same time).
        if (state_ == LinNodeState::Sleep && t > sleep_since_ && !pulse)
        {
            pulse = t;
        }
    });
    if (state_ == LinNodeState::Sleep)
    {
        if (wakeup_requested_)
        {
            controller_.Wakeup(); // sends the pulse and makes the controller operational
            {
                const std::lock_guard lock(mutex_);
                ++stats_.wakeups_by_master;
            }
            wake(now, now);
        }
        else if (pulse)
        {
            controller_.WakeupInternal();
            {
                const std::lock_guard lock(mutex_);
                ++stats_.wakeups_by_slaves;
            }
            wake(now, *pulse);
        }
    }
    wakeup_requested_ = false;
    if (state_ == LinNodeState::Sleep)
    {
        sleep_requested_ = false;
        return;
    }
    if (now < next_header_time_)
    {
        return;
    }

    // The previous slot ends now; its response has arrived (see the class comment).
    const auto done = end_slot();
    if (sleep_requested_)
    {
        sleep_requested_ = false;
        state_ = LinNodeState::Sleep;
        sleep_since_ = now;
        {
            const std::lock_guard lock(mutex_);
            ++stats_.sleep_commands;
        }
        // Sends the go-to-sleep command and puts the controller to sleep.
        controller_.GoToSleep();
        notify(now, LinNodeState::Sleep);
        report(done);
        return;
    }
    if (pending_schedule_)
    {
        schedule_ = std::move(*pending_schedule_);
        pending_schedule_.reset();
        next_entry_ = 0;
    }
    const LinScheduleEntry entry = schedule_[next_entry_];
    // The slot ends at next_header_time_ even if sending throws.
    next_header_time_ = now + entry.slot;
    next_entry_ = (next_entry_ + 1) % schedule_.size();
    if (entry.id == kMasterRequestId && !master_request_)
    {
        // A master request slot without a request stays silent.
        report(done);
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        outstanding_ = LinScheduleResult{now, entry.id, entry.publisher, LinResultStatus::NoResponse, 0, {}};
        answered_ = false;
        ++stats_.headers_sent;
    }
    configure(entry);
    // With no responder configured and no dynamic-response slave on the bus, SIL Kit reports
    // LIN_RX_NO_RESPONSE synchronously from here, so the lock must not be held. The master's own frame is reported when SIL Kit delivers the header
    // back to the master, after this step and before the next one.
    if (entry.publisher == LinPublisher::Master)
    {
        silkit_lin::LinFrame frame;
        frame.id = entry.id;
        frame.checksumModel = entry.checksum_model;
        frame.dataLength = entry.data_length;
        if (entry.id == kMasterRequestId)
        {
            std::copy(master_request_->begin(), master_request_->end(), frame.data.begin());
            master_request_.reset();
        }
        else
        {
            std::copy_n(master_data_[entry.id].begin(), entry.data_length, frame.data.begin());
        }
        controller_.SendFrame(frame, silkit_lin::LinFrameResponseType::MasterResponse);
    }
    else
    {
        controller_.SendFrameHeader(entry.id);
    }
    // Report after sending, so that a throwing handler cannot leave the new header unsent.
    report(done);
}

LinMasterStatistics LinMaster::statistics() const
{
    const std::lock_guard lock(mutex_);
    auto stats = stats_;
    stats.inbox_overflows = wakeups_.overflows();
    return stats;
}

} // namespace vehicle_sil::lin
