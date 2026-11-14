// SPDX-License-Identifier: MIT
#include "vehicle_sil/vector_xl/can_bridge.hpp"

#include <silkit/services/can/ICanController.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::vector_xl {
namespace {

namespace can = SilKit::Services::Can;

constexpr can::CanFrameFlagMask flag(can::CanFrameFlag f) noexcept
{
    return static_cast<can::CanFrameFlagMask>(f);
}

constexpr can::CanFrameFlagMask kClassicFlags = flag(can::CanFrameFlag::Ide) | flag(can::CanFrameFlag::Rtr);
constexpr std::uint32_t kMaxExtendedId = 0x1FFF'FFFFu;
constexpr std::uint32_t kMaxStandardId = 0x7FFu;
constexpr std::uint16_t kMaxClassicDlc = 15;
constexpr std::size_t kMaxClassicPayload = 8;

} // namespace

std::string format_statistics(const CanBridgeStatistics& s)
{
    const std::pair<const char*, std::uint64_t> fields[] = {
        {"silkit_to_xl", s.silkit_to_external},
        {"xl_to_silkit", s.external_to_silkit},
        {"own_tx_dropped", s.own_transmissions_dropped},
        {"error_frames", s.external_error_frames},
        {"overruns", s.external_overruns},
        {"silkit_rejected", s.silkit_rejected},
        {"xl_rejected", s.external_rejected},
        {"xl_queue_full", s.external_queue_full},
        {"xl_tx_failures", s.external_transmit_failures},
        {"silkit_tx_failures", s.silkit_transmit_failures},
        {"xl_tx_dropped", s.external_transmit_dropped},
        {"xl_tx_unsent", s.external_transmit_unsent},
    };
    std::string line = "stats";
    for (const auto& [key, value] : fields)
    {
        line.append(" ").append(key).append("=").append(std::to_string(value));
    }
    return line;
}

std::string_view to_string(ExternalBusState::Level level) noexcept
{
    switch (level)
    {
    case ExternalBusState::Level::ErrorActive: return "error active";
    case ExternalBusState::Level::ErrorWarning: return "error warning";
    case ExternalBusState::Level::ErrorPassive: return "error passive";
    case ExternalBusState::Level::BusOff: return "bus off";
    case ExternalBusState::Level::Unknown: break;
    }
    return "state unknown";
}

namespace {

using ProblemDeltas = std::array<std::pair<const char*, std::uint64_t>, 8>;

ProblemDeltas problem_deltas(const CanBridgeStatistics& b, const CanBridgeStatistics& n) noexcept
{
    return {{
        {"error_frames", n.external_error_frames - b.external_error_frames},
        {"overruns", n.external_overruns - b.external_overruns},
        {"silkit_rejected", n.silkit_rejected - b.silkit_rejected},
        {"xl_rejected", n.external_rejected - b.external_rejected},
        {"xl_queue_full", n.external_queue_full - b.external_queue_full},
        {"xl_tx_failures", n.external_transmit_failures - b.external_transmit_failures},
        {"silkit_tx_failures", n.silkit_transmit_failures - b.silkit_transmit_failures},
        {"xl_tx_dropped", n.external_transmit_dropped - b.external_transmit_dropped},
    }};
}

} // namespace

bool problems_grew(const CanBridgeStatistics& before, const CanBridgeStatistics& now) noexcept
{
    for (const auto& [key, delta] : problem_deltas(before, now))
    {
        if (delta != 0)
        {
            return true;
        }
    }
    return false;
}

std::string format_bus_state(const ExternalBusState& state)
{
    std::string text(to_string(state.level));
    if (state.level != ExternalBusState::Level::Unknown)
    {
        text.append(" (tx errors ")
            .append(std::to_string(state.tx_errors))
            .append(", rx errors ")
            .append(std::to_string(state.rx_errors))
            .append(")");
    }
    return text;
}

std::optional<std::string> format_health_change(const CanBridgeHealth& before, const CanBridgeHealth& now,
                                                bool problems_before)
{
    std::string changes;
    for (const auto& [key, delta] : problem_deltas(before.statistics, now.statistics))
    {
        if (delta != 0)
        {
            changes.append(changes.empty() ? "; since the last report: " : ", ")
                .append(key)
                .append(" +")
                .append(std::to_string(delta));
        }
    }
    const auto from = before.bus_state.level;
    const auto to = now.bus_state.level;
    const bool firstHealthyAnswer = from == ExternalBusState::Level::Unknown && to == ExternalBusState::Level::ErrorActive;
    const std::string line = "XL bus: " + format_bus_state(now.bus_state);
    if (!changes.empty() || (from != to && !firstHealthyAnswer))
    {
        return line + changes;
    }
    if (problems_before)
    {
        return line + "; no new problems since the last report";
    }
    return std::nullopt;
}

std::string_view validate_classic_frame(const can::CanFrame& frame) noexcept
{
    if ((frame.flags & ~kClassicFlags) != 0)
    {
        return "not a classic CAN frame (CAN FD/XL or unknown flags)";
    }
    const bool extended = (frame.flags & flag(can::CanFrameFlag::Ide)) != 0;
    if (frame.canId > (extended ? kMaxExtendedId : kMaxStandardId))
    {
        return extended ? "extended identifier exceeds 29 bits" : "standard identifier exceeds 11 bits";
    }
    if (frame.dlc > kMaxClassicDlc)
    {
        return "DLC exceeds 15";
    }
    const bool remote = (frame.flags & flag(can::CanFrameFlag::Rtr)) != 0;
    const std::size_t expected = remote ? 0 : std::min<std::size_t>(frame.dlc, kMaxClassicPayload);
    if (frame.dataField.size() != expected)
    {
        return remote ? "remote frame carries data" : "data length does not match DLC";
    }
    return {};
}

CanBridge::CanBridge(can::ICanController& silkit, ExternalCanPort& external) :
  silkit_(silkit),
  external_(external)
{
}

CanBridge::~CanBridge()
{
    stop();
}

void CanBridge::start()
{
    const std::lock_guard lock(lifecycle_mutex_);
    if (running_.load())
    {
        return;
    }
    {
        const std::lock_guard errors(error_mutex_);
        bus_state_ = {};
        fault_.reset();
        warnings_.clear();
    }
    bool externalStarted = false;
    try
    {
        // External side first, so that frames arriving from SIL Kit can be sent right away.
        external_.start([this](const ExternalCanEvent& event) { on_external_event(event); });
        externalStarted = true;
        silkit_handler_ = silkit_.AddFrameHandler(
            [this](can::ICanController*, const can::CanFrameEvent& event) { on_silkit_frame(event); },
            static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX));
        if (communication_ready_)
        {
            // A start after the participant's CommunicationReady (a restart): nothing else will.
            silkit_.Start();
            controller_started_.store(true);
        }
        running_.store(true);
    }
    catch (...)
    {
        teardown(externalStarted, false);
        throw;
    }
}

void CanBridge::start_controller() noexcept
{
    const std::lock_guard lock(lifecycle_mutex_);
    communication_ready_ = true;
    if (!running_.load() || controller_started_.load())
    {
        return; // not running: start() starts the controller
    }
    try
    {
        silkit_.Start();
        controller_started_.store(true);
    }
    catch (const std::exception& e)
    {
        // Nothing can go to SIL Kit: a fault, so that the caller stops.
        std::string why = std::string("starting the SIL Kit CAN controller failed: ") + e.what();
        const std::lock_guard errors(error_mutex_);
        last_error_ = why;
        fault_ = std::move(why);
    }
}

void CanBridge::stop() noexcept
{
    const std::lock_guard lock(lifecycle_mutex_);
    if (running_.exchange(false))
    {
        teardown(true, controller_started_.exchange(false));
    }
}

void CanBridge::teardown(bool external_started, bool silkit_started) noexcept
{
    // 1. No more SIL Kit -> external traffic: RemoveFrameHandler waits for a running handler.
    //    First, so that the external port can still send what it accepted while it stops.
    try
    {
        if (silkit_handler_)
        {
            silkit_.RemoveFrameHandler(*silkit_handler_);
            silkit_handler_.reset();
        }
    }
    catch (const std::exception& e)
    {
        set_last_error("stopping the SIL Kit side failed: ", e.what());
    }
    // 2. No more external -> SIL Kit traffic: stop() waits for the external receive handler
    //    (and reports what it could not send as TransmitUnsent).
    if (external_started)
    {
        external_.stop();
    }
    // 3. The controller last: nothing uses it any more.
    try
    {
        if (silkit_started)
        {
            silkit_.Stop();
        }
    }
    catch (const std::exception& e)
    {
        set_last_error("stopping the SIL Kit side failed: ", e.what());
    }
}

void CanBridge::on_silkit_frame(const can::CanFrameEvent& event)
{
    if (!running_.load())
    {
        return;
    }
    if (const auto error = validate_classic_frame(event.frame); !error.empty())
    {
        silkit_rejected_.fetch_add(1);
        set_last_error("rejected SIL Kit frame: ", error);
        return;
    }
    switch (external_.transmit(event.frame))
    {
    case TransmitResult::Ok:
        silkit_to_external_.fetch_add(1);
        break;
    case TransmitResult::QueueFull:
        external_queue_full_.fetch_add(1);
        set_last_error("external transmit queue full, frame dropped");
        break;
    case TransmitResult::Failed:
        external_transmit_failures_.fetch_add(1);
        set_last_error("external transmit failed: ", external_.last_error());
        break;
    case TransmitResult::Dropped:
        external_transmit_dropped_.fetch_add(1);
        set_last_error("frame dropped: the external bus did not take the frames before it in time");
        break;
    }
}

void CanBridge::on_external_event(const ExternalCanEvent& event)
{
    switch (event.kind)
    {
    case ExternalEventKind::OwnTransmission:
        own_transmissions_dropped_.fetch_add(1);
        return;
    case ExternalEventKind::ErrorFrame:
        external_error_frames_.fetch_add(1);
        return;
    case ExternalEventKind::Overrun:
        external_overruns_.fetch_add(1);
        set_last_error("external receive queue overrun, frames were lost");
        return;
    case ExternalEventKind::BusState:
    {
        const std::lock_guard lock(error_mutex_);
        bus_state_ = event.bus_state;
        return;
    }
    case ExternalEventKind::TransmitUnsent:
        external_transmit_unsent_.fetch_add(event.count);
        return;
    case ExternalEventKind::Fault:
    {
        // The reason comes with the event: last_error() may already tell of a later transmit.
        std::string why(event.detail);
        if (why.empty())
        {
            why = "the external port stopped working";
        }
        const std::lock_guard lock(error_mutex_);
        last_error_ = why;
        fault_ = std::move(why);
        return;
    }
    case ExternalEventKind::Warning:
    {
        const std::lock_guard lock(error_mutex_);
        warnings_.emplace_back(event.detail);
        return;
    }
    case ExternalEventKind::Frame:
        break;
    }
    // Before the controller is started, SIL Kit drops what it is given without telling.
    if (!running_.load() || !controller_started_.load())
    {
        return;
    }
    if (const auto error = validate_classic_frame(event.frame); !error.empty())
    {
        external_rejected_.fetch_add(1);
        set_last_error("rejected external frame: ", error);
        return;
    }
    try
    {
        silkit_.SendFrame(event.frame);
        external_to_silkit_.fetch_add(1);
    }
    catch (const std::exception& e)
    {
        silkit_transmit_failures_.fetch_add(1);
        set_last_error("SIL Kit SendFrame failed: ", e.what());
    }
}

CanBridgeStatistics CanBridge::statistics() const noexcept
{
    CanBridgeStatistics stats;
    stats.silkit_to_external = silkit_to_external_.load();
    stats.external_to_silkit = external_to_silkit_.load();
    stats.own_transmissions_dropped = own_transmissions_dropped_.load();
    stats.external_error_frames = external_error_frames_.load();
    stats.external_overruns = external_overruns_.load();
    stats.silkit_rejected = silkit_rejected_.load();
    stats.external_rejected = external_rejected_.load();
    stats.external_queue_full = external_queue_full_.load();
    stats.external_transmit_failures = external_transmit_failures_.load();
    stats.silkit_transmit_failures = silkit_transmit_failures_.load();
    stats.external_transmit_dropped = external_transmit_dropped_.load();
    stats.external_transmit_unsent = external_transmit_unsent_.load();
    return stats;
}

CanBridgeHealth CanBridge::health() const
{
    CanBridgeHealth result;
    result.statistics = statistics();
    const std::lock_guard lock(error_mutex_);
    result.bus_state = bus_state_;
    return result;
}

std::optional<std::string> CanBridge::fault() const
{
    const std::lock_guard lock(error_mutex_);
    return fault_;
}

std::vector<std::string> CanBridge::take_warnings()
{
    const std::lock_guard lock(error_mutex_);
    return std::exchange(warnings_, {});
}

std::string CanBridge::last_error() const
{
    const std::lock_guard lock(error_mutex_);
    return last_error_;
}

void CanBridge::set_last_error(std::string_view message, std::string_view detail)
{
    // Reuses the buffer: this runs on the receive threads when frames are dropped.
    const std::lock_guard lock(error_mutex_);
    last_error_.assign(message).append(detail);
}

} // namespace vehicle_sil::vector_xl
