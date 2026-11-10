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
    std::vector<std::string> participants;
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

/// What a call needs: one row of the authorization table.
struct RpcRule
{
    std::string_view service; ///< the service's name in the proto ("ControlService")
    std::string_view method;  ///< the RPC's name in the proto ("Write")
    Role need;                ///< the least role (Agent: only an agent)
    /// An allowed call leaves a line with its outcome in the audit log. (A refused call always
    /// does.)
    bool audited;
    std::string_view what; ///< the action, for the audit log and the refusal ("write")
};

inline constexpr std::string_view kControlService = "ControlService";
inline constexpr std::string_view kAgentService = "AgentService";

/// The authorization table: a rule for every RPC of both services.
[[nodiscard]] const std::vector<RpcRule>& rpc_rules();
/// The rule of `service`/`method`; nullptr if the table has none (the call is refused).
[[nodiscard]] const RpcRule* find_rule(std::string_view service, std::string_view method);

} // namespace vehicle_sil::control_grpc::detail
