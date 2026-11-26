// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_silkit/silkit_eth_port.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>

namespace vehicle_sil::eth {
namespace {

namespace sk = SilKit::Services::Ethernet;

constexpr const char* kWho = "vehicle_sil::eth::SilKitEthPort: ";
constexpr std::size_t kMinCorrTableCapacity = 1024;
constexpr std::uint64_t kContextMarker = 0xA7ull << 56;
constexpr std::uint64_t kContextMarkerMask = 0xFFull << 56;
constexpr std::uint32_t kGenMask = 0x00FF'FFFFu;

static_assert(sizeof(void*) == sizeof(std::uint64_t),
              "SilKitEthPort 0xA7 userContext correlation requires 64-bit pointers");

void* encode_context(std::uint32_t slot, std::uint32_t gen) noexcept
{
    const std::uint64_t bits =
        kContextMarker | (static_cast<std::uint64_t>(gen & kGenMask) << 32) | static_cast<std::uint64_t>(slot);
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(bits));
}

bool decode_context(const void* context, std::uint32_t& slot, std::uint32_t& gen) noexcept
{
    const auto bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(context));
    if ((bits & kContextMarkerMask) != kContextMarker)
    {
        return false;
    }
    slot = static_cast<std::uint32_t>(bits & 0xFFFF'FFFFull);
    gen = static_cast<std::uint32_t>((bits >> 32) & kGenMask);
    return true;
}

std::size_t validated_tx_queue_capacity(const SilKitEthPortOptions& options)
{
    SilKitEthPort::validate(options);
    return options.transmit_queue_capacity;
}

} // namespace

void SilKitEthPort::validate(const SilKitEthPortOptions& options)
{
    if (options.inbox_capacity == 0)
    {
        throw std::invalid_argument(std::string(kWho) + "inbox_capacity must be > 0");
    }
    if (options.event_capacity == 0)
    {
        throw std::invalid_argument(std::string(kWho) + "event_capacity must be > 0");
    }
    if (options.transmit_queue_capacity > options.event_capacity)
    {
        throw std::invalid_argument(std::string(kWho) + "transmit_queue_capacity must be <= event_capacity");
    }
    if (options.event_capacity > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::invalid_argument(std::string(kWho) + "event_capacity exceeds 32-bit correlation slot limit");
    }
}

SilKitEthPort::SilKitEthPort(sk::IEthernetController& controller, SilKitEthPortOptions options) :
  controller_(controller),
  transmit_queue_capacity_(validated_tx_queue_capacity(options)),
  inbox_(options.inbox_capacity),
  events_(options.event_capacity)
{
    const std::size_t corrCapacity =
        std::max<std::size_t>({options.event_capacity, options.transmit_queue_capacity, kMinCorrTableCapacity});
    corr_slots_.resize(corrCapacity);
    corr_free_ring_.resize(corrCapacity);
    for (std::size_t i = 0; i < corrCapacity; ++i)
    {
        corr_free_ring_[i] = static_cast<std::uint32_t>(i);
    }
    corr_free_head_ = 0;
    corr_free_tail_ = 0;
    corr_free_count_ = corrCapacity;

    const std::lock_guard lock(lifecycle_mutex_);
    try
    {
        frame_handler_ = controller_.AddFrameHandler(
            [this](sk::IEthernetController*, const sk::EthernetFrameEvent& event) { on_frame(event); },
            static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX));
        transmit_handler_ = controller_.AddFrameTransmitHandler(
            [this](sk::IEthernetController*, const sk::EthernetFrameTransmitEvent& event) { on_transmit(event); },
            SilKit_EthernetTransmitStatus_DefaultMask);
        state_handler_ = controller_.AddStateChangeHandler(
            [this](sk::IEthernetController*, const sk::EthernetStateChangeEvent& event) {
                link_up_.store(event.state == sk::EthernetState::LinkUp, std::memory_order_relaxed);
            });
        open_.store(true, std::memory_order_release);
    }
    catch (...)
    {
        // Leave nothing behind that could call into a half-built object.
        try
        {
            if (frame_handler_)
            {
                controller_.RemoveFrameHandler(*frame_handler_);
            }
            if (transmit_handler_)
            {
                controller_.RemoveFrameTransmitHandler(*transmit_handler_);
            }
            if (state_handler_)
            {
                controller_.RemoveStateChangeHandler(*state_handler_);
            }
        }
        catch (...)
        {
        }
        throw;
    }
}

SilKitEthPort::~SilKitEthPort()
{
    close();
}

void SilKitEthPort::start()
{
    const std::lock_guard lock(lifecycle_mutex_);
    if (!is_open() || is_started())
    {
        return;
    }
    controller_.Activate();
    started_.store(true, std::memory_order_release);
}

void SilKitEthPort::require_started(const char* what) const
{
    if (!is_started())
    {
        throw std::logic_error(std::string("vehicle_sil::eth::SilKitEthPort: ") + what +
                               " before start(): the controller is not activated, so SIL Kit would refuse every "
                               "frame this port sends. Call start() from the participant's CommunicationReady handler "
                               "(ParticipantRuntime::add_communication_ready_handler())");
    }
}

void SilKitEthPort::close() noexcept
{
    const std::lock_guard lock(lifecycle_mutex_);
    open_.store(false, std::memory_order_release);
    const bool wasStarted = started_.exchange(false, std::memory_order_acq_rel);
    // RemoveFrameHandler waits for a running handler call to finish, so afterwards SIL Kit no
    // longer touches `this`.
    try
    {
        if (frame_handler_)
        {
            controller_.RemoveFrameHandler(*frame_handler_);
        }
    }
    catch (...)
    {
    }
    try
    {
        if (transmit_handler_)
        {
            controller_.RemoveFrameTransmitHandler(*transmit_handler_);
        }
    }
    catch (...)
    {
    }
    try
    {
        if (state_handler_)
        {
            controller_.RemoveStateChangeHandler(*state_handler_);
        }
    }
    catch (...)
    {
    }
    frame_handler_.reset();
    transmit_handler_.reset();
    state_handler_.reset();
    if (wasStarted)
    {
        try
        {
            controller_.Deactivate();
        }
        catch (...)
        {
        }
    }
    inbox_.clear();
    events_.clear();
    {
        const std::lock_guard overflowLock(overflow_mutex_);
        overflow_.clear();
        event_overflow_.clear();
    }
    {
        const std::lock_guard corrLock(corr_mutex_);
        clear_corr_table_locked();
    }
    // Keep outstanding_ inspectable after close() so callers can detect in-flight frames lost
    // at shutdown.
}

bool SilKitEthPort::allocate_slot_locked(std::uint64_t token, SimTime send_time, std::uint32_t& out_slot,
                                         std::uint32_t& out_gen) noexcept
{
    if (corr_free_count_ == 0)
    {
        return false;
    }
    const std::uint32_t slot = corr_free_ring_[corr_free_head_];
    corr_free_head_ = (corr_free_head_ + 1) % corr_free_ring_.size();
    --corr_free_count_;
    CorrelationSlot& entry = corr_slots_[slot];
    entry.token = token;
    entry.send_time = send_time;
    entry.in_use = true;
    out_slot = slot;
    out_gen = entry.generation;
    return true;
}

bool SilKitEthPort::resolve_and_free_locked(std::uint32_t slot, std::uint32_t gen, std::uint64_t& out_token,
                                            SimTime& out_send_time) noexcept
{
    if (slot >= corr_slots_.size())
    {
        return false;
    }
    CorrelationSlot& entry = corr_slots_[slot];
    if (!entry.in_use || entry.generation != gen)
    {
        return false;
    }
    out_token = entry.token;
    out_send_time = entry.send_time;
    entry.in_use = false;
    entry.generation = (entry.generation + 1u) & kGenMask;
    corr_free_ring_[corr_free_tail_] = slot;
    corr_free_tail_ = (corr_free_tail_ + 1) % corr_free_ring_.size();
    ++corr_free_count_;
    return true;
}

bool SilKitEthPort::reclaim_if_unused_locked(std::uint32_t slot, std::uint32_t gen) noexcept
{
    std::uint64_t unusedToken = 0;
    SimTime unusedSendTime{};
    return resolve_and_free_locked(slot, gen, unusedToken, unusedSendTime);
}

void SilKitEthPort::clear_corr_table_locked() noexcept
{
    for (std::size_t i = 0; i < corr_slots_.size(); ++i)
    {
        if (corr_slots_[i].in_use)
        {
            corr_slots_[i].in_use = false;
            corr_slots_[i].generation = (corr_slots_[i].generation + 1u) & kGenMask;
        }
        corr_free_ring_[i] = static_cast<std::uint32_t>(i);
    }
    corr_free_head_ = 0;
    corr_free_tail_ = 0;
    corr_free_count_ = corr_slots_.size();
}

SimTime SilKitEthPort::stamp(std::chrono::nanoseconds timestamp) const noexcept
{
    if (timestamp < std::chrono::nanoseconds::zero())
    {
        return SimTime{} + std::chrono::nanoseconds(step_time_ns_.load(std::memory_order_acquire));
    }
    return from_silkit_time(timestamp);
}

void SilKitEthPort::on_frame(const sk::EthernetFrameEvent& event)
{
    const auto raw = event.frame.raw;
    Frame frame;
    if (!frame.assign(std::span<const std::uint8_t>(raw.data(), raw.size())))
    {
        receive_rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const SimTime time = stamp(event.timestamp);
    if (inbox_.push(time, frame))
    {
        received_.fetch_add(1, std::memory_order_relaxed);
        if (event.timestamp < std::chrono::nanoseconds::zero())
        {
            received_without_time_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const std::lock_guard lock(overflow_mutex_);
    overflow_.note(time);
}

void SilKitEthPort::on_transmit(const sk::EthernetFrameTransmitEvent& event) noexcept
{
    PortEvent out;
    out.kind = PortEvent::Kind::TransmitFailed;
    switch (event.status)
    {
    case sk::EthernetTransmitStatus::Transmitted:
        tx_transmitted_.fetch_add(1, std::memory_order_relaxed);
        out.kind = PortEvent::Kind::TransmitCompleted;
        break;
    case sk::EthernetTransmitStatus::ControllerInactive:
        tx_controller_inactive_.fetch_add(1, std::memory_order_relaxed);
        break;
    case sk::EthernetTransmitStatus::LinkDown:
        tx_link_down_.fetch_add(1, std::memory_order_relaxed);
        break;
    case sk::EthernetTransmitStatus::Dropped:
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
        break;
    case sk::EthernetTransmitStatus::InvalidFrameFormat:
        tx_invalid_frame_.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        return;
    }
    // Resolve and free the correlation slot before pushing into events_ so a full event queue
    // never leaks correlation slots.
    std::uint32_t slot = 0;
    std::uint32_t gen = 0;
    if (decode_context(event.userContext, slot, gen))
    {
        const std::lock_guard lock(corr_mutex_);
        std::uint64_t token = 0;
        SimTime sendTime{};
        if (resolve_and_free_locked(slot, gen, token, sendTime))
        {
            out.send_time = sendTime;
            out.token = token;
            out.correlated = true;
        }
    }
    if (!out.correlated)
    {
        tx_uncorrelated_.fetch_add(1, std::memory_order_relaxed);
    }
    out.time = stamp(event.timestamp);
    if (!events_.push(out.time, out) && out.correlated) // a full queue counts correlated drops
    {
        const std::lock_guard lock(overflow_mutex_);
        event_overflow_.note(out.time);
    }
}

std::size_t SilKitEthPort::release_before(SimTime now, FrameSink sink)
{
    if (!is_open())
    {
        return 0;
    }
    require_started("a step ran");
    // A frame without a time that arrives from here on belongs to this step.
    step_time_ns_.store(to_ns(now), std::memory_order_release);
    return inbox_.drain_before(now, [&sink](SimTime time, Frame& frame) { sink(TimedFrame{time, frame}); });
}

SendResult SilKitEthPort::send(const TimedFrame& frame)
{
    return send(frame, 0);
}

SendResult SilKitEthPort::send(const TimedFrame& frame, std::uint64_t token)
{
    if (!is_open() || frame.frame.size() < kHeaderBytes)
    {
        send_failed_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::Failed;
    }
    require_started("send()");
    if (transmit_queue_capacity_ > 0 && outstanding_.load(std::memory_order_acquire) >= transmit_queue_capacity_)
    {
        send_queue_full_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::QueueFull;
    }
    std::uint32_t slot = 0;
    std::uint32_t gen = 0;
    {
        const std::lock_guard lock(corr_mutex_);
        if (!allocate_slot_locked(token, frame.time, slot, gen))
        {
            send_queue_full_.fetch_add(1, std::memory_order_relaxed);
            return SendResult::QueueFull;
        }
    }
    void* const context = encode_context(slot, gen);
    const auto bytes = frame.frame.bytes();
    // At least the minimum frame size (without the FCS): a receiving SIL Kit controller pads a
    // shorter frame itself, but a network simulator may refuse it.
    std::array<std::uint8_t, kDefaultMinFrameBytes> padded{};
    SilKit::Util::Span<const std::uint8_t> out(bytes.data(), bytes.size());
    if (bytes.size() < padded.size())
    {
        std::copy(bytes.begin(), bytes.end(), padded.begin());
        out = SilKit::Util::Span<const std::uint8_t>(padded.data(), padded.size());
    }
    // Increment outstanding_ before SendFrame() so a synchronous callback or concurrent
    // observer sees the frame as in flight immediately.
    outstanding_.fetch_add(1, std::memory_order_release);
    try
    {
        // corr_mutex_ is released before SendFrame() so synchronous trivial-sim callbacks in
        // on_transmit() never re-enter corr_mutex_.
        controller_.SendFrame(sk::EthernetFrame{out}, context);
    }
    catch (const std::exception&)
    {
        bool reclaimed = false;
        {
            const std::lock_guard lock(corr_mutex_);
            reclaimed = reclaim_if_unused_locked(slot, gen);
        }
        if (reclaimed)
        {
            outstanding_.fetch_sub(1, std::memory_order_acq_rel);
            send_failed_.fetch_add(1, std::memory_order_relaxed);
            return SendResult::Failed;
        }
        // on_transmit() already resolved this slot and committed the correlated event:
        // treat the frame as accepted so callers receive one notification and outstanding_
        // stays balanced.
        sent_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::Accepted;
    }
    catch (...)
    {
        bool reclaimed = false;
        {
            const std::lock_guard lock(corr_mutex_);
            reclaimed = reclaim_if_unused_locked(slot, gen);
        }
        if (reclaimed)
        {
            outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        }
        throw;
    }
    sent_.fetch_add(1, std::memory_order_relaxed);
    return SendResult::Accepted;
}

std::size_t SilKitEthPort::release_events_before(SimTime now, PortEventSink sink)
{
    if (!is_open())
    {
        return 0;
    }
    std::optional<PortEvent> overflow;
    std::optional<PortEvent> eventOverflow;
    {
        const std::lock_guard lock(overflow_mutex_);
        overflow = overflow_.take_before(now);
        eventOverflow = event_overflow_.take_before(now);
    }
    auto accountingSink = [&](const PortEvent& event) {
        if ((event.kind == PortEvent::Kind::TransmitCompleted || event.kind == PortEvent::Kind::TransmitFailed) &&
            event.correlated)
        {
            const std::size_t cur = outstanding_.load(std::memory_order_relaxed);
            if (cur > 0)
            {
                outstanding_.store(cur - 1, std::memory_order_release);
            }
        }
        else if (event.kind == PortEvent::Kind::EventOverflow)
        {
            const std::size_t cur = outstanding_.load(std::memory_order_relaxed);
            const std::size_t drop = static_cast<std::size_t>(std::min<std::uint64_t>(cur, event.count));
            outstanding_.store(cur - drop, std::memory_order_release);
        }
        sink(event);
    };
    // Both overflow events at their place in time order among the queued events.
    return OverflowCollector::release_merged(overflow, accountingSink, [&](auto&& forward) {
        return OverflowCollector::release_merged(eventOverflow, forward, [&](auto&& inner) {
            return events_.drain_before(now, [&inner](SimTime, PortEvent& event) { inner(event); });
        });
    });
}

SilKitEthPortStatistics SilKitEthPort::statistics() const
{
    SilKitEthPortStatistics s;
    s.received = received_.load(std::memory_order_relaxed);
    s.received_without_time = received_without_time_.load(std::memory_order_relaxed);
    s.receive_rejected = receive_rejected_.load(std::memory_order_relaxed);
    s.receive_overflows = inbox_.overflows();
    s.sent = sent_.load(std::memory_order_relaxed);
    s.send_failed = send_failed_.load(std::memory_order_relaxed);
    s.tx_transmitted = tx_transmitted_.load(std::memory_order_relaxed);
    s.tx_controller_inactive = tx_controller_inactive_.load(std::memory_order_relaxed);
    s.tx_link_down = tx_link_down_.load(std::memory_order_relaxed);
    s.tx_dropped = tx_dropped_.load(std::memory_order_relaxed);
    s.tx_invalid_frame = tx_invalid_frame_.load(std::memory_order_relaxed);
    s.event_overflows = events_.overflows();
    s.link_up = link_up_.load(std::memory_order_relaxed);
    s.send_queue_full = send_queue_full_.load(std::memory_order_relaxed);
    s.outstanding = outstanding_.load(std::memory_order_acquire);
    s.tx_uncorrelated = tx_uncorrelated_.load(std::memory_order_relaxed);
    return s;
}

} // namespace vehicle_sil::eth
