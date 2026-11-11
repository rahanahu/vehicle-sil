// SPDX-License-Identifier: MIT
#include "vehicle_sil/lin/lin_master.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::lin {
namespace {

/// The first time on the grid of `step` (multiples of `step` from T = 0) at or after `t`.
SimTime ceil_to_grid(SimTime t, SimDuration step) noexcept
{
    const auto since = t.time_since_epoch();
    const auto steps = (since + step - SimDuration(1)) / step;
    return SimTime{steps * step};
}

} // namespace

LinMaster::LinMaster(LinPort& port, LinScheduleTable schedule, SimDuration step, LinMasterOptions options) :
  port_(port), step_(step), options_(options), schedule_(std::move(schedule))
{
    validate(schedule_, step_, options_);
    remember_frame_configs(schedule_);
    // Last: nothing above may throw once the port knows the master.
    port_.set_listener(&listener_);
}

LinMaster::~LinMaster()
{
    port_.set_listener(nullptr);
}

void LinMaster::init()
{
    port_.open();
}

void LinMaster::on_wakeup(SimTime time)
{
    // The port reports only the pulses of other nodes.
    wakeups_.push(time, WakeupPulse{});
}

void LinMaster::validate(const LinScheduleTable& schedule, SimDuration step, const LinMasterOptions& options)
{
    validate_schedule(schedule, step);
    static_cast<void>(merge_frame_configs(FrameConfigs{}, schedule));
    if (options.wakeup_delay < SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::lin::LinMaster: wakeup_delay is " +
                                    std::to_string(options.wakeup_delay.count()) + " ns; it must be >= 0");
    }
}

LinMaster::FrameConfigs LinMaster::merge_frame_configs(const FrameConfigs& before, const LinScheduleTable& table)
{
    FrameConfigs merged = before;
    for (const auto& entry : table)
    {
        const FrameConfig config{entry.publisher, entry.data_length, entry.checksum_model};
        auto& known = merged[entry.id];
        if (known && *known != config)
        {
            // SIL Kit does not let a controller reconfigure an identifier once configured.
            throw std::invalid_argument("vehicle_sil::lin::LinMaster: schedule entry for id " + std::to_string(entry.id) +
                                        ": an id keeps its data_length, checksum_model and publisher in every "
                                        "schedule given to the master; use another id for the other configuration");
        }
        known = config;
    }
    return merged;
}

void LinMaster::remember_frame_configs(const LinScheduleTable& table)
{
    frame_configs_ = merge_frame_configs(frame_configs_, table);
}

void LinMaster::set_frame_data(LinId id, std::span<const std::uint8_t> data)
{
    if (!valid_lin_frame(id, data.size()))
    {
        throw std::invalid_argument("vehicle_sil::lin::LinMaster: set_frame_data(id " + std::to_string(id) + ", " +
                                    std::to_string(data.size()) + " bytes): the id must be 0..63 and the data 1..8 bytes");
    }
    if (id == kMasterRequestId)
    {
        throw std::invalid_argument("vehicle_sil::lin::LinMaster: set_frame_data(id " + std::to_string(id) +
                                    "): the master request frame is sent with send_master_request()");
    }
    const auto& config = frame_configs_[id];
    if (!config || config->publisher != LinPublisher::Master || config->data_length != data.size())
    {
        throw std::invalid_argument("vehicle_sil::lin::LinMaster: set_frame_data(id " + std::to_string(id) + ", " +
                                    std::to_string(data.size()) +
                                    " bytes): the id is not a master frame of this length in any schedule given to the "
                                    "master; add it with LinPublisher::Master and this data_length to a schedule");
    }
    auto& stored = master_data_[id];
    stored.fill(0);
    std::copy(data.begin(), data.end(), stored.begin());
}

bool LinMaster::send_master_request(std::span<const std::uint8_t> data)
{
    if (data.size() != kDiagnosticFrameLength)
    {
        throw std::invalid_argument("vehicle_sil::lin::LinMaster: send_master_request() got " + std::to_string(data.size()) +
                                    " bytes; a master request has 8 bytes");
    }
    if (data[0] == 0)
    {
        throw std::invalid_argument(
            "vehicle_sil::lin::LinMaster: send_master_request(): NAD 0 is the go-to-sleep command; use request_sleep()");
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
    validate_schedule(schedule, step_);
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

void LinMaster::on_response(SimTime /*header_time*/, const LinFrame& frame, LinResultStatus status)
{
    const std::lock_guard lock(mutex_);
    if (!outstanding_ || frame.id != outstanding_->id)
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
    outstanding_->status = status;
    if (outstanding_->status == LinResultStatus::Ok)
    {
        outstanding_->data_length = std::min<std::uint8_t>(frame.data_length, kMaxLinDataLength);
        outstanding_->data = frame.data;
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
    next_header_time_ = std::max(now, ceil_to_grid(pulse_time + options_.wakeup_delay, step_));
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
            port_.send_wakeup(now); // sends the pulse and makes the port operational
            {
                const std::lock_guard lock(mutex_);
                ++stats_.wakeups_by_master;
            }
            wake(now, now);
        }
        else if (pulse)
        {
            port_.resume();
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
        // Sends the go-to-sleep command and puts the port to sleep.
        port_.send_go_to_sleep(now);
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
    // The port may report the result from inside the call (no response; on the virtual bus
    // every result), so the lock must not be held. SIL Kit reports the master's own frame when
    // it delivers the header back to the master, after this step and before the next one.
    if (entry.publisher == LinPublisher::Master)
    {
        LinFrame frame;
        frame.id = entry.id;
        frame.checksum_model = entry.checksum_model;
        frame.data_length = entry.data_length;
        if (entry.id == kMasterRequestId)
        {
            std::copy(master_request_->begin(), master_request_->end(), frame.data.begin());
            master_request_.reset();
        }
        else
        {
            std::copy_n(master_data_[entry.id].begin(), entry.data_length, frame.data.begin());
        }
        port_.send_frame(now, frame);
    }
    else
    {
        port_.send_header(now, entry.id, entry.data_length, entry.checksum_model);
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
