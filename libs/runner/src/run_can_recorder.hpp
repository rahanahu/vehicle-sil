// SPDX-License-Identifier: MIT
//
// Passive SIL Kit CAN frame observer attached to the launcher's SimulationController for opt-in
// virtual-time CAN recording (--record-can --timeline <file>). Internal.
#pragma once

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <silkit/services/can/CanDatatypes.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace vehicle_sil::runner::detail {

/// Fixed-width POD CAN frame captured on the SIL Kit I/O thread without heap allocations.
struct CapturedCanFrame
{
    static constexpr std::uint8_t kFlagExtended = 1u << 0;
    static constexpr std::uint8_t kFlagRtr = 1u << 1;
    static constexpr std::uint8_t kFlagFd = 1u << 2;

    SimTime at{};
    std::uint16_t net_idx = 0;
    std::uint32_t id = 0;
    std::uint8_t dlc = 0;
    std::uint8_t data_len = 0;
    std::uint8_t flags = 0;
    std::array<std::uint8_t, 64> data{};

    [[nodiscard]] constexpr bool extended() const noexcept { return (flags & kFlagExtended) != 0; }
    [[nodiscard]] constexpr bool rtr() const noexcept { return (flags & kFlagRtr) != 0; }
    [[nodiscard]] constexpr bool fd() const noexcept { return (flags & kFlagFd) != 0; }
};
static_assert(std::is_trivially_copyable_v<CapturedCanFrame>);

/// Passive CAN observer on the launcher's SimulationController participant.
///
/// Threads and locks:
/// - Constructed and torn down on the main thread (RunnerImpl).
/// - tap() runs on the main thread before SimulationController::start(): creates one passive
///   ICanController per CAN network ("vehicle_sil_can_tap_<network>"), registers an RX-only
///   frame handler, and schedules can->Start() in ParticipantRuntime::add_communication_ready_handler().
///   NEVER calls SetBaudRate() or SendFrame(), so CanBusSimulator bitrate conflict checks and
///   bus arbitration timing are completely unaffected.
/// - Callback state lives in std::shared_ptr<State> guarded by a leaf std::mutex (no other lock
///   is ever taken while holding it, and callbacks never touch RunCanRecorder or RunnerImpl).
///   untap() marks closed = true under state->mutex before close_connection(), so any late SIL Kit
///   callback on a detached closing thread returns safely without dangling references.
/// - recording_begins() runs on the start window thread; take() runs on the scenario thread
///   (RunScenario) or main thread (RunRecorder); recording_ends() runs on the scenario thread
///   (at scenario completion / after_scenario: pause handover) or main thread; take_and_end() and
///   untap() run on the main thread.
class RunCanRecorder
{
public:
    /// Batch drained from the recorder: captured timed frames and per-network delta of untimed
    /// frames dropped because SIL Kit CanFrameEvent::timestamp < 0.
    struct Taken
    {
        std::vector<CapturedCanFrame> frames{};
        std::vector<std::uint64_t> untimed_dropped{};
    };

    RunCanRecorder() = default;
    ~RunCanRecorder();
    RunCanRecorder(const RunCanRecorder&) = delete;
    RunCanRecorder& operator=(const RunCanRecorder&) = delete;

    /// Attaches passive RX-only CAN controllers for `networks` on `runtime`. No-op if `networks`
    /// is empty. Main thread, before controller->start().
    void tap(ParticipantRuntime& runtime, std::vector<std::string> networks);

    /// Marks the shared callback state closed and clears queued frames. Safe to call even if tap()
    /// was never called or called already. Main thread, before close_connection().
    void untap() noexcept;

    [[nodiscard]] bool tapped() const noexcept { return tapped_; }
    [[nodiscard]] bool recording() const noexcept;

    /// Networks registered by tap(), indexed by CapturedCanFrame::net_idx.
    [[nodiscard]] const std::vector<std::string>& networks() const noexcept { return networks_; }
    [[nodiscard]] std::string_view network_name(std::uint16_t net_idx) const noexcept;

    /// Starts accepting CAN frames into the queue (discards anything seen before simulation start).
    void recording_begins() noexcept;
    /// Stops accepting CAN frames and clears any unread frames/counters.
    void recording_ends() noexcept;

    /// Drains all queued CAN frames and per-network untimed-drop deltas since the last call.
    [[nodiscard]] Taken take();
    /// Atomically drains remaining frames/deltas and stops recording under one lock.
    [[nodiscard]] Taken take_and_end();

    /// Test helper: feeds a SIL Kit CanFrameEvent directly into the frame handler logic without
    /// needing a SIL Kit transport (initializes state if untap()/tap() was not called).
    void initialize_for_test(std::vector<std::string> networks);
    void feed_for_test(std::uint16_t net_idx, const SilKit::Services::Can::CanFrameEvent& event) noexcept;

private:
    struct State
    {
        std::mutex mutex;
        bool closed = false;
        bool recording = false;
        std::vector<CapturedCanFrame> queue;
        std::vector<std::uint64_t> untimed_dropped;
    };

    static void on_frame(State& state, std::uint16_t net_idx,
                         const SilKit::Services::Can::CanFrameEvent& event) noexcept;

    std::vector<std::string> networks_{};
    std::shared_ptr<State> state_{};
    bool tapped_ = false;
};

} // namespace vehicle_sil::runner::detail
