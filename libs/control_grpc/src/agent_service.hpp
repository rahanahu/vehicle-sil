// SPDX-License-Identifier: MIT
//
// The agents' service of the control server (internal).
#pragma once

#include "audit.hpp"
#include "auth.hpp"
#include "vehicle_sil/control/hub.hpp"

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>

#include <memory>
#include <mutex>
#include <vector>

namespace vehicle_sil::control_grpc::detail {

/// The live agent streams, so that the server can drop them (ControlServer::disconnect_agents()).
struct StreamRegistry
{
    struct Handle
    {
        std::recursive_mutex mutex;
        grpc::CallbackServerContext* context = nullptr; // nullptr once the stream is done
    };
    std::mutex mutex;
    std::vector<std::weak_ptr<Handle>> handles;

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

/// AgentService: one bidirectional stream per agent connection.
std::unique_ptr<grpc::Service> make_agent_service(control::Hub& hub, const Authorizer& auth, const AuditLog& audit,
                                                  StreamRegistry& streams);

} // namespace vehicle_sil::control_grpc::detail
