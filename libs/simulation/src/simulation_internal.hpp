// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>

/// Internal to the library and its tests; not installed, not part of the API.
namespace vehicle_sil::detail {

/// For tests: how long a Simulation without a controller of its own waits, after a failure, for
/// the controller elsewhere to abort (5 s unless set). Applies to the runs of this process that
/// fail afterwards.
void set_abort_wait(std::chrono::milliseconds wait) noexcept;

} // namespace vehicle_sil::detail
