// SPDX-License-Identifier: MIT
//
// The clients' service of the control server (internal).
#pragma once

#include "audit.hpp"
#include "auth.hpp"
#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_grpc/server.hpp"

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>

#include <atomic>
#include <memory>

namespace vehicle_sil::control_grpc {
class SimulationMonitor;
} // namespace vehicle_sil::control_grpc

namespace vehicle_sil::control_grpc::detail {

/// ControlService. `monitor` is nullptr without ServerOptions::time.
std::unique_ptr<grpc::Service> make_control_service(control::Hub& hub, const ServerOptions& options, SimulationMonitor* monitor,
                                                    const std::atomic<bool>& stopping, const Authorizer& auth, const AuditLog& audit);

} // namespace vehicle_sil::control_grpc::detail
