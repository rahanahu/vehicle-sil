// SPDX-License-Identifier: MIT
#include "vehicle_sil/vector_xl/can_bridge.hpp"

#include <silkit/services/can/ICanController.hpp>

#include <algorithm>
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
    };
    std::string line = "stats";
    for (const auto& [key, value] : fields)
    {
        line.append(" ").append(key).append("=").append(std::to_string(value));
    }
    return line;
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
    bool externalStarted = false;
    try
    {
        // External side first, so that frames arriving from SIL Kit can be sent right away.
        external_.start([this](const ExternalCanEvent& event) { on_external_event(event); });
        externalStarted = true;
        silkit_handler_ = silkit_.AddFrameHandler(
            [this](can::ICanController*, const can::CanFrameEvent& event) { on_silkit_frame(event); },
            static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX));
        silkit_.Start();
        running_.store(true);
    }
    catch (...)
    {
        teardown(externalStarted, false);
        throw;
    }
}

void CanBridge::stop() noexcept
{
    const std::lock_guard lock(lifecycle_mutex_);
    if (running_.exchange(false))
    {
        teardown(true, true);
    }
}

void CanBridge::teardown(bool external_started, bool silkit_started) noexcept
{
    // 1. No more external -> SIL Kit traffic: stop() waits for the external receive handler.
    if (external_started)
    {
        external_.stop();
    }
    // 2. No more SIL Kit -> external traffic: RemoveFrameHandler waits for a running handler.
    try
    {
        if (silkit_handler_)
        {
            silkit_.RemoveFrameHandler(*silkit_handler_);
            silkit_handler_.reset();
        }
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
    case ExternalEventKind::Frame:
        break;
    }
    if (!running_.load())
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
    return stats;
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
