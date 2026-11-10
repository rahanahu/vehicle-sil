// SPDX-License-Identifier: MIT
//
// Authentication and authorization of the control server's calls (internal).
#pragma once

#include "vehicle_sil/control_grpc/server.hpp"

#include <grpcpp/grpcpp.h>

#include <optional>
#include <string>
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

} // namespace vehicle_sil::control_grpc::detail
