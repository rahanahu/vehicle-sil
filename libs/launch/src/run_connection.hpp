// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run: what one run has of SIL Kit, with the hub that it serves, as one thing
// with one owner (Runner::run(), as RunConnection in run.cpp), which the closing thread takes over
// at the end (Runner::close_connection()). No dependencies but run_close.hpp: the order and the
// lifetime are tested as written, with parts that only note when they go.
#pragma once

#include "run_close.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace vehicle_sil::launch::detail {

/// The hub of a run and the three SIL Kit objects that the run makes for it, in the order of their
/// making: the registry, the controller (the launcher's participant) and the hub's transport on
/// that participant. They go in the reverse order (close(), and the destructor): the transport
/// first (it tells the hub that every link has ended, and it uses the controller's participant),
/// then the controller, then the registry; the hub last, when its last holder lets go of it.
///
/// Lifetime: Runner::run() makes one and is its only holder while the run lasts. At the end it
/// gives it to the closing thread (close_connection_within()), and touches it no more. If that
/// thread is left at its limit, it alone keeps the three and its share of the hub, for as long as
/// it takes.
///
/// The hub is shared (not a member by value) because others hold it too, each for itself: the
/// Runner, whose parts use it until they are destroyed (the scenario's port, the samples' tap),
/// and whoever was given it by RunLent::control_endpoint or before_start (a control server). A
/// holder of the hub holds the hub only: it never keeps the SIL Kit objects.
///
/// Threads: no lock. One thread at a time: the main thread until the hand-over, then the closing
/// thread.
template <class Hub, class Registry, class Controller, class Transport>
struct ConnectionOf
{
    explicit ConnectionOf(std::shared_ptr<Hub> served) : hub(std::move(served)) {}
    ~ConnectionOf() { close(); }
    ConnectionOf(const ConnectionOf&) = delete;
    ConnectionOf& operator=(const ConnectionOf&) = delete;

    /// Destroys the transport, the controller and the registry, in this order; the hub stays.
    /// Each may wait (SIL Kit joins the thread of its connections). Idempotent.
    void close() noexcept
    {
        transport.reset();
        controller.reset();
        registry.reset();
    }

    // In the order of their making. close() is what fixes the order in which they go; the order
    // of the members agrees with it.
    const std::shared_ptr<Hub> hub;
    std::unique_ptr<Registry> registry;
    std::unique_ptr<Controller> controller;
    std::unique_ptr<Transport> transport;
};

/// Closes `connection` (ConnectionOf::close()) on a thread of its own and waits for it as
/// close_within() does. The caller gives its hold away: from here on the closing thread is the
/// only one that touches the connection, and the connection lives until that thread has ended,
/// also if it is left (the result is not empty) and whatever the caller destroys meanwhile.
template <class Connection>
[[nodiscard]] std::optional<CloseLeft> close_connection_within(std::shared_ptr<Connection> connection,
                                                               std::chrono::milliseconds limit, std::chrono::milliseconds poll,
                                                               const std::function<bool()>& interrupted)
{
    return close_within([closing = std::move(connection)] { closing->close(); }, limit, poll, interrupted);
}

} // namespace vehicle_sil::launch::detail
