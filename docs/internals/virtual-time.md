# Shared virtual time

[日本語](../ja/internals/virtual-time.md)

This page gives the guarantees and limitations the implementation keeps for how virtual time advances, for the time gate (pause, step and speed) and for the order of reception.

All peers share one virtual time through SIL Kit's `ITimeSyncService`. Each peer can have its own step size (for example: physics model 1 ms, LIN slave 2 ms, LIN master 5 ms, J1939 ECU 10 ms). Each peer is a separate process, on the same computer or on different computers.

| Section | Contents |
|---|---|
| [Time types](#time-types) | `SimTime`, `SimDuration`, `SimClock` |
| [Joining and advancing](#joining-and-advancing) | Joining time synchronization, step order, `stop_at` and `end`, aborts, exceptions, stopping |
| [Coupling to real time](#coupling-to-real-time) | Keeping pace with the wall clock with `real_time_factor` |
| [Pause, step and speed (time gate)](#pause-step-and-speed-time-gate) | Where it stops, operations, how lateness is measured, limitations |
| [Reception handling (determinism)](#reception-handling-determinism) | Which messages step T hands over, and in what order |
| [Running J1939 in virtual time](#running-j1939-in-virtual-time) | `Network` and the host's step, timers |
| [Integrating a fixed-period tick()](#integrating-a-fixed-period-tick) | `TickScheduler`, `every`, constraints when integrating a DLL |
| [Test coverage](#test-coverage) | What the unit and integration tests check |

## Time types

- Simulation time is expressed with `vehicle_sil::SimTime` (`std::chrono::time_point<SimClock, nanoseconds>`) and `SimDuration`.
- `SimClock` is not a wall clock and has no `now()`.
- `SimTime` is a different type from the time points of `steady_clock` and `system_clock`, so mixing them is a compile error.
- `SimTime{}` is the start time of the simulation (T = 0).

## Joining and advancing

### Joining time synchronization, and the step handler

If `ParticipantRuntimeOptions::virtual_time` is set, the participant joins time synchronization. The handler is the asynchronous version (`SetSimulationStepHandlerAsync`), and it calls `CompleteSimulationStep()` as soon as the user's handler returns. To the user, it behaves like the blocking version: the step ends when the handler returns.

SIL Kit calls the step handler and the receive handlers on the same I/O thread, so while a step runs, no other handler of that participant is called.

`CompleteSimulationStep()` queues the completion as a task on the I/O thread. So not only the messages sent in the step, but also the messages SIL Kit sends when it delivers those messages to the participant itself (frames the LIN master sends), are sent before the time of the next step is announced. With the blocking version, this second-stage send comes after the announcement, and there is no guarantee that it arrives in time for the receiver's step (see [dependencies.md](../dependencies.md)).

You pass the per-step work with `set_step_handler()`. If you do not pass it before `start()`, `start()` throws `std::logic_error`.

### Start and order of advance

Start and stop are aligned with `LifecycleMode::Coordinated`. `SimulationController` (or `vehicle_sil_sim_controller`) distributes the workflow configuration once the required participants are all present. As a result, all participants start together at T = 0.

SIL Kit makes a participant wait to run step T until all the others have finished their steps before T. The slowest peer sets the pace of the whole simulation.

- If a step handler takes long in wall-clock time (a heavy `tick()`, for example), the other peers wait for it. No step is skipped.
- So whatever the execution speed, each peer's sequence of steps and the data it receives are the same.

### End time (`stop_at` and `VirtualTimeOptions::end`)

If `stop_at` of `SimulationController` is set, the controller joins time synchronization with a single step of length stop_at, and stops the whole simulation at time stop_at. At that point, every other peer has finished all its steps before stop_at. Whether a step at exactly stop_at runs is not defined.

So set the range of each peer's work with `VirtualTimeOptions::end`. With end ≤ stop_at, the last step a peer processes does not depend on the timing of the stop.

| Step | Step handler | `steps_completed()` | Time synchronization and coupling to real time |
|---|---|---|---|
| Starts before end | Called | Counted | Takes part |
| At or after end | Not called | Not counted | Takes part |

From end on, the handler no longer empties the receive buffers (`StepInbox`, the receive queue of `SilKitCanPort`), so they overflow if other peers keep sending. Overflow is judged over the range of steps that were processed.

### Run limits and aborting (`vehicle_sil_sim_controller`)

`--timeout-ms` of `vehicle_sil_sim_controller` limits the wait for the start. If `--stop-at-ms` is given, the same value is also applied separately as a limit on the run after the start. Without `--stop-at-ms`, the simulation continues until Ctrl+C or SIGTERM. Ctrl+C during the wait for the start ends the process at once.

If a required participant leaves during the run, the controller aborts the simulation under these conditions and exits with exit code 1:

| How the participant left | When the controller aborts |
|---|---|
| In Error (a step failure, a lost connection) | At once |
| In any other state | When the simulation has not ended 5 seconds later |

The 5 seconds are a grace period so that a simulation a participant stopped on its own is not mistaken for a failure.

This decision is `vehicle_sil::FailureCheck` (`vehicle_sil/core/failure_check.hpp`), shared by `vehicle_sil::Simulation`, `vehicle_sil_launch run` and the Control Server's `SimulationMonitor`. The tools that own a controller (`Simulation`, `vehicle_sil_sim_controller`, `vehicle_sil_launch run`) also share the loop that waits for the end and aborts or stops: `vehicle_sil::watch_simulation()` (`vehicle_sil/core/simulation_watch.hpp`). Every tool logs the participant's name, state and reason. This matters because SIL Kit does not stop the simulation when a participant fails; virtual time just stops advancing.

### Step handler exceptions and stopping

If the step handler throws, `ParticipantRuntime` catches the exception and reports an error to SIL Kit. The participant goes to Error, and `step_error()` returns the message. `wait_until_finished()` returns on Error too, as an end. SIL Kit leaves a participant in Error as it is until it is stopped, so bring it to Shutdown with `stop()` or the destructor.

The example peer (`examples/virtual_time`) exits with exit code 1 if the simulation ends (is aborted) before reaching its last step before `--end-ms`, even in Shutdown.

Guarantees and rules for stopping:

- When `stop()` returns, the step handler is not running and is not called again.
- If a handler is running, `stop()` waits for it to finish.
- Destroy the objects the handler uses after `stop()` (or the destructor).
- From inside the step handler, call `request_stop()`, not `stop()`.
- In coordinated mode, the stop of any participant stops the whole simulation.

## Coupling to real time

- By default, virtual time advances as fast as possible (it can run faster than real time).
- If `VirtualTimeOptions::real_time_factor` is set, the peer holds each step to the wall clock. 1.0 is real time, 4.0 is 4 times as fast, 0.5 is half speed.
  - The first step ties the wall clock to virtual time. Step [T, T+Δ) does not end before "the tied wall-clock time + (T + Δ − start time) / factor" (`RealTimePacer`).
  - Coupling one peer is enough: the other peers wait for it, so the whole simulation runs at the same speed.
  - Pacing only waits, so it cannot speed up a peer that is slower than the target.
- SIL Kit's `AnimationFactor` (experimental) is not used.

## Pause, step and speed (time gate)

If `SimulationControllerOptions::time_control` is set, `SimulationController` joins time synchronization with steps of `TimeControlOptions::step` (default 10 ms). By holding the completion of its step, it stops the whole simulation (the time gate).

### Where it stops

While the controller holds step P, every other peer in time synchronization runs all its steps whose start time is P or earlier, and runs no step that starts after P. This holds whatever the step sizes (with P = 50 ms, a 1 ms peer runs up to its step at 50 ms, a 3 ms peer up to its step at 48 ms). This is called "stopped at P". The stop position is a multiple of the controller's step.

### Operations

Every operation is thread-safe and does not wait.

| Operation | What it does |
|---|---|
| `pause()` | Stops at the step held for pacing if there is one, otherwise at the next step. |
| `pause_at(T)` | Stops at the first controller step at or after T (at the next step if T has passed). |
| `resume()` | Resumes (and clears a planned stop). |
| `run_until(T)` | Advances to the first controller step at or after T and stops. Does nothing if already stopped at or after T. |
| `step(d)` | The same as `run_until(time_state().now + d)`. |
| `set_speed(f)` | Sets the speed, in virtual seconds per wall-clock second (empty means as fast as possible). |
| `time_state()` | Reads the current position (whether stopped, P, the planned stop, the speed), lateness and paused time (see "How lateness is measured" below). |
| `wait_until_paused(timeout)` | Waits until it stops (returns empty once the lifecycle has ended). |
| `TimeControlOptions::start_paused` | Starts stopped at T = 0 (all peers run only their step at time 0 and stop). |

With `set_speed(f)`, the controller delays the completion of each step until a wall-clock deadline. A speed change keeps the waiting already done, and a resume ties the wall clock again. The gate cannot make the simulation faster than the slowest peer. When a step is late, the gate does not catch up (see "How lateness is measured" below).

### Holding a step

The hold happens outside the step handler (`ParticipantRuntime::set_step_hold()` and `release_step()`). The controller's SIL Kit thread stays free, so it receives state notifications and stops even while stopped. SIL Kit's HealthCheck does not time out the held step. `stop()` and `abort()` while stopped end the whole simulation directly.

### Determinism

The time gate decides only where to stop and how fast to run. Whether a run is stopped part way, stepped one step at a time or run at changed speeds, each peer runs the same steps and receives the same data as in a run without stops.

### Combining with `stop_at`

With `stop_at`, the controller stops at the first of its own steps at or after stop_at. `wall_time_to_stop_at()` includes the time spent stopped and the time waited for speed pacing (the value without the stopped time is `TimeState::running_for`). The step at stop_at is not held, but its lateness is measured.

### When a speed change takes effect, and deadlines

Setting the same value again with `set_speed()` changes nothing. When the value changes, what happens depends on when it changes:

| When the speed changes | What happens |
|---|---|
| While a step is held for pacing | The waiting already done stays; the rest is waited at the new speed. |
| Between steps | The deadline of the next step stays (if that step is already late, its lateness stays too), and counting continues from there at the new speed. |
| Between steps (setting a speed when none was set) | The deadline of the next step becomes that moment. |
| On resuming from stopped | The wall clock is tied again from there. |
| Before the first step (before `start()`) | The tie is made at the first step (the time before the start is not counted as lateness, and the gate does not run faster to make it up). |

### How lateness is measured

The comments of `TimeControlOptions` are the authoritative definitions. Lateness is counted only while there is a speed and a tie to the wall clock (the base).

| Term | Definition |
|---|---|
| Deadline | The wall-clock time at which controller step P may start (base wall-clock time + (P − base virtual time) ÷ speed). |
| Lateness | The wall-clock time at which step P started − its deadline (0 if negative). |
| Late step (`late_steps`) | A step whose lateness exceeds `TimeControlOptions::late_tolerance` (default 10 ms; wall-clock time, independent of the step and the speed). |
| `max_lateness` | The largest lateness, including lateness within the tolerance. |
| Slip (`slipped`) | The sum of the lateness of the late steps. |
| `paused_for` | The total wall-clock time spent stopped (it also grows while stopped). |
| `running_for` | The wall-clock time from the start of the first step to the start of the last step, without the time spent stopped. |

**Deadline.** No step starts before its deadline.

**Lateness.** Step P starts when the gate has released the previous step and the controller knows that all peer steps that start before P have finished. The wake-up delay of the gate's thread is included (`TimeState::max_gate_lateness` holds that part alone, separately). If a peer finishes a step and goes straight into its next step, SIL Kit reports the finish only after that next step, so lateness can be measured larger, by up to the time of one step of that peer (never smaller).

**Slip.** A late step is not caught up with; it becomes the new base. The steps after it advance at the requested speed from there. Lateness within the tolerance does not change the base, and a step whose deadline has passed is not held, so such lateness is made up naturally.

**What is counted.** From the step after the base was taken to the last step (including the step at stop_at). A step whose deadline has passed but that has not started yet is counted with its lateness up to the call of `time_state()` (`current_lateness`). The figures of `time_state()` are "as if the run ended now", and the same step is never counted twice. The figures are fixed at the earliest of these points (`ParticipantRuntime::set_stop_observer()`):

- A request to stop (`stop()`, `request_stop()`, stop_at)
- `abort()`
- The controller's lifecycle leaving Running (also when another peer stopped it)

If a run ends while stopped, its lateness remains.

**Stopped time.** Time spent stopped is not counted (a resume takes the base again). `running_for` is the time up to now while the next step is past its deadline, and while no speed is set and the gate holds nothing. Startup and shutdown are not included.

**Step granularity.** The gate keeps pace with the wall clock only at the boundaries of controller steps. A peer step that starts at time T runs at the deadline of the first controller step at or after T. That is up to "controller step − (greatest common divisor of the controller step and the peer step)" later than the wall-clock time for T, and never earlier. When you connect devices that run in real time, make the controller step the greatest common divisor of the peer steps (`vehicle_sil_launch` does this by default when a speed is given).

### Waiting at the gate

The gate's thread waits until the deadline in `steady_clock` time (`libs/core/src/deadline_waiter.cpp`). On Windows, a timed wait on a condition variable is rounded to the resolution of the system timer (about 15.6 ms by default), so the thread waits on a high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`). On Windows versions without it, it uses an ordinary waitable timer and `timeBeginPeriod(1)` (only while a gate exists).

### Where it is implemented

The state and the calculations are in `libs/core/src/time_gate.{hpp,cpp}` (`detail::TimeGate`). It has no threads, locks or clocks (every function takes the wall-clock time as an argument). `SimulationController` calls it under a mutex and owns the thread that releases held steps. `tests/core/test_time_gate_model.cpp` checks the lateness figures with examples that fix the times.

### Limitations

- Only peers in time synchronization stop. Peers that do not use virtual time (real-time peers, the Vector XL bridge and so on) keep running.
- A smaller controller step means more synchronization round trips per step, which slows the whole simulation.
- A peer's own `real_time_factor` and `set_speed()` both apply (the slower one decides). When the controller sets the speed, do not use `real_time_factor` on the peers.
- Rewinding, and restarting (resetting) a finished simulation, are not possible. To start again, restart the processes.
- Speed pacing is per controller step. A peer's steps in (P, P + controller step] advance together once the controller releases its hold.
- Do not call `wait_until_paused()` from inside the step of a peer in the same process (the stop position is not reached until that step ends, so it always times out). The other operations can be called from inside a step.
- With time_control, the time gate uses `set_step_hold()` and `release_step()` of the controller's `runtime()`; do not call them.
- The hold decision of `ParticipantRuntime::set_step_hold()` is not called after `stop()` has returned or after a stop request. It is called for steps at or after `VirtualTimeOptions::end`.
- Do not call `release_step()` or `stop()` from inside the hold decision (`request_stop()` is allowed).

## Reception handling (determinism)

SIL Kit guarantees only that at step T every message with a timestamp before T has been received. Whether a message with a timestamp of exactly T is received before step T is not defined. vehicle-sil therefore does the following:

- Messages received outside a step are collected in a `StepInbox`.
- Step T hands over only the messages with a timestamp before T, ordered by (timestamp, tie-breaker, arrival order). Messages at exactly T are handed over in the next step.
- Data another peer sent in step T is processed by the receiver in its first step after T. For example, a value that a 1 ms physics model outputs at T is received by a 10 ms ECU in its step at T+1 ms or later (in practice, at the next 10 ms boundary).
- The capacity of `StepInbox` is fixed. When it is full, messages are dropped and counted.

CAN frames follow the same rule. `SilKitCanPort` hands over in step T only the frames with a timestamp before T. In SIL Kit, the arrival order of frames that different senders sent at the same time is not defined, so the J1939 `Network` takes frames from each sender's queue in CAN arbitration order (see "Processing in one step" in [j1939.md](j1939.md#processing-in-one-step)). The result depends neither on step size nor on arrival order.

## Running J1939 in virtual time

The J1939 `Network` runs as a step of the host, on SIL Kit's `SilKitCanPort`. Use `Peer`, or bind the host to the step handler of `ParticipantRuntime` with `bind_host(runtime, host)`. The stack creates no threads and does not read a clock. Timers (such as the 250 ms after Address Claim) advance in virtual time.

Within one step, the order is receive and process, application processing, then transmit (see "Processing in one step" in [j1939.md](j1939.md#processing-in-one-step), and for `Peer`, "Peer" in [execution.md](execution.md#peer-vehicle_silpeer)). Messages the application sends inside a step are transmitted at the end of that step, with the step's time. Timers started by a transmission (such as T3) also count from that time.

## Integrating a fixed-period tick()

Call an existing ECU model (such as a DLL with a fixed-period `tick()`) from the step handler through `TickScheduler`. vehicle-sil is a shared library that a DLL can use; it does not take in the DLL itself.

| How it is called | Relation between period and step size |
|---|---|
| `TickScheduler` | A task's period and offset must be multiples of the participant's step size. Otherwise `add()` throws. |
| `every(period, fn)` of `Ecu` | Not checked (called at most once per step, in the first step at or after the period's time). |

With `TickScheduler`, a task never runs silently off its period. The example ECU checks at startup with `on_step_grid()` that the period is a multiple of the step size before it uses `every`.

One participant can host several tasks with different periods (for example: a 5 ms step with a 10 ms `tick()` and a 20 ms diagnostic task).

`tick()` runs inside the step handler, so:

- While `tick()` runs, no other SIL Kit handler of that participant is called. No mutual exclusion with receive callbacks is needed.
- A long `tick()` makes the whole simulation wait.

### What to check before integrating a DLL

Inside `tick()`, you cannot wait for another peer's response within the same step (it deadlocks). The step completes when the handler returns (see "Joining and advancing"), so there is no way to wait for a response before calling `CompleteSimulationStep()`. Before you integrate a DLL, check that it does not do this.

Places where the DLL depends on real time internally (sleeps, reading the time with `GetTickCount` and the like, XL driver timestamps) cannot be replaced by vehicle-sil. Check each DLL you integrate.

## Test coverage

The unit tests use an in-process registry or a fake CAN controller and run in virtual time. Except for the tests that compare results at different speeds, they do not depend on wall-clock speed. They check:

- Shared virtual time
  - Participants with different step sizes keep SIL Kit's rule for advancing (when step T starts, every other participant has finished its steps before T)
  - The whole simulation waits for a slow participant
  - The speed set with `real_time_factor` is kept
  - Exceptions are reported
  - The periods of `TickScheduler`
  - The order and capacity of `StepInbox`

- Every upstream patch ([dependencies.md](../dependencies.md)) has a vehicle-sil test that fails without the patch.

The multi-process integration tests check:

- The integration tests (`virtual_time.*`) run the registry, `vehicle_sil_sim_controller`, a physics model (1 ms), an ECU (10 ms, a `tick()` from a shared library, and J1939), a LIN sensor (2 ms) and a body controller (5 ms, a LIN master and J1939 reception) as separate processes. Every peer runs on `Peer`.
  - They compare the traces of the peers and check that data passes between peers at the expected virtual times.
  - The ECU sends Address Claimed in its first step and holds its address 250 ms later. It sends CCVS only after that. The times and contents of the Address Claimed and CCVS that the body controller received are compared with the ECU's trace.
  - The traces match byte for byte under each condition: full speed, a slow `tick()`, coupling at 4 times speed, and coupling only one peer at 0.5 times speed.
  - The wall-clock duration is shorter than real time (full speed, 4 times speed) or longer (slow `tick()`, 0.5 times speed).

## See also

- Starting and stopping a participant, threads: [core.md](core.md)
- Overview: [architecture.md](../architecture.md)
- Where things are: [code-map.md](../code-map.md)
- The other internals pages: "[Internals](../architecture.md#internals)" in architecture.md
