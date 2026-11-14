// SPDX-License-Identifier: MIT
//
// Steps a vehicle_sil::step::Host from the virtual-time steps of a ParticipantRuntime
// (vehicle_sil::core).
#pragma once

namespace vehicle_sil {

class ParticipantRuntime;

namespace step {
class Host;
} // namespace step

/// Steps `host` from the step handler of `runtime`: each SIL Kit step at `now` runs
/// host.step(now). Requires TimeDomain::Virtual of the host's members
/// (step::Host::require_time_domain(), std::logic_error). Before the first step it passes the
/// participant's step size to step::Host::set_step_size(). `runtime` must use virtual time
/// (std::invalid_argument otherwise) and must not have started. An exception from the host makes
/// the participant go to Error (see ParticipantRuntime). `host` must outlive the runtime's steps:
/// stop the runtime first.
void bind_host(ParticipantRuntime& runtime, step::Host& host);

} // namespace vehicle_sil
