// SPDX-License-Identifier: MIT
#include "vehicle_sil/control_grpc/simulation_monitor.hpp"

#include <exception>

namespace vehicle_sil::control_grpc {

namespace {

bool same(const std::optional<ParticipantFailure>& a, const std::optional<ParticipantFailure>& b)
{
    if (a.has_value() != b.has_value())
    {
        return false;
    }
    return !a || (a->participant == b->participant && a->state == b->state && a->reason == b->reason);
}

} // namespace

bool SimulationSnapshot::same_state(const SimulationSnapshot& other) const
{
    return time_error == other.time_error && participants == other.participants && participants_error == other.participants_error
           && same(failure, other.failure) && ended == other.ended;
}

std::string SimulationSnapshot::state_of(const std::string& participant) const
{
    for (const auto& [name, state] : participants)
    {
        if (name == participant)
        {
            return state;
        }
    }
    return {};
}

SimulationMonitor::Subscription::Subscription(Subscription&& other) noexcept :
  monitor_(std::exchange(other.monitor_, nullptr)), id_(std::exchange(other.id_, 0))
{
}

SimulationMonitor::Subscription& SimulationMonitor::Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other)
    {
        reset();
        monitor_ = std::exchange(other.monitor_, nullptr);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

SimulationMonitor::Subscription::~Subscription()
{
    reset();
}

void SimulationMonitor::Subscription::reset()
{
    if (auto* m = std::exchange(monitor_, nullptr))
    {
        m->unsubscribe(id_);
    }
}

SimulationMonitor::SimulationMonitor(TimeControl& time, SimulationMonitorOptions options) :
  time_(time), options_(std::move(options)), check_(options_.leave_grace)
{
    if (!options_.clock)
    {
        options_.clock = [] { return std::chrono::steady_clock::now(); };
    }
    poll();
    if (options_.period > std::chrono::milliseconds::zero())
    {
        thread_ = std::thread([this] { run(); });
    }
}

SimulationMonitor::~SimulationMonitor()
{
    {
        const std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

SimulationSnapshot SimulationMonitor::snapshot() const
{
    const std::lock_guard lock(mutex_);
    return snapshot_;
}

SimulationMonitor::Subscription SimulationMonitor::subscribe(Callback callback)
{
    const std::lock_guard lock(mutex_);
    const auto id = next_id_++;
    subscribers_.emplace(id, std::move(callback));
    return Subscription(this, id);
}

void SimulationMonitor::unsubscribe(std::uint64_t id)
{
    {
        const std::lock_guard lock(mutex_);
        subscribers_.erase(id);
    }
    // A poll that took the subscriber before it was erased calls it under poll_mutex_.
    const std::lock_guard wait(poll_mutex_);
}

void SimulationMonitor::poll()
{
    const std::lock_guard polling(poll_mutex_);
    SimulationSnapshot next = snapshot();
    try
    {
        next.time = time_.state();
        next.time_error.clear();
    }
    catch (const std::exception& e)
    {
        next.time_error = e.what();
    }
    next.ended = next.time.ended;
    std::optional<LeftParticipant> left;
    try
    {
        next.participants = time_.participants();
        left = time_.left_participant();
        next.participants_error.clear();
    }
    catch (const std::exception& e)
    {
        next.participants_error = e.what();
    }
    // A failure stays: once the simulation is aborted, nobody is "left" any more.
    if (!next.failure)
    {
        next.failure = check_.check(left, options_.clock());
    }

    std::vector<Callback> callbacks;
    {
        const std::lock_guard lock(mutex_);
        if (snapshot_.version != 0 && next.time == snapshot_.time && next.same_state(snapshot_))
        {
            return;
        }
        next.version = snapshot_.version + 1;
        snapshot_ = next;
        for (const auto& [id, callback] : subscribers_)
        {
            callbacks.push_back(callback);
        }
    }
    for (const auto& callback : callbacks)
    {
        try
        {
            callback(next);
        }
        catch (const std::exception&)
        {
            // A subscriber must not stop the others (or the monitor's thread).
        }
    }
}

void SimulationMonitor::run()
{
    std::unique_lock lock(mutex_);
    while (!stop_)
    {
        if (cv_.wait_for(lock, options_.period, [this] { return stop_; }))
        {
            break;
        }
        lock.unlock();
        poll();
        lock.lock();
    }
}

} // namespace vehicle_sil::control_grpc
