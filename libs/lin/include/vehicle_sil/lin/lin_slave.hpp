// SPDX-License-Identifier: MIT
//
// LIN slave: responses, frame reception, sleep and wakeup, in virtual time, on a LinPort
// (vehicle_sil::lin, no SIL Kit).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/core/step_inbox.hpp"
#include "vehicle_sil/lin/lin_port.hpp"
#include "vehicle_sil/lin/lin_types.hpp"

#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
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

/// A slave node of a LIN cluster, in virtual time. It reaches the bus only through its LinPort
/// (a VirtualLinBus endpoint, or a SilKitLinPort of vehicle_sil::lin_silkit); Ecu::lin_slave()
/// makes it with the port of the runtime.
///
/// Responses: an application (e.g. a sensor model) calls set_response() from its step, and the
/// following headers carry the value. The slave receives each header (LinPortListener::
/// on_header(); on SIL Kit the dynamic response mode, experimental API in SIL Kit v5.0.8) and
/// answers it with the newest value that was set at a step strictly before the header's time.
/// A static TX buffer would not be deterministic: neither SIL Kit nor the step order of the
/// virtual bus fixes whether a header stamped T is processed before or after the slave's own
/// step at T. On SIL Kit the slave's step must be smaller than every slot it answers in (see
/// LinMaster).
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
/// - The sleep state exists only in LinSlave: the port stays operational (a SIL Kit controller
///   stays operational). A sleeping SIL Kit controller drops frames, the go-to-sleep command
///   among them, so whether it saw a command would depend on arrival order.
///
/// - init() opens the port (SIL Kit: configures the controller as dynamic-response slave). Call
///   it once before the first step (with SIL Kit: from
///   ParticipantRuntime::set_communication_ready_handler()).
/// - step(now) must be called once per simulation step, before the application reads state().
///
/// Ownership: the slave does not own its port; the port must outlive it. The slave listens to
/// the port from its constructor to its destructor.
///
/// Thread safety: set_response(), clear_response() and set_slave_response() may be called from
/// any thread. init(), step(), the handler setters and request_wakeup() belong to the step of
/// the node's runtime. The port reports on the thread that runs the steps (SIL Kit: its I/O
/// thread; virtual bus: inside the sender's step); the state its events share with step() is
/// still guarded.
class LinSlave
{
public:
    using FrameHandler = std::function<void(const LinReceivedFrame&)>;
    using StateHandler = std::function<void(SimTime time, LinNodeState state)>;

    /// A slave on `port` (LinRole::Slave). Throws std::invalid_argument for an invalid
    /// subscription or a non-positive idle timeout; it then leaves the port as it was.
    explicit LinSlave(LinPort& port, LinSlaveOptions options = {});
    ~LinSlave();

    /// Checks the constructor's options without making a slave: throws std::invalid_argument
    /// exactly where the constructor would. Check before making a port that cannot be removed
    /// again (a SIL Kit LIN controller stays with its participant).
    static void validate(const LinSlaveOptions& options);

    LinSlave(const LinSlave&) = delete;
    LinSlave& operator=(const LinSlave&) = delete;
    LinSlave(LinSlave&&) = delete;
    LinSlave& operator=(LinSlave&&) = delete;

    /// Called in step() for every subscribed frame the master sent (LinSlaveOptions::
    /// subscriptions), stamped with its header's time. Set it before the first step (not thread
    /// safe); an empty function reports nothing.
    void on_frame(FrameHandler handler) { on_frame_ = std::move(handler); }
    /// Called in step() when the slave goes to sleep or wakes up, with the time of the event.
    /// Set it before the first step (not thread safe); an empty function reports nothing.
    void on_state(StateHandler handler) { on_state_ = std::move(handler); }

    void init();
    void step(SimTime now);

    /// From the step at `now` on, headers for `id` stamped after `now` are answered with `data`
    /// (1..8 bytes). Throws std::invalid_argument for an invalid length or identifier or a
    /// diagnostic frame identifier (kMasterRequestId, kSlaveResponseId; see
    /// set_slave_response()), and std::logic_error if `now` is earlier than a previous call for
    /// the same identifier.
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
        std::optional<LinFrame> frame; ///< nullopt: do not answer
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

    /// Receives the port's events (LinSlave does not show them in its own interface).
    class PortListener final : public LinPortListener
    {
    public:
        explicit PortListener(LinSlave& slave) : slave_(slave) {}
        void on_header(SimTime time, LinId id) override { slave_.on_header(time, id); }
        void on_response(SimTime header_time, const LinFrame& frame, LinResultStatus status) override
        {
            slave_.on_response(header_time, frame, status);
        }
        void on_go_to_sleep(SimTime time) override { slave_.on_go_to_sleep(time); }
        void on_wakeup(SimTime time) override { slave_.on_wakeup(time); }

    private:
        LinSlave& slave_;
    };

    void store(SimTime now, LinId id, std::optional<LinFrame> frame, bool once = false);
    void on_header(SimTime header_time, LinId id);
    void on_response(SimTime header_time, const LinFrame& frame, LinResultStatus status);
    void on_go_to_sleep(SimTime time);
    void on_wakeup(SimTime time);
    void push(SimTime time, const BusEvent& event);
    void set_state(SimTime time, LinNodeState state);

    LinPort& port_;
    const std::optional<SimDuration> bus_idle_timeout_;
    FrameHandler on_frame_;
    StateHandler on_state_;
    std::bitset<kMaxLinId + 1> subscriptions_;
    PortListener listener_{*this};

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
