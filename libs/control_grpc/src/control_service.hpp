// SPDX-License-Identifier: MIT
//
// The clients' service of the control server (internal).
#pragma once

#include "audit.hpp"
#include "auth.hpp"
#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/control_grpc/simulation_monitor.hpp"

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace vehicle_sil::control_grpc {
class SimulationMonitor;
} // namespace vehicle_sil::control_grpc

namespace vehicle_sil::control_grpc::detail {

/// The monitor's snapshots a Watch holds until its stream takes them, in order. Beyond
/// `capacity`, the oldest is dropped and counted (Update.dropped). Thread safe.
class SnapshotQueue
{
public:
    explicit SnapshotQueue(std::size_t capacity) : capacity_(capacity) {}

    void push(const SimulationSnapshot& snapshot)
    {
        const std::lock_guard lock(mutex_);
        if (snapshots_.size() >= capacity_)
        {
            snapshots_.pop_front(); // a slow client misses intermediate states
            ++dropped_;
        }
        snapshots_.push_back(snapshot);
    }
    /// Everything held, oldest first.
    [[nodiscard]] std::deque<SimulationSnapshot> take()
    {
        std::deque<SimulationSnapshot> out;
        const std::lock_guard lock(mutex_);
        out.swap(snapshots_);
        return out;
    }
    /// Snapshots dropped so far.
    [[nodiscard]] std::uint64_t dropped() const
    {
        const std::lock_guard lock(mutex_);
        return dropped_;
    }

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::deque<SimulationSnapshot> snapshots_;
    std::uint64_t dropped_ = 0;
};

/// ControlService. `monitor` is nullptr without ServerOptions::time.
std::unique_ptr<grpc::Service> make_control_service(control::Hub& hub, const ServerOptions& options, SimulationMonitor* monitor,
                                                    const std::atomic<bool>& stopping, const Authorizer& auth, const AuditLog& audit);

} // namespace vehicle_sil::control_grpc::detail
