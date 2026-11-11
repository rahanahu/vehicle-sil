// SPDX-License-Identifier: MIT
//
// The toolbox of the J1939 stack (vehicle_sil::j1939) for applications and protocol
// parts: Subscription (a handler registration that removes itself) and Timer (a virtual-time
// timer). Node::attach() (network.hpp) completes it.
#pragma once

#include "vehicle_sil/j1939/network.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace vehicle_sil::j1939 {

/// Owns one handler or request responder of a node (HandlerId) and removes it when destroyed
/// or reset():
///     Subscription sub{node, node.on_message(pgn, [this](const Message& m) { ... })};
/// After reset() or destruction the handler is not called any more, also when that happens
/// during a delivery (in the handler itself or in another one); the handler object is destroyed
/// after the delivery. Safe after the network was destroyed (it then does nothing). Moving
/// keeps the registration. Not thread safe: use it from the network's thread only.
class Subscription
{
public:
    Subscription() = default;
    /// Takes over `id`, a registration of `node` (Node::on_message(), on_request(), ...).
    Subscription(Node& node, HandlerId id) noexcept;
    ~Subscription() { reset(); }
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    /// Removes the handler now (nothing if it is already removed).
    void reset() noexcept;
    /// true while it holds a registration (and the network exists).
    [[nodiscard]] bool active() const noexcept { return node_ != nullptr && !alive_.expired(); }

private:
    Node* node_ = nullptr;
    HandlerId id_{};
    std::weak_ptr<const bool> alive_;
};

/// A timer of a network in virtual time that calls `callback` once per start, in a step (see
/// TimerKind for where it fires relative to the frames). Its slot is reserved when it is
/// created (NetworkOptions::timer_capacity, shared with address claiming), so start_at() never
/// fails for lack of room, and the timers of the protocol keep theirs.
///
/// - start_at() and start_after() restart a running timer. They are allowed everywhere: in a
///   handler, a responder, a timer, the application stage, and outside a step. A deadline that
///   has passed is not refused: the timer fires at the next chance (the next frame or the end of
///   the step, or the next step). Started in a timer's callback, it fires in the same round of
///   firing only if its deadline is after that timer's deadline (so a periodic timer catches up
///   within a step); otherwise in the next round.
/// - start_after(d) counts from the time of the event being handled: the time of the message a
///   handler (or responder) got, the deadline of the Timer that fires, otherwise the step time
///   (time 0 before the first step). A timer that restarts itself with start_after() keeps its
///   period exactly, whatever the step size.
/// - The callback may start, cancel and destroy this timer and others; destroying the Timer in
///   its own callback is safe (the callback object lives until it returns).
/// - Destroying the Timer cancels it and returns its slot. Safe after the network was destroyed
///   (start_at() then throws std::logic_error, cancel() does nothing).
/// Not thread safe: use it from the network's thread only. Not movable (the network refers to
/// it while it runs).
class Timer final : private TimerTarget
{
public:
    using Callback = std::function<void()>;

    /// Reserves a timer slot of `network`; std::length_error if all of timer_capacity is
    /// reserved (by nodes with a NAME, 2 each, and other Timers; Network::timers_free() is 0).
    /// std::invalid_argument for an empty callback.
    Timer(Network& network, TimerKind kind, Callback callback);
    Timer(Node& node, TimerKind kind, Callback callback) : Timer(node.network(), kind, std::move(callback)) {}
    ~Timer();
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    /// Starts (or restarts) the timer to fire at `deadline`.
    void start_at(SimTime deadline);
    /// Starts (or restarts) the timer to fire `delay` after the time of the current event.
    void start_after(SimDuration delay);
    /// Stops the timer. false if it was not running.
    bool cancel() noexcept;
    [[nodiscard]] bool running() const noexcept;
    /// The deadline of the running timer, or in its callback the deadline it fired for (until
    /// it is restarted); empty otherwise.
    [[nodiscard]] std::optional<SimTime> deadline() const noexcept;

private:
    void on_timer(std::uint32_t tag) override;

    Network* network_;
    std::weak_ptr<const bool> alive_;
    TimerKind kind_;
    Callback callback_;
    TimerId id_{};
    SimTime deadline_{};
    bool* destroyed_ = nullptr; ///< Set while the callback runs: tells it the Timer is gone.
};

} // namespace vehicle_sil::j1939
