// SPDX-License-Identifier: MIT
#include "run_can_recorder.hpp"

#include <silkit/services/can/ICanController.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::runner::detail {

namespace {

namespace sk = SilKit::Services::Can;

constexpr sk::CanFrameFlagMask can_flag(sk::CanFrameFlag f) noexcept
{
    return static_cast<sk::CanFrameFlagMask>(f);
}

} // namespace

RunCanRecorder::~RunCanRecorder()
{
    untap();
}

void RunCanRecorder::tap(ParticipantRuntime& runtime, std::vector<std::string> networks)
{
    untap();
    if (networks.empty())
    {
        return;
    }
    if (networks.size() > std::numeric_limits<std::uint16_t>::max())
    {
        throw std::invalid_argument("RunCanRecorder: too many CAN networks");
    }
    auto state = std::make_shared<State>();
    state->untimed_dropped.assign(networks.size(), 0);
    networks_ = std::move(networks);
    state_ = state;
    tapped_ = true;

    try
    {
        for (std::size_t idx = 0; idx < networks_.size(); ++idx)
        {
            const std::string& net = networks_[idx];
            const auto net_idx = static_cast<std::uint16_t>(idx);
            sk::ICanController* const can =
                runtime.participant().CreateCanController("vehicle_sil_can_tap_" + net, net);
            // RX-only passive observer: never calls SetBaudRate() or SendFrame().
            static_cast<void>(can->AddFrameHandler(
                [state, net_idx](sk::ICanController*, const sk::CanFrameEvent& event) {
                    on_frame(*state, net_idx, event);
                },
                static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX)));
            runtime.add_communication_ready_handler([can, state] {
                {
                    const std::lock_guard lock(state->mutex);
                    if (state->closed)
                    {
                        return;
                    }
                }
                can->Start();
            });
        }
    }
    catch (...)
    {
        untap();
        throw;
    }
}

void RunCanRecorder::untap() noexcept
{
    if (state_)
    {
        const std::lock_guard lock(state_->mutex);
        state_->closed = true;
        state_->recording = false;
        state_->queue.clear();
        std::fill(state_->untimed_dropped.begin(), state_->untimed_dropped.end(), 0);
    }
    state_.reset();
    tapped_ = false;
}

bool RunCanRecorder::recording() const noexcept
{
    if (!state_)
    {
        return false;
    }
    const std::lock_guard lock(state_->mutex);
    return !state_->closed && state_->recording;
}

std::string_view RunCanRecorder::network_name(std::uint16_t net_idx) const noexcept
{
    return net_idx < networks_.size() ? std::string_view(networks_[net_idx]) : std::string_view{};
}

void RunCanRecorder::recording_begins() noexcept
{
    if (!state_)
    {
        return;
    }
    const std::lock_guard lock(state_->mutex);
    if (state_->closed)
    {
        return;
    }
    state_->recording = true;
    state_->queue.clear();
    std::fill(state_->untimed_dropped.begin(), state_->untimed_dropped.end(), 0);
}

void RunCanRecorder::recording_ends() noexcept
{
    if (!state_)
    {
        return;
    }
    const std::lock_guard lock(state_->mutex);
    state_->recording = false;
    state_->queue.clear();
    std::fill(state_->untimed_dropped.begin(), state_->untimed_dropped.end(), 0);
}

RunCanRecorder::Taken RunCanRecorder::take()
{
    Taken out;
    if (!state_)
    {
        return out;
    }
    const std::lock_guard lock(state_->mutex);
    out.frames.swap(state_->queue);
    out.untimed_dropped.resize(state_->untimed_dropped.size());
    for (std::size_t i = 0; i < state_->untimed_dropped.size(); ++i)
    {
        out.untimed_dropped[i] = std::exchange(state_->untimed_dropped[i], 0);
    }
    return out;
}

RunCanRecorder::Taken RunCanRecorder::take_and_end()
{
    Taken out;
    if (!state_)
    {
        return out;
    }
    const std::lock_guard lock(state_->mutex);
    out.frames.swap(state_->queue);
    out.untimed_dropped.resize(state_->untimed_dropped.size());
    for (std::size_t i = 0; i < state_->untimed_dropped.size(); ++i)
    {
        out.untimed_dropped[i] = std::exchange(state_->untimed_dropped[i], 0);
    }
    state_->recording = false;
    return out;
}

void RunCanRecorder::initialize_for_test(std::vector<std::string> networks)
{
    untap();
    auto state = std::make_shared<State>();
    state->untimed_dropped.assign(networks.size(), 0);
    networks_ = std::move(networks);
    state_ = std::move(state);
    tapped_ = true;
}

void RunCanRecorder::feed_for_test(std::uint16_t net_idx, const sk::CanFrameEvent& event) noexcept
{
    if (state_)
    {
        on_frame(*state_, net_idx, event);
    }
}

void RunCanRecorder::on_frame(State& state, std::uint16_t net_idx, const sk::CanFrameEvent& event) noexcept
{
    const std::lock_guard lock(state.mutex);
    if (state.closed || !state.recording)
    {
        return;
    }
    // Reject untimed frames (event.timestamp < 0, e.g. before the controller's first step) rather
    // than fabricating a virtual timestamp.
    if (event.timestamp.count() < 0)
    {
        if (net_idx < state.untimed_dropped.size())
        {
            ++state.untimed_dropped[net_idx];
        }
        return;
    }
    CapturedCanFrame f{};
    f.at = from_silkit_time(event.timestamp);
    f.net_idx = net_idx;
    const sk::CanFrame& in = event.frame;
    const bool extended = (in.flags & can_flag(sk::CanFrameFlag::Ide)) != 0;
    const bool rtr = (in.flags & can_flag(sk::CanFrameFlag::Rtr)) != 0;
    const bool fd = (in.flags & can_flag(sk::CanFrameFlag::Fdf)) != 0;
    f.id = in.canId & (extended ? 0x1FFFFFFFu : 0x7FFu);
    f.flags = static_cast<std::uint8_t>((extended ? CapturedCanFrame::kFlagExtended : 0u) |
                                        (rtr ? CapturedCanFrame::kFlagRtr : 0u) |
                                        (fd ? CapturedCanFrame::kFlagFd : 0u));
    f.dlc = static_cast<std::uint8_t>(std::min<std::uint16_t>(in.dlc, 15u));
    const std::size_t max_len = fd ? std::size_t{64} : std::size_t{8};
    const std::size_t len = rtr ? std::size_t{0} : std::min<std::size_t>(in.dataField.size(), max_len);
    f.data_len = static_cast<std::uint8_t>(len);
    if (len > 0)
    {
        std::copy_n(in.dataField.data(), len, f.data.begin());
    }
    try
    {
        state.queue.push_back(f);
    }
    catch (...)
    {
        // Allocation failure on callback thread: do not throw across SIL Kit C++ callback boundary.
    }
}

} // namespace vehicle_sil::runner::detail
