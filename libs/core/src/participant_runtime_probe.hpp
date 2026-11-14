// SPDX-License-Identifier: MIT
//
// A way for tests to stand at the places where ParticipantRuntime::start() and the SIL Kit
// thread can overtake each other, so that a test decides the order instead of the scheduler.
// Internal to vehicle_sil::core and not installed: nothing but the tests of the library sets a
// probe. Without one, each place costs one atomic load.
#pragma once

#include <functional>

namespace vehicle_sil::detail {

enum class StartProbePoint
{
    /// On the SIL Kit thread, in the starting handler (no virtual time), after the runtime has
    /// noted that the handler ran. The state is ReadyToRun; SIL Kit sets Running after the
    /// handler has returned.
    StartingHandlerEntered,
    /// On the thread that calls start(), in every round of its wait: after it has read whether
    /// the starting handler (or a step) ran, and before it reads the lifecycle state.
    BetweenFlagsAndState,
    /// On the thread that calls start(), after StartLifecycle() has returned and before the
    /// runtime notes that SIL Kit takes a reported error from now on.
    AfterStartLifecycle,
};

using StartProbe = std::function<void(StartProbePoint)>;

/// Sets the probe of every ParticipantRuntime in this process, or removes it (nullptr). The
/// probe must outlive every runtime that could call it: set it before the runtime is created,
/// remove it after the runtime is destroyed. The probe is called from two threads.
void set_start_probe(const StartProbe* probe) noexcept;

} // namespace vehicle_sil::detail
