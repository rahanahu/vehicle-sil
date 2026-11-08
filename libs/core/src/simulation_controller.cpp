// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/experimental/participant/ParticipantExtensions.hpp>
#include <silkit/experimental/services/orchestration/ISystemController.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vehicle_sil {
namespace {

namespace orch = SilKit::Services::Orchestration;

std::int64_t wall_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool has_any_state(const PeerStatus& status) noexcept
{
    return status.state.has_value();
}

void check_speed(const std::optional<double>& speed)
{
    if (speed && !(*speed >= RealTimePacer::kMinFactor && *speed <= RealTimePacer::kMaxFactor))
    {
        throw std::invalid_argument("SimulationController speed must be within [1e-6, 1e6]");
    }
}

} // namespace

/// State of the time gate. Every field is guarded by `mutex`.
struct SimulationController::Gate
{
    using Wall = std::chrono::steady_clock;
    struct Anchor
    {
        SimTime sim;
        Wall::time_point wall;
    };

    mutable std::mutex mutex;
    std::condition_variable cv;
    TimeState state;
    bool pause_next = false;   ///< pause() while no step was held: stand still at the next one
    bool held = false;         ///< the step at state.now is held (paused, or paced)
    SimDuration held_step{};   ///< size of the held step
    std::optional<Anchor> anchor; ///< pacing: wall-clock time of a simulation time
    Wall::time_point deadline{};  ///< pacing: when the held step may complete
    bool quit = false;
    std::thread thread; ///< completes held steps (run_gate())

    /// When the step [start, start + step) may end at the current speed (RealTimePacer's rule).
    [[nodiscard]] Wall::time_point deadline_of(SimTime start, SimDuration step) const
    {
        const auto simElapsed = std::chrono::duration<double, std::nano>((start + step) - anchor->sim);
        return anchor->wall + std::chrono::duration_cast<Wall::duration>(simElapsed / *state.speed);
    }

    /// A new speed: a held step keeps the part of its wait that has passed and waits for the
    /// rest at the new speed; between two steps, the next one is paced from now.
    void change_speed(std::optional<double> speed)
    {
        if (speed == state.speed)
        {
            return;
        }
        if (held && !state.paused && state.speed && speed && anchor)
        {
            const auto wall = Wall::now();
            const auto remainingWall = std::max(Wall::duration::zero(), deadline - wall);
            const auto remainingSim = std::chrono::duration<double, std::nano>(remainingWall) * *state.speed;
            state.speed = speed;
            // The held step [now, now + held_step) has remainingSim left, starting from `wall`.
            anchor = Anchor{state.now + held_step - std::chrono::duration_cast<SimDuration>(remainingSim), wall};
            deadline = deadline_of(state.now, held_step);
            return;
        }
        state.speed = speed;
        if (!held && speed)
        {
            // Between two steps: pace the next step from now on, counting from the end of the
            // last one.
            anchor = Anchor{state.now + held_step, Wall::now()};
            return;
        }
        reanchor();
    }

    /// Starts pacing anew from now: on resume, and when pacing starts.
    void reanchor()
    {
        anchor.reset();
        if (held && state.speed)
        {
            anchor = Anchor{state.now, Wall::now()};
            deadline = deadline_of(state.now, held_step);
        }
    }

    void run_until(SimTime t)
    {
        if (state.paused && state.now >= t)
        {
            return; // already there
        }
        pause_next = false;
        state.pause_target = t;
        if (std::exchange(state.paused, false))
        {
            reanchor();
        }
        else if (held && state.now >= t)
        {
            pause_now_or_next();
        }
    }

    /// Stand still now if a step is held, else at the next one.
    void pause_now_or_next()
    {
        if (held)
        {
            state.paused = true;
            state.pause_target.reset();
        }
        else
        {
            pause_next = true;
        }
    }
};

SimulationController::SimulationController(SimulationControllerOptions options) : options_(std::move(options))
{
    if (options_.required_participants.empty())
    {
        throw std::invalid_argument("SimulationController needs at least one required participant");
    }
    if (std::find(options_.required_participants.begin(), options_.required_participants.end(),
                  options_.participant_name) != options_.required_participants.end())
    {
        throw std::invalid_argument("SimulationController must not list itself as a required participant");
    }
    if (options_.stop_at && *options_.stop_at <= SimDuration::zero())
    {
        throw std::invalid_argument("SimulationController stop_at must be > 0");
    }
    if (options_.time_control)
    {
        if (options_.time_control->step <= SimDuration::zero())
        {
            throw std::invalid_argument("SimulationController time_control.step must be > 0");
        }
        check_speed(options_.time_control->speed);
    }

    ParticipantRuntimeOptions runtimeOptions;
    runtimeOptions.participant_name = options_.participant_name;
    runtimeOptions.registry_uri = options_.registry_uri;
    runtimeOptions.participant_configuration = options_.participant_configuration;
    runtimeOptions.shutdown_timeout = options_.shutdown_timeout;
    runtimeOptions.lifecycle_mode = LifecycleMode::Coordinated;
    if (options_.time_control)
    {
        runtimeOptions.virtual_time = VirtualTimeOptions{options_.time_control->step, std::nullopt};
    }
    else if (options_.stop_at)
    {
        // One step of size stop_at: steps at 0 and at stop_at only, so the controller never
        // holds back the other participants before stop_at.
        runtimeOptions.virtual_time = VirtualTimeOptions{*options_.stop_at, std::nullopt};
    }
    runtime_ = std::make_unique<ParticipantRuntime>(std::move(runtimeOptions));
    system_controller_ = SilKit::Experimental::Participant::CreateSystemController(&runtime_->participant());

    if (runtime_->uses_virtual_time())
    {
        runtime_->set_step_handler([this](SimTime now, SimDuration) {
            if (now == SimTime{})
            {
                wall_start_ns_.store(wall_ns(), std::memory_order_release);
            }
            if (options_.stop_at && now.time_since_epoch() >= *options_.stop_at
                && wall_stop_ns_.load(std::memory_order_acquire) < 0)
            {
                wall_stop_ns_.store(wall_ns(), std::memory_order_release);
                runtime_->request_stop("SimulationController: stop_at reached");
            }
        });
    }
    if (options_.time_control)
    {
        gate_ = std::make_unique<Gate>();
        gate_->state.speed = options_.time_control->speed;
        if (options_.time_control->start_paused)
        {
            gate_->state.pause_target = SimTime{};
        }
        runtime_->set_step_hold([this](SimTime now, SimDuration step) { return hold_step(now, step); });
        gate_->thread = std::thread([this] { run_gate(); }); // last: nothing below may throw
    }
}

SimulationController::~SimulationController()
{
    // Stop first: the step handler and the hold use the members below, which are destroyed
    // before runtime_.
    runtime_->stop("SimulationController destroyed");
    if (gate_)
    {
        {
            const std::lock_guard lock(gate_->mutex);
            gate_->quit = true;
        }
        gate_->cv.notify_all();
        gate_->thread.join();
    }
}

SimulationController::Gate& SimulationController::gate() const
{
    if (!gate_)
    {
        throw std::logic_error("SimulationController: time control needs SimulationControllerOptions::time_control");
    }
    return *gate_;
}

bool SimulationController::hold_step(SimTime now, SimDuration step)
{
    auto& g = *gate_;
    const std::lock_guard lock(g.mutex);
    g.state.now = now;
    g.held_step = step;
    if (std::exchange(g.pause_next, false) || (g.state.pause_target && now >= *g.state.pause_target))
    {
        g.state.paused = true;
        g.state.pause_target.reset();
        g.held = true;
        g.cv.notify_all();
        return true;
    }
    if (g.state.speed)
    {
        const auto wall = Gate::Wall::now();
        if (!g.anchor)
        {
            g.anchor = Gate::Anchor{now, wall};
        }
        g.deadline = g.deadline_of(now, step);
        if (g.deadline > wall)
        {
            g.held = true;
            g.cv.notify_all();
            return true;
        }
    }
    return false;
}

void SimulationController::run_gate()
{
    auto& g = *gate_;
    std::unique_lock lock(g.mutex);
    while (!g.quit)
    {
        if (g.held && !g.state.paused)
        {
            if (!g.state.speed || Gate::Wall::now() >= g.deadline)
            {
                g.held = false;
                lock.unlock(); // release_step() takes the runtime's hold lock, which hold_step() holds while taking ours
                runtime_->release_step();
                lock.lock();
                continue;
            }
            g.cv.wait_until(lock, g.deadline);
            continue;
        }
        g.cv.wait(lock);
    }
}

void SimulationController::pause()
{
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        if (!g.state.paused)
        {
            g.pause_now_or_next();
        }
    }
    g.cv.notify_all();
}

void SimulationController::pause_at(SimTime t)
{
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        g.pause_next = false;
        g.state.pause_target = t;
        if (!g.state.paused && g.held && g.state.now >= t)
        {
            g.pause_now_or_next();
        }
    }
    g.cv.notify_all();
}

void SimulationController::resume()
{
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        g.pause_next = false;
        g.state.pause_target.reset();
        if (std::exchange(g.state.paused, false))
        {
            g.reanchor();
        }
    }
    g.cv.notify_all();
}

void SimulationController::run_until(SimTime t)
{
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        g.run_until(t);
    }
    g.cv.notify_all();
}

void SimulationController::step(SimDuration d)
{
    if (d <= SimDuration::zero())
    {
        throw std::invalid_argument("SimulationController::step() needs d > 0");
    }
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        g.run_until(g.state.now + d);
    }
    g.cv.notify_all();
}

void SimulationController::set_speed(std::optional<double> speed)
{
    check_speed(speed);
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        g.change_speed(speed);
    }
    g.cv.notify_all();
}

TimeState SimulationController::time_state() const
{
    auto& g = gate();
    TimeState state;
    {
        const std::lock_guard lock(g.mutex);
        state = g.state;
    }
    // A simulation that has ended no longer stands still anywhere.
    if (state.paused && runtime_->started() && has_finished(PeerStatus{runtime_->state()}))
    {
        state.paused = false;
    }
    return state;
}

std::optional<SimTime> SimulationController::wait_until_paused(std::chrono::milliseconds timeout)
{
    auto& g = gate();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        // A simulation that has ended no longer stands still anywhere, even if it was paused.
        if (runtime_->started() && has_finished(PeerStatus{runtime_->state()}))
        {
            return std::nullopt;
        }
        {
            std::unique_lock lock(g.mutex);
            if (g.state.paused)
            {
                return g.state.now;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                return std::nullopt;
            }
            // Short slices: the end of the lifecycle is not signalled on this condition variable.
            g.cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)),
                            [&] { return g.state.paused; }); // checked again at the top
        }
    }
}

void SimulationController::start(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto remaining = [&] {
        return std::max(std::chrono::milliseconds(0), std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          deadline - std::chrono::steady_clock::now()));
    };

    // A participant has connected once its first lifecycle status (ServicesCreated) is known.
    // The workflow configuration is published only then, as sil-kit-system-controller does,
    // so that every required participant receives it.
    for (const auto& name : options_.required_participants)
    {
        if (!runtime_->wait_for_peer(name, has_any_state, remaining()))
        {
            throw std::runtime_error("SimulationController: required participant '" + name + "' did not connect within " +
                                     std::to_string(timeout.count()) + " ms");
        }
    }

    std::vector<std::string> required = options_.required_participants;
    required.push_back(options_.participant_name);
    system_controller_->SetWorkflowConfiguration(orch::WorkflowConfiguration{required});

    runtime_->start(remaining());
}

std::optional<orch::ParticipantState> SimulationController::wait_until_finished(std::chrono::milliseconds timeout)
{
    return runtime_->wait_until_finished(timeout);
}

void SimulationController::stop(std::string_view reason) noexcept
{
    runtime_->stop(reason);
}

void SimulationController::abort() noexcept
{
    try
    {
        system_controller_->AbortSimulation();
    }
    catch (const std::exception&)
    {
        // Best effort: destroying the participant disconnects it anyway.
    }
}

std::optional<std::string> SimulationController::failed_participant() const
{
    // Once the controller stops the simulation, the others leave on purpose.
    if (runtime_->stop_requested() || runtime_->state() != orch::ParticipantState::Running)
    {
        return std::nullopt;
    }
    for (const auto& name : options_.required_participants)
    {
        // Error, or already stopped and shut down (a failed peer may get there quickly).
        if (has_finished(runtime_->peer_status(name)))
        {
            return name;
        }
    }
    return std::nullopt;
}

std::optional<std::chrono::nanoseconds> SimulationController::wall_time_to_stop_at() const
{
    const auto start = wall_start_ns_.load(std::memory_order_acquire);
    const auto stop = wall_stop_ns_.load(std::memory_order_acquire);
    if (start < 0 || stop < 0)
    {
        return std::nullopt;
    }
    return std::chrono::nanoseconds(stop - start);
}

} // namespace vehicle_sil
