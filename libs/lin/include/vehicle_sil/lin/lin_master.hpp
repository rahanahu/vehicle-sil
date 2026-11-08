// SPDX-License-Identifier: MIT
//
// LIN master: schedule table, frame transmission and reception, sleep and wakeup, in SIL Kit
// virtual time (vehicle_sil::lin).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/core/step_inbox.hpp"
#include "vehicle_sil/lin/lin_schedule.hpp"
#include "vehicle_sil/lin/lin_types.hpp"

#include <silkit/services/lin/ILinController.hpp>

#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>

namespace vehicle_sil::lin {

enum class LinResultStatus
{
    Ok,         ///< The publisher sent the response (for a master frame: the master sent it).
    NoResponse, ///< Nobody responded to the header.
    Error,      ///< Conflicting responses, or a length or checksum mismatch.
};

/// What became of one header of the schedule.
struct LinScheduleResult
{
    SimTime header_time; ///< Simulation time at which the master sent the header.
    LinId id = 0;
    LinPublisher publisher = LinPublisher::Slave;
    LinResultStatus status = LinResultStatus::NoResponse;
    std::uint8_t data_length = 0;
    std::array<std::uint8_t, kMaxLinDataLength> data{};
};

struct LinMasterStatistics
{
    std::uint64_t headers_sent = 0;
    std::uint64_t responses_ok = 0; ///< Including the master's own frames.
    std::uint64_t no_responses = 0;
    std::uint64_t errors = 0;
    std::uint64_t sleep_commands = 0;   ///< Go-to-sleep commands sent.
    std::uint64_t wakeups_by_master = 0; ///< Wakeup pulses this master sent.
    std::uint64_t wakeups_by_slaves = 0; ///< Wakeups caused by a slave's pulse.
    std::uint64_t inbox_overflows = 0;   ///< Wakeup pulses lost because nobody called step(); an error.
};

struct LinMasterOptions
{
    /// Step size of the participant that calls step(). Every slot must be a multiple of it.
    SimDuration step{};
    /// After a wakeup pulse, the schedule restarts at its first entry at the first step at least
    /// this long after the pulse. LIN 2.x gives the slaves 100 ms to become ready.
    SimDuration wakeup_delay{std::chrono::milliseconds(100)};
};

/// The master node of a LIN cluster on a SIL Kit LIN controller, in virtual time.
///
/// - init() configures the controller as master. Call it from
///   ParticipantRuntime::set_communication_ready_handler().
/// - step(now) must be called once per simulation step, with the step's time. Everything the
///   master puts on the bus happens in step(): headers, its own frames, the go-to-sleep command
///   and wakeup pulses. Requests (set_frame_data(), set_schedule(), request_sleep(),
///   request_wakeup()) made from the step handler before step(now) take effect in that step.
///
/// Schedule: the first header goes out at T = 0. When a slot ends, step() reports the result
/// of the header sent at its start, then sends the next header. Every slot must be a multiple
/// of the master's step, so headers go out exactly on their slot times.
///
/// Frames the master publishes (LinPublisher::Master) carry the newest set_frame_data() value
/// for their identifier, zeros until the first one.
///
/// Determinism of slave responses: a slave answers a header from its SIL Kit receive handler,
/// so its response is not bound to a step. Let the header be sent at T with slot length S, and
/// let the responding slave's step size be D < S (strictly). The slave may already have
/// executed its step at T, but it cannot announce a time of T + S or later before it has
/// executed a step after T, and it can execute that step only after the master has announced its
/// next time, i.e. after the header has reached it. So the response has arrived before the
/// master's step at T + S. The master pairs the response with the header itself and reports it
/// at T + S, independent of wall-clock speed. With D >= S the response may arrive after T + S:
/// it is then reported as NoResponse, or taken for the response to a later header of the same
/// identifier. vehicle-sil does not support that configuration.
///
/// Sleep and wakeup:
/// - request_sleep(): at the next slot boundary the master sends the go-to-sleep command instead
///   of the next header and goes to sleep. The ended slot is reported as usual.
/// - request_wakeup(): if asleep, the next step() sends a wakeup pulse.
/// - A slave's wakeup pulse stamped T wakes the master at its first step after T; the state
///   handler reports that step's time. A pulse stamped at the time of the go-to-sleep command
///   does not wake the master (the command wins).
/// - After a wakeup the schedule restarts at its first entry, at the first step at least
///   `wakeup_delay` after the pulse. Requests that do not apply to the current state (a wakeup
///   while operational, a sleep while asleep) are dropped.
///
/// Diagnostic frames: a master request slot (kMasterRequestId) sends the request queued with
/// send_master_request(), once; without a queued request the slot stays silent (no header, no
/// result). A slave response slot (kSlaveResponseId) is a normal slave frame: its header goes out
/// every time, and a slave answers it only when it has a response (LinSlave::set_slave_response()).
///
/// Every identifier has one publisher, data length and checksum model in all schedules. A second
/// response to one header (two slaves, or a slave answering a frame the master publishes) is a
/// collision, reported as LinResultStatus::Error, as on a real bus.
///
/// Thread safety: init(), step() and the request functions belong to the participant's step
/// handler. SIL Kit runs the step handler and the receive handlers on one thread (its I/O
/// thread), and its LIN controller is not thread safe, so the master calls the controller only
/// from there. The state the handlers share with step() is still guarded.
class LinMaster
{
public:
    using ResultHandler = std::function<void(const LinScheduleResult&)>;
    using StateHandler = std::function<void(SimTime time, LinNodeState state)>;

    /// Throws std::invalid_argument for an invalid schedule (validate_schedule()), a step that
    /// is not positive or a negative wakeup delay.
    LinMaster(SilKit::Services::Lin::ILinController& controller, LinScheduleTable schedule, LinMasterOptions options,
              ResultHandler on_result, StateHandler on_state = {});
    ~LinMaster();

    LinMaster(const LinMaster&) = delete;
    LinMaster& operator=(const LinMaster&) = delete;
    LinMaster(LinMaster&&) = delete;
    LinMaster& operator=(LinMaster&&) = delete;

    void init();
    void step(SimTime now);

    /// Sets the response of a frame the master publishes, from the next header for `id` on.
    /// Throws std::invalid_argument unless `id` is a master frame with this data length in a
    /// schedule given to the master (constructor or set_schedule()), or if `id` is the master
    /// request frame (use send_master_request()).
    void set_frame_data(LinId id, std::span<const std::uint8_t> data);

    /// Queues a diagnostic master request: the next master request slot (kMasterRequestId) sends
    /// `data` once. A request queued before step(now) goes out in a slot that starts at `now`.
    /// The request stays queued while the master sleeps and while no schedule has a master
    /// request slot. Returns false, and queues nothing, while the previous request has not been
    /// sent. Throws std::invalid_argument unless `data` is kDiagnosticFrameLength bytes with a
    /// first byte (NAD) other than 0, which is the go-to-sleep command (use request_sleep()).
    bool send_master_request(std::span<const std::uint8_t> data);

    /// true from send_master_request() until the slot that sends the request.
    [[nodiscard]] bool master_request_pending() const noexcept { return master_request_.has_value(); }

    /// Drops a queued master request that has not been sent (e.g. after a diagnostic timeout).
    /// set_schedule() and sleep keep a queued request.
    void cancel_master_request() noexcept { master_request_.reset(); }

    /// Replaces the schedule at the next slot boundary (asleep: at the restart); the new table
    /// starts at its first entry. Throws std::invalid_argument for an invalid table, or for an
    /// identifier whose length, checksum or publisher differs from an earlier schedule: SIL Kit
    /// cannot reconfigure an identifier.
    void set_schedule(LinScheduleTable schedule);

    void request_sleep();
    void request_wakeup();

    [[nodiscard]] LinNodeState state() const noexcept { return state_; }
    [[nodiscard]] LinMasterStatistics statistics() const;

private:
    struct WakeupPulse
    {
    };
    struct FrameConfig
    {
        LinPublisher publisher;
        std::uint8_t data_length;
        LinChecksumModel checksum_model;
        bool operator==(const FrameConfig&) const = default;
    };

    void on_frame_status(const SilKit::Services::Lin::LinFrameStatusEvent& event);
    /// Ends the outstanding slot and returns its result, if any.
    std::optional<LinScheduleResult> end_slot();
    void report(const std::optional<LinScheduleResult>& result);
    /// Adds the identifiers of `table` to frame_configs_; throws std::invalid_argument (and adds
    /// nothing) if one differs from a schedule given before.
    void remember_frame_configs(const LinScheduleTable& table);
    void configure(const LinScheduleEntry& entry);
    void wake(SimTime now, SimTime pulse_time);
    void notify(SimTime time, LinNodeState state);

    SilKit::Services::Lin::ILinController& controller_;
    const LinMasterOptions options_;
    const ResultHandler on_result_;
    const StateHandler on_state_;
    std::optional<SilKit::Util::HandlerId> status_handler_;
    std::optional<SilKit::Util::HandlerId> wakeup_handler_;

    // Step-handler state.
    LinScheduleTable schedule_;
    std::optional<LinScheduleTable> pending_schedule_;
    std::size_t next_entry_ = 0;
    SimTime next_header_time_{};
    LinNodeState state_ = LinNodeState::Operational;
    SimTime sleep_since_{};
    bool sleep_requested_ = false;
    bool wakeup_requested_ = false;
    /// Configuration of every identifier in any schedule given to the master; never changes.
    std::array<std::optional<FrameConfig>, kMaxLinId + 1> frame_configs_{};
    std::bitset<kMaxLinId + 1> configured_; ///< Identifiers configured as Rx in the controller.
    std::array<std::array<std::uint8_t, kMaxLinDataLength>, kMaxLinId + 1> master_data_{};
    std::optional<std::array<std::uint8_t, kDiagnosticFrameLength>> master_request_;

    StepInbox<WakeupPulse> wakeups_{256};

    mutable std::mutex mutex_; // guards outstanding_, answered_ and stats_
    /// Result of the header in the current slot; NoResponse until the response arrives.
    std::optional<LinScheduleResult> outstanding_;
    bool answered_ = false;
    LinMasterStatistics stats_;
};

} // namespace vehicle_sil::lin
