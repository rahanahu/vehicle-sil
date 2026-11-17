// SPDX-License-Identifier: MIT
//
// Whether a run keeps up with the speed set for it (PacingWatch): the `pacing` of the run's state.
// Used by RunNotes (run_notes.hpp). No dependencies but the name of the answer; tested as written.
#pragma once

#include "vehicle_sil/launch/run_controls.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace vehicle_sil::launch::detail {

/// Whether a run keeps up with the speed set for it (the state's `pacing`), from the looks of the
/// watch at the time gate. Late: the step that has not begun is later than the tolerance, or a
/// step was late within the last kHold; it is on time again only after kHold without either, so
/// that the answer does not flicker. Nothing to keep up with (not paced) forgets what was late.
/// No clock of its own, no lock: one thread.
class PacingWatch
{
public:
    static constexpr std::chrono::seconds kHold{5};

    /// One look at `wall`. `paced`: the simulation goes, at a speed that is a number. `late_now`:
    /// TimeState::current_lateness is more than the tolerance. `late_steps`: TimeState::late_steps.
    [[nodiscard]] Pacing look(std::chrono::steady_clock::time_point wall, bool paced, bool late_now,
                              std::uint64_t late_steps) noexcept
    {
        const bool more = late_steps != late_steps_;
        late_steps_ = late_steps;
        if (!paced)
        {
            late_until_.reset();
            return Pacing::NotPaced;
        }
        if (late_now || more)
        {
            late_until_ = wall + kHold;
        }
        return late_until_ && wall < *late_until_ ? Pacing::Late : Pacing::OnTime;
    }

private:
    std::uint64_t late_steps_ = 0;
    std::optional<std::chrono::steady_clock::time_point> late_until_{};
};

} // namespace vehicle_sil::launch::detail
