// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/simulation_controller.hpp"

#include "deadline_waiter.hpp"
#include "time_gate.hpp"

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

/// The time gate: its state (detail::TimeGate, guarded by `mutex`) and the thread that completes
/// the steps it holds back.
struct SimulationController::Gate
{
    using Wall = std::chrono::steady_clock;

    explicit Gate(const TimeControlOptions& options) :
      model(options.speed, std::chrono::duration_cast<Wall::duration>(options.late_tolerance), options.start_paused)
    {
    }

    mutable std::mutex mutex;
    std::condition_variable cv;    ///< wait_until_paused()
    detail::DeadlineWaiter waiter; ///< the thread (run_gate())
    detail::TimeGate model;
    bool quit = false;
    std::thread thread; ///< completes held steps (run_gate())

    /// After a change of `model` (made under `mutex`, called without it).
    void wake() noexcept
    {
        cv.notify_all();
        waiter.notify();
    }
};

/// The start window (set_start_window()).
struct SimulationController::Window
{
    std::function<void(const StartWindow&)> function;
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;      ///< the participants wait before their first step (SIL Kit thread)
    bool cancelled = false; ///< start() gave up, or the controller goes: the function is not called (any more)
    bool running = false;   ///< the function runs
    bool quit = false;      ///< the controller goes: the thread ends even if the window never opened
    std::string error;      ///< what the function threw
    /// run_window(); started by start(). It lives until the window has opened and been completed,
    /// or the controller goes: a window that opens after start() gave up is completed all the same.
    std::thread thread;
};

bool StartWindow::cancelled() const
{
    return cancelled_();
}

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
        if (options_.time_control->late_tolerance < std::chrono::nanoseconds::zero())
        {
            throw std::invalid_argument("SimulationController time_control.late_tolerance must not be negative");
        }
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

    if (options_.time_control)
    {
        gate_ = std::make_unique<Gate>(*options_.time_control);
    }
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
                if (gate_)
                {
                    // The last step: the hold is not asked once a stop is requested, so its
                    // lateness is measured here.
                    const std::lock_guard lock(gate_->mutex);
                    gate_->model.begin_last_step(now, Gate::Wall::now());
                }
                runtime_->request_stop("SimulationController: stop_at reached");
            }
        });
    }
    if (gate_)
    {
        runtime_->set_step_hold([this](SimTime now, SimDuration step) { return hold_step(now, step); });
        runtime_->set_stop_observer([this] { end_gate(); });
        gate_->thread = std::thread([this] { run_gate(); }); // last: nothing below may throw
    }
}

SimulationController::~SimulationController()
{
    // The window's function first: it uses what the owner destroys next.
    cancel_window();
    // Then stop: the step handler and the hold use the members below, which are destroyed
    // before runtime_. The window's thread still lives: a window that opens while the stop waits
    // is completed, so that the stop reaches a lifecycle that runs.
    runtime_->stop("SimulationController destroyed");
    end_window();
    if (gate_)
    {
        {
            const std::lock_guard lock(gate_->mutex);
            gate_->quit = true;
        }
        gate_->wake();
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
    bool hold = false;
    {
        const std::lock_guard lock(g.mutex);
        hold = g.model.begin_step(now, step, Gate::Wall::now());
    }
    if (hold)
    {
        g.wake();
    }
    return hold;
}

void SimulationController::run_gate()
{
    auto& g = *gate_;
    std::unique_lock lock(g.mutex);
    while (!g.quit)
    {
        const auto at = g.model.release_at();
        if (!at)
        {
            g.waiter.wait(lock);
            continue;
        }
        const auto wall = Gate::Wall::now();
        if (wall < *at)
        {
            g.waiter.wait_until(lock, *at);
            continue;
        }
        g.model.released(wall);
        lock.unlock(); // release_step() takes the runtime's hold lock, which hold_step() holds while taking ours
        runtime_->release_step();
        lock.lock();
    }
}

void SimulationController::end_gate() noexcept
{
    if (gate_)
    {
        const std::lock_guard lock(gate_->mutex);
        gate_->model.end(Gate::Wall::now());
    }
}

void SimulationController::pause()
{
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        g.model.pause(Gate::Wall::now());
    }
    g.wake();
}

void SimulationController::pause_at(SimTime t)
{
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        g.model.pause_at(t, Gate::Wall::now());
    }
    g.wake();
}

void SimulationController::resume()
{
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        g.model.resume(Gate::Wall::now());
    }
    g.wake();
}

void SimulationController::run_until(SimTime t)
{
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        g.model.run_until(t, Gate::Wall::now());
    }
    g.wake();
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
        g.model.step(d, Gate::Wall::now());
    }
    g.wake();
}

void SimulationController::set_speed(std::optional<double> speed)
{
    check_speed(speed);
    auto& g = gate();
    {
        const std::lock_guard lock(g.mutex);
        g.model.set_speed(speed, Gate::Wall::now());
    }
    g.wake();
}

TimeState SimulationController::time_state() const
{
    auto& g = gate();
    TimeState state;
    {
        const std::lock_guard lock(g.mutex);
        state = g.model.state(Gate::Wall::now());
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
            if (g.model.paused())
            {
                return g.model.now();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                return std::nullopt;
            }
            // Short slices: the end of the lifecycle is not signalled on this condition variable.
            g.cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)),
                            [&] { return g.model.paused(); }); // checked again at the top
        }
    }
}

void SimulationController::set_start_window(std::function<void(const StartWindow& window)> window)
{
    if (!window)
    {
        throw std::invalid_argument("SimulationController::set_start_window(): the function is empty");
    }
    if (window_ || runtime_->started())
    {
        throw std::logic_error("SimulationController::set_start_window() must be called once, before start()");
    }
    auto w = std::make_unique<Window>();
    w->function = std::move(window);
    runtime_->set_communication_ready_handler_async([w = w.get()] {
        {
            const std::lock_guard lock(w->mutex);
            w->open = true;
        }
        w->cv.notify_all();
    });
    window_ = std::move(w);
}

void SimulationController::run_window(std::chrono::steady_clock::time_point deadline)
{
    auto& w = *window_;
    {
        std::unique_lock lock(w.mutex);
        w.cv.wait(lock, [&] { return w.open || w.quit; });
        if (!w.open)
        {
            return; // the controller goes and the window never opened: nothing to complete
        }
        if (w.cancelled)
        {
            // It opened after start() had given up: the function is not called, but the window
            // is completed, so that every participant gets to its first step and can be stopped
            // in order.
            lock.unlock();
            runtime_->complete_communication_ready();
            return;
        }
        w.running = true; // under the lock that start() takes to give up: it then waits for the function
    }
    const StartWindow window(deadline, [this, &w] {
        {
            const std::lock_guard lock(w.mutex);
            if (w.cancelled)
            {
                return true;
            }
        }
        return runtime_->stop_requested() || has_finished(PeerStatus{runtime_->state()});
    });
    std::string error;
    try
    {
        w.function(window);
    }
    catch (const std::exception& e)
    {
        error = *e.what() != '\0' ? e.what() : "exception without message";
    }
    catch (...)
    {
        error = "unknown exception";
    }
    {
        const std::lock_guard lock(w.mutex);
        w.error = std::move(error);
        w.running = false;
    }
    w.cv.notify_all();
    // In every case: nobody's first step runs before this.
    runtime_->complete_communication_ready();
}

void SimulationController::cancel_window() noexcept
{
    if (!window_)
    {
        return;
    }
    std::unique_lock lock(window_->mutex);
    window_->cancelled = true;
    window_->cv.notify_all();
    // The function sees StartWindow::cancelled() and returns.
    window_->cv.wait(lock, [this] { return !window_->running; });
}

void SimulationController::end_window() noexcept
{
    if (!window_)
    {
        return;
    }
    {
        const std::lock_guard lock(window_->mutex);
        window_->quit = true;
    }
    window_->cv.notify_all();
    if (window_->thread.joinable())
    {
        window_->thread.join();
    }
}

void SimulationController::start(std::chrono::milliseconds timeout, const std::function<std::string()>& give_up)
{
    // No limit: the deadline that never comes (give_up decides). Waits go in slices then, so no
    // duration is added to "now" that does not fit.
    const bool limited = timeout != kNoStartLimit;
    const auto deadline = limited ? std::chrono::steady_clock::now() + timeout : std::chrono::steady_clock::time_point::max();
    auto remaining = [&] {
        return limited ? std::max(std::chrono::milliseconds(0), std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                    deadline - std::chrono::steady_clock::now()))
                       : kNoStartLimit;
    };
    constexpr std::chrono::milliseconds kSlice{200};

    // A participant has connected once its first lifecycle status (ServicesCreated) is known.
    // The workflow configuration is published only then, as sil-kit-system-controller does,
    // so that every required participant receives it.
    for (const auto& name : options_.required_participants)
    {
        while (!runtime_->wait_for_peer(name, has_any_state, std::min(remaining(), kSlice)))
        {
            if (give_up)
            {
                if (std::string why = give_up(); !why.empty())
                {
                    throw std::runtime_error("SimulationController: the start was given up: " + why);
                }
            }
            if (remaining() > std::chrono::milliseconds(0))
            {
                continue;
            }
            const std::vector<std::string> missing = missing_participants();
            std::string names;
            for (const auto& m : missing.empty() ? std::vector<std::string>{name} : missing)
            {
                names += (names.empty() ? "'" : ", '") + m + "'";
            }
            throw std::runtime_error("SimulationController: required participant" + std::string(missing.size() > 1 ? "s " : " ") +
                                     names + " did not connect within " +
                                     std::to_string(timeout.count()) + " ms");
        }
    }

    std::vector<std::string> required = options_.required_participants;
    required.push_back(options_.participant_name);
    system_controller_->SetWorkflowConfiguration(orch::WorkflowConfiguration{required});

    if (window_ && !window_->thread.joinable())
    {
        window_->thread = std::thread([this, deadline] { run_window(deadline); });
    }
    try
    {
        runtime_->start(remaining(), give_up);
    }
    catch (...)
    {
        // The function is told to give up and waited for: once start() has returned, either way,
        // it does not run any more (what it uses may be destroyed). The thread stays: if the
        // window opens later, it completes it.
        cancel_window();
        throw;
    }
    if (window_)
    {
        // Running: the window was completed, so the thread is on its way out.
        window_->thread.join();
        const std::lock_guard lock(window_->mutex);
        if (!window_->error.empty())
        {
            throw std::runtime_error("SimulationController: the start window failed: " + window_->error);
        }
    }
}

std::vector<std::string> SimulationController::missing_participants() const
{
    std::vector<std::string> missing;
    for (const auto& name : options_.required_participants)
    {
        if (!has_any_state(runtime_->peer_status(name)))
        {
            missing.push_back(name);
        }
    }
    return missing;
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
    end_gate(); // the run ends here, not when the abort has gone round
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
