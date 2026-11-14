// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/bind_host.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/step/host.hpp"

#include <stdexcept>

namespace vehicle_sil {

void bind_host(ParticipantRuntime& runtime, step::Host& host)
{
    if (!runtime.uses_virtual_time())
    {
        throw std::invalid_argument("vehicle_sil::bind_host: the participant must use virtual time");
    }
    host.require_time_domain(step::TimeDomain::Virtual);
    runtime.set_step_handler([&host](SimTime now, SimDuration step) {
        if (!host.last_step().has_value() && !host.failed())
        {
            host.set_step_size(step);
        }
        host.step(now);
    });
}

} // namespace vehicle_sil
