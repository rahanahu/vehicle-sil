// SPDX-License-Identifier: MIT
//
// Minimal vehicle-sil raw-CAN ECU used by tests/standard_silkit_participant to verify
// asymmetric-period bidirectional CAN exchange, virtual-time causality, and cross-run isolation
// against examples/standard_silkit_participant.
//
// Strips its own --ecu-* options before delegating remaining arguments to vehicle_sil::ecu_main().

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/raw_channel.hpp"
#include "vehicle_sil/process/paths.hpp"
#include "vehicle_sil/simulation/ecu_main.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct EcuOptions
{
    std::string network = "TEST_CAN";
    std::int64_t step_ms = 10;
    std::uint32_t tx_id = 0x100u;
    std::int64_t tx_period_ms = 10;
    std::uint8_t run_tag = 0x11u;
    std::string log_path;
    std::string summary_path;
};

struct CommEvent
{
    char dir = 'T';
    std::int64_t tx_ns = 0;
    std::int64_t rx_step_ns = -1;
    std::uint32_t can_id = 0;
    std::uint16_t seq = 0;
    std::uint8_t length = 0;
    std::array<std::uint8_t, vehicle_sil::can::kMaxPayload> data{};
};

struct RunState
{
    bool have_first_step = false;
    std::int64_t first_step_ns = -1;
    std::int64_t last_step_ns = -1;
    std::int64_t prev_step_ns = -1;
    std::uint64_t steps = 0;
    std::uint64_t non_monotonic_steps = 0;
    std::uint64_t wrong_delta_steps = 0;
    std::uint64_t tx_total = 0;
    std::uint64_t rx_total = 0;
    std::uint64_t rx_causality_violations = 0;
    std::vector<CommEvent> events;
};

std::string hex_bytes(const std::array<std::uint8_t, vehicle_sil::can::kMaxPayload>& data, std::size_t len)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2u);
    for (std::size_t i = 0; i < len; ++i)
    {
        out.push_back(kHex[(data[i] >> 4u) & 0xFu]);
        out.push_back(kHex[data[i] & 0xFu]);
    }
    return out;
}

std::uint16_t read_u16_le(const std::array<std::uint8_t, vehicle_sil::can::kMaxPayload>& data, std::size_t offset)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset]) |
                                      (static_cast<std::uint16_t>(data[offset + 1u]) << 8u));
}

class TestCanEcu
{
public:
    TestCanEcu(vehicle_sil::Ecu& ecu, const EcuOptions& opt, RunState& state) : opt_(opt), state_(state)
    {
        can_ = &ecu.can(opt_.network, {.send_capacity = 64, .receive_capacity = 1024});
        can_->on_frame([this](const vehicle_sil::can::TimedFrame& tf) { step_rx_.push_back(tf); });
        ecu.on_step([this](vehicle_sil::SimTime now) { step(now); });
    }

private:
    void step(vehicle_sil::SimTime now)
    {
        const std::int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        const std::int64_t step_ns = opt_.step_ms * 1'000'000LL;
        const std::int64_t tx_period_ns = opt_.tx_period_ms * 1'000'000LL;

        if (!state_.have_first_step)
        {
            state_.have_first_step = true;
            state_.first_step_ns = now_ns;
        }
        else
        {
            if (now_ns <= state_.prev_step_ns)
            {
                ++state_.non_monotonic_steps;
            }
            if (now_ns - state_.prev_step_ns != step_ns)
            {
                ++state_.wrong_delta_steps;
            }
        }
        state_.prev_step_ns = now_ns;
        state_.last_step_ns = now_ns;

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
                          if (a.frame.length != b.frame.length)
                          {
                              return a.frame.length < b.frame.length;
                          }
                          return std::memcmp(a.frame.data.data(), b.frame.data.data(), a.frame.length) < 0;
                      });

            for (const auto& tf : step_rx_)
            {
                const std::int64_t tx_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(tf.time.time_since_epoch()).count();
                if (tx_ns >= now_ns)
                {
                    ++state_.rx_causality_violations;
                }
                CommEvent ev;
                ev.dir = 'R';
                ev.tx_ns = tx_ns;
                ev.rx_step_ns = now_ns;
                ev.can_id = tf.frame.id;
                ev.seq = tf.frame.length >= 4u ? read_u16_le(tf.frame.data, 2u) : 0u;
                ev.length = tf.frame.length;
                ev.data = tf.frame.data;
                state_.events.push_back(ev);
                ++state_.rx_total;
            }
            step_rx_.clear();
        }

        if (tx_period_ns > 0 && ((now_ns - state_.first_step_ns) % tx_period_ns == 0))
        {
            const std::uint32_t now_ms_u32 = static_cast<std::uint32_t>(now_ns / 1'000'000LL);
            vehicle_sil::can::Frame frame{};
            frame.id = opt_.tx_id;
            frame.format = vehicle_sil::can::IdFormat::Standard;
            frame.length = 8u;
            frame.data[0] = opt_.run_tag;
            frame.data[1] = static_cast<std::uint8_t>(state_.steps & 0xFFu);
            frame.data[2] = static_cast<std::uint8_t>(tx_seq_ & 0xFFu);
            frame.data[3] = static_cast<std::uint8_t>((tx_seq_ >> 8u) & 0xFFu);
            frame.data[4] = static_cast<std::uint8_t>(now_ms_u32 & 0xFFu);
            frame.data[5] = static_cast<std::uint8_t>((now_ms_u32 >> 8u) & 0xFFu);
            frame.data[6] = static_cast<std::uint8_t>((now_ms_u32 >> 16u) & 0xFFu);
            frame.data[7] = static_cast<std::uint8_t>((now_ms_u32 >> 24u) & 0xFFu);

            static_cast<void>(can_->send(frame));

            CommEvent ev;
            ev.dir = 'T';
            ev.tx_ns = now_ns;
            ev.rx_step_ns = -1;
            ev.can_id = frame.id;
            ev.seq = tx_seq_;
            ev.length = frame.length;
            ev.data = frame.data;
            state_.events.push_back(ev);

            ++tx_seq_;
            ++state_.tx_total;
        }

        ++state_.steps;
    }

    const EcuOptions& opt_;
    RunState& state_;
    vehicle_sil::can::RawChannel* can_ = nullptr;
    std::vector<vehicle_sil::can::TimedFrame> step_rx_;
    std::uint16_t tx_seq_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    EcuOptions opt;
    std::vector<char*> forwarded;
    forwarded.reserve(static_cast<std::size_t>(argc));
    forwarded.push_back(argv[0]);

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc)
            {
                std::cerr << "missing value for " << flag << "\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--ecu-network")
        {
            opt.network = need("--ecu-network");
        }
        else if (arg == "--ecu-step-ms")
        {
            opt.step_ms = std::stoll(need("--ecu-step-ms"));
        }
        else if (arg == "--ecu-tx-id")
        {
            opt.tx_id = static_cast<std::uint32_t>(std::stoul(need("--ecu-tx-id"), nullptr, 0));
        }
        else if (arg == "--ecu-tx-period-ms")
        {
            opt.tx_period_ms = std::stoll(need("--ecu-tx-period-ms"));
        }
        else if (arg == "--ecu-run-tag")
        {
            opt.run_tag = static_cast<std::uint8_t>(std::stoul(need("--ecu-run-tag"), nullptr, 0));
        }
        else if (arg == "--ecu-log")
        {
            opt.log_path = need("--ecu-log");
        }
        else if (arg == "--ecu-summary")
        {
            opt.summary_path = need("--ecu-summary");
        }
        else
        {
            forwarded.push_back(argv[i]);
        }
    }

    RunState state;
    const int rc = vehicle_sil::ecu_main<TestCanEcu>(
        static_cast<int>(forwarded.size()), forwarded.data(),
        vehicle_sil::EcuMainOptions{.name = "VehicleEcu", .step = std::chrono::milliseconds(opt.step_ms)}, opt, state);

    if (rc == 0 && state.steps > 0)
    {
        if (!opt.log_path.empty())
        {
            std::ofstream out(vehicle_sil::process::path_from_utf8(opt.log_path), std::ios::binary | std::ios::trunc);
            if (!out)
            {
                std::cerr << "failed to write ecu log: " << opt.log_path << "\n";
                return 1;
            }
            out << "dir\ttx_ns\ttx_ms\trx_step_ns\trx_step_ms\tcan_id\tseq\tpayload\n";
            for (const auto& ev : state.events)
            {
                if (ev.dir == 'T')
                {
                    out << "T\t" << ev.tx_ns << "\t" << (ev.tx_ns / 1'000'000LL) << "\t-\t-\t0x" << std::hex
                        << ev.can_id << std::dec << "\t" << ev.seq << "\t" << hex_bytes(ev.data, ev.length) << "\n";
                }
                else
                {
                    out << "R\t" << ev.tx_ns << "\t" << (ev.tx_ns / 1'000'000LL) << "\t" << ev.rx_step_ns << "\t"
                        << (ev.rx_step_ns / 1'000'000LL) << "\t0x" << std::hex << ev.can_id << std::dec << "\t"
                        << ev.seq << "\t" << hex_bytes(ev.data, ev.length) << "\n";
                }
            }
        }
        if (!opt.summary_path.empty())
        {
            std::ofstream out(vehicle_sil::process::path_from_utf8(opt.summary_path), std::ios::binary | std::ios::trunc);
            if (!out)
            {
                std::cerr << "failed to write ecu summary: " << opt.summary_path << "\n";
                return 1;
            }
            out << "{\n"
                << "  \"steps\": " << state.steps << ",\n"
                << "  \"first_step_ns\": " << state.first_step_ns << ",\n"
                << "  \"last_step_ns\": " << state.last_step_ns << ",\n"
                << "  \"non_monotonic_steps\": " << state.non_monotonic_steps << ",\n"
                << "  \"wrong_delta_steps\": " << state.wrong_delta_steps << ",\n"
                << "  \"tx_total\": " << state.tx_total << ",\n"
                << "  \"rx_total\": " << state.rx_total << ",\n"
                << "  \"rx_causality_violations\": " << state.rx_causality_violations << "\n"
                << "}\n";
        }
    }
    return rc;
}
