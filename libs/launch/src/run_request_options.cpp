// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/run.hpp"

#include "vehicle_sil/process/paths.hpp"

namespace vehicle_sil::launch {

RunOptions run_options_of(const RunRequest& request)
{
    RunOptions out;
    out.check = check_options_of(request);
    out.speed = request.intent.speed;
    out.start_paused = request.intent.start_paused;
    out.log_dir = request.host.log_dir;
    out.timeline = process::path_from_utf8(request.host.timeline);
    out.timeline_shown = request.host.timeline;
    out.timeout = request.host.timeout;
    return out;
}

} // namespace vehicle_sil::launch
