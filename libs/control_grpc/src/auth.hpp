// SPDX-License-Identifier: MIT
//
// Authentication and authorization of the control server's calls (internal).
#pragma once

#include "vehicle_sil/control_grpc/server.hpp"

#include <grpcpp/grpcpp.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::control_grpc::detail {

/// Who is calling, as far as the server knows.
struct Who
{
    std::string name = "anonymous";
    Role role = Role::Admin;
    bool anyone = true; ///< the server has no credentials: every client may do everything
};

bool allows(const Who& who, Role need);

class Authorizer
{
public:
    explicit Authorizer(const SecurityOptions& security) : security_(security) {}

    /// @returns the caller, or nullopt if credentials are required and the call has none that fits.
    [[nodiscard]] std::optional<Who> identify(const grpc::ServerContextBase& context) const;

private:
    const SecurityOptions& security_;
};

std::string role_name(Role role);

/// What a call does to the simulation. A server that only shows it (ServerOptions::observe_only)
/// refuses every Change.
enum class Effect
{
    Read,   ///< nothing: it reads or waits
    Change, ///< it writes variables, changes the hub's peers or moves the time
    End,    ///< it ends the simulation (Stop): never refused for observe_only, so that a run can always be stopped
};

/// What a call needs: one row of the authorization table.
struct RpcRule
{
    std::string_view service; ///< the service's name in the proto ("ControlService")
    std::string_view method;  ///< the RPC's name in the proto ("Write")
    Role need;                ///< the least role
    Effect effect;            ///< what it does to the simulation
    /// An allowed call leaves a line with its outcome in the audit log. (A refused call always
    /// does.)
    bool audited;
    std::string_view what; ///< the action, for the audit log and the refusal ("write")
};

inline constexpr std::string_view kControlService = "ControlService";

/// The authorization table: a rule for every RPC of the service.
[[nodiscard]] const std::vector<RpcRule>& rpc_rules();
/// The rule of `service`/`method`; nullptr if the table has none (the call is refused).
[[nodiscard]] const RpcRule* find_rule(std::string_view service, std::string_view method);

} // namespace vehicle_sil::control_grpc::detail
