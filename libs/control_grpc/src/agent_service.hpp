// SPDX-License-Identifier: MIT
//
// The agents' service of the control server (internal).
#pragma once

#include "audit.hpp"
#include "auth.hpp"
#include "vehicle_sil/control/hub.hpp"

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::control_grpc {
class SimulationMonitor;
} // namespace vehicle_sil::control_grpc

namespace vehicle_sil::control_grpc::detail {

/// The live agent streams, so that the server can drop them (ControlServer::disconnect_agents()).
struct StreamRegistry
{
    struct Handle
    {
        std::recursive_mutex mutex;
        grpc::CallbackServerContext* context = nullptr; // nullptr once the stream is done
    };
    /// The last hello of a participant: who replaces an older connection of it.
    struct Hello
    {
        std::string connection;
        std::string incarnation;
        std::string peer; ///< the address it came from
    };

    std::mutex mutex;
    std::vector<std::weak_ptr<Handle>> handles;
    std::map<std::string, Hello> latest; ///< by participant

    void hello(const std::string& participant, Hello h)
    {
        const std::lock_guard lock(mutex);
        latest.insert_or_assign(participant, std::move(h));
    }

    [[nodiscard]] std::optional<Hello> latest_hello(const std::string& participant)
    {
        const std::lock_guard lock(mutex);
        const auto it = latest.find(participant);
        return it == latest.end() ? std::nullopt : std::optional<Hello>(it->second);
    }

    std::shared_ptr<Handle> add(grpc::CallbackServerContext* context)
    {
        auto handle = std::make_shared<Handle>();
        handle->context = context;
        const std::lock_guard lock(mutex);
        std::erase_if(handles, [](const std::weak_ptr<Handle>& h) { return h.expired(); });
        handles.push_back(handle);
        return handle;
    }

    void cancel_all()
    {
        std::vector<std::shared_ptr<Handle>> live;
        {
            const std::lock_guard lock(mutex);
            for (const auto& h : handles)
            {
                if (auto p = h.lock())
                {
                    live.push_back(std::move(p));
                }
            }
        }
        for (const auto& h : live)
        {
            const std::lock_guard lock(h->mutex);
            if (h->context != nullptr)
            {
                h->context->TryCancel();
            }
        }
    }
};

/// AgentService: one bidirectional stream per agent connection. `monitor` (nullptr without one)
/// gives the time of the events the server adds.
std::unique_ptr<grpc::Service> make_agent_service(control::Hub& hub, const Authorizer& auth, const AuditLog& audit,
                                                  StreamRegistry& streams, const SimulationMonitor* monitor);

} // namespace vehicle_sil::control_grpc::detail
