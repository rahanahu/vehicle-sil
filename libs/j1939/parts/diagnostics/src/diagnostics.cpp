// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/diagnostics.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <span>
#include <stdexcept>

namespace vehicle_sil::j1939 {

namespace {

constexpr auto kPeriod = std::chrono::seconds(1);
constexpr std::uint8_t kMaxOccurrences = 126; // 127: not available
constexpr std::size_t kDm1 = 0;
constexpr std::size_t kDm2 = 1;

// DM22 control bytes and reasons.
constexpr std::uint8_t kClearPreviouslyActive = 0x01;
constexpr std::uint8_t kPreviouslyActiveAck = 0x02;
constexpr std::uint8_t kPreviouslyActiveNack = 0x03;
constexpr std::uint8_t kClearActive = 0x11;
constexpr std::uint8_t kActiveAck = 0x12;
constexpr std::uint8_t kActiveNack = 0x13;
constexpr std::uint8_t kNoReason = 0xFF;
constexpr std::uint8_t kUnknownDtc = 0x02;
constexpr std::uint8_t kDtcIsActive = 0x03;           // asked to clear a previously active one
constexpr std::uint8_t kDtcIsPreviouslyActive = 0x04; // asked to clear an active one

[[nodiscard]] constexpr std::size_t index_of(DtcClear kind) noexcept { return kind == DtcClear::PreviouslyActive ? 0 : 1; }

/// The bit position of a lamp's 2-bit field in the lamp status and lamp flash bytes.
[[nodiscard]] constexpr unsigned shift_of(Lamp lamp) noexcept
{
    switch (lamp)
    {
    case Lamp::MalfunctionIndicator:
        return 6;
    case Lamp::RedStop:
        return 4;
    case Lamp::AmberWarning:
        return 2;
    default:
        return 0;
    }
}

[[nodiscard]] DiagnosticsOptions checked(DiagnosticsOptions options)
{
    if (options.dtc_capacity == 0 || options.dtc_capacity > kMaxDtcs)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Diagnostics: dtc_capacity must be 1 .. 445");
    }
    return options;
}

} // namespace

Diagnostics::Diagnostics(Node& node, DiagnosticsOptions options)
  : node_(node),
    options_(checked(std::move(options))), // before anything is attached to the node
    suppression_(BroadcastSuppression::of(node)),
    periodic_(node, TimerKind::Scheduled,
              [this] {
                  if (owns_address())
                  {
                      broadcast_dm1();
                      restart_period();
                  }
              }),
    change_(node, TimerKind::Scheduled,
            [this] {
                // A DM1 since the change (the periodic one in the same step) showed it already.
                if (owns_address() && change_pending_)
                {
                    broadcast_dm1();
                    restart_period(); // the period counts from this change
                }
            }),
    acknowledge_(node, TimerKind::Scheduled, [this] { answer_deferred(); })
{
    dtcs_.reserve(options_.dtc_capacity);
    buffer_.resize(std::max<std::size_t>(8, 2 + 4 * options_.dtc_capacity));
    deferred_.reserve(options_.deferred_capacity);

    dm1_ = Subscription{node, node.on_request(kPgnDm1, [this](RequestContext& r) { answer_dm(r, true); })};
    dm2_ = Subscription{node, node.on_request(kPgnDm2, [this](RequestContext& r) { answer_dm(r, false); })};
    dm3_ = Subscription{node, node.on_request(kPgnDm3, [this](RequestContext& r) { on_clear_request(r, DtcClear::PreviouslyActive); })};
    dm11_ = Subscription{node, node.on_request(kPgnDm11, [this](RequestContext& r) { on_clear_request(r, DtcClear::Active); })};
    dm22_ = Subscription{node, node.on_message(kPgnDm22, [this](const Message& m) { on_dm22(m); })};
    address_ = Subscription{node, node.on_address_change([this](const AddressChange& change) {
        if (owns_address())
        {
            if (!periodic_.running())
            {
                periodic_.start_after(SimDuration::zero());
            }
        }
        else
        {
            periodic_.cancel();
            change_.cancel();
        }
        if (change.address != change.previous_address)
        {
            deferred_.clear(); // the requests were for the old address
        }
    })};
    transfers_ = Subscription{node, node.on_transfer_end([this](const TransferEnd& end) {
        if (end.destination != kGlobalAddress)
        {
            return;
        }
        const std::size_t i = end.pgn == kPgnDm1 ? kDm1 : end.pgn == kPgnDm2 ? kDm2 : 2;
        if (i < 2 && open_broadcasts_[i] > 0)
        {
            --open_broadcasts_[i];
        }
    })};
    if (owns_address())
    {
        periodic_.start_after(SimDuration::zero());
    }
}

bool Diagnostics::owns_address() const noexcept
{
    return node_.address_state() == AddressState::Claimed || node_.address_state() == AddressState::Fixed;
}

Diagnostics::Entry* Diagnostics::find(std::uint32_t spn, std::uint8_t fmi) noexcept
{
    for (auto& e : dtcs_)
    {
        if (e.spn == spn && e.fmi == fmi)
        {
            return &e;
        }
    }
    return nullptr;
}

const Diagnostics::Entry* Diagnostics::find(std::uint32_t spn, std::uint8_t fmi) const noexcept
{
    return const_cast<Diagnostics*>(this)->find(spn, fmi);
}

bool Diagnostics::set_active(const Dtc& dtc, bool active)
{
    if (dtc.spn > 0x7FFFFu || dtc.fmi > 31u)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Diagnostics: SPN above 19 bits or FMI above 31");
    }
    Entry* entry = find(dtc.spn, dtc.fmi);
    if (entry == nullptr)
    {
        if (!active)
        {
            return false;
        }
        if (dtcs_.size() >= options_.dtc_capacity)
        {
            throw std::length_error("vehicle_sil::j1939::Diagnostics: too many DTCs (dtc_capacity)");
        }
        dtcs_.push_back(Entry{dtc.spn, dtc.fmi, dtc.lamp, dtc.flash, 0, false, std::nullopt});
        entry = &dtcs_.back();
    }
    if (entry->active == active)
    {
        return false;
    }
    entry->active = active;
    if (active)
    {
        entry->lamp = dtc.lamp;
        entry->flash = dtc.flash;
        if (entry->occurrences < kMaxOccurrences)
        {
            ++entry->occurrences;
        }
    }
    changed(*entry);
    return true;
}

bool Diagnostics::is_active(const Dtc& dtc) const noexcept
{
    const Entry* e = find(dtc.spn, dtc.fmi);
    return e != nullptr && e->active;
}

bool Diagnostics::is_previously_active(const Dtc& dtc) const noexcept
{
    const Entry* e = find(dtc.spn, dtc.fmi);
    return e != nullptr && !e->active;
}

std::optional<std::uint8_t> Diagnostics::occurrence_count(const Dtc& dtc) const noexcept
{
    const Entry* e = find(dtc.spn, dtc.fmi);
    return e != nullptr ? std::optional<std::uint8_t>(e->occurrences) : std::nullopt;
}

void Diagnostics::restart_period()
{
    periodic_.start_after(kPeriod);
    // Counted from an event long past (a Timer started at an old deadline): from the step time
    // instead, so the DM1 does not go out again and again until the period caught up.
    const auto now = node_.network().now();
    if (now && *periodic_.deadline() <= *now)
    {
        periodic_.start_at(*now + kPeriod);
    }
}

void Diagnostics::changed(Entry& entry)
{
    if (!owns_address())
    {
        return; // the first DM1 after the claim shows it
    }
    const SimTime now = node_.network().event_time(); // independent of the step size
    // An event time before the last report (a Timer started at a past deadline) counts as new.
    if (entry.reported && *entry.reported <= now && now - *entry.reported < kPeriod)
    {
        return; // the next DM1 shows it
    }
    entry.reported = now;
    change_pending_ = true;
    if (!change_.running())
    {
        change_.start_after(SimDuration::zero());
    }
}

std::size_t Diagnostics::encode(bool active)
{
    std::uint8_t status = 0;
    std::uint8_t flash = 0xFF;
    std::array<int, 5> level{}; // per lamp: 0 off, 1 solid, 2 slow, 3 fast
    std::size_t n = 2;
    for (const auto& e : dtcs_)
    {
        if (e.active != active)
        {
            continue;
        }
        auto& l = level[static_cast<std::size_t>(e.lamp)];
        l = std::max(l, e.flash == LampFlash::Fast ? 3 : e.flash == LampFlash::Slow ? 2 : 1);
        buffer_[n++] = static_cast<std::uint8_t>(e.spn);
        buffer_[n++] = static_cast<std::uint8_t>(e.spn >> 8);
        buffer_[n++] = static_cast<std::uint8_t>((((e.spn >> 16) & 0x7u) << 5) | (e.fmi & 0x1Fu));
        buffer_[n++] = static_cast<std::uint8_t>(e.occurrences & 0x7Fu);
    }
    for (const Lamp lamp : {Lamp::MalfunctionIndicator, Lamp::RedStop, Lamp::AmberWarning, Lamp::Protect})
    {
        const int l = level[static_cast<std::size_t>(lamp)];
        if (l == 0)
        {
            continue;
        }
        const unsigned shift = shift_of(lamp);
        status = static_cast<std::uint8_t>(status | (0x1u << shift));
        const unsigned code = l == 3 ? 0x1u : l == 2 ? 0x0u : 0x3u;
        flash = static_cast<std::uint8_t>((flash & ~(0x3u << shift)) | (code << shift));
    }
    buffer_[0] = status;
    buffer_[1] = flash;
    if (n == 2)
    {
        std::fill_n(buffer_.begin() + 2, 4, std::uint8_t{0});
        n = 6;
    }
    while (n < 8)
    {
        buffer_[n++] = 0xFF;
    }
    return n;
}

void Diagnostics::broadcast_dm1()
{
    change_pending_ = false; // this DM1 (or the suppression) takes care of the changes so far
    if (suppression_.suspended())
    {
        ++statistics_.broadcasts_suppressed;
        return;
    }
    if (open_broadcasts_[kDm1] > 0)
    {
        return; // the DM1 under way goes on; the next one follows the period
    }
    const std::size_t n = encode(true);
    const SendStatus status = node_.send(kPgnDm1, std::span(buffer_.data(), n), kGlobalAddress, kDefaultPriority, WhenBusy::Queue);
    if (status != SendStatus::Ok)
    {
        ++statistics_.send_failures;
    }
    else if (n > 8)
    {
        ++open_broadcasts_[kDm1];
    }
}

void Diagnostics::answer_dm(RequestContext& request, bool active)
{
    const std::size_t i = active ? kDm1 : kDm2;
    const std::size_t n = encode(active);
    const bool toAll = n <= 8 || !request.is_destination_specific();
    if (toAll)
    {
        // One answer to all nodes serves every request with the same time (requests that came
        // together), and a broadcast under way serves them too.
        const SimTime now = node_.network().event_time();
        if (open_broadcasts_[i] > 0 || (answered_[i] && answered_[i] == now))
        {
            return;
        }
        answered_[i] = now;
    }
    const SendStatus status = request.respond(std::span(buffer_.data(), n));
    if (status != SendStatus::Ok)
    {
        ++statistics_.send_failures;
    }
    else if (toAll && n > 8)
    {
        ++open_broadcasts_[i];
    }
}

void Diagnostics::on_clear_request(RequestContext& request, DtcClear kind)
{
    const std::size_t k = index_of(kind);
    std::optional<AckCode> answer;
    if (!clearing_[k])
    {
        // Settled before the application is asked: it may call complete_clear() at once.
        clearing_[k] = true;
        const ClearDecision decision = options_.on_clear ? options_.on_clear(kind) : ClearDecision::Clear;
        if (decision != ClearDecision::Defer)
        {
            clearing_[k] = false;
            if (decision == ClearDecision::Clear)
            {
                clear(kind);
            }
            // Nothing for a request to all nodes.
            static_cast<void>(request.acknowledge(decision == ClearDecision::Clear ? AckCode::Positive : AckCode::Negative));
            return;
        }
        if (!clearing_[k])
        {
            answer = completed_[k]; // completed inside on_clear: answered with the others
        }
    }
    if (!request.is_destination_specific())
    {
        return;
    }
    if (deferred_.size() >= options_.deferred_capacity)
    {
        ++statistics_.requests_refused;
        static_cast<void>(request.acknowledge(AckCode::CannotRespond));
        return;
    }
    deferred_.push_back(Deferred{kind, request.defer(), answer});
    if (answer)
    {
        acknowledge_.start_after(SimDuration::zero());
    }
}

bool Diagnostics::complete_clear(DtcClear which, bool cleared)
{
    const std::size_t k = index_of(which);
    if (!clearing_[k])
    {
        return false;
    }
    clearing_[k] = false;
    completed_[k] = cleared ? AckCode::Positive : AckCode::Negative;
    if (cleared)
    {
        clear(which);
    }
    for (auto& d : deferred_)
    {
        if (d.kind == which && !d.answer)
        {
            d.answer = completed_[k];
        }
    }
    acknowledge_.start_after(SimDuration::zero());
    return true;
}

void Diagnostics::answer_deferred()
{
    // Only what has an answer now; each request is copied out before it is answered.
    std::size_t i = 0;
    while (i < deferred_.size())
    {
        if (!deferred_[i].answer)
        {
            ++i;
            continue;
        }
        const Deferred d = deferred_[i];
        deferred_.erase(deferred_.begin() + static_cast<std::ptrdiff_t>(i));
        static_cast<void>(node_.acknowledge(d.request, *d.answer));
    }
}

void Diagnostics::clear(DtcClear kind)
{
    if (kind == DtcClear::PreviouslyActive)
    {
        std::erase_if(dtcs_, [](const Entry& e) { return !e.active; });
        return;
    }
    for (auto& e : dtcs_)
    {
        if (e.active)
        {
            e.active = false; // kept as previously active
            changed(e);
        }
    }
}

void Diagnostics::on_dm22(const Message& message)
{
    if (message.data.size() < 8 || message.destination == kGlobalAddress)
    {
        return;
    }
    const std::uint8_t control = message.data[0];
    if (control != kClearPreviouslyActive && control != kClearActive)
    {
        return;
    }
    const std::uint32_t spn = static_cast<std::uint32_t>(message.data[5]) | (static_cast<std::uint32_t>(message.data[6]) << 8) |
                              (static_cast<std::uint32_t>(message.data[7] >> 5) << 16);
    const auto fmi = static_cast<std::uint8_t>(message.data[7] & 0x1Fu);
    const bool active = control == kClearActive;
    std::uint8_t answer = active ? kActiveAck : kPreviouslyActiveAck;
    std::uint8_t reason = kNoReason;
    Entry* entry = find(spn, fmi);
    if (entry == nullptr)
    {
        reason = kUnknownDtc;
    }
    else if (entry->active != active)
    {
        reason = active ? kDtcIsPreviouslyActive : kDtcIsActive;
    }
    else if (active)
    {
        entry->active = false; // kept as previously active
        changed(*entry);
    }
    else
    {
        dtcs_.erase(dtcs_.begin() + (entry - dtcs_.data()));
    }
    if (reason != kNoReason)
    {
        answer = active ? kActiveNack : kPreviouslyActiveNack;
    }
    const std::array<std::uint8_t, 8> data{answer, reason, 0xFF, 0xFF, 0xFF, message.data[5], message.data[6], message.data[7]};
    if (node_.send(kPgnDm22, data, message.source) != SendStatus::Ok)
    {
        ++statistics_.send_failures;
    }
}

// --- Reading a DTC list (the inverse of encode())

ReadDtc DtcListView::iterator::operator*() const noexcept
{
    ReadDtc dtc;
    dtc.spn = static_cast<std::uint32_t>(at_[0]) | (static_cast<std::uint32_t>(at_[1]) << 8) |
              (static_cast<std::uint32_t>(at_[2] >> 5) << 16);
    dtc.fmi = static_cast<std::uint8_t>(at_[2] & 0x1Fu);
    dtc.occurrences = static_cast<std::uint8_t>(at_[3] & 0x7Fu);
    dtc.byte4_high_bit = (at_[3] & 0x80u) != 0;
    dtc.raw = {at_[0], at_[1], at_[2], at_[3]};
    return dtc;
}

std::optional<LampFlash> DtcListView::lamp(Lamp which) const noexcept
{
    if (!valid_ || which == Lamp::None)
    {
        return std::nullopt;
    }
    const unsigned shift = shift_of(which);
    if (((data_[0] >> shift) & 0x3u) != 0x1u)
    {
        return std::nullopt;
    }
    const unsigned flash = (data_[1] >> shift) & 0x3u;
    return flash == 0x1u ? LampFlash::Fast : flash == 0x0u ? LampFlash::Slow : LampFlash::Solid;
}

bool DtcListView::has(std::uint32_t spn, std::uint8_t fmi) const noexcept
{
    return std::any_of(begin(), end(), [&](const ReadDtc& dtc) { return dtc.spn == spn && dtc.fmi == fmi; });
}

DtcListView::DtcListView(std::span<const std::uint8_t> data) noexcept : data_(data)
{
    // Up to 8 bytes: one DTC and padding. Longer: exactly 2 + 4 * n bytes.
    if (data.size() < 6 || (data.size() > 8 && (data.size() - 2) % 4 != 0))
    {
        return;
    }
    valid_ = true;
    count_ = (data.size() - 2) / 4;
    if (count_ == 1 && data[2] == 0 && data[3] == 0 && data[4] == 0 && data[5] == 0)
    {
        count_ = 0; // the list without DTCs
    }
}

DtcListView parse_dtc_list(std::span<const std::uint8_t> data) noexcept { return DtcListView(data); }

} // namespace vehicle_sil::j1939
