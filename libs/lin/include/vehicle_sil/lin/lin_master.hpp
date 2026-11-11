// SPDX-License-Identifier: MIT
//
// LIN master: schedule table, frame transmission and reception, sleep and wakeup, in virtual
// time, on a LinPort (vehicle_sil::lin, no SIL Kit).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/core/step_inbox.hpp"
#include "vehicle_sil/lin/lin_port.hpp"
#include "vehicle_sil/lin/lin_schedule.hpp"
#include "vehicle_sil/lin/lin_types.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace vehicle_sil::lin {

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
    /// After a wakeup pulse, the schedule restarts at its first entry at the first step at least
    /// this long after the pulse. LIN 2.x gives the slaves 100 ms to become ready.
    SimDuration wakeup_delay{std::chrono::milliseconds(100)};
};

/// The master node of a LIN cluster, in virtual time. It reaches the bus only through its
/// LinPort (a VirtualLinBus endpoint, or a SilKitLinPort of vehicle_sil::lin_silkit); Ecu::
/// lin_master() makes it with the port of the runtime.
///
/// - init() opens the port (SIL Kit: configures the controller as master). Call it once before
///   the first step (with SIL Kit: from ParticipantRuntime::set_communication_ready_handler()).
/// - step(now) must be called once per simulation step, with the step's time; the steps are
///   `step` apart, starting at T = 0. Everything the master puts on the bus happens in step():
///   headers, its own frames, the go-to-sleep command and wakeup pulses. Requests
///   (set_frame_data(), set_schedule(), request_sleep(), request_wakeup()) made in the step
///   before step(now) take effect in that step.
///
/// Schedule: the first header goes out at T = 0. When a slot ends, step() reports the result
/// of the header sent at its start, then sends the next header. Every slot must be a multiple
/// of the master's step, so headers go out exactly on their slot times.
///
/// Frames the master publishes (LinPublisher::Master) carry the newest set_frame_data() value
/// for their identifier, zeros until the first one.
///
/// Determinism of slave responses: a slave answers a header from its port's header event, so its
/// response is not bound to a step. On the virtual bus it arrives inside send_header(). On SIL
/// Kit, let the header be sent at T with slot length S, and let the responding slave's step size
/// be D < S (strictly). The slave may already have executed its step at T, but it cannot
/// announce a time of T + S or later before it has executed a step after T, and it can execute
/// that step only after the master has announced its next time, i.e. after the header has
/// reached it. So the response has arrived before the master's step at T + S. The master pairs
/// the response with the header itself and reports it at T + S, independent of wall-clock speed.
/// With D >= S the response may arrive after T + S: it is then reported as NoResponse, or taken
/// for the response to a later header of the same identifier. vehicle-sil does not support that
/// configuration.
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
/// Ownership: the master does not own its port; the port must outlive it. The master listens
/// to the port from its constructor to its destructor.
///
/// Thread safety: init(), step(), the handler setters and the request functions belong to the
/// step of the node's runtime. The port reports on the thread that runs the steps (SIL Kit: its
/// I/O thread; virtual bus: inside the sender's step); the state its events share with step() is
/// still guarded.
class LinMaster
{
public:
    using ResultHandler = std::function<void(const LinScheduleResult&)>;
    using StateHandler = std::function<void(SimTime time, LinNodeState state)>;

    /// A master on `port` (LinRole::Master) stepped every `step`. Throws std::invalid_argument
    /// for an invalid schedule (validate_schedule(): also a step that is not positive) or a
    /// negative wakeup delay; it then leaves the port as it was.
    LinMaster(LinPort& port, LinScheduleTable schedule, SimDuration step, LinMasterOptions options = {});
    ~LinMaster();

    /// Checks the constructor's arguments without making a master: throws std::invalid_argument
    /// exactly where the constructor would. Check before making a port that cannot be removed
    /// again (a SIL Kit LIN controller stays with its participant).
    static void validate(const LinScheduleTable& schedule, SimDuration step, const LinMasterOptions& options = {});

    LinMaster(const LinMaster&) = delete;
    LinMaster& operator=(const LinMaster&) = delete;
    LinMaster(LinMaster&&) = delete;
    LinMaster& operator=(LinMaster&&) = delete;

    /// Called with the result of every header, at the end of its slot (in step()). Set it before
    /// the first step (not thread safe); an empty function reports nothing.
    void on_result(ResultHandler handler) { on_result_ = std::move(handler); }
    /// Called when the master goes to sleep or wakes up, in step(), with the step's time. Set it
    /// before the first step (not thread safe); an empty function reports nothing.
    void on_state(StateHandler handler) { on_state_ = std::move(handler); }

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
    /// identifier whose length, checksum or publisher differs from an earlier schedule (a SIL
    /// Kit controller cannot reconfigure an identifier; the rule holds on every port).
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

    /// Receives the port's events (LinMaster does not show them in its own interface).
    class PortListener final : public LinPortListener
    {
    public:
        explicit PortListener(LinMaster& master) : master_(master) {}
        void on_header(SimTime, LinId) override {}
        void on_response(SimTime header_time, const LinFrame& frame, LinResultStatus status) override
        {
            master_.on_response(header_time, frame, status);
        }
        void on_go_to_sleep(SimTime) override {}
        void on_wakeup(SimTime time) override { master_.on_wakeup(time); }

    private:
        LinMaster& master_;
    };

    void on_response(SimTime header_time, const LinFrame& frame, LinResultStatus status);
    void on_wakeup(SimTime time);
    /// Ends the outstanding slot and returns its result, if any.
    std::optional<LinScheduleResult> end_slot();
    void report(const std::optional<LinScheduleResult>& result);
    using FrameConfigs = std::array<std::optional<FrameConfig>, kMaxLinId + 1>;

    /// `before` with the identifiers of `table` added; throws std::invalid_argument if one differs
    /// from `before` or within `table`.
    static FrameConfigs merge_frame_configs(const FrameConfigs& before, const LinScheduleTable& table);
    /// Adds the identifiers of `table` to frame_configs_; throws std::invalid_argument (and adds
    /// nothing) if one differs from a schedule given before.
    void remember_frame_configs(const LinScheduleTable& table);
    void wake(SimTime now, SimTime pulse_time);
    void notify(SimTime time, LinNodeState state);

    LinPort& port_;
    const SimDuration step_;
    const LinMasterOptions options_;
    ResultHandler on_result_;
    StateHandler on_state_;
    PortListener listener_{*this};

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
    FrameConfigs frame_configs_{};
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
