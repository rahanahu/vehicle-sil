// SPDX-License-Identifier: MIT
//
// run --control in a build without the control server (run_control_server.hpp): there is none to make.
#include "run_control_server.hpp"

#include <stdexcept>

namespace vehicle_sil::launch::detail {

ControlServerOfRun make_control_server_of_run(const ControlHost&, RunControls&)
{
    throw std::invalid_argument(
      "RunHost::control is set, but this build has no control server; configure with "
      "-DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON (it needs gRPC) and build again, or leave `control` empty.");
}

} // namespace vehicle_sil::launch::detail
