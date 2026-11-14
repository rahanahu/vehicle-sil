// SPDX-License-Identifier: MIT
//
// A protocol part of the J1939 stack: the diagnostic trouble codes of one node and the
// J1939-73 messages about them (DM1, DM2, DM3, DM11, DM22). Built on the node's public API and
// the toolbox only. parse_dtc_list() reads the DTC lists (DM1, DM2) of other nodes.
#pragma once

#include "vehicle_sil/j1939/broadcast_suppression.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

namespace vehicle_sil::j1939 {

inline constexpr Pgn kPgnDm1{0xFECAu};  ///< Active DTCs.
inline constexpr Pgn kPgnDm2{0xFECBu};  ///< Previously active DTCs.
inline constexpr Pgn kPgnDm3{0xFECCu};  ///< Clear the previously active DTCs (requested).
inline constexpr Pgn kPgnDm11{0xFED3u}; ///< Clear the active DTCs (requested).
inline constexpr Pgn kPgnDm22{0xC300u}; ///< Clear one DTC (PDU1).

/// The most DTCs one DM1 or DM2 carries in one transfer (2 + 4 * 445 <= 1785 bytes).
inline constexpr std::size_t kMaxDtcs = 445;

/// The lamp a DTC turns on.
enum class Lamp : std::uint8_t
{
    None,
    MalfunctionIndicator,
    RedStop,
    AmberWarning,
    Protect,
};

enum class LampFlash : std::uint8_t
{
    Solid,
    Slow,
    Fast,
};

/// A diagnostic trouble code: SPN (19 bits) and FMI (5 bits) identify it; the lamp is what it
/// shows while it is listed.
struct Dtc
{
    std::uint32_t spn = 0;
    std::uint8_t fmi = 0;
    Lamp lamp = Lamp::None;
    LampFlash flash = LampFlash::Solid;
};

/// Which DTCs a request clears.
enum class DtcClear : std::uint8_t
{
    PreviouslyActive, ///< DM3.
    Active,           ///< DM11 (they become previously active).
};

/// The application's answer to a DM3 or DM11.
enum class ClearDecision : std::uint8_t
{
    Clear,  ///< Clear now; a destination-specific request is acknowledged at once.
    Refuse, ///< Do not clear; a destination-specific request gets a negative acknowledgement.
    Defer,  ///< The application clears later and reports it with Diagnostics::complete_clear().
};

struct DiagnosticsOptions
{
    /// The most DTCs (active and previously active together), 1 .. kMaxDtcs.
    std::size_t dtc_capacity = 32;
    /// Destination-specific DM3/DM11 requests that wait for complete_clear(); more get the
    /// acknowledgement "cannot respond".
    std::size_t deferred_capacity = 8;
    /// Called for each DM3 and DM11 (not while a deferred clear of the same kind runs: those
    /// requests wait for the same complete_clear()). It may call complete_clear() itself before
    /// it returns Defer. It must not destroy the Diagnostics. Empty: always Clear.
    std::function<ClearDecision(DtcClear)> on_clear;
};

struct DiagnosticsStatistics
{
    std::uint64_t broadcasts_suppressed = 0; ///< DM1 broadcasts not sent because of the suppression.
    std::uint64_t send_failures = 0;         ///< DM1, DM2 and DM22 the node could not send.
    std::uint64_t requests_refused = 0;      ///< DM3/DM11 refused because deferred_capacity was reached.
};

/// The DTCs of one node and their messages:
/// - DM1 (active DTCs) to all nodes every second while the node owns its address (Claimed, or a
///   node without a NAME), starting when it gets it; and at once (the next chance) when a DTC
///   becomes active or inactive, at most once a second per DTC (a later change in that second
///   shows in the next DM1). The periodic DM1 then counts its second from there. Not sent while
///   BroadcastSuppression::of(node) is suspended (the period goes on; nothing is made up for).
///   A DM1 longer than 8 bytes is a broadcast (BAM) that waits for the node's transfer before
///   it (WhenBusy::Queue); no new DM1 is broadcast while one is still under way.
/// - Requests for DM1 and DM2 are answered (also while suppressed): up to 8 bytes one frame to
///   all nodes, longer to all nodes (BAM) or to the requester (RTS/CTS) for a destination-specific
///   request. Requests with the same time that the same answer to all nodes serves get one, and
///   none is sent while the same broadcast is under way. The limits above count in event time
///   (Network::event_time()), so the results do not depend on the step size.
/// - DM3 clears the previously active DTCs, DM11 makes the active ones previously active (see
///   DiagnosticsOptions::on_clear). A destination-specific request is acknowledged when the
///   clear is done (also with nothing to clear), a request to all nodes is not.
/// - DM22 to this node clears one active DTC (it becomes previously active) or one previously
///   active DTC, and answers the requester with DM22.
/// DM1/DM2 data: the lamp status and lamp flash bytes (of the listed DTCs), then 4 bytes per DTC
/// (SPN, FMI, occurrence count); without DTCs 4 zero bytes; padded to 8 bytes with 0xFF. The
/// occurrence count counts the activations (1 .. 126).
///
/// Uses kTimers (three) Timers of the network and BroadcastSuppression::of(node) (attached if the
/// node has none, with three Timers of its own; Network::timers_free()). Create one per node, with
/// node.attach<Diagnostics>() or owned by the application.
class Diagnostics
{
public:
    /// The Timers of the network the part uses (without BroadcastSuppression's).
    static constexpr std::size_t kTimers = 3;
    /// std::invalid_argument if dtc_capacity is not 1 .. kMaxDtcs.
    explicit Diagnostics(Node& node, DiagnosticsOptions options = {});
    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

    /// Makes the DTC (SPN and FMI) active or inactive; an active DTC takes the lamp of `dtc`.
    /// true if its state changed. Allowed everywhere on the thread that steps the network (other
    /// threads: Host::post()), also outside a step (the DM1 goes out at the next chance).
    /// std::invalid_argument for an SPN above 19 bits or an FMI above 31, std::length_error if
    /// dtc_capacity DTCs are known already.
    bool set_active(const Dtc& dtc, bool active);
    [[nodiscard]] bool is_active(const Dtc& dtc) const noexcept;
    [[nodiscard]] bool is_previously_active(const Dtc& dtc) const noexcept;
    /// The occurrence count of a known DTC; empty if it is neither active nor previously active.
    [[nodiscard]] std::optional<std::uint8_t> occurrence_count(const Dtc& dtc) const noexcept;

    /// Ends a clear that on_clear deferred: clears the DTCs if `cleared`, and acknowledges the
    /// waiting destination-specific requests (positive if cleared, negative otherwise) at the
    /// next chance. false if no clear of that kind was deferred. Allowed everywhere on the thread
    /// that steps the network (other threads: Host::post()).
    bool complete_clear(DtcClear which, bool cleared);

    [[nodiscard]] const DiagnosticsStatistics& statistics() const noexcept { return statistics_; }

private:
    struct Entry
    {
        std::uint32_t spn;
        std::uint8_t fmi;
        Lamp lamp;
        LampFlash flash;
        std::uint8_t occurrences;
        bool active;
        std::optional<SimTime> reported; ///< When its last change triggered a DM1.
    };
    struct Deferred
    {
        DtcClear kind;
        PendingRequest request;
        std::optional<AckCode> answer; ///< Set by complete_clear(); sent by the next chance.
    };

    [[nodiscard]] Entry* find(std::uint32_t spn, std::uint8_t fmi) noexcept;
    [[nodiscard]] const Entry* find(std::uint32_t spn, std::uint8_t fmi) const noexcept;
    [[nodiscard]] bool owns_address() const noexcept;
    /// A DTC changed state: a DM1 at the next chance, unless it did so in the last second.
    void changed(Entry& entry);
    /// Starts the next periodic DM1 a period after the current event.
    void restart_period();
    /// Encodes the active (or previously active) DTCs into buffer_; returns the length.
    [[nodiscard]] std::size_t encode(bool active);
    void broadcast_dm1();
    void answer_dm(RequestContext& request, bool active);
    void on_clear_request(RequestContext& request, DtcClear kind);
    void clear(DtcClear kind);
    void on_dm22(const Message& message);
    void answer_deferred();

    Node& node_;
    DiagnosticsOptions options_;
    BroadcastSuppression& suppression_;
    DiagnosticsStatistics statistics_;
    std::vector<Entry> dtcs_;       ///< Capacity dtc_capacity, in the order they became known.
    std::vector<std::uint8_t> buffer_;
    std::vector<Deferred> deferred_; ///< Capacity deferred_capacity.
    bool clearing_[2] = {false, false};
    bool change_pending_ = false; ///< A change waits for its DM1 (change_).
    AckCode completed_[2] = {AckCode::Positive, AckCode::Positive}; ///< The last complete_clear().
    /// Broadcasts (BAM) of DM1 / DM2 under way: the next answer to all nodes would repeat them.
    std::size_t open_broadcasts_[2] = {0, 0};
    /// The event time of the last answer of DM1 / DM2 to all nodes.
    std::optional<SimTime> answered_[2];
    Timer periodic_;
    Timer change_;
    Timer acknowledge_;
    Subscription dm1_;
    Subscription dm2_;
    Subscription dm3_;
    Subscription dm11_;
    Subscription dm22_;
    Subscription address_;
    Subscription transfers_;
};

/// One DTC of a list read with parse_dtc_list(). SPN, FMI and occurrence count are read as the
/// inverse of the layout Diagnostics sends.
struct ReadDtc
{
    std::uint32_t spn = 0;        ///< 19 bits.
    std::uint8_t fmi = 0;         ///< 5 bits.
    std::uint8_t occurrences = 0; ///< The low 7 bits of the fourth byte.
    /// The top bit of the fourth byte, passed on as it is and not interpreted: its meaning is not
    /// checked against the standard, so the user decides what it means. Diagnostics sends 0.
    bool byte4_high_bit = false;
    std::array<std::uint8_t, 4> raw{}; ///< The four bytes of this DTC as received.

    friend bool operator==(const ReadDtc&, const ReadDtc&) = default;
};

/// A DTC list (the data of a DM1 or DM2) read in place: it allocates nothing and refers to the
/// bytes it was read from, so it is valid only while they live (in a handler: during the call,
/// like Message::data). Copy the ReadDtc values to keep them.
///
/// The layout is the inverse of what Diagnostics sends: the lamp status and lamp flash bytes,
/// then 4 bytes per DTC. A list of up to 8 bytes holds one DTC (the bytes after it are padding);
/// a longer list is 2 + 4 * n bytes, or it is not valid. A list whose only DTC is 4 zero bytes
/// (the list without DTCs) is valid and empty. Not valid: size() is 0 and no lamp is on.
class DtcListView
{
public:
    /// Reads the DTCs one after the other (a forward iterator; it yields ReadDtc by value).
    class iterator
    {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = ReadDtc;
        using difference_type = std::ptrdiff_t;
        using reference = ReadDtc;

        iterator() noexcept = default;
        [[nodiscard]] ReadDtc operator*() const noexcept;
        iterator& operator++() noexcept
        {
            at_ += 4;
            return *this;
        }
        iterator operator++(int) noexcept
        {
            iterator before = *this;
            at_ += 4;
            return before;
        }
        friend bool operator==(const iterator&, const iterator&) = default;

        /// At the first of the 4 bytes of a DTC; begin() and end() make them.
        explicit iterator(const std::uint8_t* at) noexcept : at_(at) {}

    private:
        const std::uint8_t* at_ = nullptr;
    };

    /// Not valid (like data of the wrong length).
    DtcListView() noexcept = default;
    /// Reads `data`; the same as parse_dtc_list(data).
    explicit DtcListView(std::span<const std::uint8_t> data) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    /// How the lamp shows: empty if it is off (also for Lamp::None and a list that is not
    /// valid). As the inverse of Diagnostics: on is the status value 1; the flash value 0 is
    /// Slow, 1 is Fast and the others Solid. Another status value counts as off; data() has the
    /// raw bytes for another reading.
    [[nodiscard]] std::optional<LampFlash> lamp(Lamp which) const noexcept;
    /// Whether the list holds the DTC with this SPN and FMI.
    [[nodiscard]] bool has(std::uint32_t spn, std::uint8_t fmi) const noexcept;
    [[nodiscard]] iterator begin() const noexcept { return iterator(entries()); }
    [[nodiscard]] iterator end() const noexcept { return iterator(entries() + 4 * count_); }
    /// The bytes the list was read from, as given to parse_dtc_list().
    [[nodiscard]] std::span<const std::uint8_t> data() const noexcept { return data_; }

private:
    [[nodiscard]] const std::uint8_t* entries() const noexcept { return count_ == 0 ? nullptr : data_.data() + 2; }

    std::span<const std::uint8_t> data_;
    std::size_t count_ = 0;
    bool valid_ = false;
};

/// Reads a DTC list (see DtcListView) without allocating. Any length is safe: data of the wrong
/// length gives a list that is not valid.
[[nodiscard]] DtcListView parse_dtc_list(std::span<const std::uint8_t> data) noexcept;

} // namespace vehicle_sil::j1939
