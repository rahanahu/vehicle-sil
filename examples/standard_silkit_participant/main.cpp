// SPDX-License-Identifier: MIT
//
// Minimal standard Vector SIL Kit C++17 participant (zero vehicle-sil headers or libraries).
//
// Joins a coordinated virtual-time SIL Kit simulation, exchanges CAN frames on a configured
// network, and handles simultaneous TX/RX timestamps deterministically:
//   - AddFrameHandler buffers every received CAN frame under a mutex (DeterministicRxInbox).
//   - SetSimulationStepHandler processes ONLY frames whose timestamp is strictly earlier than
//     the current step time (frame_timestamp < current_step_time).
//   - Frames with frame_timestamp >= current_step_time are NEVER dropped: they stay queued in
//     the inbox and are processed on the next simulation step.
//   - Drained frames are sorted canonically before processing so output order is reproducible
//     regardless of IPC delivery order or simulation pacing (--speed 1 vs --speed max).

#include "rx_inbox.hpp"

#include <silkit/SilKit.hpp>
#include <silkit/SilKitVersion.hpp>
#include <silkit/config/all.hpp>
#include <silkit/services/can/all.hpp>
#include <silkit/services/orchestration/all.hpp>
#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using standard_silkit_participant::DeterministicRxInbox;
using standard_silkit_participant::RxRecord;

// Maximum millisecond duration whose conversion to nanoseconds fits in int64_t without overflow.
constexpr std::int64_t kMaxMillis = 1'000'000'000'000LL;
constexpr std::uint32_t kMaxStdCanId = 0x7FFu;
constexpr std::uint32_t kMaxExtCanId = 0x1FFFFFFFu;

struct Options
{
    std::string name = "StandardPeer";
    std::string registry_uri = "silkit://127.0.0.1:8500";
    std::string network = "J1939_CAN";
    std::string controller = "CanController1";
    std::int64_t step_ms = 25;
    std::uint32_t tx_id = 0x200u;
    bool tx_extended = false;
    std::int64_t tx_period_ms = 25;
    std::uint8_t run_tag = 0x01u;
    std::string silkit_config_path;
    std::string log_path;
    std::string summary_path;
};

struct CommEvent
{
    char dir = 'T'; // 'T' (TX) or 'R' (RX)
    std::int64_t tx_ns = 0;
    std::int64_t rx_step_ns = -1;
    std::uint32_t can_id = 0;
    std::uint16_t seq = 0;
    std::size_t length = 0;
    std::array<std::uint8_t, 64> data{};
};

std::string hex_bytes(const std::array<std::uint8_t, 64>& data, std::size_t len)
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

std::string json_escape(const std::string& input)
{
    std::string out;
    out.reserve(input.size() + 4u);
    for (char c : input)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::uint16_t read_u16_le(const std::array<std::uint8_t, 64>& data, std::size_t offset)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset]) |
                                      (static_cast<std::uint16_t>(data[offset + 1u]) << 8u));
}

std::int64_t parse_i64_strict(const char* raw, const char* flag, std::int64_t min_val, std::int64_t max_val)
{
    const std::string s(raw);
    if (s.empty() || std::isspace(static_cast<unsigned char>(s.front())))
    {
        throw std::invalid_argument(std::string("invalid integer for ") + flag + ": " + s);
    }
    std::size_t pos = 0;
    const long long parsed = std::stoll(s, &pos, 10);
    if (pos != s.size())
    {
        throw std::invalid_argument(std::string("trailing characters in ") + flag + ": " + s);
    }
    const std::int64_t value = static_cast<std::int64_t>(parsed);
    if (value < min_val || value > max_val)
    {
        throw std::out_of_range(std::string(flag) + " out of range [" + std::to_string(min_val) + ", " +
                                std::to_string(max_val) + "]: " + s);
    }
    return value;
}

std::uint64_t parse_u64_strict(const char* raw, const char* flag, std::uint64_t max_val)
{
    const std::string s(raw);
    if (s.empty() || std::isspace(static_cast<unsigned char>(s.front())) || s.front() == '-' || s.front() == '+')
    {
        throw std::invalid_argument(std::string("invalid unsigned integer for ") + flag + ": " + s);
    }
    std::size_t pos = 0;
    const unsigned long long parsed = std::stoull(s, &pos, 0);
    if (pos != s.size())
    {
        throw std::invalid_argument(std::string("trailing characters in ") + flag + ": " + s);
    }
    const std::uint64_t value = static_cast<std::uint64_t>(parsed);
    if (value > max_val)
    {
        throw std::out_of_range(std::string(flag) + " out of range [0, " + std::to_string(max_val) + "]: " + s);
    }
    return value;
}

bool parse_args(int argc, char** argv, Options& opt)
{
    std::uint64_t raw_tx_id = opt.tx_id;
    bool tx_id_specified = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc)
            {
                throw std::invalid_argument(std::string("missing value for ") + flag);
            }
            return argv[++i];
        };

        if (arg == "--name")
        {
            opt.name = need("--name");
        }
        else if (arg == "--registry-uri")
        {
            opt.registry_uri = need("--registry-uri");
        }
        else if (arg == "--network")
        {
            opt.network = need("--network");
        }
        else if (arg == "--controller")
        {
            opt.controller = need("--controller");
        }
        else if (arg == "--step-ms")
        {
            opt.step_ms = parse_i64_strict(need("--step-ms"), "--step-ms", 1, kMaxMillis);
        }
        else if (arg == "--tx-id")
        {
            raw_tx_id = parse_u64_strict(need("--tx-id"), "--tx-id", kMaxExtCanId);
            tx_id_specified = true;
        }
        else if (arg == "--tx-extended")
        {
            opt.tx_extended = true;
        }
        else if (arg == "--tx-period-ms")
        {
            opt.tx_period_ms = parse_i64_strict(need("--tx-period-ms"), "--tx-period-ms", 0, kMaxMillis);
        }
        else if (arg == "--run-tag")
        {
            opt.run_tag = static_cast<std::uint8_t>(parse_u64_strict(need("--run-tag"), "--run-tag", 0xFFu));
        }
        else if (arg == "--silkit-config")
        {
            opt.silkit_config_path = need("--silkit-config");
        }
        else if (arg == "--log")
        {
            opt.log_path = need("--log");
        }
        else if (arg == "--summary")
        {
            opt.summary_path = need("--summary");
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "usage: standard_silkit_participant [--name NAME] [--registry-uri URI]\n"
                      << "  [--network NET] [--controller CTRL] [--step-ms MS]\n"
                      << "  [--tx-id ID] [--tx-extended] [--tx-period-ms MS] [--run-tag BYTE]\n"
                      << "  [--silkit-config FILE] [--log TSV] [--summary JSON]\n";
            return false;
        }
        else
        {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    if (tx_id_specified)
    {
        const std::uint32_t max_id = opt.tx_extended ? kMaxExtCanId : kMaxStdCanId;
        if (raw_tx_id > max_id)
        {
            throw std::out_of_range("--tx-id exceeds " + std::string(opt.tx_extended ? "29-bit" : "11-bit") +
                                    " CAN ID range");
        }
        opt.tx_id = static_cast<std::uint32_t>(raw_tx_id);
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    try
    {
        if (!parse_args(argc, argv, opt))
        {
            return 0;
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "invalid argument: " << ex.what() << "\n";
        return 2;
    }

    try
    {
        // Declare all callback-captured state BEFORE `participant` so that even on exception
        // unwind, C++ reverse-order destruction tears down `participant` (and stops SIL Kit's
        // background I/O threads) before any captured object is destroyed.
        DeterministicRxInbox rx_inbox(256u);
        std::atomic<std::uint64_t> rx_callback_total{0};
        std::atomic<std::uint64_t> tx_ack_ok{0};
        std::atomic<std::uint64_t> tx_ack_err{0};

        std::vector<CommEvent> events;
        events.reserve(1024u);

        bool have_first_step = false;
        std::int64_t first_step_ns = -1;
        std::int64_t last_step_ns = -1;
        std::int64_t prev_step_ns = -1;
        std::uint64_t steps = 0;
        std::uint64_t non_monotonic_steps = 0;
        std::uint64_t wrong_delta_steps = 0;
        std::uint64_t tx_total = 0;
        std::uint64_t rx_processed = 0;
        std::uint64_t rx_deferred_same_ts = 0;
        std::uint64_t rx_causality_violations = 0;
        std::uint16_t tx_seq = 0;

        const std::int64_t step_ns = opt.step_ms * 1'000'000LL;
        const std::int64_t tx_period_ns = opt.tx_period_ms * 1'000'000LL;
        const std::string silkit_version = SilKit::Version::String();

        std::shared_ptr<SilKit::Config::IParticipantConfiguration> config;
        if (!opt.silkit_config_path.empty())
        {
            config = SilKit::Config::ParticipantConfigurationFromFile(opt.silkit_config_path);
        }
        else
        {
            config = SilKit::Config::ParticipantConfigurationFromString("");
        }

        auto participant = SilKit::CreateParticipant(config, opt.name, opt.registry_uri);
        auto* can_ctrl = participant->CreateCanController(opt.controller, opt.network);

        // Buffer received CAN frames; step logic decides which frames are ready to process.
        can_ctrl->AddFrameHandler(
            [&](SilKit::Services::Can::ICanController*, const SilKit::Services::Can::CanFrameEvent& ev) {
                RxRecord rec;
                rec.tx_ns = ev.timestamp.count();
                rec.can_id = ev.frame.canId;
                rec.flags = ev.frame.flags;
                rec.dlc = ev.frame.dlc;
                rec.length = std::min<std::size_t>(ev.frame.dataField.size(), rec.data.size());
                if (rec.length > 0u)
                {
                    std::memcpy(rec.data.data(), ev.frame.dataField.data(), rec.length);
                }
                rx_inbox.push(rec);
                rx_callback_total.fetch_add(1u, std::memory_order_relaxed);
            });

        can_ctrl->AddFrameTransmitHandler(
            [&](SilKit::Services::Can::ICanController*, const SilKit::Services::Can::CanFrameTransmitEvent& ack) {
                if (ack.status == SilKit::Services::Can::CanTransmitStatus::Transmitted)
                {
                    tx_ack_ok.fetch_add(1u, std::memory_order_relaxed);
                }
                else
                {
                    tx_ack_err.fetch_add(1u, std::memory_order_relaxed);
                }
            });

        SilKit::Services::Orchestration::LifecycleConfiguration lifecycle_cfg{};
        lifecycle_cfg.operationMode = SilKit::Services::Orchestration::OperationMode::Coordinated;
        auto* lifecycle = participant->CreateLifecycleService(lifecycle_cfg);

        lifecycle->SetCommunicationReadyHandler([can_ctrl]() {
            can_ctrl->SetBaudRate(500'000, 2'000'000, 0);
            can_ctrl->Start();
        });

        auto* time_sync = lifecycle->CreateTimeSyncService();
        time_sync->SetSimulationStepHandler(
            [&](std::chrono::nanoseconds now, std::chrono::nanoseconds /*duration*/) {
                const std::int64_t now_ns = now.count();
                if (!have_first_step)
                {
                    have_first_step = true;
                    first_step_ns = now_ns;
                }
                else
                {
                    if (now_ns <= prev_step_ns)
                    {
                        ++non_monotonic_steps;
                    }
                    if (now_ns - prev_step_ns != step_ns)
                    {
                        ++wrong_delta_steps;
                    }
                }
                prev_step_ns = now_ns;
                last_step_ns = now_ns;

                // Deterministic simultaneous-timestamp handling:
                // Process ONLY frames sent strictly before this step (tx_ns < now_ns).
                // Keep equal-or-later timestamp frames queued for the next step (do NOT drop them).
                const std::vector<RxRecord> ready = rx_inbox.drain_before(now_ns, &rx_deferred_same_ts);

                for (const auto& rec : ready)
                {
                    if (rec.tx_ns >= now_ns)
                    {
                        ++rx_causality_violations;
                    }
                    CommEvent ev;
                    ev.dir = 'R';
                    ev.tx_ns = rec.tx_ns;
                    ev.rx_step_ns = now_ns;
                    ev.can_id = rec.can_id;
                    ev.seq = (rec.length >= 4u) ? read_u16_le(rec.data, 2u) : 0u;
                    ev.length = rec.length;
                    ev.data = rec.data;
                    events.push_back(ev);
                    ++rx_processed;
                }

                // Transmit a periodic CAN frame carrying run_tag, step counter, sequence, and virtual time (ms).
                if (tx_period_ns > 0 && ((now_ns - first_step_ns) % tx_period_ns == 0))
                {
                    const std::uint32_t now_ms_u32 = static_cast<std::uint32_t>(now_ns / 1'000'000LL);
                    std::array<std::uint8_t, 8> payload{};
                    payload[0] = opt.run_tag;
                    payload[1] = static_cast<std::uint8_t>(steps & 0xFFu);
                    payload[2] = static_cast<std::uint8_t>(tx_seq & 0xFFu);
                    payload[3] = static_cast<std::uint8_t>((tx_seq >> 8u) & 0xFFu);
                    payload[4] = static_cast<std::uint8_t>(now_ms_u32 & 0xFFu);
                    payload[5] = static_cast<std::uint8_t>((now_ms_u32 >> 8u) & 0xFFu);
                    payload[6] = static_cast<std::uint8_t>((now_ms_u32 >> 16u) & 0xFFu);
                    payload[7] = static_cast<std::uint8_t>((now_ms_u32 >> 24u) & 0xFFu);

                    SilKit::Services::Can::CanFrame frame{};
                    frame.canId = opt.tx_id;
                    frame.flags = opt.tx_extended
                                      ? static_cast<SilKit::Services::Can::CanFrameFlagMask>(
                                            SilKit::Services::Can::CanFrameFlag::Ide)
                                      : 0u;
                    frame.dlc = 8u;
                    frame.dataField = SilKit::Util::Span<const std::uint8_t>(payload.data(), payload.size());

                    can_ctrl->SendFrame(frame);

                    CommEvent ev;
                    ev.dir = 'T';
                    ev.tx_ns = now_ns;
                    ev.rx_step_ns = -1;
                    ev.can_id = frame.canId;
                    ev.seq = tx_seq;
                    ev.length = payload.size();
                    std::memcpy(ev.data.data(), payload.data(), payload.size());
                    events.push_back(ev);

                    ++tx_seq;
                    ++tx_total;
                }

                ++steps;
            },
            std::chrono::milliseconds(opt.step_ms));

        auto final_state_future = lifecycle->StartLifecycle();
        const auto final_state = final_state_future.get();

        // Tear down the participant immediately after shutdown so SIL Kit stops invoking
        // AddFrameHandler / AddFrameTransmitHandler before we read final counts or write files.
        participant.reset();

        const std::uint64_t rx_queued_at_shutdown = static_cast<std::uint64_t>(rx_inbox.queued_count());

        if (!opt.log_path.empty())
        {
            std::ofstream out(opt.log_path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                std::cerr << "failed to open log path: " << opt.log_path << "\n";
                return 1;
            }
            out << "dir\ttx_ns\ttx_ms\trx_step_ns\trx_step_ms\tcan_id\tseq\tpayload\n";
            for (const auto& ev : events)
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

        std::ostringstream json;
        json << "{\n"
             << "  \"participant\": \"" << json_escape(opt.name) << "\",\n"
             << "  \"silkit_version\": \"" << json_escape(silkit_version) << "\",\n"
             << "  \"network\": \"" << json_escape(opt.network) << "\",\n"
             << "  \"step_ms\": " << opt.step_ms << ",\n"
             << "  \"run_tag_hex\": \"0x" << std::hex << static_cast<unsigned>(opt.run_tag) << std::dec << "\",\n"
             << "  \"final_state\": \"" << SilKit::Services::Orchestration::to_string(final_state) << "\",\n"
             << "  \"steps\": " << steps << ",\n"
             << "  \"first_step_ns\": " << first_step_ns << ",\n"
             << "  \"last_step_ns\": " << last_step_ns << ",\n"
             << "  \"non_monotonic_steps\": " << non_monotonic_steps << ",\n"
             << "  \"wrong_delta_steps\": " << wrong_delta_steps << ",\n"
             << "  \"tx_total\": " << tx_total << ",\n"
             << "  \"tx_ack_ok\": " << tx_ack_ok.load() << ",\n"
             << "  \"tx_ack_err\": " << tx_ack_err.load() << ",\n"
             << "  \"rx_processed\": " << rx_processed << ",\n"
             << "  \"rx_callback_total\": " << rx_callback_total.load() << ",\n"
             << "  \"rx_deferred_same_ts\": " << rx_deferred_same_ts << ",\n"
             << "  \"rx_queued_at_shutdown\": " << rx_queued_at_shutdown << ",\n"
             << "  \"rx_causality_violations\": " << rx_causality_violations << "\n"
             << "}\n";

        if (!opt.summary_path.empty())
        {
            std::ofstream sout(opt.summary_path, std::ios::binary | std::ios::trunc);
            if (!sout)
            {
                std::cerr << "failed to open summary path: " << opt.summary_path << "\n";
                return 1;
            }
            sout << json.str();
        }
        std::cout << json.str() << std::flush;

        return (final_state == SilKit::Services::Orchestration::ParticipantState::Shutdown) ? 0 : 1;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "standard_silkit_participant error: " << ex.what() << "\n";
        return 1;
    }
}
