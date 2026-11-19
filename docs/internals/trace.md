# Profiling trace (`vehicle_sil::trace`)

[日本語](../ja/internals/trace.md)

The profiling trace shows where the wall-clock time of a virtual-time run goes: which participant completes each step last and what it waited for, and how long each part of a scenario write's round trip takes. These need the virtual time and the write's sequence number, which a sampling profiler does not know. For CPU hot spots, use a sampling profiler (`perf` on Linux, WPR and WPA on Windows).

The trace is an internal tool for developing vehicle-sil. The file format, the record names and the output of `scripts/trace-report.py` may change with any release; do not build on them.

| Section | Contents |
|---|---|
| [Taking a trace](#taking-a-trace) | The environment variables, the files, when a file is not written |
| [Reading the summary](#reading-the-summary) | What each line of `scripts/trace-report.py` means |
| [Viewing the timeline](#viewing-the-timeline) | Perfetto, the threads and the arrows |
| [Records](#records) | Where each record is taken and what its virtual time and argument are |
| [Clocks and other tools](#clocks-and-other-tools) | `perf` on the same clock, several hosts |
| [Cost and limits](#cost-and-limits) | What the trace costs when it is off and when it is on |
| [Rules for changes](#rules-for-changes) | What a new record must keep |

## Taking a trace

Set `VEHICLE_SIL_TRACE` to a directory when you start the launcher. The ECUs it starts inherit the variable, so every process of the run writes its own file there. Then summarize the directory:

```sh
VEHICLE_SIL_TRACE=build/trace vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
scripts/trace-report.py build/trace --merged build/trace/merged.json
```

On Windows, set the variable in the shell in the same way before you start the launcher. An ECU that you start by hand, or that runs on another computer, needs the variable in its own environment.

| Variable | Meaning |
|---|---|
| `VEHICLE_SIL_TRACE` | The directory for the files (made when a file is written). Read once, when the process starts. A relative path is taken from the current directory of the process that reads it first, which sets the variable to the absolute path for the processes it starts (the launcher starts the ECUs in the configuration file's directory). Without it, nothing is recorded. |
| `VEHICLE_SIL_TRACE_EVENTS` | Records kept per thread, from 1 to 16777216 (default 262144, rounded up to a power of two). Each thread that records keeps the newest ones in a ring of its own, of 48 bytes per record. Another value is reported on stderr and the default is used; a ring that cannot be allocated is reported once per process, and that thread records nothing. |

- Each process writes `trace-<participant>-<pid>.json` when it ends (the launcher's is `SimulationController`). A process that recorded nothing, such as an ECU asked for `--describe`, writes no file. Use an empty directory for each run: `scripts/trace-report.py` reads every `trace-*.json` in it.
- A process that is killed writes no file: an ECU interrupted a second time, or before the simulation started, and a process that is killed from outside.
- When a ring overflows, the file keeps the newest records and counts the others as `dropped`. The summary then says so, and covers only the steps that every process still has; raise `VEHICLE_SIL_TRACE_EVENTS`.
- Trace one run per process: `vehicle_sil_launch run`. `vehicle_sil_launch test` runs every scenario in one process, so its file mixes the runs (their virtual times start from 0 each), and the rings of every run's threads stay allocated until the end. `test` says so on stderr when the trace is on, and `scripts/trace-report.py` refuses such a file.
- Do not take a speed baseline with the trace on.

## Reading the summary

`scripts/trace-report.py` prints one block per part, with the mean, p50, p90 and p99 in µs over the steps after `--skip-ms` (200 by default) and before the last five. The details, with every participant's step, go to `build/logs/trace-report.json` (`--json`).

| Line | What it is |
|---|---|
| step period | The time from the start of one step to the start of the next (the launcher's, when it takes part) |
| last to complete | The participant that completed each step last: the critical one. A step is completed when its handler returns, or, for a step the launcher held for its scenario, when the gate released it |
| its step (with the hold) | The critical participant's time from its step's start to its completion |
| sync (its completion to next start) | From that completion to the first start of the next step: SIL Kit's time synchronization messages and the wake-up of the threads |
| slowest ECU step | The longest step handler among the ECUs (without the launcher) |
| launcher hold | For held steps, with and without writes: the launcher's step, the scenario thread's wake-up, the scenario's writes (handed to the hub without waiting), its one wait for the agents' answers to them (settle), the rest of its tick (samples, judging, the engine), and the gate thread's wake-up until the release |
| write round trip | For each write, through the ECU that answered last (a write may go to several): the hub up to the send, the send (encoding and publishing), the way to the ECU's handler, the handler, the way back, the hub's handler, the wait for the answers to the other writes of its tick, the scenario thread's wake-up after the last answer. It also gives the share of commands that arrived while the ECU was in a step |
| clock offset from the launcher | An estimate of each ECU's clock offset, from the four times of each round trip (see [Clocks and other tools](#clocks-and-other-tools)) |

The exit code is 0 when it summarized the directory, 1 when there was nothing to summarize (no file, a file that is not whole, too few steps; the message says what to do), and 2 on a usage error.

## Viewing the timeline

`--merged` writes all processes into one file. Open it in [Perfetto](https://ui.perfetto.dev) (the page reads the file in the browser) or in `chrome://tracing`. A process is a participant, and the threads that record are named `silkit` (SIL Kit's thread: steps and the transport's handlers), `scenario` and `gate` (the launcher's). An arrow goes from each command the hub sends to the agent's handler (`command`), and from the agent's answer to the hub's handler (`answer`). Each agent of a write has arrows of its own: their ids come from the write's seq and the agent's session.

## Records

Each record has the steady clock's time, the virtual time of its step (`vt`, ns; -1 when it has none) and one integer (`arg`).

| Name | Kind | Where | `vt` | `arg` |
|---|---|---|---|---|
| `step` | span | `ParticipantRuntime`: the whole step on SIL Kit's thread, up to the hold's decision | the step | 1 when the step was held |
| `step.handler` | span | The step handler (the participant's own work) | the step | |
| `step.release` | instant | `release_step()` of a held step | the step | |
| `gate.release` | span | The gate thread releases a held step | the step | |
| `scenario.woken` | instant | The scenario thread finds a held step | the step | |
| `scenario.tick` | span | The scenario's work in a held step, up to the guard of the next | the step | |
| `scenario.drain` | span | The samples handed to the scenario | | samples |
| `scenario.write` | span | One write of the scenario handed to the hub (and sent), without waiting for the agents | write time | seq |
| `scenario.settle` | span | The scenario waits once until every agent has queued or refused the writes it handed out in the tick | | writes |
| `hub.write` | span | `Hub::write()`: checks, then the commands sent | write time | seq |
| `hub.send` | span | The command encoded and published to one agent (start of a `command` arrow) | write time | seq |
| `hub.receive_step` | span | An agent's step message (results, events, the sample) received by the hub | the agent's step | bytes |
| `hub.receive_answer` | span | An agent's answer received (end of an `answer` arrow) | | seq |
| `agent.receive_command` | span | A command for this agent: submitted and answered (end of `command`, start of `answer`) | | seq |
| `agent.receive_other` | span | A message for another agent of the same group (the agents of a group receive each other's commands; see "Topics" in internals/control.md) | | |
| `agent.step_begin` | span | The agent applies its due commands | the step | commands |
| `agent.apply` | instant | One command applied | the step | seq |
| `agent.late` | instant | One command refused as late | the step | seq |
| `agent.step_end` | span | The agent samples and sends its step message | the step | 1 with a sample |
| `agent.publish` | span | The step message encoded and published | the step | 1 with a sample |

## Clocks and other tools

The times are `std::chrono::steady_clock`: `CLOCK_MONOTONIC` on Linux, `QueryPerformanceCounter` on Windows. Every process of one host reads the same clock, so the files of a host line up as they are.

- On Linux, `perf record -k CLOCK_MONOTONIC` records on the same clock. For example, `perf record -e dummy:u --switch-events -k CLOCK_MONOTONIC` shows when each thread ran, so you can tell a thread that was asleep from one that was busy.
- On Windows, WPR and WPA record the same clock.
- The files of several hosts are not on one clock. `scripts/trace-report.py` reads them together without moving them, and estimates each ECU's offset from the launcher with the round trips of writes, as NTP does. The estimate is off by the difference of the two directions (the thread wake-ups), so it is good to a few µs at best. Each file also holds a pair of `steady_ns` and `system_ns`, taken when it was written, for a rough match through the system clocks.
- With files of several hosts, "last to complete", "sync" and the round trip's ways out and back compare times of different clocks as they are, so they are off by the offsets. The script tells the processes apart by file, so equal process ids of several hosts do not mix.

## Cost and limits

- Off, each place that records reads one atomic flag and branches. Nothing is allocated (`trace.a_trace_that_is_off_allocates_nothing`).
- On, a span reads the clock twice and stores 48 bytes into the thread's ring; there is no lock. A thread's ring is allocated at its first record, and its memory is touched only as records are written.
- The files are written when the process ends: by `atexit`, or, when the launcher ends with `std::_Exit` because a thread of the run did not close, just before it (and then without a word on stderr, which nothing may read any more). Writing takes time in proportion to the records.
- The launcher writes its file while a SIL Kit thread may still record. A record that such a thread writes at that moment may come out mixed with an older one, or be left out.

## Rules for changes

- A place that records must not allocate, lock or format when the trace is off: call `trace::Span`, `trace::instant()` and the others only with values that are already at hand.
- Names are string literals (only the pointer is kept), named `<area>.<what>`.
- `vehicle_sil::trace` links no other vehicle-sil library and includes no SIL Kit header (`trace.boundary` checks the includes), so that every layer can record.
- A process that ends without `atexit` calls `trace::flush()` first.
- When you add, rename or remove a record, update [Records](#records). When `scripts/trace-report.py` reads it, update the script and `tests/scripts/test_trace_report.py` too.
