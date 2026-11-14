// SPDX-License-Identifier: MIT
#include "vehicle_sil/ethernet_bridge/ethernet_bridge.hpp"

#include <silkit/services/ethernet/IEthernetController.hpp>

#include <array>
#include <exception>
#include <utility>

namespace vehicle_sil::ethernet_bridge {
namespace {

namespace ethernet = SilKit::Services::Ethernet;
using eth_link::FrameVerdict;
using eth_link::LinkEventKind;
using eth_link::LinkState;
using eth_link::MacSideTable;
using eth_link::TransmitResult;

/// Every status but Transmitted: the acknowledgements the bridge counts as failures.
constexpr ethernet::EthernetTransmitStatusMask kFailedTransmissions =
    SilKit_EthernetTransmitStatus_ControllerInactive | SilKit_EthernetTransmitStatus_LinkDown |
    SilKit_EthernetTransmitStatus_Dropped | SilKit_EthernetTransmitStatus_InvalidFrameFormat;

std::string_view describe_status(ethernet::EthernetTransmitStatus status) noexcept
{
    switch (status)
    {
    case ethernet::EthernetTransmitStatus::Transmitted: return "transmitted";
    case ethernet::EthernetTransmitStatus::ControllerInactive: return "the controller is not active";
    case ethernet::EthernetTransmitStatus::LinkDown: return "the link is down";
    case ethernet::EthernetTransmitStatus::Dropped: return "the transmit queue is full";
    case ethernet::EthernetTransmitStatus::InvalidFrameFormat: return "the frame format is invalid";
    }
    return "unknown status";
}

using ProblemDeltas = std::array<std::pair<const char*, std::uint64_t>, 8>;

ProblemDeltas problem_deltas(const EthernetBridgeStatistics& b, const EthernetBridgeStatistics& n) noexcept
{
    return {{
        {"loop_suspected", n.loop_suspected - b.loop_suspected},
        {"mac_unguarded", n.mac_unguarded - b.mac_unguarded},
        {"oversize", n.oversize - b.oversize},
        {"malformed", n.malformed - b.malformed},
        {"ext_overruns", n.ext_overruns - b.ext_overruns},
        {"ext_queue_full", n.ext_queue_full - b.ext_queue_full},
        {"ext_tx_failures", n.ext_tx_failures - b.ext_tx_failures},
        {"silkit_tx_failures", n.silkit_tx_failures - b.silkit_tx_failures},
    }};
}

} // namespace

std::string format_statistics(const EthernetBridgeStatistics& s)
{
    const std::pair<const char*, std::uint64_t> fields[] = {
        {"silkit_to_ext", s.silkit_to_ext},
        {"ext_to_silkit", s.ext_to_silkit},
        {"own_tx_dropped", s.own_tx_dropped},
        {"loop_suspected", s.loop_suspected},
        {"mac_unguarded", s.mac_unguarded},
        {"filtered_to_ext", s.filtered_to_ext},
        {"filtered_from_ext", s.filtered_from_ext},
        {"oversize", s.oversize},
        {"malformed", s.malformed},
        {"ext_overruns", s.ext_overruns},
        {"ext_queue_full", s.ext_queue_full},
        {"ext_tx_failures", s.ext_tx_failures},
        {"silkit_tx_failures", s.silkit_tx_failures},
    };
    std::string line = "stats";
    for (const auto& [key, value] : fields)
    {
        line.append(" ").append(key).append("=").append(std::to_string(value));
    }
    return line;
}

bool problems_grew(const EthernetBridgeStatistics& before, const EthernetBridgeStatistics& now) noexcept
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

std::optional<std::string> format_health_change(const EthernetBridgeHealth& before, const EthernetBridgeHealth& now,
                                                std::string_view interface_name, bool problems_before)
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
    const auto from = before.link_state;
    const auto to = now.link_state;
    const bool firstHealthyReport = from == LinkState::Unknown && to == LinkState::Up;
    const std::string line = "interface " + std::string(interface_name) + ": " + std::string(eth_link::to_string(to));
    if (!changes.empty() || (from != to && !firstHealthyReport))
    {
        return line + changes;
    }
    if (problems_before)
    {
        return line + "; no new problems since the last report";
    }
    return std::nullopt;
}

EthernetBridge::EthernetBridge(ethernet::IEthernetController& silkit, eth_link::ExternalLink& external,
                               EthernetBridgeOptions options) :
  silkit_(silkit),
  external_(external),
  filter_(std::move(options.filter)),
  table_(options.mac_guard ? std::make_unique<MacSideTable>(options.mac_table_size) : nullptr)
{
    if (table_)
    {
        for (const auto& address : filter_.options().silkit_macs)
        {
            (void)table_->add_silkit_address(address);
        }
    }
}

EthernetBridge::~EthernetBridge()
{
    stop();
}

void EthernetBridge::start()
{
    const std::lock_guard lock(lifecycle_mutex_);
    if (running_.load())
    {
        return;
    }
    {
        const std::lock_guard errors(error_mutex_);
        link_state_ = LinkState::Unknown;
        fault_.reset();
        warnings_.clear();
    }
    bool externalStarted = false;
    bool activated = false;
    try
    {
        link_echoes_ = external_.echoes_own_transmissions();
        // The link first, so that frames arriving from SIL Kit can be sent right away.
        external_.start([this](const eth_link::LinkEvent& event) { on_external_event(event); });
        externalStarted = true;
        frame_handler_ = silkit_.AddFrameHandler(
            [this](ethernet::IEthernetController*, const ethernet::EthernetFrameEvent& event) { on_silkit_frame(event); },
            static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX));
        transmit_handler_ = silkit_.AddFrameTransmitHandler(
            [this](ethernet::IEthernetController*, const ethernet::EthernetFrameTransmitEvent& event) {
                on_silkit_transmit(event);
            },
            kFailedTransmissions);
        if (communication_ready_)
        {
            // A start after the participant's CommunicationReady (a restart): nothing else will.
            silkit_.Activate();
            activated = true;
            controller_started_.store(true);
        }
        running_.store(true);
    }
    catch (...)
    {
        teardown(externalStarted, activated);
        throw;
    }
}

void EthernetBridge::start_controller() noexcept
{
    const std::lock_guard lock(lifecycle_mutex_);
    communication_ready_ = true;
    if (!running_.load() || controller_started_.load())
    {
        return; // not running: start() activates the controller
    }
    try
    {
        silkit_.Activate();
        controller_started_.store(true);
    }
    catch (const std::exception& e)
    {
        // Nothing can go to SIL Kit: a fault, so that the caller stops.
        std::string why = std::string("activating the SIL Kit Ethernet controller failed: ") + e.what();
        const std::lock_guard errors(error_mutex_);
        last_error_ = why;
        fault_ = std::move(why);
    }
}

void EthernetBridge::stop() noexcept
{
    const std::lock_guard lock(lifecycle_mutex_);
    if (running_.exchange(false))
    {
        teardown(true, controller_started_.exchange(false));
    }
}

void EthernetBridge::teardown(bool external_started, bool silkit_activated) noexcept
{
    // 1. No more SIL Kit -> link traffic: RemoveFrameHandler waits for a running handler.
    try
    {
        if (frame_handler_)
        {
            silkit_.RemoveFrameHandler(*frame_handler_);
            frame_handler_.reset();
        }
    }
    catch (const std::exception& e)
    {
        set_last_error("stopping the SIL Kit side failed: ", e.what());
    }
    // 2. No more link -> SIL Kit traffic: stop() waits for the link's receive handler (and may
    //    report the last overruns).
    if (external_started)
    {
        external_.stop();
    }
    // 3. The transmit acknowledgements and the controller last: nothing sends any more.
    try
    {
        if (transmit_handler_)
        {
            silkit_.RemoveFrameTransmitHandler(*transmit_handler_);
            transmit_handler_.reset();
        }
        if (silkit_activated)
        {
            silkit_.Deactivate();
        }
    }
    catch (const std::exception& e)
    {
        set_last_error("stopping the SIL Kit side failed: ", e.what());
    }
}

void EthernetBridge::on_silkit_frame(const ethernet::EthernetFrameEvent& event)
{
    if (!running_.load())
    {
        return;
    }
    const std::span<const std::uint8_t> frame(event.frame.raw.data(), event.frame.raw.size());
    switch (filter_.to_external(frame))
    {
    case FrameVerdict::Pass: break;
    case FrameVerdict::Filtered: filtered_to_ext_.fetch_add(1); return;
    case FrameVerdict::Oversize:
        oversize_.fetch_add(1);
        set_last_error("dropped a frame from SIL Kit longer than the maximum frame length");
        return;
    case FrameVerdict::Malformed:
        malformed_.fetch_add(1);
        set_last_error("dropped a frame from SIL Kit shorter than an Ethernet header");
        return;
    }
    if (table_)
    {
        const auto source = *eth_link::source_of(frame);
        const auto check = table_->on_silkit_source(source);
        if (check.table_full)
        {
            warn_table_full();
        }
        switch (check.outcome)
        {
        case MacSideTable::FromSilKit::Forward: break;
        case MacSideTable::FromSilKit::NewConflict:
            loop_suspected_.fetch_add(1);
            warn_conflict(source, "the external link", "SIL Kit");
            return;
        case MacSideTable::FromSilKit::Blocked: loop_suspected_.fetch_add(1); return;
        }
        if (check.not_learned)
        {
            mac_unguarded_.fetch_add(1);
        }
    }
    TransmitResult result = TransmitResult::Failed;
    try
    {
        result = external_.transmit(frame);
    }
    catch (const std::exception& e)
    {
        // Breaks the link's contract; not let through into SIL Kit's receive thread.
        ext_tx_failures_.fetch_add(1);
        set_last_error("sending on the external link threw: ", e.what());
        return;
    }
    catch (...)
    {
        ext_tx_failures_.fetch_add(1);
        set_last_error("sending on the external link threw an unknown exception");
        return;
    }
    switch (result)
    {
    case TransmitResult::Ok: silkit_to_ext_.fetch_add(1); break;
    case TransmitResult::QueueFull:
        ext_queue_full_.fetch_add(1);
        set_last_error("the external link's transmit queue is full, frame dropped");
        break;
    case TransmitResult::Failed:
        ext_tx_failures_.fetch_add(1);
        set_last_error("sending on the external link failed: ", external_.last_error());
        break;
    }
}

void EthernetBridge::on_silkit_transmit(const ethernet::EthernetFrameTransmitEvent& event)
{
    if (event.status == ethernet::EthernetTransmitStatus::Transmitted)
    {
        return;
    }
    silkit_tx_failures_.fetch_add(1);
    set_last_error("SIL Kit did not transmit a frame from the external link: ", describe_status(event.status));
}

void EthernetBridge::on_external_event(const eth_link::LinkEvent& event)
{
    switch (event.kind)
    {
    case LinkEventKind::Frame: on_external_frame(event.frame); return;
    case LinkEventKind::Overrun:
        ext_overruns_.fetch_add(event.count);
        set_last_error("the OS lost frames received on the external link");
        return;
    case LinkEventKind::LinkState:
    {
        const std::lock_guard lock(error_mutex_);
        link_state_ = event.state;
        return;
    }
    case LinkEventKind::Fault:
    {
        std::string why(event.detail);
        if (why.empty())
        {
            why = "the external link stopped working";
        }
        const std::lock_guard lock(error_mutex_);
        last_error_ = why;
        fault_ = std::move(why);
        return;
    }
    case LinkEventKind::Warning: warn(std::string(event.detail)); return;
    }
}

void EthernetBridge::on_external_frame(std::span<const std::uint8_t> frame)
{
    // Before the controller is activated, SIL Kit refuses what it is given (ControllerInactive).
    if (!running_.load() || !controller_started_.load())
    {
        return;
    }
    const auto verdict = filter_.from_external(frame);
    switch (verdict)
    {
    case FrameVerdict::Pass:
    case FrameVerdict::Filtered: break; // the bridge's own frames are told apart first
    case FrameVerdict::Oversize:
        oversize_.fetch_add(1);
        set_last_error("dropped a frame from the external link longer than the maximum frame length");
        return;
    case FrameVerdict::Malformed:
        malformed_.fetch_add(1);
        set_last_error("dropped a frame from the external link shorter than an Ethernet header");
        return;
    }
    if (table_)
    {
        const auto source = *eth_link::source_of(frame);
        const auto check = table_->on_external_source(source, verdict == FrameVerdict::Pass);
        if (check.table_full)
        {
            warn_table_full();
        }
        switch (check.outcome)
        {
        case MacSideTable::FromExternal::Forward: break;
        case MacSideTable::FromExternal::SilKitSide:
            if (is_own_frame_coming_back(frame))
            {
                own_tx_dropped_.fetch_add(1);
                return;
            }
            // The link does not echo: someone else outside sends with an address of the SIL Kit side.
            loop_suspected_.fetch_add(1);
            if (table_->mark_both_sides(source))
            {
                warn_conflict(source, "SIL Kit", "the external link");
            }
            return;
        case MacSideTable::FromExternal::Blocked: loop_suspected_.fetch_add(1); return;
        }
        if (check.not_learned)
        {
            mac_unguarded_.fetch_add(1);
        }
    }
    if (verdict == FrameVerdict::Filtered)
    {
        filtered_from_ext_.fetch_add(1);
        return;
    }
    try
    {
        // SendFrame copies the bytes: the link's buffer is only valid during this call.
        silkit_.SendFrame(ethernet::EthernetFrame{SilKit::Util::Span<const std::uint8_t>(frame.data(), frame.size())});
        ext_to_silkit_.fetch_add(1);
    }
    catch (const std::exception& e)
    {
        silkit_tx_failures_.fetch_add(1);
        set_last_error("SIL Kit SendFrame failed: ", e.what());
    }
    catch (...)
    {
        silkit_tx_failures_.fetch_add(1);
        set_last_error("SIL Kit SendFrame failed with an unknown exception");
    }
}

bool EthernetBridge::is_own_frame_coming_back(std::span<const std::uint8_t> /*frame*/) const noexcept
{
    return link_echoes_;
}

EthernetBridgeStatistics EthernetBridge::statistics() const noexcept
{
    EthernetBridgeStatistics s;
    s.silkit_to_ext = silkit_to_ext_.load();
    s.ext_to_silkit = ext_to_silkit_.load();
    s.own_tx_dropped = own_tx_dropped_.load();
    s.loop_suspected = loop_suspected_.load();
    s.mac_unguarded = mac_unguarded_.load();
    s.filtered_to_ext = filtered_to_ext_.load();
    s.filtered_from_ext = filtered_from_ext_.load();
    s.oversize = oversize_.load();
    s.malformed = malformed_.load();
    s.ext_overruns = ext_overruns_.load();
    s.ext_queue_full = ext_queue_full_.load();
    s.ext_tx_failures = ext_tx_failures_.load();
    s.silkit_tx_failures = silkit_tx_failures_.load();
    return s;
}

EthernetBridgeHealth EthernetBridge::health() const
{
    EthernetBridgeHealth result;
    result.statistics = statistics();
    const std::lock_guard lock(error_mutex_);
    result.link_state = link_state_;
    return result;
}

std::optional<std::string> EthernetBridge::fault() const
{
    const std::lock_guard lock(error_mutex_);
    return fault_;
}

std::vector<std::string> EthernetBridge::take_warnings()
{
    const std::lock_guard lock(error_mutex_);
    return std::exchange(warnings_, {});
}

std::string EthernetBridge::last_error() const
{
    const std::lock_guard lock(error_mutex_);
    return last_error_;
}

void EthernetBridge::set_last_error(std::string_view message, std::string_view detail)
{
    // Reuses the buffer: this runs on the receive threads when frames are dropped.
    const std::lock_guard lock(error_mutex_);
    last_error_.assign(message).append(detail);
}

void EthernetBridge::warn(std::string text)
{
    const std::lock_guard lock(error_mutex_);
    warnings_.push_back(std::move(text));
}

void EthernetBridge::warn_conflict(const eth_link::MacAddress& address, std::string_view seen_first,
                                   std::string_view seen_now)
{
    const std::lock_guard lock(error_mutex_);
    if (named_conflicts_ < kMaxNamedConflicts)
    {
        warnings_.push_back("MAC " + eth_link::to_string(address) + " is on both sides: frames from it came from " +
                            std::string(seen_first) + " and now come from " + std::string(seen_now) +
                            ", so frames from it are dropped both ways (loop_suspected). Run one bridge between "
                            "these networks and give each device its own MAC address, then restart the bridge: "
                            "it keeps dropping them until it restarts.");
    }
    else if (named_conflicts_ == kMaxNamedConflicts)
    {
        warnings_.push_back("more MAC addresses are on both sides than the first " + std::to_string(kMaxNamedConflicts) +
                            " named: frames from them are dropped both ways too (loop_suspected).");
    }
    ++named_conflicts_;
}

void EthernetBridge::warn_table_full()
{
    warn("the MAC table is full (" + std::to_string(table_->capacity()) +
         " addresses): frames from addresses that do not fit are forwarded without the check against loops "
         "(mac_unguarded); addresses of the SIL Kit side take the places of the oldest addresses outside. Raise "
         "--mac-table-size if this many devices are expected.");
}

} // namespace vehicle_sil::ethernet_bridge
