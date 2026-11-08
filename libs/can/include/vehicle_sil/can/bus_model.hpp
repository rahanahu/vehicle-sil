// SPDX-License-Identifier: MIT
//
// Arbitration and transmission time of classic CAN frames on one bus (vehicle_sil::can).
// No SIL Kit, no threads, no clock: the caller passes the times.
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vehicle_sil::can {

/// Bits a classic CAN data frame occupies the bus for, including the 3-bit intermission and
/// without stuff bits: 47 + 8 × length with an 11-bit identifier, 67 + 8 × length with a 29-bit
/// identifier. Real frames are longer by their stuff bits (up to about a fifth).
[[nodiscard]] constexpr std::uint32_t frame_bits(const Frame& frame) noexcept
{
    const std::uint32_t length = frame.length < kMaxPayload ? frame.length : static_cast<std::uint32_t>(kMaxPayload);
    return (frame.format == IdFormat::Extended ? 67u : 47u) + 8u * length;
}

/// Time `frame` occupies a bus of `bitrate` bit/s, rounded up to whole nanoseconds.
/// Throws std::invalid_argument if `bitrate` is 0.
[[nodiscard]] SimDuration frame_duration(const Frame& frame, std::uint32_t bitrate);

/// The arbitration field as the bus compares it, most significant bit first; the lower value
/// wins. The 11-bit base identifier comes first. With equal base identifiers, an 11-bit frame
/// wins against a 29-bit frame (its RTR bit is dominant where the 29-bit frame sends the
/// recessive SRR bit), and two 29-bit frames continue with the 18-bit identifier extension.
[[nodiscard]] constexpr std::uint32_t arbitration_key(const Frame& frame) noexcept
{
    if (frame.format == IdFormat::Standard)
    {
        return (frame.id & kMaxStandardId) << 20;
    }
    const std::uint32_t base = (frame.id >> 18) & kMaxStandardId;
    return (base << 20) | (1u << 19) | (1u << 18) | (frame.id & 0x3FFFFu);
}

/// The order of two frames that start at the same time: arbitration_key(), then (only for the
/// same identifier and format, which a real bus does not resolve) the shorter payload, then the
/// payload bytes. true if `a` goes first. Frames that are equal by this order are left to the
/// caller (CanBusModel uses the sender's index).
[[nodiscard]] bool goes_first(const Frame& a, const Frame& b) noexcept;

struct CanBusModelOptions
{
    /// Bit rate in bit/s (> 0).
    std::uint32_t bitrate = 500'000;
    /// Frames each sender can have waiting for the bus (> 0). Storage is allocated per sender
    /// in add_sender().
    std::size_t queue_capacity = 64;
};

/// One classic CAN bus: a transmit queue (FIFO) per sender and arbitration between the queue
/// heads.
///
/// A frame is ready from its `ready` time on. Whenever the bus is free, the ready queue heads
/// compete and the first by goes_first() (then the lower sender index) is transmitted for
/// frame_duration(). A transmission is never interrupted; frames that become ready meanwhile
/// compete when it ends. A sender's frames leave in queue order, so a frame waits for the
/// frames queued before it even if it would win arbitration (a controller with one transmit
/// FIFO).
///
/// Not modelled: stuff bits, error frames, ACK errors and retransmission, error states, remote
/// and CAN FD frames.
///
/// Deterministic: the result depends only on the calls. Not thread safe. After add_sender(),
/// no call allocates.
class CanBusModel
{
public:
    struct Transmission
    {
        std::size_t sender = 0;
        Frame frame{};
        SimTime ready{}; ///< The frame's ready time (submit()).
        SimTime start{};
        SimTime end{};             ///< start + frame_duration()
        std::uint64_t context = 0; ///< As passed to submit().
    };
    using TransmissionSink = SinkRef<Transmission>;

    /// Throws std::invalid_argument for a bitrate or queue capacity of 0.
    explicit CanBusModel(CanBusModelOptions options = {});

    /// Adds a sender and returns its index (0, 1, ...).
    std::size_t add_sender();

    /// Queues `frame` of `sender`, ready from `ready` on. `context` comes back with the
    /// transmission. Returns false (and queues nothing) if the sender's queue is full. Throws
    /// std::invalid_argument for an unknown sender or an invalid frame (is_valid()).
    /// The queue is a FIFO: a frame does not start before the frames queued ahead of it, even
    /// with an earlier `ready`.
    bool submit(std::size_t sender, const Frame& frame, SimTime ready, std::uint64_t context = 0);

    /// Transmits, in bus order, every frame whose transmission starts before `until`, and calls
    /// `sink` for each. No transmission starts before `from` or before the previous one ended.
    /// The caller must have submitted every frame that is ready before `until` and may compete
    /// (see the class comment); a frame submitted later cannot take part in a transmission that
    /// was already decided. Returns the number of transmissions.
    std::size_t run(SimTime from, SimTime until, TransmissionSink sink);

    /// Removes the queued frames of `sender` (e.g. a disconnected controller) and returns how
    /// many there were.
    std::size_t clear(std::size_t sender);

    /// Changes the bit rate of later transmissions (> 0, std::invalid_argument otherwise).
    void set_bitrate(std::uint32_t bitrate);

    [[nodiscard]] std::uint32_t bitrate() const noexcept { return bitrate_; }
    /// End of the last transmission (the bus is busy before it).
    [[nodiscard]] SimTime bus_free() const noexcept { return bus_free_; }
    /// Frames queued by `sender`.
    [[nodiscard]] std::size_t queued(std::size_t sender) const;
    /// Frames queued by all senders.
    [[nodiscard]] std::size_t queued() const noexcept { return total_queued_; }
    [[nodiscard]] std::size_t sender_count() const noexcept { return queues_.size(); }
    [[nodiscard]] std::size_t queue_capacity() const noexcept { return capacity_; }
    /// Transmissions so far.
    [[nodiscard]] std::uint64_t transmissions() const noexcept { return transmissions_; }

private:
    struct Pending
    {
        Frame frame{};
        SimTime ready{};
        std::uint64_t context = 0;
    };
    struct Queue
    {
        std::vector<Pending> slots; // ring buffer of capacity_ entries
        std::size_t head = 0;
        std::size_t size = 0;

        [[nodiscard]] const Pending& front() const { return slots[head]; }
    };

    void pop(Queue& queue) noexcept;

    std::uint32_t bitrate_;
    std::size_t capacity_;
    std::vector<Queue> queues_;
    std::size_t total_queued_ = 0;
    SimTime bus_free_{};
    std::uint64_t transmissions_ = 0;
};

} // namespace vehicle_sil::can
