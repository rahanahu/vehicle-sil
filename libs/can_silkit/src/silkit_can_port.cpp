// SPDX-License-Identifier: MIT
#include "vehicle_sil/can_silkit/silkit_can_port.hpp"

#include "vehicle_sil/can_silkit/send_time_tag.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::can {
namespace {

namespace sk = SilKit::Services::Can;

constexpr sk::CanFrameFlagMask flag(sk::CanFrameFlag f) noexcept
{
    return static_cast<sk::CanFrameFlagMask>(f);
}

constexpr sk::CanFrameFlagMask kUnsupportedFlags = flag(sk::CanFrameFlag::Rtr) | flag(sk::CanFrameFlag::Fdf) |
                                                   flag(sk::CanFrameFlag::Xlf);

constexpr std::uint16_t kCorrSlotMask = 0x3FFFu;
constexpr unsigned kCorrSlotBits = 14;
constexpr std::uint8_t kCorrGenMask = 0x3u;
constexpr std::size_t kMinCorrCapacity = 1024;

std::size_t validated_inbox_capacity(const SilKitCanPortOptions& options)
{
    SilKitCanPort::validate(options);
    return options.real_time_clock != nullptr ? 1 : options.inbox_capacity;
}

} // namespace

void SilKitCanPort::validate(const SilKitCanPortOptions& options)
{
    if (options.inbox_capacity == 0)
    {
        throw std::invalid_argument("vehicle_sil::can::SilKitCanPort: inbox_capacity must be > 0");
    }
    if (options.event_capacity == 0)
    {
        throw std::invalid_argument("vehicle_sil::can::SilKitCanPort: event_capacity must be > 0");
    }
    if (options.transmit_queue_capacity > options.event_capacity)
    {
        // An acknowledgement lost to a full event queue would keep its place for good.
        throw std::invalid_argument(
            "vehicle_sil::can::SilKitCanPort: transmit_queue_capacity must not exceed event_capacity");
    }
    if (options.real_time_clock == nullptr && options.transmit_queue_capacity > kMaxCorrelatedInFlight)
    {
        throw std::invalid_argument(
            "vehicle_sil::can::SilKitCanPort: transmit_queue_capacity must not exceed kMaxCorrelatedInFlight");
    }
}

std::optional<Frame> from_silkit_frame(const sk::CanFrame& in) noexcept
{
    if ((in.flags & kUnsupportedFlags) != 0)
    {
        return std::nullopt;
    }
    const std::size_t length = in.dataField.size();
    if (length > kMaxPayload)
    {
        return std::nullopt;
    }
    // Classic CAN: DLC 0..8 is the byte count, DLC 9..15 also mean 8 bytes.
    const bool dlcMatches = (in.dlc == length) || ((length == kMaxPayload) && (in.dlc > 8) && (in.dlc <= 15));
    if (!dlcMatches)
    {
        return std::nullopt;
    }
    Frame out;
    out.id = in.canId;
    out.format = (in.flags & flag(sk::CanFrameFlag::Ide)) != 0 ? IdFormat::Extended : IdFormat::Standard;
    out.length = static_cast<std::uint8_t>(length);
    std::copy_n(in.dataField.begin(), length, out.data.begin());
    if (!is_valid(out))
    {
        return std::nullopt;
    }
    return out;
}

bool to_silkit_frame(const Frame& in, sk::CanFrame& out) noexcept
{
    if (!is_valid(in))
    {
        return false;
    }
    out = sk::CanFrame{};
    out.canId = in.id;
    out.flags = in.format == IdFormat::Extended ? flag(sk::CanFrameFlag::Ide) : sk::CanFrameFlagMask{0};
    out.dlc = in.length;
    out.dataField = SilKit::Util::Span<const std::uint8_t>(in.data.data(), in.length);
    return true;
}

SilKitCanPort::SilKitCanPort(sk::ICanController& controller, SilKitCanPortOptions options) :
  controller_(controller),
  // validate() runs via validated_inbox_capacity() before StepInbox or RealTimeReceiver are built.
  // With real time, the RealTimeReceiver holds the frames and events instead.
  inbox_(validated_inbox_capacity(options)),
  events_(options.real_time_clock != nullptr ? 1 : options.event_capacity),
  uncorrelated_events_(options.real_time_clock != nullptr ? 1 : options.event_capacity),
  transmit_capacity_(options.transmit_queue_capacity),
  bitrate_(options.bitrate)
{
    if (options.real_time_clock != nullptr)
    {
        real_time_.emplace(*options.real_time_clock, options.inbox_capacity, options.event_capacity);
    }
    else
    {
        uncorrelated_drain_.reserve(options.event_capacity);
        const std::size_t corrCapacity = std::clamp<std::size_t>(
            std::max({options.event_capacity, options.transmit_queue_capacity, kMinCorrCapacity}), 1,
            kMaxCorrelatedInFlight);
        corr_slots_.resize(corrCapacity);
        corr_free_ring_.resize(corrCapacity);
        clear_corr_table();
    }
    const std::lock_guard lock(lifecycle_mutex_);
    try
    {
        frame_handler_ = controller_.AddFrameHandler(
            [this](sk::ICanController*, const sk::CanFrameEvent& event) { on_frame(event); },
            static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX));
        constexpr auto kAll = static_cast<sk::CanTransmitStatusMask>(sk::CanTransmitStatus::Transmitted) |
                              static_cast<sk::CanTransmitStatusMask>(sk::CanTransmitStatus::Canceled) |
                              static_cast<sk::CanTransmitStatusMask>(sk::CanTransmitStatus::TransmitQueueFull);
        transmit_handler_ = controller_.AddFrameTransmitHandler(
            [this](sk::ICanController*, const sk::CanFrameTransmitEvent& event) { on_transmit(event); }, kAll);
        error_state_handler_ = controller_.AddErrorStateChangeHandler(
            [this](sk::ICanController*, const sk::CanErrorStateChangeEvent& event) { on_error_state(event); });
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
            if (error_state_handler_)
            {
                controller_.RemoveErrorStateChangeHandler(*error_state_handler_);
            }
        }
        catch (...)
        {
        }
        throw;
    }
}

SilKitCanPort::~SilKitCanPort()
{
    close();
}

void SilKitCanPort::start()
{
    const std::lock_guard lock(lifecycle_mutex_);
    if (!is_open() || is_started())
    {
        return;
    }
    if (bitrate_ > 0)
    {
        controller_.SetBaudRate(bitrate_, 0, 0);
    }
    controller_.Start();
    started_.store(true, std::memory_order_release);
}

void SilKitCanPort::require_started(const char* what) const
{
    if (!is_started())
    {
        throw std::logic_error(std::string("vehicle_sil::can::SilKitCanPort: ") + what +
                               " before start(): the controller is not started, so SIL Kit would drop the frames "
                               "this port sends without telling. Call start() from the participant's "
                               "CommunicationReady handler (ParticipantRuntime::add_communication_ready_handler(); "
                               "vehicle_sil::Peer does it for its own ports)");
    }
}

void SilKitCanPort::close() noexcept
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
        if (error_state_handler_)
        {
            controller_.RemoveErrorStateChangeHandler(*error_state_handler_);
        }
    }
    catch (...)
    {
    }
    frame_handler_.reset();
    transmit_handler_.reset();
    error_state_handler_.reset();
    if (wasStarted)
    {
        try
        {
            controller_.Stop();
        }
        catch (...)
        {
        }
    }
    inbox_.clear();
    events_.clear();
    uncorrelated_events_.clear();
    {
        const std::lock_guard overflowLock(overflow_mutex_);
        overflow_.clear();
        event_overflow_.clear();
    }
    {
        const std::lock_guard corrLock(corr_mutex_);
        clear_corr_table();
    }
    if (real_time_)
    {
        real_time_->set_wakeup({});
        real_time_->clear();
    }
}

std::optional<std::uint16_t> SilKitCanPort::allocate_corr_slot(std::uint64_t token, SimTime send_time) noexcept
{
    if (corr_free_count_ == 0)
    {
        return std::nullopt;
    }
    const std::uint16_t slot = corr_free_ring_[corr_free_head_];
    corr_free_head_ = (corr_free_head_ + 1) % corr_free_ring_.size();
    --corr_free_count_;
    CorrSlot& entry = corr_slots_[slot];
    entry.token = token;
    entry.send_time = send_time;
    entry.in_use = true;
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(entry.gen) << kCorrSlotBits) | slot);
}

bool SilKitCanPort::resolve_and_free_corr_slot(std::uint16_t tag, std::uint64_t& token, SimTime& send_time) noexcept
{
    const std::uint16_t slot = static_cast<std::uint16_t>(tag & kCorrSlotMask);
    const std::uint8_t gen = static_cast<std::uint8_t>((tag >> kCorrSlotBits) & kCorrGenMask);
    if (slot >= corr_slots_.size())
    {
        return false;
    }
    CorrSlot& entry = corr_slots_[slot];
    if (!entry.in_use || entry.gen != gen)
    {
        return false;
    }
    token = entry.token;
    send_time = entry.send_time;
    entry.in_use = false;
    entry.gen = static_cast<std::uint8_t>((entry.gen + 1u) & kCorrGenMask);
    const std::size_t tail = (corr_free_head_ + corr_free_count_) % corr_free_ring_.size();
    corr_free_ring_[tail] = slot;
    ++corr_free_count_;
    return true;
}

bool SilKitCanPort::reclaim_corr_slot_if_unused(std::uint16_t tag) noexcept
{
    std::uint64_t unusedToken = 0;
    SimTime unusedSendTime{};
    return resolve_and_free_corr_slot(tag, unusedToken, unusedSendTime);
}

void SilKitCanPort::clear_corr_table() noexcept
{
    for (std::size_t i = 0; i < corr_slots_.size(); ++i)
    {
        corr_slots_[i].in_use = false;
        corr_slots_[i].gen = 0;
        corr_free_ring_[i] = static_cast<std::uint16_t>(i);
    }
    corr_free_head_ = 0;
    corr_free_count_ = corr_free_ring_.size();
}

SimTime SilKitCanPort::stamp(std::chrono::nanoseconds timestamp) const noexcept
{
    if (timestamp < std::chrono::nanoseconds::zero())
    {
        return SimTime{} + std::chrono::nanoseconds(step_time_ns_.load(std::memory_order_acquire));
    }
    return from_silkit_time(timestamp);
}

void SilKitCanPort::set_wakeup(Wakeup wakeup)
{
    if (real_time_)
    {
        real_time_->set_wakeup(std::move(wakeup));
    }
}

void SilKitCanPort::push_event(const PortEvent& event, bool acknowledgement) noexcept
{
    if (real_time_)
    {
        // Every outstanding frame will bring an acknowledgement: keep a place for each of them
        // (acknowledgements already queued are counted twice, which only errs on the safe side).
        const std::size_t reserve = acknowledgement ? 0 : outstanding_.load(std::memory_order_acquire);
        static_cast<void>(real_time_->push_event(event, reserve)); // counts a refusal or a full queue
        return;
    }
    if (event.correlated)
    {
        if (!events_.push(event.time, event)) // a full queue counts it
        {
            const std::lock_guard lock(overflow_mutex_);
            event_overflow_.note(event.time, 1);
        }
        return;
    }
    static_cast<void>(uncorrelated_events_.push(event.time, event)); // a full queue counts it
}

void SilKitCanPort::on_error_state(const sk::CanErrorStateChangeEvent& event) noexcept
{
    PortEvent out;
    out.kind = PortEvent::Kind::BusStateChanged;
    switch (event.errorState)
    {
    case sk::CanErrorState::ErrorActive:
        out.bus_state = BusState::ErrorActive;
        break;
    case sk::CanErrorState::ErrorPassive:
        out.bus_state = BusState::ErrorPassive;
        break;
    case sk::CanErrorState::BusOff:
        out.bus_state = BusState::BusOff;
        break;
    default:
        return; // NotAvailable: the controller is not initialised; nothing to report
    }
    error_state_changes_.fetch_add(1, std::memory_order_relaxed);
    out.time = stamp(event.timestamp);
    push_event(out, false);
}

void SilKitCanPort::on_frame(const sk::CanFrameEvent& event)
{
    const auto frame = from_silkit_frame(event.frame);
    if (!frame)
    {
        receive_rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (real_time_)
    {
        if (real_time_->push_frame(*frame))
        {
            received_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const SimTime time = stamp(event.timestamp);
    if (inbox_.push(time, *frame))
    {
        received_.fetch_add(1, std::memory_order_relaxed);
        if (event.timestamp < std::chrono::nanoseconds::zero())
        {
            received_without_time_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const std::lock_guard lock(overflow_mutex_);
    overflow_.note(time, 1);
}

void SilKitCanPort::on_transmit(const sk::CanFrameTransmitEvent& event) noexcept
{
    PortEvent out;
    out.kind = PortEvent::Kind::TransmitFailed;
    switch (event.status)
    {
    case sk::CanTransmitStatus::Transmitted:
        tx_transmitted_.fetch_add(1, std::memory_order_relaxed);
        out.kind = PortEvent::Kind::TransmitCompleted;
        break;
    case sk::CanTransmitStatus::Canceled:
        tx_canceled_.fetch_add(1, std::memory_order_relaxed);
        break;
    case sk::CanTransmitStatus::TransmitQueueFull:
        tx_queue_full_.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        return;
    }
    out.time = stamp(event.timestamp);
    out.id = event.canId;
    if (!real_time_)
    {
        if (const auto tag = decode_correlated_tag(event.userContext))
        {
            std::uint64_t token = 0;
            SimTime sendTime{};
            bool resolved = false;
            {
                const std::lock_guard lock(corr_mutex_);
                resolved = resolve_and_free_corr_slot(*tag, token, sendTime);
            }
            if (resolved)
            {
                out.send_time = sendTime;
                out.token = token;
                out.correlated = true;
            }
            else
            {
                tx_uncorrelated_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else
        {
            out.send_time = decode_send_time(event.userContext);
            tx_uncorrelated_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    push_event(out, true);
}

std::size_t SilKitCanPort::release_before(SimTime now, FrameSink sink)
{
    if (!is_open())
    {
        return 0;
    }
    require_started("a step ran");
    if (real_time_)
    {
        return real_time_->release_before(now, sink);
    }
    // A frame without a time that arrives from here on belongs to this step.
    step_time_ns_.store(to_ns(now), std::memory_order_release);
    return inbox_.drain_before(now, [&sink](SimTime time, Frame& frame) { sink(TimedFrame{time, frame}); });
}

SendResult SilKitCanPort::send(const TimedFrame& frame)
{
    return send(frame, 0);
}

SendResult SilKitCanPort::send(const TimedFrame& frame, std::uint64_t token)
{
    sk::CanFrame out;
    if (!is_open() || !to_silkit_frame(frame.frame, out))
    {
        send_failed_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::Failed;
    }
    require_started("send()");
    if (transmit_capacity_ > 0 && outstanding_.load(std::memory_order_relaxed) >= transmit_capacity_)
    {
        send_queue_full_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::QueueFull;
    }
    void* context = nullptr;
    std::optional<std::uint16_t> tag;
    if (!real_time_)
    {
        {
            const std::lock_guard lock(corr_mutex_);
            tag = allocate_corr_slot(token, frame.time);
        }
        if (!tag)
        {
            send_queue_full_.fetch_add(1, std::memory_order_relaxed);
            return SendResult::QueueFull;
        }
        context = encode_correlated_send_time(frame.time, *tag);
    }
    outstanding_.fetch_add(1, std::memory_order_release);
    try
    {
        controller_.SendFrame(out, context);
    }
    catch (const std::exception&)
    {
        if (tag)
        {
            bool reclaimed = false;
            {
                const std::lock_guard lock(corr_mutex_);
                reclaimed = reclaim_corr_slot_if_unused(*tag);
            }
            if (!reclaimed)
            {
                // on_transmit() already resolved this slot synchronously and queued a correlated
                // PortEvent; treat the send as Accepted so outstanding_ and channel notifications
                // stay conserved without double-notifying or stealing another frame's outstanding.
                sent_.fetch_add(1, std::memory_order_relaxed);
                return SendResult::Accepted;
            }
        }
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        send_failed_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::Failed;
    }
    catch (...)
    {
        bool rollBackOutstanding = true;
        if (tag)
        {
            const std::lock_guard lock(corr_mutex_);
            rollBackOutstanding = reclaim_corr_slot_if_unused(*tag);
        }
        if (rollBackOutstanding)
        {
            outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        }
        throw;
    }
    sent_.fetch_add(1, std::memory_order_relaxed);
    return SendResult::Accepted;
}

std::size_t SilKitCanPort::release_events_before(SimTime now, PortEventSink sink)
{
    if (!is_open())
    {
        return 0;
    }
    const auto release = [this, &sink](const PortEvent& event) {
        const bool transmit =
            event.kind == PortEvent::Kind::TransmitCompleted || event.kind == PortEvent::Kind::TransmitFailed;
        if (transmit && (real_time_ || event.correlated))
        {
            if (outstanding_.load(std::memory_order_relaxed) > 0)
            {
                outstanding_.fetch_sub(1, std::memory_order_relaxed); // ends one frame this port sent
            }
        }
        else if (event.kind == PortEvent::Kind::EventOverflow)
        {
            const std::size_t current = outstanding_.load(std::memory_order_relaxed);
            const std::size_t dropped = static_cast<std::size_t>(std::min<std::uint64_t>(current, event.count));
            if (dropped > 0)
            {
                outstanding_.fetch_sub(dropped, std::memory_order_relaxed);
            }
        }
        sink(event);
    };
    if (real_time_)
    {
        return real_time_->release_events_before(now, release);
    }
    std::optional<PortEvent> overflow;
    std::optional<PortEvent> eventOverflow;
    {
        const std::lock_guard lock(overflow_mutex_);
        overflow = overflow_.take_before(now);
        eventOverflow = event_overflow_.take_before(now);
    }
    uncorrelated_drain_.clear();
    uncorrelated_events_.drain_before(now, [this](SimTime, PortEvent& event) {
        uncorrelated_drain_.push_back(event);
    });
    std::size_t ui = 0;
    const std::size_t count = OverflowCollector::release_merged(overflow, release, [&](auto&& forward) {
        return OverflowCollector::release_merged(eventOverflow, forward, [&](auto&& inner) {
            const std::size_t correlated = events_.drain_before(now, [&](SimTime, PortEvent& event) {
                while (ui < uncorrelated_drain_.size() && uncorrelated_drain_[ui].time < event.time)
                {
                    inner(uncorrelated_drain_[ui++]);
                }
                inner(event);
            });
            while (ui < uncorrelated_drain_.size())
            {
                inner(uncorrelated_drain_[ui++]);
            }
            return correlated + uncorrelated_drain_.size();
        });
    });
    uncorrelated_drain_.clear();
    return count;
}

SilKitCanPortStatistics SilKitCanPort::statistics() const
{
    SilKitCanPortStatistics s;
    s.received = received_.load(std::memory_order_relaxed);
    s.received_without_time = received_without_time_.load(std::memory_order_relaxed);
    s.receive_rejected = receive_rejected_.load(std::memory_order_relaxed);
    s.receive_overflows = real_time_ ? real_time_->overflows() : inbox_.overflows();
    s.error_state_changes = error_state_changes_.load(std::memory_order_relaxed);
    s.sent = sent_.load(std::memory_order_relaxed);
    s.send_failed = send_failed_.load(std::memory_order_relaxed);
    s.send_queue_full = send_queue_full_.load(std::memory_order_relaxed);
    s.outstanding = outstanding_.load(std::memory_order_relaxed);
    s.tx_transmitted = tx_transmitted_.load(std::memory_order_relaxed);
    s.tx_canceled = tx_canceled_.load(std::memory_order_relaxed);
    s.tx_queue_full = tx_queue_full_.load(std::memory_order_relaxed);
    s.tx_uncorrelated = tx_uncorrelated_.load(std::memory_order_relaxed);
    s.event_overflows = real_time_ ? real_time_->event_overflows() : events_.overflows() + uncorrelated_events_.overflows();
    return s;
}

} // namespace vehicle_sil::can
