// SPDX-License-Identifier: MIT
//
// vehicle_sil_bench_ecu: deterministic synthetic ECU executable for the coordinated virtual-time
// benchmark suite (scripts/benchmark-vtime.py).
//
// Accepts --bench-* flags (stripped before delegating remaining arguments to vehicle_sil::ecu_main())
// and writes per-ECU steady-state timing, self-CPU delta, deterministic state digest, and a
// timestamped CAN communication trace after ecu_main() returns.

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/raw_channel.hpp"
#include "vehicle_sil/cli/utf8.hpp"
#include "vehicle_sil/json/json_writer.hpp"
#include "vehicle_sil/process/paths.hpp"
#include "vehicle_sil/simulation/ecu_main.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace {

constexpr int kExitOk = 0;
constexpr int kExitSetupFailed = 3;
constexpr int kExitUsage = 4;

constexpr std::uint32_t kCanBaseId = 0x100u;
constexpr const char* kCanNetworkName = "BENCH_CAN";

struct BenchConfig
{
    std::int64_t bench_id = 0;
    std::int64_t step_ms = 1;
    std::int64_t vars = 250;
    std::int64_t sample_ms = 10;
    std::int64_t can_period_ms = 10;
    std::int64_t can_offset_ms = 0;
    std::int64_t can_frames = 1;
    std::int64_t step_work = 0;
    std::int64_t warmup_ms = 200;
    std::int64_t measure_ms = 20000;
    std::int64_t tail_ms = 10;
    std::string out_dir;
    bool build_info = false;
};

struct CpuSample
{
    bool available = false;
    const char* reason = nullptr;
    double user_ms = 0.0;
    double sys_ms = 0.0;
};

struct CommEvent
{
    char dir = 'T'; // 'T' (TX) or 'R' (RX)
    std::int64_t tx_ms = 0;
    std::int64_t rx_ms = -1; // -1 for TX
    std::uint32_t id = 0;
    std::uint16_t seq = 0;
    std::uint8_t length = 0;
    std::array<std::uint8_t, vehicle_sil::can::kMaxPayload> data{};
};

/// State owned outside ecu_main() so it outlives Simulation destruction and can be written to disk
/// after ecu_main() returns.
struct BenchSummaryState
{
    std::string name;
    bool sample_every_step_active = false;
    std::int64_t effective_sample_period_ms = 10;

    bool saw_window_start = false;
    bool saw_window_end = false;
    std::int64_t window_start_step_ms = -1;
    std::int64_t window_end_step_ms = -1;
    std::int64_t steady_start_ns = 0;
    std::int64_t steady_end_ns = 0;
    CpuSample cpu_start;
    CpuSample cpu_end;

    std::int64_t window_steps = 0;
    std::int64_t total_steps = 0;
    std::int64_t tx_in_window = 0;
    std::int64_t rx_in_window = 0;
    std::int64_t tx_total = 0;
    std::int64_t rx_total = 0;
    std::int64_t sub_ms_rx_timestamps = 0;

    std::uint64_t state_digest = 0;
    double probe_cmd_last = 0.0;
    double probe_echo_last = 0.0;
    std::int64_t probe_seen_ms = -1;

    vehicle_sil::can::RawChannelStatistics can_stats{};
    std::vector<CommEvent> comm_events;
};

[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

[[nodiscard]] std::uint64_t payload_u64(const vehicle_sil::can::Frame& f) noexcept
{
    std::uint64_t v = 0;
    const std::size_t n = std::min<std::size_t>(f.length, vehicle_sil::can::kMaxPayload);
    for (std::size_t i = 0; i < n; ++i)
    {
        v |= static_cast<std::uint64_t>(f.data[i]) << (i * 8u);
    }
    return v;
}

[[nodiscard]] std::uint16_t read_u16_le(const std::array<std::uint8_t, vehicle_sil::can::kMaxPayload>& data,
                                        std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset]) |
                                      (static_cast<std::uint16_t>(data[offset + 1]) << 8u));
}

[[nodiscard]] std::string hex_u64_lower(std::uint64_t value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (std::size_t i = 0; i < 16; ++i)
    {
        out[15 - i] = kHex[(value >> (i * 4u)) & 0xFu];
    }
    return out;
}

[[nodiscard]] std::string hex_bytes_lower(const std::array<std::uint8_t, vehicle_sil::can::kMaxPayload>& data,
                                          std::uint8_t length)
{
    static constexpr char kHex[] = "0123456789abcdef";
    const std::size_t n = std::min<std::size_t>(length, vehicle_sil::can::kMaxPayload);
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i)
    {
        out.push_back(kHex[(data[i] >> 4u) & 0xFu]);
        out.push_back(kHex[data[i] & 0xFu]);
    }
    return out;
}

[[nodiscard]] CpuSample sample_self_cpu() noexcept
{
    CpuSample sample;
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exit_time{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user) != 0)
    {
        ULARGE_INTEGER k{};
        k.LowPart = kernel.dwLowDateTime;
        k.HighPart = kernel.dwHighDateTime;
        ULARGE_INTEGER u{};
        u.LowPart = user.dwLowDateTime;
        u.HighPart = user.dwHighDateTime;
        sample.available = true;
        sample.user_ms = static_cast<double>(u.QuadPart) / 10000.0;
        sample.sys_ms = static_cast<double>(k.QuadPart) / 10000.0;
    }
    else
    {
        sample.available = false;
        sample.reason = "GetProcessTimes failed";
    }
#else
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0)
    {
        sample.available = true;
        sample.user_ms = static_cast<double>(ru.ru_utime.tv_sec) * 1000.0 +
                         static_cast<double>(ru.ru_utime.tv_usec) / 1000.0;
        sample.sys_ms = static_cast<double>(ru.ru_stime.tv_sec) * 1000.0 +
                        static_cast<double>(ru.ru_stime.tv_usec) / 1000.0;
    }
    else
    {
        sample.available = false;
        sample.reason = "getrusage(RUSAGE_SELF) failed";
    }
#endif
    return sample;
}

[[nodiscard]] std::string build_info_json()
{
    vehicle_sil::json::JsonWriter w;
    w.begin_object();
#if defined(__clang__)
    w.string("compiler_id", "Clang");
    w.string("compiler_version",
             std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) + "." +
                 std::to_string(__clang_patchlevel__));
#elif defined(__GNUC__)
    w.string("compiler_id", "GNU");
    w.string("compiler_version",
             std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
                 std::to_string(__GNUC_PATCHLEVEL__));
#elif defined(_MSC_VER)
    w.string("compiler_id", "MSVC");
    w.string("compiler_version", std::to_string(_MSC_VER));
#else
    w.string("compiler_id", "unknown");
    w.string("compiler_version", "unknown");
#endif

#ifdef NDEBUG
    w.boolean("ndebug", true);
#else
    w.boolean("ndebug", false);
#endif

#ifdef __OPTIMIZE__
    w.integer("optimize_macro", static_cast<std::int64_t>(__OPTIMIZE__));
#else
    w.integer("optimize_macro", 0);
#endif

    w.begin_array_on_one_line("sanitizers");
#if defined(__SANITIZE_ADDRESS__)
    w.string({}, "address");
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    w.string({}, "address");
#endif
#endif
#if defined(__SANITIZE_THREAD__)
    w.string({}, "thread");
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    w.string({}, "thread");
#endif
#endif
#if defined(__has_feature)
#if __has_feature(undefined_behavior_sanitizer)
    w.string({}, "undefined");
#endif
#endif
    w.end_array();
    w.end_object();
    return w.take() + "\n";
}

[[nodiscard]] std::optional<std::int64_t> parse_int64(std::string_view text) noexcept
{
    if (text.empty())
    {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last)
    {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::int64_t compute_effective_sample_period_ms(std::int64_t step_ms, std::int64_t configured_ms,
                                                              bool sample_every_step_active) noexcept
{
    if (sample_every_step_active || configured_ms == 0)
    {
        return step_ms;
    }
    return ((configured_ms + step_ms - 1) / step_ms) * step_ms;
}

[[nodiscard]] std::string validate_config(const BenchConfig& cfg)
{
    if (cfg.bench_id < 0 || cfg.bench_id > 255)
    {
        return "--bench-id must be between 0 and 255";
    }
    if (cfg.step_ms <= 0)
    {
        return "--bench-step-ms must be > 0";
    }
    if (cfg.vars < 4)
    {
        return "--bench-vars must be >= 4";
    }
    if (cfg.sample_ms < 0)
    {
        return "--bench-sample-ms must be >= 0";
    }
    if (cfg.can_period_ms < 0)
    {
        return "--bench-can-period-ms must be >= 0";
    }
    if (cfg.can_period_ms > 0)
    {
        if (cfg.can_offset_ms < 0 || cfg.can_offset_ms >= cfg.can_period_ms)
        {
            return "--bench-can-offset-ms must satisfy 0 <= offset < can-period-ms";
        }
        if (cfg.can_offset_ms % cfg.step_ms != 0)
        {
            return "--bench-can-offset-ms must be a multiple of --bench-step-ms";
        }
        if (cfg.can_frames < 1 || cfg.can_frames > 64)
        {
            return "--bench-can-frames must be between 1 and 64";
        }
    }
    if (cfg.step_work < 0)
    {
        return "--bench-step-work must be >= 0";
    }
    if (cfg.warmup_ms < 0)
    {
        return "--bench-warmup-ms must be >= 0";
    }
    if (cfg.measure_ms <= 0)
    {
        return "--bench-measure-ms must be > 0";
    }
    if (cfg.tail_ms < cfg.step_ms)
    {
        return "--bench-tail-ms must be >= --bench-step-ms";
    }
    if (cfg.warmup_ms > std::numeric_limits<std::int64_t>::max() - cfg.measure_ms)
    {
        return "--bench-warmup-ms + --bench-measure-ms overflows 64-bit integer";
    }
    const std::int64_t align_ms =
        cfg.can_period_ms > 0 ? std::lcm(cfg.step_ms, cfg.can_period_ms) : cfg.step_ms;
    if (cfg.warmup_ms % align_ms != 0)
    {
        return "--bench-warmup-ms (" + std::to_string(cfg.warmup_ms) +
               ") must be a multiple of lcm(step_ms, can_period_ms) = " + std::to_string(align_ms);
    }
    if (cfg.measure_ms % align_ms != 0)
    {
        return "--bench-measure-ms (" + std::to_string(cfg.measure_ms) +
               ") must be a multiple of lcm(step_ms, can_period_ms) = " + std::to_string(align_ms);
    }
    return {};
}

class BenchApp
{
public:
    BenchApp(vehicle_sil::Ecu& ecu, const BenchConfig& cfg, BenchSummaryState& state)
        : cfg_(cfg), state_(state), measure_end_ms_(cfg.warmup_ms + cfg.measure_ms)
    {
        // Configure Control sampling period before declaring any variable.
        vehicle_sil::control::AgentOptions agent_opts;
        agent_opts.sample_period = std::chrono::milliseconds(cfg_.sample_ms);
        ecu.set_agent_options(agent_opts);

        declare_variables(ecu);

        // Pre-reserve buffers before start() so W < now < W + M allocates nothing.
        step_rx_.reserve(256);
        if (cfg_.can_period_ms > 0)
        {
            const std::int64_t slots = (measure_end_ms_ / cfg_.can_period_ms) + 2;
            const std::size_t estimated = static_cast<std::size_t>(slots) *
                                          static_cast<std::size_t>(cfg_.can_frames) * 16u;
            state_.comm_events.reserve(std::min<std::size_t>(estimated, 2000000u));

            can_channel_ = &ecu.can(kCanNetworkName, {.send_capacity = 64, .receive_capacity = 1024});
            can_channel_->on_frame([this](const vehicle_sil::can::TimedFrame& tf) {
                step_rx_.push_back(tf);
            });
        }

        state_word_ = splitmix64(0x5653494C424E4348ULL ^ static_cast<std::uint64_t>(cfg_.bench_id));
        state_.state_digest = splitmix64(state_word_ ^ 0xD16E570000000001ULL);

        ecu.on_step([this](vehicle_sil::SimTime now) { step(now); });
    }

private:
    void declare_variables(vehicle_sil::Ecu& ecu)
    {
        // Default 250 declared variables: 50 double inputs, 120 double outputs, 50 int64 outputs, 30 bool outputs.
        // For other values >= 4, preserve the 4 required probe/time variables and scale proportionally.
        std::size_t n_in_d = 50;
        std::size_t n_out_d = 120;
        std::size_t n_out_i = 50;
        std::size_t n_out_b = 30;
        if (cfg_.vars != 250)
        {
            const std::size_t total = static_cast<std::size_t>(cfg_.vars);
            const std::size_t rem = total - 4;
            n_in_d = 1 + (rem * 50) / 250;
            n_out_d = 1 + (rem * 120) / 250;
            n_out_i = 2 + (rem * 50) / 250;
            n_out_b = total - n_in_d - n_out_d - n_out_i;
        }

        in_doubles_.assign(n_in_d, 0.0);
        out_doubles_.assign(n_out_d, 0.0);
        out_ints_.assign(n_out_i, 0);
        out_bools_count_ = n_out_b;
        out_bools_ = std::make_unique<bool[]>(n_out_b);
        std::fill_n(out_bools_.get(), n_out_b, false);

        ecu.input("probe.cmd", &in_doubles_[0]);
        for (std::size_t i = 1; i < n_in_d; ++i)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "in.d%02zu", i);
            ecu.input(buf, &in_doubles_[i]);
        }

        ecu.output("probe.echo", &out_doubles_[0]);
        for (std::size_t i = 1; i < n_out_d; ++i)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "out.d%03zu", i);
            ecu.output(buf, &out_doubles_[i]);
        }

        out_ints_[0] = -1; // probe.seen_ms before first write
        ecu.output("probe.seen_ms", &out_ints_[0]);
        ecu.output("t_ms", &out_ints_[1]);
        for (std::size_t i = 2; i < n_out_i; ++i)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "out.i%02zu", i);
            ecu.output(buf, &out_ints_[i]);
        }

        for (std::size_t i = 0; i < n_out_b; ++i)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "out.b%02zu", i);
            ecu.output(buf, &out_bools_[i]);
        }
    }

    void step(vehicle_sil::SimTime now)
    {
        const std::int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

        // Snapshot steady_clock and self CPU exactly once at t = W and once at t = W + M.
        // Zero benchmark-added syscalls execute during W < now < W + M.
        if (now_ms == cfg_.warmup_ms)
        {
            state_.steady_start_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            state_.cpu_start = sample_self_cpu();
            state_.window_start_step_ms = now_ms;
            state_.saw_window_start = true;
        }
        else if (now_ms == measure_end_ms_)
        {
            state_.steady_end_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            state_.cpu_end = sample_self_cpu();
            state_.window_end_step_ms = now_ms;
            state_.saw_window_end = true;
        }

        // Canonical RX sort by (tx_ns, id, payload_u64) to eliminate SIL Kit thread-arrival nondeterminism.
        if (!step_rx_.empty())
        {
            std::sort(step_rx_.begin(), step_rx_.end(),
                      [](const vehicle_sil::can::TimedFrame& a, const vehicle_sil::can::TimedFrame& b) {
                          const auto ta = a.time.time_since_epoch().count();
                          const auto tb = b.time.time_since_epoch().count();
                          if (ta != tb)
                          {
                              return ta < tb;
                          }
                          if (a.frame.id != b.frame.id)
                          {
                              return a.frame.id < b.frame.id;
                          }
                          return payload_u64(a.frame) < payload_u64(b.frame);
                      });

            if (now_ms <= measure_end_ms_)
            {
                for (const auto& tf : step_rx_)
                {
                    if ((tf.time.time_since_epoch() % std::chrono::milliseconds(1)) != vehicle_sil::SimDuration::zero())
                    {
                        ++state_.sub_ms_rx_timestamps;
                    }
                    const std::int64_t tx_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(tf.time.time_since_epoch()).count();
                    const std::uint64_t p64 = payload_u64(tf.frame);
                    state_word_ = splitmix64(state_word_ ^ static_cast<std::uint64_t>(tx_ms));
                    state_word_ = splitmix64(state_word_ ^ (static_cast<std::uint64_t>(tf.frame.id) << 32u) ^
                                             static_cast<std::uint64_t>(now_ms));
                    state_word_ = splitmix64(state_word_ ^ p64);

                    CommEvent ev;
                    ev.dir = 'R';
                    ev.tx_ms = tx_ms;
                    ev.rx_ms = now_ms;
                    ev.id = tf.frame.id;
                    ev.seq = tf.frame.length >= 4 ? read_u16_le(tf.frame.data, 2) : 0u;
                    ev.length = tf.frame.length;
                    ev.data = tf.frame.data;
                    state_.comm_events.push_back(ev);

                    ++state_.rx_total;
                    if (tx_ms >= cfg_.warmup_ms && tx_ms < measure_end_ms_)
                    {
                        ++state_.rx_in_window;
                    }
                }
            }
            step_rx_.clear();
        }

        // Probe input echo and timestamp tracking.
        const double cmd = in_doubles_[0];
        if (cmd != prev_probe_cmd_)
        {
            prev_probe_cmd_ = cmd;
            out_ints_[0] = now_ms; // probe.seen_ms
        }
        out_doubles_[0] = cmd; // probe.echo
        out_ints_[1] = now_ms; // t_ms

        if (now_ms < measure_end_ms_)
        {
            // Pure 64-bit integer state mixing (immune to compiler FMA contraction differences).
            const std::int64_t cmd_fixed = std::isfinite(cmd) ? static_cast<std::int64_t>(std::llround(cmd * 1024.0)) : 0;
            state_word_ = splitmix64(state_word_ ^ static_cast<std::uint64_t>(now_ms));
            state_word_ = splitmix64(state_word_ ^ static_cast<std::uint64_t>(cmd_fixed));
            for (std::int64_t k = 0; k < cfg_.step_work; ++k)
            {
                state_word_ = splitmix64(state_word_ ^ static_cast<std::uint64_t>(k));
            }
            state_.state_digest = splitmix64(state_.state_digest ^ state_word_);

            if (now_ms >= cfg_.warmup_ms)
            {
                ++state_.window_steps;
            }

            update_outputs(now_ms);

            // Cyclic CAN TX strictly for now_ms < W + M.
            if (can_channel_ != nullptr && cfg_.can_period_ms > 0 && now_ms >= cfg_.can_offset_ms &&
                ((now_ms - cfg_.can_offset_ms) % cfg_.can_period_ms == 0))
            {
                for (std::int64_t f = 0; f < cfg_.can_frames; ++f)
                {
                    vehicle_sil::can::Frame frame;
                    frame.id = kCanBaseId + static_cast<std::uint32_t>(cfg_.bench_id);
                    frame.format = vehicle_sil::can::IdFormat::Standard;
                    frame.length = 8;

                    const std::uint16_t seq = tx_seq_;
                    const std::uint32_t w32 = static_cast<std::uint32_t>(state_word_ & 0xFFFFFFFFULL);
                    frame.data[0] = static_cast<std::uint8_t>(cfg_.bench_id & 0xFF);
                    frame.data[1] = static_cast<std::uint8_t>(f & 0xFF);
                    frame.data[2] = static_cast<std::uint8_t>(seq & 0xFFu);
                    frame.data[3] = static_cast<std::uint8_t>((seq >> 8u) & 0xFFu);
                    frame.data[4] = static_cast<std::uint8_t>(w32 & 0xFFu);
                    frame.data[5] = static_cast<std::uint8_t>((w32 >> 8u) & 0xFFu);
                    frame.data[6] = static_cast<std::uint8_t>((w32 >> 16u) & 0xFFu);
                    frame.data[7] = static_cast<std::uint8_t>((w32 >> 24u) & 0xFFu);

                    static_cast<void>(can_channel_->send(frame));

                    CommEvent ev;
                    ev.dir = 'T';
                    ev.tx_ms = now_ms;
                    ev.rx_ms = -1;
                    ev.id = frame.id;
                    ev.seq = seq;
                    ev.length = frame.length;
                    ev.data = frame.data;
                    state_.comm_events.push_back(ev);

                    ++tx_seq_;
                    ++state_.tx_total;
                    if (now_ms >= cfg_.warmup_ms)
                    {
                        ++state_.tx_in_window;
                    }
                }
            }

            state_.probe_cmd_last = cmd;
            state_.probe_echo_last = out_doubles_[0];
            state_.probe_seen_ms = out_ints_[0];
        }
        else if (now_ms == measure_end_ms_)
        {
            // Fold final causal RX at W + M into state_digest; stop benchmark CAN TX at now >= W + M.
            state_.state_digest = splitmix64(state_.state_digest ^ state_word_);
            update_outputs(now_ms);
            state_.probe_cmd_last = cmd;
            state_.probe_echo_last = out_doubles_[0];
            state_.probe_seen_ms = out_ints_[0];
        }

        // Snapshot RawChannelStatistics safely inside on_step() after W + M (RawChannel is destroyed
        // before emplaced objects when Ecu tears down).
        if (can_channel_ != nullptr && now_ms >= measure_end_ms_)
        {
            state_.can_stats = can_channel_->statistics();
        }

        ++state_.total_steps;
    }

    void update_outputs(std::int64_t now_ms) noexcept
    {
        // Deterministic integer-derived outputs using exact power-of-2 double scaling (no FMA drift).
        const std::uint64_t base = state_word_;
        for (std::size_t i = 1; i < out_doubles_.size(); ++i)
        {
            const std::uint64_t v = (base >> ((i % 7u) * 8u)) + static_cast<std::uint64_t>(i);
            out_doubles_[i] = static_cast<double>(v & 0x3FFULL) * 0.25;
        }
        for (std::size_t i = 2; i < out_ints_.size(); ++i)
        {
            out_ints_[i] = static_cast<std::int64_t>((base + static_cast<std::uint64_t>(now_ms) + i) & 0xFFFFULL);
        }
        for (std::size_t i = 0; i < out_bools_count_; ++i)
        {
            out_bools_[i] = (((base >> (i % 64u)) & 1ULL) != 0ULL);
        }
    }

    const BenchConfig& cfg_;
    BenchSummaryState& state_;
    std::int64_t measure_end_ms_ = 0;
    vehicle_sil::can::RawChannel* can_channel_ = nullptr;
    std::vector<vehicle_sil::can::TimedFrame> step_rx_;

    std::vector<double> in_doubles_;
    std::vector<double> out_doubles_;
    std::vector<std::int64_t> out_ints_;
    std::unique_ptr<bool[]> out_bools_;
    std::size_t out_bools_count_ = 0;

    double prev_probe_cmd_ = 0.0;
    std::uint64_t state_word_ = 0;
    std::uint16_t tx_seq_ = 0;
};

[[nodiscard]] bool write_outputs(const BenchConfig& cfg, const BenchSummaryState& state, std::string& error_out)
{
    const std::filesystem::path dir = vehicle_sil::process::path_from_utf8(cfg.out_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        error_out = "failed to create output directory '" + cfg.out_dir + "': " + ec.message();
        return false;
    }

    const std::string prefix = "ecu_" + state.name;
    const std::filesystem::path summary_path = dir / vehicle_sil::process::path_from_utf8(prefix + ".summary.json");
    const std::filesystem::path trace_path = dir / vehicle_sil::process::path_from_utf8(prefix + ".comm.tsv");

    // 1. Write summary JSON.
    vehicle_sil::json::JsonWriter w;
    w.begin_object();
    w.string("name", state.name);
    w.integer("bench_id", cfg.bench_id);
    w.integer("step_ms", cfg.step_ms);
    w.integer("configured_sample_period_ms", cfg.sample_ms);
    w.integer("effective_sample_period_ms", state.effective_sample_period_ms);
    w.boolean("sample_every_step_active", state.sample_every_step_active);
    w.integer("declared_vars", cfg.vars);
    w.integer("can_period_ms", cfg.can_period_ms);
    w.integer("can_offset_ms", cfg.can_offset_ms);
    w.integer("can_frames", cfg.can_frames);
    w.integer("step_work", cfg.step_work);
    w.integer("warmup_ms", cfg.warmup_ms);
    w.integer("measure_ms", cfg.measure_ms);
    w.integer("tail_ms", cfg.tail_ms);
    w.integer("measure_end_ms", cfg.warmup_ms + cfg.measure_ms);
    w.boolean("saw_window_start", state.saw_window_start);
    w.boolean("saw_window_end", state.saw_window_end);
    if (state.saw_window_start)
    {
        w.integer("window_start_step_ms", state.window_start_step_ms);
        w.integer("steady_start_ns", state.steady_start_ns);
    }
    else
    {
        w.null("window_start_step_ms");
        w.null("steady_start_ns");
    }
    if (state.saw_window_end)
    {
        w.integer("window_end_step_ms", state.window_end_step_ms);
        w.integer("steady_end_ns", state.steady_end_ns);
    }
    else
    {
        w.null("window_end_step_ms");
        w.null("steady_end_ns");
    }
    if (state.saw_window_start && state.saw_window_end)
    {
        w.integer("steady_wall_ns", std::max<std::int64_t>(0, state.steady_end_ns - state.steady_start_ns));
    }
    else
    {
        w.null("steady_wall_ns");
    }
    w.integer("window_steps", state.window_steps);
    w.integer("total_steps", state.total_steps);
    w.integer("tx_in_window", state.tx_in_window);
    w.integer("rx_in_window", state.rx_in_window);
    w.integer("tx_total", state.tx_total);
    w.integer("rx_total", state.rx_total);
    w.integer("sub_ms_rx_timestamps", state.sub_ms_rx_timestamps);
    w.string("state_digest_hex", hex_u64_lower(state.state_digest));
    w.real("probe_cmd_last", state.probe_cmd_last);
    w.real("probe_echo_last", state.probe_echo_last);
    w.integer("probe_seen_ms", state.probe_seen_ms);

    w.begin_object("can_stats");
    w.integer("frames_received", static_cast<std::int64_t>(state.can_stats.frames_received));
    w.integer("receive_overflows", static_cast<std::int64_t>(state.can_stats.receive_overflows));
    w.integer("frames_sent", static_cast<std::int64_t>(state.can_stats.frames_sent));
    w.integer("sends_rejected", static_cast<std::int64_t>(state.can_stats.sends_rejected));
    w.integer("frames_dropped", static_cast<std::int64_t>(state.can_stats.frames_dropped));
    w.integer("port_receive_overflows", static_cast<std::int64_t>(state.can_stats.port_receive_overflows));
    w.end_object();

    w.begin_object("cpu");
    if (state.saw_window_start && state.saw_window_end && state.cpu_start.available && state.cpu_end.available)
    {
        const double user_ms = std::max(0.0, state.cpu_end.user_ms - state.cpu_start.user_ms);
        const double sys_ms = std::max(0.0, state.cpu_end.sys_ms - state.cpu_start.sys_ms);
        w.string("status", "available");
        w.null("reason");
        w.real("user_ms", user_ms);
        w.real("sys_ms", sys_ms);
        w.real("total_ms", user_ms + sys_ms);
    }
    else
    {
        std::string reason = "steady-state window not completed";
        if (state.saw_window_start && state.saw_window_end)
        {
            const char* raw_reason = !state.cpu_start.available ? state.cpu_start.reason : state.cpu_end.reason;
            reason = (raw_reason != nullptr && raw_reason[0] != '\0') ? raw_reason : "process CPU sampling unavailable";
        }
        w.string("status", "unavailable");
        w.string("reason", reason);
        w.null("user_ms");
        w.null("sys_ms");
        w.null("total_ms");
    }
    w.end_object();
    w.end_object();

    {
        std::ofstream out(summary_path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error_out = "failed to open summary file for writing";
            return false;
        }
        const std::string text = w.take() + "\n";
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out)
        {
            error_out = "failed to write summary file";
            return false;
        }
    }

    // 2. Write timestamped communication trace TSV.
    {
        std::ofstream out(trace_path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error_out = "failed to open comm trace file for writing";
            return false;
        }
        std::string buf;
        buf.reserve(65536);
        char line[128];
        for (const auto& ev : state.comm_events)
        {
            const std::string hex = hex_bytes_lower(ev.data, ev.length);
            int n = 0;
            if (ev.dir == 'T')
            {
                n = std::snprintf(line, sizeof(line), "T\t%lld\t-\t0x%x\t%u\t%s\n",
                                  static_cast<long long>(ev.tx_ms), static_cast<unsigned>(ev.id),
                                  static_cast<unsigned>(ev.seq), hex.c_str());
            }
            else
            {
                n = std::snprintf(line, sizeof(line), "R\t%lld\t%lld\t0x%x\t%u\t%s\n",
                                  static_cast<long long>(ev.tx_ms), static_cast<long long>(ev.rx_ms),
                                  static_cast<unsigned>(ev.id), static_cast<unsigned>(ev.seq), hex.c_str());
            }
            if (n > 0 && static_cast<std::size_t>(n) < sizeof(line))
            {
                buf.append(line, static_cast<std::size_t>(n));
            }
            else
            {
                error_out = "failed to format comm trace line";
                return false;
            }
            if (buf.size() >= 60000)
            {
                out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
                buf.clear();
            }
        }
        if (!buf.empty())
        {
            out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        }
        if (!out)
        {
            error_out = "failed to write comm trace file";
            return false;
        }
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> all_args = vehicle_sil::cli::utf8_arguments(argc, argv);

    BenchConfig cfg;
    std::vector<std::string> forwarded;
    forwarded.reserve(all_args.size() + 1);
    forwarded.push_back(argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : "vehicle_sil_bench_ecu");

    bool is_describe = false;
    bool sample_every_step_active = false;
    std::optional<std::string> explicit_name;

    auto require_value = [&](std::size_t& i, std::string_view flag) -> std::optional<std::string> {
        if (i + 1 >= all_args.size())
        {
            std::cerr << "error: vehicle_sil_bench_ecu: missing value for " << flag << "\n";
            return std::nullopt;
        }
        return all_args[++i];
    };

    auto parse_int_flag = [&](std::size_t& i, std::string_view flag, std::int64_t& target) -> bool {
        const auto val = require_value(i, flag);
        if (!val)
        {
            return false;
        }
        const auto parsed = parse_int64(*val);
        if (!parsed)
        {
            std::cerr << "error: vehicle_sil_bench_ecu: invalid integer for " << flag << ": '" << *val << "'\n";
            return false;
        }
        target = *parsed;
        return true;
    };

    for (std::size_t i = 0; i < all_args.size(); ++i)
    {
        const std::string& arg = all_args[i];
        if (arg == "--bench-build-info")
        {
            cfg.build_info = true;
        }
        else if (arg == "--bench-id")
        {
            if (!parse_int_flag(i, arg, cfg.bench_id)) return kExitUsage;
        }
        else if (arg == "--bench-step-ms")
        {
            if (!parse_int_flag(i, arg, cfg.step_ms)) return kExitUsage;
        }
        else if (arg == "--bench-vars")
        {
            if (!parse_int_flag(i, arg, cfg.vars)) return kExitUsage;
        }
        else if (arg == "--bench-sample-ms")
        {
            if (!parse_int_flag(i, arg, cfg.sample_ms)) return kExitUsage;
        }
        else if (arg == "--bench-can-period-ms")
        {
            if (!parse_int_flag(i, arg, cfg.can_period_ms)) return kExitUsage;
        }
        else if (arg == "--bench-can-offset-ms")
        {
            if (!parse_int_flag(i, arg, cfg.can_offset_ms)) return kExitUsage;
        }
        else if (arg == "--bench-can-frames")
        {
            if (!parse_int_flag(i, arg, cfg.can_frames)) return kExitUsage;
        }
        else if (arg == "--bench-step-work")
        {
            if (!parse_int_flag(i, arg, cfg.step_work)) return kExitUsage;
        }
        else if (arg == "--bench-warmup-ms")
        {
            if (!parse_int_flag(i, arg, cfg.warmup_ms)) return kExitUsage;
        }
        else if (arg == "--bench-measure-ms")
        {
            if (!parse_int_flag(i, arg, cfg.measure_ms)) return kExitUsage;
        }
        else if (arg == "--bench-tail-ms")
        {
            if (!parse_int_flag(i, arg, cfg.tail_ms)) return kExitUsage;
        }
        else if (arg == "--bench-out-dir")
        {
            const auto val = require_value(i, arg);
            if (!val) return kExitUsage;
            cfg.out_dir = *val;
        }
        else if (arg.starts_with("--bench-"))
        {
            std::cerr << "error: vehicle_sil_bench_ecu: unknown benchmark option '" << arg << "'\n";
            return kExitUsage;
        }
        else
        {
            if (arg == "--describe")
            {
                is_describe = true;
            }
            else if (arg == "--sample-every-step")
            {
                sample_every_step_active = true;
            }
            else if (arg == "--name" && i + 1 < all_args.size())
            {
                explicit_name = all_args[i + 1];
            }
            forwarded.push_back(arg);
        }
    }

    if (cfg.build_info)
    {
        const std::string info = build_info_json();
        std::cout << info << std::flush;
        return std::cout ? kExitOk : kExitSetupFailed;
    }

    if (const std::string err = validate_config(cfg); !err.empty())
    {
        std::cerr << "error: vehicle_sil_bench_ecu: " << err << "\n";
        return kExitUsage;
    }

    const std::string default_name = "ecu" + std::to_string(cfg.bench_id);
    BenchSummaryState state;
    state.name = explicit_name.value_or(default_name);
    state.sample_every_step_active = sample_every_step_active;
    state.effective_sample_period_ms =
        compute_effective_sample_period_ms(cfg.step_ms, cfg.sample_ms, sample_every_step_active);

    std::vector<const char*> forwarded_ptrs;
    forwarded_ptrs.reserve(forwarded.size() + 1);
    for (const auto& s : forwarded)
    {
        forwarded_ptrs.push_back(s.c_str());
    }
    forwarded_ptrs.push_back(nullptr);

    vehicle_sil::EcuMainOptions ecu_opts;
    ecu_opts.name = default_name;
    ecu_opts.step = std::chrono::milliseconds(cfg.step_ms);

    const int rc = vehicle_sil::ecu_main(
        static_cast<int>(forwarded.size()), forwarded_ptrs.data(), ecu_opts,
        [&](vehicle_sil::Ecu& ecu) { ecu.emplace<BenchApp>(ecu, cfg, state); });

    if (rc == kExitOk && !is_describe && !cfg.out_dir.empty())
    {
        std::string err;
        if (!write_outputs(cfg, state, err))
        {
            std::cerr << "error: " << state.name << ": " << err << "\n";
            return kExitSetupFailed;
        }
    }
    return rc;
}
