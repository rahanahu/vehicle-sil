// SPDX-License-Identifier: MIT
//
// A protocol of one's own, "on a request, send these values every period" (pid_stream.hpp). An
// engine ECU exposes three values by id; a tool subscribes, renews the subscription with another
// period and ids, and stops it; a second tool subscribes after that and gets a stream of its own.
// The run is repeated with a different byte layout: only the Format changes. The tools check what
// arrived, when, and that nothing arrives after Stop.
#include "pid_stream.hpp"

#include "vehicle_sil/simulation/simulation.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <iterator>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimTime;

namespace {

constexpr std::uint8_t kRpm = 0x01, kCoolant = 0x02, kLoad = 0x03; // ids of this example

class Engine
{
public:
    Engine(vehicle_sil::Ecu& ecu, const pid_stream::Format& format)
    {
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x00});
        pid_stream::PidTable table{{kRpm, [this] { return rpm_; }},
                                   {kCoolant, [] { return std::uint16_t{85}; }},
                                   {kLoad, [this] { return static_cast<std::uint16_t>(rpm_ / 30); }}};
        streamer_ = &node.attach<pid_stream::PidStreamer>(std::move(table), pid_stream::StreamerOptions{.format = format});
        ecu.every(10ms, [this](SimTime) { rpm_ += 25; }); // the model
    }
    pid_stream::PidStreamer* streamer_ = nullptr;

private:
    std::uint16_t rpm_ = 800;
};

class Tool
{
public:
    Tool(vehicle_sil::Ecu& ecu, const pid_stream::Format& format)
    {
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x80});
        auto& client = node.attach<pid_stream::PidClient>(format);
        auto record = [this](const pid_stream::Reply& r) { replies_.push_back(r); };
        // The tool's script. Each action runs once, in the first step at or after its time (also
        // with a step size that does not divide it), from the step: subscribe() sends.
        ecu.at(SimTime{} + 100ms, [&client, record](SimTime) {
            client.subscribe(0x00, {kRpm, kCoolant, kLoad}, 50ms, record); // 11 bytes: transport protocol
        });
        ecu.at(SimTime{} + 300ms, [&client, record](SimTime) {
            client.subscribe(0x00, {kRpm, 0x7E}, 100ms, record); // renew: new period, 0x7E is unknown
        });
        ecu.at(SimTime{} + 600ms, [&client](SimTime) { client.unsubscribe(); });
    }
    std::vector<pid_stream::Reply> replies_;
};

/// A second tool, at another address, that subscribes after the first one stopped.
class Logger
{
public:
    Logger(vehicle_sil::Ecu& ecu, const pid_stream::Format& format)
    {
        auto& client = ecu.j1939("J1939_CAN").add_node({.address = 0x81}).attach<pid_stream::PidClient>(format);
        ecu.at(SimTime{} + 650ms, [this, &client](SimTime) {
            client.subscribe(0x00, {kCoolant}, 100ms, [this](const pid_stream::Reply& r) { replies_.push_back(r); });
        });
        ecu.at(SimTime{} + 1000ms, [&client](SimTime) { client.unsubscribe(); });
    }
    std::vector<pid_stream::Reply> replies_;
};

bool run(const char* label, const pid_stream::Format& format)
{
    vehicle_sil::Simulation sim;
    auto& engine = sim.add<Engine>("Engine", 10ms, format);
    auto& tool = sim.add<Tool>("Tool", 5ms, format);
    auto& logger = sim.add<Logger>("Logger", 5ms, format);
    sim.run_for(700ms);
    const auto count_at_stop = tool.replies_.size();
    sim.run_for(500ms); // a stopped stream stays stopped
    const auto& r = tool.replies_;

    // The first subscription gives 3 values per reply (11 bytes: the transport protocol), the
    // renewed one 2 values (a single frame, so its time is the exact send time).
    std::size_t big = 0, small = 0;
    bool periodic = true, rising = true;
    std::uint16_t last_rpm[2]{}; // per kind: a reply of the old stream can still be on its way after the renewal
    for (std::size_t i = 0; i < r.size(); ++i)
    {
        const std::size_t kind = r[i].readings.size() == 3 ? 0 : 1;
        (kind == 0 ? big : small) += 1;
        rising = rising && r[i].readings[0].value > last_rpm[kind];
        last_rpm[kind] = r[i].readings[0].value;
        if (i > 0 && r[i].readings.size() == 2 && r[i - 1].readings.size() == 2)
        {
            periodic = periodic && r[i].time - r[i - 1].time == 100ms;
        }
    }
    const bool unknown_id = !r.empty() && r.back().readings.size() == 2 && r.back().readings[1].id == 0x7E &&
                            r.back().readings[1].value == 0xFFFF;
    const bool stopped = r.size() == count_at_stop && engine.streamer_->streams() == 0;
    // The second tool: the first reply in the step that handled its Start (660 ms), then every 100 ms
    // counted from the Start's time (650 ms); its Stop at 1000 ms comes before 1050 ms. The coolant only.
    const auto& l = logger.replies_;
    const long long expected[]{660, 750, 850, 950};
    bool second = l.size() == std::size(expected);
    for (std::size_t i = 0; second && i < l.size(); ++i)
    {
        second = (l[i].time - SimTime{}) / 1ms == expected[i] && l[i].readings.size() == 1 && l[i].readings[0].id == kCoolant &&
                 l[i].readings[0].value == 85;
    }
    std::printf("%-9s %zu replies of 3 values, %zu of 2 (every 100 ms: %s); rpm rises: %s; stays stopped: %s; second tool: %s\n",
                label, big, small, periodic ? "yes" : "NO", rising ? "yes" : "NO", stopped ? "yes" : "NO", second ? "yes" : "NO");
    return big >= 4 && small >= 3 && periodic && rising && unknown_id && stopped && second;
}

} // namespace

int main()
{
    try
    {
        pid_stream::Format standard;
        // Another layout: values big endian, no sequence byte. The readers and writers are the only change.
        pid_stream::Format compact = standard;
        compact.write_reply = [](auto& out, std::uint8_t, std::span<const pid_stream::Reading> readings) {
            out.push_back(0x81);
            for (const auto& x : readings) out.insert(out.end(), {x.id, static_cast<std::uint8_t>(x.value >> 8), static_cast<std::uint8_t>(x.value)});
        };
        compact.read_reply = [](std::span<const std::uint8_t> d) -> std::optional<pid_stream::Reply> {
            if (d.empty() || d[0] != 0x81 || (d.size() - 1) % 3 != 0) return std::nullopt;
            pid_stream::Reply reply;
            for (std::size_t i = 1; i < d.size(); i += 3) reply.readings.push_back({d[i], static_cast<std::uint16_t>((d[i + 1] << 8) | d[i + 2])});
            return reply;
        };
        const bool standard_ok = run("standard", standard); // both always run
        return run("compact", compact) && standard_ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
