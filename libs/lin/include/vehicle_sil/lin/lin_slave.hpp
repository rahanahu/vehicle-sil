// SPDX-License-Identifier: MIT
//
// LIN slave: responses, frame reception, sleep and wakeup, in SIL Kit virtual time
// (vehicle_sil::lin).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/core/step_inbox.hpp"
#include "vehicle_sil/lin/lin_types.hpp"

#include <silkit/services/lin/ILinController.hpp>

#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace vehicle_sil::lin {

struct LinSlaveStatistics
{
    std::uint64_t responses_sent = 0;
    std::uint64_t headers_ignored = 0; ///< Headers not answered: no value (yet), or another node's frame.
    std::uint64_t frames_received = 0; ///< Frames handed to the frame handler.
    std::uint64_t wakeups_sent = 0;
    std::uint64_t inbox_overflows = 0; ///< Bus events lost because step() was not called; an error.
};

struct LinSlaveOptions
{
    /// Identifiers of frames published by the master that step() hands to the frame handler.
    std::vector<LinId> subscriptions;
    /// The slave goes to sleep when the bus has been idle this long (LIN 2.x: 4 s). std::nullopt
    /// disables it.
    std::optional<SimDuration> bus_idle_timeout{std::chrono::seconds(4)};
};

/// A slave node of a LIN cluster on a SIL Kit LIN controller, in virtual time.
///
/// Responses: an application (e.g. a sensor model) calls set_response() from its step, and the
/// following headers carry the value. The slave uses the SIL Kit dynamic response mode
/// (experimental API in SIL Kit v5.0.8): it receives each header and answers it with the newest
/// value that was set at a step strictly before the header's time. A static TX buffer would not
/// be deterministic: SIL Kit does not fix whether a header stamped T is processed before or
/// after the slave's own step at T. The slave's step must be smaller than every slot it answers
/// in (see LinMaster).
///
/// Bus events (frames, go-to-sleep commands, wakeup pulses, headers) are queued and handed to
/// the application by step(now), in time order, for events stamped before `now`. Everything
/// the master and the other slaves send from their steps at T has arrived by every step after
/// T, so what step() reports does not depend on wall-clock speed.
///
/// - Reception: frames the master publishes on a subscribed identifier, stamped with their
///   header time. Responses of other slaves are not supported: their arrival is not bound to a
///   step of the sender.
/// - Sleep: the go-to-sleep command, or a bus idle for `bus_idle_timeout`, puts the slave to
///   sleep. A go-to-sleep command wins over a wakeup pulse of the same time.
/// - Wakeup: a wakeup pulse or a header wakes the slave. A header is answered even if the slave
///   was asleep: its break field is itself a wakeup signal, and answering it keeps the response
///   independent of the arrival order of other nodes' pulses. request_wakeup() makes the next
///   step() send a wakeup pulse if the slave is asleep.
/// - The sleep state exists only in LinSlave: the SIL Kit controller stays operational. A
///   sleeping SIL Kit controller drops frames, the go-to-sleep command among them, so whether it
///   saw a command would depend on arrival order.
///
/// - init() configures the controller as dynamic-response slave. Call it from
///   ParticipantRuntime::set_communication_ready_handler().
/// - step(now) must be called once per simulation step, before the application reads state().
///
/// Thread safety: set_response(), clear_response() and set_slave_response() may be called from
/// any thread. init(), step() and request_wakeup() belong to the participant's step handler.
/// SIL Kit runs the step handler and the receive handlers on one thread (its I/O thread), and
/// its LIN controller is not thread safe; the slave calls the controller only from there. The
/// state the handlers share with step() is still guarded.
class LinSlave
{
public:
    using FrameHandler = std::function<void(const LinReceivedFrame&)>;
    using StateHandler = std::function<void(SimTime time, LinNodeState state)>;

    /// Throws std::invalid_argument for an invalid subscription or a non-positive idle timeout.
    explicit LinSlave(SilKit::Services::Lin::ILinController& controller, LinSlaveOptions options = {},
                      FrameHandler on_frame = {}, StateHandler on_state = {});
    ~LinSlave();

    LinSlave(const LinSlave&) = delete;
    LinSlave& operator=(const LinSlave&) = delete;
    LinSlave(LinSlave&&) = delete;
    LinSlave& operator=(LinSlave&&) = delete;

    void init();
    void step(SimTime now);

    /// From the step at `now` on, headers for `id` stamped after `now` are answered with `data`
    /// (1..8 bytes). Throws std::invalid_argument for an invalid length or identifier, a
    /// diagnostic frame identifier (kMasterRequestId, kSlaveResponseId; see
    /// set_slave_response()) or LinChecksumModel::Unknown, and std::logic_error if `now` is
    /// earlier than a previous call for the same identifier.
    void set_response(SimTime now, LinId id, std::span<const std::uint8_t> data,
                      LinChecksumModel checksum_model = LinChecksumModel::Enhanced);

    /// Diagnostic slave response: the first slave response header (kSlaveResponseId) stamped
    /// after `now` is answered with `data` (kDiagnosticFrameLength bytes, classic checksum), once;
    /// later headers are not answered until the next call. Every call is a new response to send
    /// once: for headers stamped after `now` it replaces a response set earlier, whether or not
    /// that one was sent (headers up to `now` still see the earlier one).
    /// clear_response(now, kSlaveResponseId) withdraws it. Throws std::invalid_argument for
    /// another length and std::logic_error if `now` is earlier than a previous call.
    void set_slave_response(SimTime now, std::span<const std::uint8_t> data);

    /// From the step at `now` on, headers for `id` stamped after `now` are not answered; for
    /// kSlaveResponseId this withdraws a set_slave_response() that has not been sent. Throws
    /// std::invalid_argument for an identifier above 63 and std::logic_error if `now` is earlier
    /// than a previous call for the same identifier.
    void clear_response(SimTime now, LinId id);

    void request_wakeup();

    [[nodiscard]] LinNodeState state() const noexcept { return state_; }
    [[nodiscard]] LinSlaveStatistics statistics() const;

private:
    struct Value
    {
        SimTime since;
        std::optional<SilKit::Services::Lin::LinFrame> frame; ///< nullopt: do not answer
        bool once = false; ///< answer only the first header (set_slave_response())
        bool sent = false; ///< a header was answered with a `once` value
    };
    /// Headers that arrive after set_response(now, ...) are stamped >= now, so two values per
    /// identifier suffice: the one set at the latest step and the one set before it.
    struct Values
    {
        std::optional<Value> previous;
        std::optional<Value> current;
    };

    /// A bus event. Equal timestamps are ordered by kind, then identifier (BusEventOrder).
    struct BusEvent
    {
        /// A go-to-sleep command wins over a wakeup pulse of the same time, as at the master.
        enum class Kind
        {
            WakeupPulse,
            GoToSleep,
            Header,
            Frame,
        };
        Kind kind = Kind::Header;
        LinId id = 0;
        std::uint8_t data_length = 0;
        std::array<std::uint8_t, kMaxLinDataLength> data{};
    };
    struct BusEventOrder
    {
        bool operator()(const BusEvent& a, const BusEvent& b) const noexcept
        {
            return a.kind != b.kind ? a.kind < b.kind : a.id < b.id;
        }
    };

    void store(SimTime now, LinId id, std::optional<SilKit::Services::Lin::LinFrame> frame, bool once = false);
    void on_header(SimTime header_time, LinId id);
    void on_frame_status(const SilKit::Services::Lin::LinFrameStatusEvent& event);
    void on_go_to_sleep(SimTime time);
    void on_wakeup(SimTime time, SilKit::Services::TransmitDirection direction);
    void push(SimTime time, const BusEvent& event);
    void set_state(SimTime time, LinNodeState state);

    SilKit::Services::Lin::ILinController& controller_;
    const std::optional<SimDuration> bus_idle_timeout_;
    const FrameHandler on_frame_;
    const StateHandler on_state_;
    std::bitset<kMaxLinId + 1> subscriptions_;
    std::optional<SilKit::Util::HandlerId> header_handler_;
    std::optional<SilKit::Util::HandlerId> status_handler_;
    std::optional<SilKit::Util::HandlerId> sleep_handler_;
    std::optional<SilKit::Util::HandlerId> wakeup_handler_;

    // Step-handler state.
    LinNodeState state_ = LinNodeState::Operational;
    SimTime last_activity_{};
    bool wakeup_requested_ = false;

    StepInbox<BusEvent, BusEventOrder> events_{4096};

    mutable std::mutex mutex_; // guards values_ and stats_
    std::array<Values, kMaxLinId + 1> values_; // indexed by identifier
    LinSlaveStatistics stats_;

};

} // namespace vehicle_sil::lin
