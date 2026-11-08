// SPDX-License-Identifier: MIT
//
// Helpers shared by the virtual-time example peers: command line, common runtime options,
// the vehicle state exchanged over SIL Kit publish/subscribe, and the trace file format.
//
// Trace files contain one record per line: a record type followed by key=value fields, e.g.
//   tick t_ms=120 speed_in=2400
// Records only depend on simulation time and data, never on wall-clock time, so two runs of the
// same scenario must produce identical traces regardless of how fast they ran.
#pragma once

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/core/step_inbox.hpp"

#include <silkit/services/orchestration/string_utils.hpp>
#include <silkit/services/pubsub/all.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>

namespace vehicle_sil::vt {

using cli::Args;
using cli::hex;
using cli::kExitFailed;
using cli::kExitOk;
using cli::kExitSetupFailed;
using cli::kExitTimeout;
using cli::kExitUsage;
using cli::log;
using cli::run_main;
using cli::UsageError;

/// Options every virtual-time peer understands.
struct CommonOptions
{
    std::string registry_uri;
    std::string participant;
    SimDuration step;
    SimTime end;                           ///< The step handler runs only for steps before `end`.
    std::optional<double> real_time_factor; ///< --real-time-factor
    std::chrono::milliseconds timeout;     ///< --timeout-ms: limit for start-up
    std::optional<std::chrono::milliseconds> run_timeout; ///< --run-timeout-ms: limit for the whole run
    std::string trace_path;                ///< --trace (empty: no trace file)
};

inline constexpr const char* kCommonUsage =
    "  --registry-uri <uri>          SIL Kit registry (required)\n"
    "  --participant <name>          participant name\n"
    "  --step-us <us>                simulation step of this participant\n"
    "  --end-ms <ms>                 the peer works and traces only in steps before this time (required)\n"
    "  --real-time-factor <f>        pace to f x real time (default: as fast as possible)\n"
    "  --timeout-ms <ms>             wall-clock limit for start-up (60000)\n"
    "  --run-timeout-ms <ms>         wall-clock limit for the whole run (default: none; the\n"
    "                                system controller ends the simulation)\n"
    "  --trace <file>                write the trace to this file\n";

inline CommonOptions parse_common(const Args& args, const std::string& default_name, SimDuration default_step)
{
    CommonOptions o;
    o.registry_uri = args.required("registry-uri");
    o.participant = args.str("participant", default_name);
    o.step = std::chrono::microseconds(
        args.u64("step-us", static_cast<std::uint64_t>(default_step / std::chrono::microseconds(1)), 10'000'000));
    if (o.step <= SimDuration::zero())
    {
        throw UsageError("--step-us must be > 0");
    }
    o.end = SimTime{} + std::chrono::milliseconds(args.required_u64("end-ms", Args::kMaxMillis));
    o.real_time_factor = args.positive_double("real-time-factor");
    if (o.real_time_factor &&
        (*o.real_time_factor < RealTimePacer::kMinFactor || *o.real_time_factor > RealTimePacer::kMaxFactor))
    {
        throw UsageError("--real-time-factor must be within [1e-6, 1e6]");
    }
    o.timeout = args.millis("timeout-ms", std::chrono::milliseconds(60000));
    o.run_timeout = args.optional_millis("run-timeout-ms");
    o.trace_path = args.str("trace", "");
    return o;
}

/// Runtime options for a coordinated, time-synchronised peer. The step handler runs only for
/// steps before `--end-ms` (VirtualTimeOptions::end), so a peer's work and trace end at the same
/// virtual time however quickly the system controller's stop arrives.
inline ParticipantRuntimeOptions runtime_options(const CommonOptions& o)
{
    ParticipantRuntimeOptions options;
    options.participant_name = o.participant;
    options.registry_uri = o.registry_uri;
    options.lifecycle_mode = LifecycleMode::Coordinated;
    options.virtual_time = VirtualTimeOptions{o.step, o.real_time_factor, o.end};
    return options;
}

/// Line-oriented trace file. Only the step handler thread writes to it.
class Trace
{
public:
    explicit Trace(const std::string& path)
    {
        if (!path.empty())
        {
            file_ = std::fopen(path.c_str(), "w");
            if (file_ == nullptr)
            {
                throw std::runtime_error("cannot open trace file " + path);
            }
        }
    }
    ~Trace()
    {
        if (file_ != nullptr)
        {
            std::fclose(file_);
        }
    }
    Trace(const Trace&) = delete;
    Trace& operator=(const Trace&) = delete;

#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    void
    line(const char* format, ...)
    {
        if (file_ == nullptr)
        {
            return;
        }
        std::va_list args;
        va_start(args, format);
        std::vfprintf(file_, format, args);
        va_end(args);
        std::fputc('\n', file_);
    }

private:
    std::FILE* file_ = nullptr;
};

inline long long ms(SimTime t)
{
    return static_cast<long long>(t.time_since_epoch() / std::chrono::milliseconds(1));
}

inline long long us(SimTime t)
{
    return static_cast<long long>(t.time_since_epoch() / std::chrono::microseconds(1));
}

// --- Vehicle state published by the physics peer -------------------------------------------

/// SIL Kit topic and media type of the vehicle state.
inline SilKit::Services::PubSub::PubSubSpec vehicle_state_spec()
{
    return {"VehicleState", "application/vnd.vehicle-sil.vehicle-state.v1"};
}

struct VehicleState
{
    std::uint32_t speed_mm_s = 0;  ///< vehicle speed in mm/s
    std::uint64_t distance_mm = 0; ///< travelled distance in mm
};

inline constexpr std::size_t kVehicleStateSize = 12;

inline std::array<std::uint8_t, kVehicleStateSize> encode(const VehicleState& s)
{
    std::array<std::uint8_t, kVehicleStateSize> out{};
    for (std::size_t i = 0; i < 4; ++i)
    {
        out[i] = static_cast<std::uint8_t>(s.speed_mm_s >> (8 * i));
    }
    for (std::size_t i = 0; i < 8; ++i)
    {
        out[4 + i] = static_cast<std::uint8_t>(s.distance_mm >> (8 * i));
    }
    return out;
}

inline std::optional<VehicleState> decode(const std::uint8_t* data, std::size_t size)
{
    if (size != kVehicleStateSize)
    {
        return std::nullopt;
    }
    VehicleState s;
    for (std::size_t i = 0; i < 4; ++i)
    {
        s.speed_mm_s |= static_cast<std::uint32_t>(data[i]) << (8 * i);
    }
    for (std::size_t i = 0; i < 8; ++i)
    {
        s.distance_mm |= static_cast<std::uint64_t>(data[4 + i]) << (8 * i);
    }
    return s;
}

/// Subscribes to the vehicle state and queues every state in `inbox`, stamped with its send
/// time. `inbox` must outlive `runtime`'s participant: declare it before the runtime.
inline void subscribe_vehicle_state(ParticipantRuntime& runtime, StepInbox<VehicleState>& inbox)
{
    runtime.participant().CreateDataSubscriber(
        "VehicleStateIn", vehicle_state_spec(), [&inbox](auto*, const SilKit::Services::PubSub::DataMessageEvent& event) {
            if (const auto state = decode(event.data.data(), event.data.size()))
            {
                inbox.push(from_silkit_time(event.timestamp), *state);
            }
        });
}

/// The wall-clock limit of the whole run: --run-timeout-ms, or (without it) as long as the
/// system controller lets the simulation run.
inline std::chrono::milliseconds run_limit(const CommonOptions& o)
{
    return o.run_timeout.value_or(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours(24 * 365)));
}

/// Reports how the simulation ended, from the final state wait_until_finished() returned
/// (nullopt: the limit passed) and the step error (empty: none). Returns the process exit code.
inline int check_finished(const std::optional<SilKit::Services::Orchestration::ParticipantState>& state,
                          const ParticipantRuntime& runtime, const std::string& stepError, const CommonOptions& o)
{
    namespace orch = SilKit::Services::Orchestration;
    if (!state)
    {
        log(o.participant, "FAIL timeout: simulation did not finish within %lld ms (last step t=%lld ms)",
            static_cast<long long>(run_limit(o).count()), ms(runtime.now()));
        return kExitTimeout;
    }
    if (*state != orch::ParticipantState::Shutdown || !stepError.empty())
    {
        log(o.participant, "FAIL: final state %s %s", orch::to_string(*state).c_str(), stepError.c_str());
        return kExitFailed;
    }
    // An aborted simulation also shuts down; it must have reached the last step before --end-ms.
    if (runtime.now() + o.step < o.end)
    {
        log(o.participant, "FAIL: the simulation ended at t=%lld ms, before the last step before --end-ms", ms(runtime.now()));
        return kExitFailed;
    }
    log(o.participant, "finished: %llu steps, last step t=%lld ms",
        static_cast<unsigned long long>(runtime.steps_completed()), ms(runtime.now()));
    return kExitOk;
}

/// Starts the runtime, waits for the end of the simulation and reports how it ended. On every
/// path it stops the runtime before returning, so the step handler is no longer running and
/// objects it uses may be destroyed. Returns the process exit code.
inline int run_until_finished(ParticipantRuntime& runtime, const CommonOptions& o)
{
    struct StopOnReturn
    {
        ParticipantRuntime& runtime;
        ~StopOnReturn() { runtime.stop("peer finished"); }
    } stopOnReturn{runtime};
    try
    {
        runtime.start(o.timeout);
    }
    catch (const std::exception& e)
    {
        log(o.participant, "FAIL setup: %s", e.what());
        return kExitSetupFailed;
    }
    log(o.participant, "running, step %lld us", static_cast<long long>(o.step / std::chrono::microseconds(1)));
    const auto state = runtime.wait_until_finished(run_limit(o));
    return check_finished(state, runtime, runtime.step_error(), o);
}

} // namespace vehicle_sil::vt
