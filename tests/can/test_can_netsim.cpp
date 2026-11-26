// SPDX-License-Identifier: MIT
//
// The simple CAN network simulator (vehicle_sil::can_netsim) on real SIL Kit participants
// (in-process registry): arbitration, transmission time and bus occupancy; the same frame times
// as the virtual bus with the same simulator step; independence from the step sizes of the
// other participants; repeatability; senders without a send time; and the configuration file
// shared with the Vector SIL Kit Network Simulator. The last test runs the executable.
#include "vehicle_sil/process/child_process.hpp"
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/bus_model.hpp"
#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/raw_channel.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"
#include "vehicle_sil/can_netsim/can_bus_simulator.hpp"
#include "vehicle_sil/can_netsim/network_simulator_config.hpp"
#include "vehicle_sil/can_silkit/send_time_tag.hpp"
#include "vehicle_sil/can_silkit/silkit_can_port.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/services/can/all.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
namespace orch = SilKit::Services::Orchestration;
namespace sk = SilKit::Services::Can;
using vehicle_sil::ParticipantRuntime;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::SimulationController;
using vehicle_sil::SimulationControllerOptions;
using vehicle_sil::to_ns;
using vehicle_sil::VirtualTimeOptions;
using vehicle_sil::can::CanBusSimulator;
using vehicle_sil::can::CanBusSimulatorOptions;
using vehicle_sil::can::Frame;
using vehicle_sil::can::Host;
using vehicle_sil::can::IdFormat;
using vehicle_sil::can::NetworkSimulatorConfigError;
using vehicle_sil::can::Port;
using vehicle_sil::can::PortEvent;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::SilKitCanPort;
using vehicle_sil::can::SilKitCanPortOptions;
using vehicle_sil::can::Steppable;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::start_together;

namespace {

constexpr const char* kNetwork = "CAN1";
constexpr std::uint32_t kBitrate = 250'000;

Frame ext(std::uint32_t id, std::uint8_t length = 8, std::uint8_t first = 0)
{
    Frame f;
    f.id = id;
    f.format = IdFormat::Extended;
    f.length = length;
    f.data[0] = first;
    return f;
}

std::uint32_t j1939_id(std::uint32_t priority, std::uint32_t pgn, std::uint32_t source)
{
    return (priority << 26) | (pgn << 8) | source;
}

SimTime at(SimDuration t)
{
    return SimTime{} + t;
}

/// Sends a scripted list of frames (at the first step at or after each time) and records what
/// it receives and what its port reports.
class Scripted final : public Steppable
{
public:
    explicit Scripted(std::vector<TimedFrame> script = {}) : script_(std::move(script)) {}

    void begin(SimTime now) override { now_ = now; }
    void receive(const TimedFrame& frame) override
    {
        trace_.push_back(std::to_string(to_ns(frame.time)) + " rx " + std::to_string(frame.frame.id) + " " +
                         std::to_string(frame.frame.data[0]));
    }
    void on_port_event(const PortEvent& event) override
    {
        const char* kind = event.kind == PortEvent::Kind::TransmitCompleted ? " done " : " failed ";
        trace_.push_back(std::to_string(to_ns(event.time)) + kind + std::to_string(event.id) + " sent " +
                         (event.send_time ? std::to_string(to_ns(*event.send_time)) : std::string("?")));
    }
    void process() override {}
    void flush(Port& port) override
    {
        while (next_ < script_.size() && script_[next_].time <= now_)
        {
            TimedFrame f = script_[next_];
            f.time = now_;
            const SendResult result = port.send(f);
            if (result == SendResult::QueueFull)
            {
                break; // the port's transmit queue is full: again at the next step
            }
            if (result != SendResult::Accepted)
            {
                throw std::runtime_error("send failed in the scenario");
            }
            ++next_;
        }
    }

    /// The trace ordered by time: how events and frames of one step interleave depends on the
    /// step size, their times do not.
    [[nodiscard]] std::vector<std::string> trace() const
    {
        auto out = trace_;
        std::stable_sort(out.begin(), out.end(), [](const std::string& x, const std::string& y) { return std::stoll(x) < std::stoll(y); });
        return out;
    }

private:
    std::vector<TimedFrame> script_;
    std::size_t next_ = 0;
    SimTime now_{};
    std::vector<std::string> trace_;
};

struct PeerSpec
{
    std::string name;
    SimDuration step;
    std::vector<TimedFrame> script;
    std::optional<std::uint32_t> bitrate{}; ///< SetBaudRate() of the port; empty: the run's
    std::size_t port_queue = 0;             ///< SilKitCanPortOptions::transmit_queue_capacity
};

using Traces = std::vector<std::vector<std::string>>;

/// A and B send (10 ms steps), R1..R3 only listen (1, 3 and 7 ms steps):
/// - 10 ms: A sends priority 6 then 3, B priority 3 (arbitration, A's FIFO);
/// - 30 ms: A floods 40 frames; 40 ms: B's priority 3 frame waits only for the frame on the bus;
/// - 80 ms: an 11-bit frame (B) and a 29-bit frame with the same base identifier (A).
std::vector<PeerSpec> scenario(SimDuration a = 10ms, SimDuration b = 10ms, std::vector<SimDuration> receivers = {1ms, 3ms, 7ms})
{
    std::vector<TimedFrame> as{{at(10ms), ext(j1939_id(6, 0xFF10, 0x80), 8, 1)}, {at(10ms), ext(j1939_id(3, 0xFF20, 0x80), 8, 2)}};
    for (std::uint8_t k = 0; k < 40; ++k)
    {
        as.push_back({at(30ms), ext(j1939_id(6, 0xFF11, 0x80), 8, k)});
    }
    as.push_back({at(80ms), ext(0x100u << 18, 1, 9)});
    Frame std100 = ext(0x100, 1, 8);
    std100.format = IdFormat::Standard;
    std::vector<TimedFrame> bs{{at(10ms), ext(j1939_id(3, 0xFF30, 0x90), 8, 3)},
                               {at(40ms), ext(j1939_id(3, 0xFF31, 0x90), 8, 4)},
                               {at(80ms), std100}};
    std::vector<PeerSpec> peers{{"A", a, as}, {"B", b, bs}};
    for (std::size_t i = 0; i < receivers.size(); ++i)
    {
        peers.push_back({"R" + std::to_string(i + 1), receivers[i], {}});
    }
    return peers;
}

constexpr SimTime kEnd = SimTime{} + 100ms;

Traces run_on_virtual_bus(const std::vector<PeerSpec>& peers, SimDuration simulatorStep)
{
    vehicle_sil::can::VirtualCanBusOptions options;
    options.timing = vehicle_sil::can::VirtualCanBusTiming{kBitrate, 64, simulatorStep};
    vehicle_sil::can::VirtualCanBus bus(options);
    std::vector<std::unique_ptr<Scripted>> nodes;
    std::vector<std::unique_ptr<Host>> hosts;
    vehicle_sil::can::StepDriver driver;
    for (const auto& peer : peers)
    {
        nodes.push_back(std::make_unique<Scripted>(peer.script));
        hosts.push_back(std::make_unique<Host>());
        hosts.back()->add(*nodes.back(), bus.attach());
        driver.add(*hosts.back(), peer.step);
    }
    driver.run_until(kEnd);
    Traces traces;
    for (const auto& node : nodes)
    {
        traces.push_back(node->trace());
    }
    return traces;
}

struct SilKitPeer
{
    SilKitPeer() = default;
    SilKitPeer(const SilKitPeer&) = delete;
    SilKitPeer& operator=(const SilKitPeer&) = delete;
    ~SilKitPeer()
    {
        if (runtime)
        {
            runtime->stop("test cleanup");
        }
        if (port)
        {
            port->close();
        }
    }

    std::unique_ptr<ParticipantRuntime> runtime;
    std::unique_ptr<SilKitCanPort> port;
    std::unique_ptr<Scripted> node;
    std::unique_ptr<Host> host;
};

enum class Simulator
{
    InProcess,  ///< CanBusSimulator in a participant of this process
    Executable, ///< vehicle_sil_can_bus_simulator as a child process
};

/// A participant "F" with a raw SIL Kit CAN controller (no vehicle-sil port), in steps of 1 ms.
struct Foreign
{
    bool start = true; ///< Start() when the controller is made
    /// Called in each step; `send(first data byte)` sends one frame with the step's time as
    /// its send time.
    std::function<void(sk::ICanController& controller, SimTime now, const std::function<void(std::uint8_t)>& send)> on_step;
    /// With it, F has a second raw controller on the network (started), driven like the first
    /// in each step, after `on_step`.
    std::function<void(sk::ICanController& controller, SimTime now, const std::function<void(std::uint8_t)>& send)> on_step_second;
};

/// The identifier of the frames a Foreign's `send` sends.
constexpr std::uint32_t kForeignId = (6u << 26) | (0xFF40u << 8) | 0x70u;

struct SilKitRun
{
    Traces traces;
    vehicle_sil::can::CanBusSimulatorStatistics statistics; ///< InProcess only
    std::vector<vehicle_sil::can::SilKitCanPortStatistics> ports;
    std::size_t started_controllers = 0; ///< peers whose controller the simulator reported as Started
    std::vector<std::string> foreign_acks;  ///< the Foreign's transmit acknowledgements: "<status> <first data byte>"
    std::vector<std::string> simulator_warnings; ///< InProcess only: what the simulator logged as a warning, in order
};

std::filesystem::path scratch_dir()
{
    const auto dir = std::filesystem::temp_directory_path() / "vehicle_sil_can_netsim_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

/// Runs the peers on SIL Kit with a simple network simulator of step `simulatorStep`.
/// `foreign`: also a participant with a raw SIL Kit controller; without `on_step` it sends at
/// 30 ms with a real pointer as userContext (no send time). `simulatorLast` (InProcess): the
/// simulator joins after the peers have made their controllers and ports.
SilKitRun run_on_silkit(const std::vector<PeerSpec>& peers, SimDuration simulatorStep, Simulator kind = Simulator::InProcess,
                        const Foreign* foreign = nullptr, CanBusSimulatorOptions simulatorOptions = {}, std::uint32_t portBitrate = kBitrate,
                        bool simulatorLast = false)
{
    InProcessRegistry registry;
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"NetworkSimulator"};
    for (const auto& peer : peers)
    {
        controllerOptions.required_participants.push_back(peer.name);
    }
    if (foreign)
    {
        controllerOptions.required_participants.push_back("F");
    }
    controllerOptions.stop_at = kEnd - SimTime{} + 20ms;
    SimulationController controller(controllerOptions);

    std::vector<ParticipantRuntime*> runtimes;
    // Declared before the runtimes whose handlers use them, so they are destroyed after them.
    std::unique_ptr<CanBusSimulator> simulator;
    int localObject = 0;
    std::atomic<bool> foreignStarted{false};
    std::mutex foreignMutex;
    std::vector<std::string> foreignAcks;
    std::atomic<std::size_t> startedControllers{0};
    std::mutex warningsMutex;
    std::vector<std::string> simulatorWarnings;
    std::unique_ptr<ParticipantRuntime> simulatorRuntime;
    std::unique_ptr<vehicle_sil::process::ChildProcess> child;
    const auto logFile = scratch_dir() / ("simulator-" + std::to_string(to_ns(at(simulatorStep))) + ".log");
    const auto join_simulator = [&] {
        simulatorRuntime = std::make_unique<ParticipantRuntime>(
            coordinated_options("NetworkSimulator", registry.uri, VirtualTimeOptions{simulatorStep, std::nullopt, kEnd}));
        // A second network that no controller joins: it must not disturb the run.
        simulator = std::make_unique<CanBusSimulator>(simulatorRuntime->participant(),
                                                      std::vector<std::string>{kNetwork, "UnusedCAN"}, simulatorOptions,
                                                      [&warningsMutex, &simulatorWarnings](CanBusSimulator::LogLevel level,
                                                                                           std::string_view message) {
                                                          if (level == CanBusSimulator::LogLevel::Warning)
                                                          {
                                                              const std::lock_guard lock(warningsMutex);
                                                              simulatorWarnings.emplace_back(message);
                                                          }
                                                      });
        simulatorRuntime->set_step_handler([&simulator](SimTime now, SimDuration step) { simulator->step(now, step); });
        runtimes.push_back(simulatorRuntime.get());
    };
    if (kind == Simulator::InProcess)
    {
        if (!simulatorLast)
        {
            join_simulator();
        }
    }
    else
    {
        // The configuration in the Vector SIL Kit Network Simulator format, with networks the
        // simple simulator does not simulate.
        // A name outside ASCII: the simulator reads its command line as UTF-8 on Windows too.
        const auto config = scratch_dir() / std::filesystem::path(u8"netsim-\u8ECA\u4E21.yaml");
        std::ofstream(config) << "SchemaVersion: 1\n"
                                 "Description: vehicle-sil test\n"
                                 "SimulationStepSizeNanoseconds: "
                              << to_ns(at(simulatorStep))
                              << "\n"
                                 "SimulatedNetworks:\n"
                                 "  - Name: CAN1\n"
                                 "    Type: CAN\n"
                                 "  - Name: LIN1\n"
                                 "    Type: LIN\n"
                                 "  - Name: UnusedCAN\n"
                                 "    Type: CAN\n";
        child = std::make_unique<vehicle_sil::process::ChildProcess>(
            VEHICLE_SIL_CAN_BUS_SIMULATOR,
            std::vector<std::string>{"--connect-uri", registry.uri, "--log", "info", "--start-timeout-ms", "60000",
                                     "--default-bitrate", std::to_string(kBitrate), vehicle_sil::process::utf8_from_path(config)},
            logFile);
        // Let the simulator come up before the peers (the order does not matter to what it learns
        // from them: simulator_learns_the_bit_rate_and_the_start_whichever_joins_first).
        const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(30s);
        while (true)
        {
            std::ifstream log(logFile);
            const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            if (text.find("waiting for the simulation to start") != std::string::npos)
            {
                break;
            }
            if (child->poll() || std::chrono::steady_clock::now() > deadline)
            {
                throw std::runtime_error("vehicle_sil_can_bus_simulator did not come up, log: " + logFile.string());
            }
            std::this_thread::sleep_for(10ms);
        }
    }

    std::vector<SilKitPeer> nodes(peers.size());
    for (std::size_t i = 0; i < peers.size(); ++i)
    {
        auto& node = nodes[i];
        node.runtime = std::make_unique<ParticipantRuntime>(
            coordinated_options(peers[i].name, registry.uri, VirtualTimeOptions{peers[i].step, std::nullopt, kEnd}));
        SilKitCanPortOptions portOptions;
        portOptions.bitrate = peers[i].bitrate.value_or(portBitrate);
        portOptions.transmit_queue_capacity = peers[i].port_queue;
        auto* canController = node.runtime->participant().CreateCanController("CAN", kNetwork);
        // With a simulator, the state changes only when the simulator answers the Start().
        canController->AddStateChangeHandler([&startedControllers](sk::ICanController*, const sk::CanStateChangeEvent& e) {
            if (e.state == sk::CanControllerState::Started)
            {
                startedControllers.fetch_add(1);
            }
        });
        node.port = std::make_unique<SilKitCanPort>(*canController, portOptions);
        node.runtime->add_communication_ready_handler([p = node.port.get()] { p->start(); });
        node.node = std::make_unique<Scripted>(peers[i].script);
        node.host = std::make_unique<Host>();
        node.host->add(*node.node, *node.port);
        vehicle_sil::can::bind_host(*node.runtime, *node.host);
        runtimes.push_back(node.runtime.get());
    }
    if (kind == Simulator::InProcess && simulatorLast)
    {
        join_simulator();
    }
    std::unique_ptr<ParticipantRuntime> foreignRuntime;
    if (foreign)
    {
        foreignRuntime = std::make_unique<ParticipantRuntime>(
            coordinated_options("F", registry.uri, VirtualTimeOptions{foreign->on_step ? 1ms : 10ms, std::nullopt, kEnd}));
        auto* raw = foreignRuntime->participant().CreateCanController("CAN", kNetwork);
        // The first data byte of a frame travels as the low bits of its userContext's send time
        // (ns), so that an acknowledgement says which frame it is for.
        raw->AddFrameTransmitHandler([&foreignMutex, &foreignAcks](sk::ICanController*, const sk::CanFrameTransmitEvent& ack) {
            const auto sent = vehicle_sil::can::decode_send_time(ack.userContext);
            const char* status = ack.status == sk::CanTransmitStatus::Transmitted  ? "transmitted"
                                 : ack.status == sk::CanTransmitStatus::Canceled   ? "canceled"
                                                                                   : "other";
            const std::lock_guard lock(foreignMutex);
            foreignAcks.push_back(std::string(status) + " " + (sent ? std::to_string(to_ns(*sent) % 1000) : std::string("?")));
        });
        raw->AddStateChangeHandler([&foreignStarted](sk::ICanController*, const sk::CanStateChangeEvent& e) {
            if (e.state == sk::CanControllerState::Started)
            {
                foreignStarted.store(true);
            }
        });
        if (foreign->start)
        {
            raw->Start();
        }
        sk::ICanController* second = nullptr;
        if (foreign->on_step_second)
        {
            second = foreignRuntime->participant().CreateCanController("CAN2", kNetwork);
            second->Start();
        }
        foreignRuntime->set_step_handler([raw, second, foreign, &localObject](SimTime now, SimDuration) {
            const auto sender = [now](sk::ICanController* from) {
                return [from, now](std::uint8_t first) {
                    sk::CanFrame f{};
                    f.canId = kForeignId;
                    f.flags = static_cast<sk::CanFrameFlagMask>(sk::CanFrameFlag::Ide);
                    const std::uint8_t data[1] = {first};
                    f.dlc = 1;
                    f.dataField = SilKit::Util::Span<const std::uint8_t>(data, 1);
                    from->SendFrame(f, vehicle_sil::can::encode_send_time(now + std::chrono::nanoseconds(first)));
                };
            };
            if (foreign->on_step)
            {
                foreign->on_step(*raw, now, sender(raw));
                if (second)
                {
                    foreign->on_step_second(*second, now, sender(second));
                }
                return;
            }
            if (now == at(30ms))
            {
                sk::CanFrame f{};
                f.canId = j1939_id(6, 0xFF40, 0x70);
                f.flags = static_cast<sk::CanFrameFlagMask>(sk::CanFrameFlag::Ide);
                const std::uint8_t data[1] = {0x44};
                f.dlc = 1;
                f.dataField = SilKit::Util::Span<const std::uint8_t>(data, 1);
                raw->SendFrame(f, &localObject);
                // A send time far ahead of the simulator (another clock): not trusted, so it is
                // not held until then.
                const std::uint8_t later[1] = {0x45};
                f.dataField = SilKit::Util::Span<const std::uint8_t>(later, 1);
                raw->SendFrame(f, vehicle_sil::can::encode_send_time(at(10s)));
            }
        });
        runtimes.push_back(foreignRuntime.get());
    }

    start_together(controller, runtimes, 60s);
    for (auto* runtime : runtimes)
    {
        const auto state = runtime->wait_until_finished(scaled_timeout(60s));
        if (!state || *state != orch::ParticipantState::Shutdown)
        {
            throw std::runtime_error(runtime->name() + " did not shut down: " + runtime->step_error());
        }
    }
    SilKitRun run;
    for (auto& node : nodes)
    {
        if (node.host->failed())
        {
            throw std::runtime_error("host failed: " + node.host->error());
        }
        run.ports.push_back(node.port->statistics());
        node.port->close();
        run.traces.push_back(node.node->trace());
    }
    run.started_controllers = startedControllers.load();
    {
        const std::lock_guard lock(foreignMutex);
        run.foreign_acks = foreignAcks;
    }
    if (foreign && foreign->start && !foreignStarted.load())
    {
        throw std::runtime_error("the simulator did not acknowledge the raw controller's Start()");
    }
    if (simulator)
    {
        {
            const std::lock_guard lock(warningsMutex);
            run.simulator_warnings = simulatorWarnings;
        }
        run.statistics = simulator->statistics(kNetwork);
        const auto unused = simulator->statistics("UnusedCAN");
        if (unused.controllers != 0 || unused.requests != 0)
        {
            throw std::runtime_error("traffic on the unused network");
        }
    }
    if (child)
    {
        const auto code = child->wait_for(scaled_timeout(30s));
        if (!code || *code != 0)
        {
            throw std::runtime_error("vehicle_sil_can_bus_simulator failed (exit " + (code ? std::to_string(*code) : std::string("none")) +
                                     "), log: " + logFile.string());
        }
    }
    return run;
}

void print_first_difference(const Traces& expected, const Traces& actual)
{
    for (std::size_t n = 0; n < expected.size() && n < actual.size(); ++n)
    {
        for (std::size_t i = 0; i < std::max(expected[n].size(), actual[n].size()); ++i)
        {
            const std::string e = i < expected[n].size() ? expected[n][i] : "<none>";
            const std::string a = i < actual[n].size() ? actual[n][i] : "<none>";
            if (e != a)
            {
                std::printf("  peer %zu line %zu: expected '%s', got '%s'\n", n, i, e.c_str(), a.c_str());
                return;
            }
        }
    }
}

bool contains(const std::vector<std::string>& trace, const std::string& line)
{
    return std::find(trace.begin(), trace.end(), line) != trace.end();
}

} // namespace

TEST_CASE(simulator_arbitrates_and_delays_frames_like_the_virtual_bus_with_its_step)
{
    const auto peers = scenario();
    const Traces reference = run_on_virtual_bus(peers, 1ms);
    // Arbitration at the 11 ms step boundary: B (priority 3), then A's FIFO (6, then 3); 524 us
    // each (29-bit, 8 bytes at 250 kbit/s).
    const auto& r1 = reference[2];
    CHECK(contains(r1, "11524000 rx " + std::to_string(j1939_id(3, 0xFF30, 0x90)) + " 3"));
    CHECK(contains(r1, "12048000 rx " + std::to_string(j1939_id(6, 0xFF10, 0x80)) + " 1"));
    CHECK(contains(r1, "12572000 rx " + std::to_string(j1939_id(3, 0xFF20, 0x80)) + " 2"));
    // The flood starts at 31 ms; B's frame of 40 ms joins at 41 ms and follows the frame on the
    // bus then (A's 19th, 40.956 .. 41.480 ms).
    CHECK(contains(r1, "42004000 rx " + std::to_string(j1939_id(3, 0xFF31, 0x90)) + " 4"));
    // The 11-bit frame wins against the 29-bit frame with the same base identifier: 1 byte,
    // 55 bits (220 us) and 75 bits (300 us).
    CHECK(contains(r1, "81220000 rx 256 8"));
    CHECK(contains(r1, "81520000 rx " + std::to_string(0x100u << 18) + " 9"));
    // A learns when its frames ended.
    CHECK(contains(reference[0], "12048000 done " + std::to_string(j1939_id(6, 0xFF10, 0x80)) + " sent 10000000"));
    CHECK(reference[2] == reference[3]);
    CHECK(reference[2] == reference[4]);
    CHECK_EQ(reference[2].size(), 2u + 1u + 40u + 1u + 2u);

    for (int run = 0; run < 2; ++run)
    {
        const auto silkit = run_on_silkit(peers, 1ms);
        if (silkit.traces != reference)
        {
            print_first_difference(reference, silkit.traces);
        }
        CHECK(silkit.traces == reference);
        CHECK_EQ(silkit.statistics.transmissions, 46u);
        CHECK_EQ(silkit.statistics.requests, 46u);
        CHECK_EQ(silkit.statistics.untagged_requests, 0u);
        CHECK_EQ(silkit.statistics.bitrate, kBitrate);
        CHECK_EQ(silkit.statistics.controllers, 5u);
        CHECK_EQ(silkit.ports[0].tx_transmitted, 43u);
        CHECK_EQ(silkit.ports[0].receive_overflows + silkit.ports[0].event_overflows, 0u);
    }
}

TEST_CASE(simulator_results_do_not_depend_on_the_other_participants_step_sizes)
{
    // The senders send at multiples of 10 ms with every step size, so the frame times stay.
    const Traces reference = run_on_silkit(scenario(), 1ms).traces;
    const auto other = run_on_silkit(scenario(5ms, 2ms, {10ms, 1ms, 13ms}), 1ms).traces;
    if (other != reference)
    {
        print_first_difference(reference, other);
    }
    CHECK(other == reference);
}

TEST_CASE(simulator_step_size_sets_the_earliest_start_deterministically)
{
    const auto peers = scenario();
    const Traces coarse = run_on_virtual_bus(peers, 4ms);
    CHECK(coarse != run_on_virtual_bus(peers, 1ms));
    // Frames sent at 10 ms start at the boundary 12 ms.
    CHECK(contains(coarse[2], "12524000 rx " + std::to_string(j1939_id(3, 0xFF30, 0x90)) + " 3"));
    const auto silkit = run_on_silkit(peers, 4ms);
    if (silkit.traces != coarse)
    {
        print_first_difference(coarse, silkit.traces);
    }
    CHECK(silkit.traces == coarse);
}

TEST_CASE(simulator_counts_a_sender_without_a_send_time_and_still_delivers_its_frame)
{
    const Foreign untagged;
    const auto run = run_on_silkit(scenario(), 1ms, Simulator::InProcess, &untagged);
    CHECK_EQ(run.statistics.untagged_requests, 2u);
    CHECK_EQ(run.statistics.transmissions, 48u);
    // Sent at 30 ms, placed at the simulator's last step (30 or 31 ms, depending on timing) and
    // transmitted after A's flood (lower identifiers): R1 receives each once, the one with the
    // far send time too.
    const auto count = [&run](const std::string& data) {
        return std::count_if(run.traces[2].begin(), run.traces[2].end(), [&](const std::string& line) {
            return line.ends_with(" rx " + std::to_string(j1939_id(6, 0xFF40, 0x70)) + " " + data);
        });
    };
    CHECK_EQ(count("68"), 1);
    CHECK_EQ(count("69"), 1);
}

TEST_CASE(simulator_refuses_frames_beyond_the_transmit_queue_with_an_acknowledgement)
{
    std::vector<TimedFrame> flood;
    for (std::uint8_t k = 0; k < 10; ++k)
    {
        flood.push_back({at(10ms), ext(j1939_id(6, 0xFF10, 0x80), 8, k)});
    }
    const std::vector<PeerSpec> peers{{"A", 10ms, flood}, {"R1", 1ms, {}}};
    CanBusSimulatorOptions options;
    options.transmit_queue_capacity = 4;
    const auto run = run_on_silkit(peers, 1ms, Simulator::InProcess, nullptr, options);
    CHECK_EQ(run.statistics.transmit_queue_full, 6u);
    CHECK_EQ(run.statistics.transmissions, 4u);
    CHECK_EQ(run.traces[1].size(), 4u);
    CHECK_EQ(run.ports[0].tx_queue_full, 6u);
    // The refusals come at the step that took the requests (11 ms), as port events.
    std::size_t failed = 0;
    for (const auto& line : run.traces[0])
    {
        if (line.rfind("11000000 failed " + std::to_string(j1939_id(6, 0xFF10, 0x80)) + " sent 10000000", 0) == 0)
        {
            ++failed;
        }
    }
    CHECK_EQ(failed, 6u);
}

TEST_CASE(port_transmit_queue_keeps_frames_in_the_stack_instead_of_losing_them)
{
    // The port allows as many unacknowledged frames as the simulator's queue holds: the frames
    // beyond it wait (QueueFull) and go out at later steps; none is refused by the simulator.
    std::vector<TimedFrame> flood;
    for (std::uint8_t k = 0; k < 10; ++k)
    {
        flood.push_back({at(10ms), ext(j1939_id(6, 0xFF10, 0x80), 8, k)});
    }
    const std::vector<PeerSpec> peers{{"A", 1ms, flood, std::nullopt, 4}, {"R1", 1ms, {}}};
    CanBusSimulatorOptions options;
    options.transmit_queue_capacity = 4;
    const auto run = run_on_silkit(peers, 1ms, Simulator::InProcess, nullptr, options);
    CHECK_EQ(run.statistics.transmit_queue_full, 0u);
    CHECK_EQ(run.statistics.transmissions, 10u);
    CHECK_EQ(run.ports[0].tx_queue_full, 0u);
    CHECK(run.ports[0].send_queue_full > 0u);
    CHECK_EQ(run.traces[1].size(), 10u);
    CHECK_EQ(run.ports[0].outstanding, 0u);
    for (std::size_t k = 0; k < run.traces[1].size(); ++k)
    {
        CHECK(run.traces[1][k].ends_with(" " + std::to_string(k))); // in send order
    }
}

TEST_CASE(simulator_uses_the_default_bitrate_until_a_controller_sets_one)
{
    std::vector<TimedFrame> one{{at(10ms), ext(j1939_id(6, 0xFF10, 0x80))}};
    const std::vector<PeerSpec> peers{{"A", 10ms, one}, {"R1", 1ms, {}}};
    CanBusSimulatorOptions options;
    options.default_bitrate = 125'000;
    const auto run = run_on_silkit(peers, 1ms, Simulator::InProcess, nullptr, options, 0);
    CHECK_EQ(run.statistics.bitrate, 125'000u);
    CHECK_EQ(run.statistics.bitrate_conflicts, 0u);
    // 131 bits at 8 us from the 11 ms boundary.
    CHECK(run.traces[1] == (std::vector<std::string>{"12048000 rx " + std::to_string(j1939_id(6, 0xFF10, 0x80)) + " 0"}));
}

TEST_CASE(simulator_learns_the_bit_rate_and_the_start_whichever_joins_first)
{
    // The ports set the bit rate and start their controllers from the CommunicationReady handler,
    // where a Coordinated participant knows every participant's services. So the simulator gets
    // both from a controller that was made before the simulator joined, too (calls made before
    // the controller has found the simulator are not sent again).
    std::vector<TimedFrame> one{{at(10ms), ext(j1939_id(6, 0xFF10, 0x80))}};
    const std::vector<PeerSpec> peers{{"A", 10ms, one}, {"R1", 1ms, {}}};
    CanBusSimulatorOptions options;
    options.default_bitrate = 125'000;
    for (const bool simulatorLast : {false, true})
    {
        const auto run = run_on_silkit(peers, 1ms, Simulator::InProcess, nullptr, options, 250'000, simulatorLast);
        CHECK_EQ(run.statistics.bitrate, 250'000u);
        CHECK_EQ(run.statistics.bitrate_conflicts, 0u);
        CHECK_EQ(run.started_controllers, 2u);
        // 131 bits at 4 us from the 11 ms boundary (the default bit rate would give 12048000).
        CHECK(run.traces[1] == (std::vector<std::string>{"11524000 rx " + std::to_string(j1939_id(6, 0xFF10, 0x80)) + " 0"}));
    }
}

TEST_CASE(simulator_keeps_the_first_bitrate_and_counts_a_conflicting_one)
{
    std::vector<TimedFrame> one{{at(10ms), ext(j1939_id(6, 0xFF10, 0x80))}};
    const std::vector<PeerSpec> peers{{"A", 10ms, one, 250'000}, {"R1", 1ms, {}, 500'000}};
    const auto run = run_on_silkit(peers, 1ms);
    CHECK_EQ(run.statistics.bitrate_conflicts, 1u);
    // Which controller set its rate first is not fixed; the frame takes that rate's time.
    CHECK(run.statistics.bitrate == 250'000u || run.statistics.bitrate == 500'000u);
    const std::string expected = std::to_string(run.statistics.bitrate == 250'000u ? 11'524'000 : 11'262'000) + " rx " +
                                 std::to_string(j1939_id(6, 0xFF10, 0x80)) + " 0";
    CHECK(run.traces[1] == (std::vector<std::string>{expected}));
}

/// What R1 received of the Foreign's frames: the first data bytes, in order.
std::vector<int> foreign_frames(const std::vector<std::string>& trace)
{
    std::vector<int> out;
    const std::string mark = " rx " + std::to_string(kForeignId) + " ";
    for (const auto& line : trace)
    {
        if (const auto at = line.find(mark); at != std::string::npos)
        {
            out.push_back(std::stoi(line.substr(at + mark.size())));
        }
    }
    return out;
}

TEST_CASE(simulator_drops_what_a_controller_sends_while_it_is_not_started)
{
    // As SIL Kit without a simulator: a controller that is stopped (or asleep) sends nothing and
    // gets no acknowledgement. What it sent before Stop() still goes out.
    Foreign f;
    f.on_step = [](sk::ICanController& c, SimTime now, const std::function<void(std::uint8_t)>& send) {
        if (now == at(20ms))
        {
            send(1);
        }
        if (now == at(30ms))
        {
            send(2); // while started: transmitted, although the Stop() follows at once
            c.Stop();
            send(3);
        }
        if (now == at(40ms))
        {
            send(4);
            c.Sleep();
            send(5);
        }
        if (now == at(50ms))
        {
            c.Start();
            send(6);
        }
    };
    const std::vector<PeerSpec> peers{{"R1", 1ms, {}}};
    const auto run = run_on_silkit(peers, 1ms, Simulator::InProcess, &f);
    CHECK(foreign_frames(run.traces[0]) == (std::vector<int>{1, 2, 6}));
    CHECK(run.foreign_acks == (std::vector<std::string>{"transmitted 1", "transmitted 2", "transmitted 6"}));
    CHECK_EQ(run.statistics.requests, 6u);
    CHECK_EQ(run.statistics.rejected_requests, 3u);
    CHECK_EQ(run.statistics.transmissions, 3u);
}

/// The simulator's warning of the first frame it drops of a controller that is not started,
/// with `<n>` in place of the controller's number (SIL Kit gives the numbers in the order the
/// simulator finds the controllers, which is not fixed). Empty: the two places of the number
/// in `warning` do not agree. `number`: the number.
std::string without_controller_number(const std::string& warning, std::string& number)
{
    const std::string mark = " of controller ";
    const auto at = warning.find(mark);
    if (at == std::string::npos)
    {
        return warning;
    }
    const auto end = warning.find_first_not_of("0123456789", at + mark.size());
    number = warning.substr(at + mark.size(), end - at - mark.size());
    std::string out = warning.substr(0, at) + mark + "<n>" + warning.substr(end);
    const std::string again = " Controller " + number + " ";
    const auto second = out.find(again);
    if (number.empty() || second == std::string::npos)
    {
        return {};
    }
    return out.replace(second, again.size(), " Controller <n> ");
}

TEST_CASE(simulator_warns_once_per_controller_that_sends_while_it_is_not_started)
{
    // The warning is the only sign of frames that vanish without an acknowledgement, so each
    // controller gets its own, with the mode the simulator heard; not one per frame, and not
    // again after a Start() and another mode.
    Foreign f;
    f.on_step = [](sk::ICanController& c, SimTime now, const std::function<void(std::uint8_t)>& send) {
        if (now == at(20ms))
        {
            c.Stop();
            send(1);
            send(2);
        }
        if (now == at(40ms))
        {
            c.Start();
            send(3);
            c.Sleep();
            send(4);
        }
    };
    f.on_step_second = [](sk::ICanController& c, SimTime now, const std::function<void(std::uint8_t)>& send) {
        if (now == at(30ms))
        {
            c.Reset();
            send(5);
            send(6);
        }
    };
    const std::vector<PeerSpec> peers{{"R1", 1ms, {}}};
    const auto run = run_on_silkit(peers, 1ms, Simulator::InProcess, &f);
    CHECK(foreign_frames(run.traces[0]) == (std::vector<int>{3}));
    CHECK_EQ(run.statistics.rejected_requests, 5u);
    CHECK_EQ(run.simulator_warnings.size(), 2u);
    if (run.simulator_warnings.size() == 2)
    {
        const auto expected = [](const char* mode) {
            return "network 'CAN1': dropped frame 0x18FF4070 of controller <n>, which is " + std::string(mode) +
                   "; the frame gets no acknowledgement. Call Start() on that controller before it sends. Controller <n> is "
                   "the number SIL Kit gave it: the simulator numbers the controllers it finds from 1, over all networks. "
                   "This is said once per controller; the later frames are only counted (rejected)";
        };
        std::string first;
        std::string second;
        CHECK(without_controller_number(run.simulator_warnings[0], first) == expected("stopped (Stop())"));
        CHECK(without_controller_number(run.simulator_warnings[1], second) == expected("reset (Reset()) and not started again"));
        CHECK(first != second);
    }
}

TEST_CASE(simulator_takes_a_controller_it_heard_no_mode_of_as_started)
{
    // A controller may have called Start() before it found the simulator; SIL Kit does not send
    // that again. So only a mode the simulator was told stops a controller's frames.
    Foreign f;
    f.start = false;
    f.on_step = [](sk::ICanController&, SimTime now, const std::function<void(std::uint8_t)>& send) {
        if (now == at(20ms))
        {
            send(1);
        }
    };
    const std::vector<PeerSpec> peers{{"R1", 1ms, {}}};
    const auto run = run_on_silkit(peers, 1ms, Simulator::InProcess, &f);
    CHECK(foreign_frames(run.traces[0]) == (std::vector<int>{1}));
    CHECK_EQ(run.statistics.rejected_requests, 0u);
}

TEST_CASE(simulator_delivers_frames_to_a_stopped_controller)
{
    // As SIL Kit without a simulator: reception does not depend on the receiver's mode.
    std::vector<TimedFrame> one{{at(40ms), ext(j1939_id(6, 0xFF10, 0x80))}};
    std::mutex mutex;
    std::vector<std::uint32_t> received;
    bool handler = false;
    Foreign f;
    f.on_step = [&](sk::ICanController& c, SimTime now, const std::function<void(std::uint8_t)>&) {
        if (!handler)
        {
            handler = true;
            c.AddFrameHandler([&](sk::ICanController*, const sk::CanFrameEvent& e) {
                const std::lock_guard lock(mutex);
                received.push_back(e.frame.canId);
            });
        }
        if (now == at(20ms))
        {
            c.Stop();
        }
    };
    const std::vector<PeerSpec> peers{{"A", 1ms, one}, {"R1", 1ms, {}}};
    const auto run = run_on_silkit(peers, 1ms, Simulator::InProcess, &f);
    CHECK_EQ(run.traces[1].size(), 1u);
    const std::lock_guard lock(mutex);
    CHECK(received == (std::vector<std::uint32_t>{j1939_id(6, 0xFF10, 0x80)}));
}

TEST_CASE(simulator_cancels_a_controllers_waiting_frames_on_reset)
{
    // Reset() asks to cancel the transmit requests and leaves the controller uninitialised: the
    // frames that have not been transmitted get a Canceled acknowledgement and never reach the
    // bus; the controller sends again after Start().
    Foreign f;
    f.on_step = [](sk::ICanController& c, SimTime now, const std::function<void(std::uint8_t)>& send) {
        if (now == at(20ms))
        {
            for (std::uint8_t k = 1; k <= 8; ++k)
            {
                send(k);
            }
        }
        if (now == at(22ms))
        {
            send(9); // not yet taken by the simulator when the Reset() arrives
            c.Reset();
            send(10); // not started any more
        }
        if (now == at(40ms))
        {
            c.Start();
            send(11);
        }
    };
    const std::vector<PeerSpec> peers{{"R1", 1ms, {}}};
    const auto run = run_on_silkit(peers, 1ms, Simulator::InProcess, &f);
    // A frame of 1 data byte takes 300 us at 250 kbit/s: four start in the step at 21 ms, three in
    // the step at 22 ms. The Reset() of F's step at 22 ms reaches the simulator before its step at
    // 22 ms or at 23 ms decides (the two steps at 22 ms run side by side), so 4 or 7 of the 8
    // frames were transmitted.
    const auto frames = foreign_frames(run.traces[0]);
    CHECK(frames == (std::vector<int>{1, 2, 3, 4, 11}) || frames == (std::vector<int>{1, 2, 3, 4, 5, 6, 7, 11}));
    const std::size_t before = frames.empty() ? 0 : frames.size() - 1; // transmitted before the Reset()
    CHECK_EQ(run.statistics.transmissions, before + 1);
    CHECK_EQ(run.statistics.requests, 11u);
    CHECK_EQ(run.statistics.rejected_requests, 10u - before);
    std::vector<std::string> expected;
    for (std::size_t k = 1; k <= before; ++k)
    {
        expected.push_back("transmitted " + std::to_string(k));
    }
    for (std::size_t k = before + 1; k <= 9; ++k)
    {
        expected.push_back("canceled " + std::to_string(k)); // 10 was never accepted: no acknowledgement
    }
    expected.push_back("transmitted 11");
    CHECK(run.foreign_acks == expected);
}

TEST_CASE(network_simulator_configuration_reads_the_vector_format)
{
    const auto yaml = vehicle_sil::can::parse_network_simulator_config(R"(---
"$schema": "./NetworkSimulatorConfiguration.schema.json"
SchemaVersion: 1
Description: Sample configuration for the network simulator
SimulationStepSizeNanoseconds: 500000
SimulatedNetworks:
- Name: CAN1
  Type: CAN
- Name: Ethernet1
  Type: Ethernet
- Name: LIN1
  Type: LIN
- Name: PowertrainCAN
  Type: CAN
Switches:
- Name: Switch1
  Ports:
  - Name: Port1
    VlanIds:
    - 1
    Network: ETH1_Link1
)");
    CHECK_EQ(yaml.schema_version, 1);
    CHECK(yaml.description == "Sample configuration for the network simulator");
    CHECK(yaml.step_size == 500us);
    CHECK_EQ(yaml.networks.size(), 4u);
    CHECK(yaml.can_networks() == (std::vector<std::string>{"CAN1", "PowertrainCAN"}));
    CHECK_EQ(yaml.warnings.size(), 3u); // Ethernet1, LIN1, Switches

    const auto json = vehicle_sil::can::parse_network_simulator_config(R"({
  "SchemaVersion": 1,
  "SimulatedNetworks": [ { "Name": "CAN1", "Type": "CAN" }, { "Name": "FlexRay1", "Type": "FlexRay" } ]
})");
    CHECK(json.step_size == 1ms); // the default
    CHECK(json.can_networks() == (std::vector<std::string>{"CAN1"}));
    CHECK_EQ(json.warnings.size(), 1u);

    // Keys of later versions are reported, not refused.
    const auto extra = vehicle_sil::can::parse_network_simulator_config("SimulatedNetworks:\n  - {Name: C, Type: CAN, Baud: 1}\nFuture: 1\n");
    CHECK_EQ(extra.warnings.size(), 2u);
}

TEST_CASE(network_simulator_configuration_refuses_what_it_cannot_use)
{
    const auto refuses = [](const std::string& text, const std::string& part) {
        return vehicle_sil::unit::throws_with<NetworkSimulatorConfigError>(
            [&] { static_cast<void>(vehicle_sil::can::parse_network_simulator_config(text, "test.yaml")); }, part);
    };
    CHECK(refuses("SchemaVersion: 2\nSimulatedNetworks: []\n", "SchemaVersion 2"));
    CHECK(refuses("SchemaVersion: 1\n", "SimulatedNetworks is missing"));
    CHECK(refuses("SimulatedNetworks: {Name: CAN1}\n", "must be a list"));
    CHECK(refuses("SimulatedNetworks:\n  - Type: CAN\n", "Name is missing"));
    CHECK(refuses("SimulatedNetworks:\n  - Name: CAN1\n", "Type is missing"));
    CHECK(refuses("SimulatedNetworks:\n  - {Name: CAN1, Type: CANFD}\n", "'CANFD'"));
    CHECK(refuses("SimulatedNetworks:\n  - {Name: A, Type: CAN}\n  - {Name: A, Type: LIN}\n", "listed twice"));
    CHECK(refuses("SimulationStepSizeNanoseconds: 0\nSimulatedNetworks: []\n", "must be > 0"));
    CHECK(refuses("SimulationStepSizeNanoseconds: 1ms\nSimulatedNetworks: []\n", "must be an integer"));
    CHECK(refuses("- a\n- b\n", "must be a mapping"));
    CHECK(refuses("", "empty"));
    CHECK(refuses("SchemaVersion: 4294967297\nSimulatedNetworks: []\n", "SchemaVersion 4294967297"));
    CHECK(refuses("SimulatedNetworks: []\nSimulatedNetworks:\n  - {Name: CAN1, Type: CAN}\n", "appears twice"));
    CHECK(refuses("SimulatedNetworks:\n  - {Name: CAN1, Type: CAN, Name: CAN2}\n", "appears twice"));
    CHECK(refuses("{\"SimulatedNetworks\": [", "test.yaml"));
    CHECK(vehicle_sil::unit::throws_with<NetworkSimulatorConfigError>(
        [] { static_cast<void>(vehicle_sil::can::load_network_simulator_config("/nonexistent/netsim.yaml")); }, "cannot open"));
}

TEST_CASE(send_time_tag_round_trips_and_ignores_other_pointers)
{
    const SimTime t = SimTime{} + 123456789ns;
    CHECK(vehicle_sil::can::decode_send_time(vehicle_sil::can::encode_send_time(t)) == t);
    CHECK(!vehicle_sil::can::decode_send_time(nullptr));
    int local = 0;
    CHECK(!vehicle_sil::can::decode_send_time(&local));
    void* const correlated = vehicle_sil::can::encode_correlated_send_time(t, 0x1234u);
    CHECK(vehicle_sil::can::decode_correlated_tag(correlated) == std::optional<std::uint16_t>(0x1234u));
    CHECK(vehicle_sil::can::decode_send_time(correlated, t) == t);
}

TEST_CASE(netsim_correlates_same_id_burst_queue_full_and_reset_cancellation_with_exact_tokens)
{
    // A SilKitCanPort + RawChannel sending a same-ID same-payload burst with distinct tokens
    // (including token == 0) into a CanBusSimulator with transmit_queue_capacity == 2:
    // frames 0 and 1 enter the bus queue (Transmitted later at tx.end), while frames 2 and 3 are
    // immediately refused with TransmitQueueFull at step T -> out-of-order completion/failure with
    // exact token correlation! Also tests forged high-residue 0xA6 context at t == 0 being counted
    // as untagged_requests (never unwrapped negative).
    vehicle_sil::unit::InProcessRegistry registry;
    constexpr const char* kNet = "CorrCAN";
    constexpr SimTime kStopTime = SimTime{} + 40ms;

    vehicle_sil::SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"Sim", "Sender", "Foreign"};
    controllerOptions.stop_at = kStopTime - SimTime{} + 20ms;
    SimulationController controller(controllerOptions);

    // Simulator participant with small per-controller transmit queue (2).
    ParticipantRuntime simRuntime(
        coordinated_options("Sim", registry.uri, VirtualTimeOptions{1ms, std::nullopt, kStopTime}));
    CanBusSimulatorOptions simOpts;
    simOpts.default_bitrate = 250'000;
    simOpts.transmit_queue_capacity = 2;
    CanBusSimulator simulator(simRuntime.participant(), {kNet}, simOpts);
    simRuntime.set_step_handler([&](SimTime now, SimDuration step) { simulator.step(now, step); });

    // Sender participant using SilKitCanPort + RawChannel (0xA6 correlated tokens).
    ParticipantRuntime senderRuntime(
        coordinated_options("Sender", registry.uri, VirtualTimeOptions{1ms, std::nullopt, kStopTime}));
    auto* senderCtrl = senderRuntime.participant().CreateCanController("CAN", kNet);
    SilKitCanPortOptions portOpts;
    portOpts.bitrate = 250'000;
    SilKitCanPort senderPort(*senderCtrl, portOpts);
    senderRuntime.add_communication_ready_handler([&] { senderPort.start(); });

    vehicle_sil::can::RawChannel raw;
    Host senderHost;
    senderHost.add(raw, senderPort);
    CHECK(raw.transmit_capabilities() == vehicle_sil::can::TransmitCapabilities::Correlated);

    std::vector<vehicle_sil::can::TransmitEvent> txEvents;
    raw.on_transmit([&](const vehicle_sil::can::TransmitEvent& ev) { txEvents.push_back(ev); });
    senderHost.set_application([&](SimTime now) {
        if (now == at(5ms))
        {
            // Identical ID, identical payload, identical send step, tokens {0, 10, 20, 30}.
            Frame identical;
            identical.id = 0x18FF5010u;
            identical.format = IdFormat::Extended;
            identical.length = 8;
            identical.data = {1, 2, 3, 4, 5, 6, 7, 8};
            CHECK(raw.send(identical, 0) == vehicle_sil::can::RawSendStatus::Ok);
            CHECK(raw.send(identical, 10) == vehicle_sil::can::RawSendStatus::Ok);
            CHECK(raw.send(identical, 20) == vehicle_sil::can::RawSendStatus::Ok);
            CHECK(raw.send(identical, 30) == vehicle_sil::can::RawSendStatus::Ok);
        }
        else if (now == at(20ms))
        {
            // Queue two frames then call Reset() so the simulator cancels them.
            Frame identical;
            identical.id = 0x18FF5010u;
            identical.format = IdFormat::Extended;
            identical.length = 8;
            identical.data = {1, 2, 3, 4, 5, 6, 7, 8};
            CHECK(raw.send(identical, 40) == vehicle_sil::can::RawSendStatus::Ok);
            CHECK(raw.send(identical, 50) == vehicle_sil::can::RawSendStatus::Ok);
        }
        else if (now == at(21ms))
        {
            senderCtrl->Reset();
        }
    });
    vehicle_sil::can::bind_host(senderRuntime, senderHost);

    // Foreign participant sending one legacy 0xA5 frame and one forged high-residue 0xA6 at t == 0.
    ParticipantRuntime foreignRuntime(
        coordinated_options("Foreign", registry.uri, VirtualTimeOptions{1ms, std::nullopt, kStopTime}));
    auto* foreignCtrl = foreignRuntime.participant().CreateCanController("CAN", kNet);
    foreignRuntime.add_communication_ready_handler([&] { foreignCtrl->Start(); });
    foreignRuntime.set_step_handler([&](SimTime now, SimDuration) {
        if (now == at(0ms))
        {
            sk::CanFrame f{};
            f.canId = 0x100;
            const std::uint8_t d[1] = {0x99};
            f.dlc = 1;
            f.dataField = SilKit::Util::Span<const std::uint8_t>(d, 1);
            // Forged high-residue 0xA6 (2^40 - 1 ns) at t == 0: must NOT unwrap negative; rejected by future_limit.
            const SimTime forgedTime =
                SimTime{} + std::chrono::nanoseconds(static_cast<std::int64_t>(vehicle_sil::can::kCorrelatedTimeMod - 1));
            foreignCtrl->SendFrame(f, vehicle_sil::can::encode_correlated_send_time(forgedTime, 1));
        }
        else if (now == at(10ms))
        {
            sk::CanFrame f{};
            f.canId = 0x101;
            const std::uint8_t d[1] = {0x55};
            f.dlc = 1;
            f.dataField = SilKit::Util::Span<const std::uint8_t>(d, 1);
            // Legacy 0xA5 alongside 0xA6 participant on same network.
            foreignCtrl->SendFrame(f, vehicle_sil::can::encode_send_time(now));
        }
    });

    std::vector<ParticipantRuntime*> runtimes{&simRuntime, &senderRuntime, &foreignRuntime};
    start_together(controller, runtimes, 60s);
    for (auto* rt : runtimes)
    {
        const auto state = rt->wait_until_finished(scaled_timeout(60s));
        CHECK(state && *state == orch::ParticipantState::Shutdown);
    }
    senderPort.close();
    CHECK(!senderHost.failed());

    // Verify burst at 5ms: tokens 20 & 30 failed with TransmitQueueFull at t=6ms BEFORE tokens 0 & 10
    // completed at t=6ms + 524us and t=6ms + 1048us!
    CHECK_EQ(txEvents.size(), 6u);
    CHECK(txEvents[0].kind == vehicle_sil::can::TransmitEvent::Kind::Failed);
    CHECK_EQ(txEvents[0].token, 20u);
    CHECK(txEvents[0].send_time == at(5ms));
    CHECK(txEvents[1].kind == vehicle_sil::can::TransmitEvent::Kind::Failed);
    CHECK_EQ(txEvents[1].token, 30u);
    CHECK(txEvents[1].send_time == at(5ms));
    CHECK(txEvents[2].kind == vehicle_sil::can::TransmitEvent::Kind::Completed);
    CHECK_EQ(txEvents[2].token, 0u);
    CHECK(txEvents[2].send_time == at(5ms));
    CHECK(txEvents[2].time > at(6ms));
    CHECK(txEvents[3].kind == vehicle_sil::can::TransmitEvent::Kind::Completed);
    CHECK_EQ(txEvents[3].token, 10u);
    CHECK(txEvents[3].send_time == at(5ms));
    CHECK(txEvents[3].time > txEvents[2].time);

    // Tokens 40 & 50 were sent at 20ms and cancelled/completed with exact tokens (conservation == 0).
    CHECK((txEvents[4].token == 40u && txEvents[5].token == 50u) ||
          (txEvents[4].token == 50u && txEvents[5].token == 40u));
    CHECK_EQ(raw.statistics().outstanding, 0u);
    CHECK_EQ(senderPort.statistics().outstanding, 0u);
    CHECK_EQ(senderPort.statistics().tx_queue_full, 2u);

    const auto simStats = simulator.statistics(kNet);
    CHECK_EQ(simStats.transmit_queue_full, 2u);
    // Forged high-residue 0xA6 at t == 0 was rejected by future_limit and counted as untagged;
    // the legacy 0xA5 frame at 10ms and all 6 correlated 0xA6 frames were properly tagged.
    CHECK_EQ(simStats.untagged_requests, 1u);
}

TEST_CASE(netsim_rejects_backward_unwrapped_0xA6_at_600s_and_round_trips_past_40bit_wrap)
{
    vehicle_sil::unit::InProcessRegistry registry;
    constexpr const char* kNet = "WrapCAN";
    constexpr SimTime kStopTime = SimTime{} + 1200s;

    vehicle_sil::SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"Sim", "Sender", "Foreign"};
    controllerOptions.stop_at = kStopTime - SimTime{} + 20s;
    SimulationController controller(controllerOptions);

    ParticipantRuntime simRuntime(
        coordinated_options("Sim", registry.uri, VirtualTimeOptions{10s, std::nullopt, kStopTime}));
    CanBusSimulatorOptions simOpts;
    simOpts.default_bitrate = 250'000;
    simOpts.future_limit = 1s; // effective symmetric window limit = max(1s, 10s) = 10s
    CanBusSimulator simulator(simRuntime.participant(), {kNet}, simOpts);
    simRuntime.set_step_handler([&](SimTime now, SimDuration step) { simulator.step(now, step); });

    ParticipantRuntime senderRuntime(
        coordinated_options("Sender", registry.uri, VirtualTimeOptions{10s, std::nullopt, kStopTime}));
    auto* senderCtrl = senderRuntime.participant().CreateCanController("CAN", kNet);
    SilKitCanPortOptions portOpts;
    portOpts.bitrate = 250'000;
    SilKitCanPort senderPort(*senderCtrl, portOpts);
    senderRuntime.add_communication_ready_handler([&] { senderPort.start(); });

    vehicle_sil::can::RawChannel raw;
    Host senderHost;
    senderHost.add(raw, senderPort);

    std::vector<vehicle_sil::can::TransmitEvent> txEvents;
    raw.on_transmit([&](const vehicle_sil::can::TransmitEvent& ev) { txEvents.push_back(ev); });
    senderHost.set_application([&](SimTime now) {
        Frame f;
        f.id = 0x18FF6010u;
        f.format = IdFormat::Extended;
        f.length = 8;
        f.data = {9, 8, 7, 6, 5, 4, 3, 2};
        if (now == SimTime{} + 600s)
        {
            CHECK(raw.send(f, 0) == vehicle_sil::can::RawSendStatus::Ok);
        }
        else if (now == SimTime{} + 1090s)
        {
            // Just before 2^40 ns (~1099.51 s).
            CHECK(raw.send(f, 101) == vehicle_sil::can::RawSendStatus::Ok);
        }
        else if (now == SimTime{} + 1100s)
        {
            // Just after 2^40 ns wraparound!
            CHECK(raw.send(f, 202) == vehicle_sil::can::RawSendStatus::Ok);
        }
        else if (now == SimTime{} + 1150s)
        {
            CHECK(raw.send(f, 303) == vehicle_sil::can::RawSendStatus::Ok);
        }
    });
    vehicle_sil::can::bind_host(senderRuntime, senderHost);

    ParticipantRuntime foreignRuntime(
        coordinated_options("Foreign", registry.uri, VirtualTimeOptions{10s, std::nullopt, kStopTime}));
    auto* foreignCtrl = foreignRuntime.participant().CreateCanController("CAN", kNet);
    foreignRuntime.add_communication_ready_handler([&] { foreignCtrl->Start(); });
    foreignRuntime.set_step_handler([&](SimTime now, SimDuration) {
        if (now == SimTime{} + 600s)
        {
            sk::CanFrame f{};
            f.canId = 0x200;
            const std::uint8_t d[1] = {0x42};
            f.dlc = 1;
            f.dataField = SilKit::Util::Span<const std::uint8_t>(d, 1);
            // Forged 0xA6 residue that unwraps backward to 500s (< last - 10s): must be rejected as untagged.
            foreignCtrl->SendFrame(f, vehicle_sil::can::encode_correlated_send_time(SimTime{} + 500s, 1));
            // Forged 0xA6 residue that unwraps forward to 700s (> last + 10s): must be rejected as untagged.
            foreignCtrl->SendFrame(f, vehicle_sil::can::encode_correlated_send_time(SimTime{} + 700s, 2));
            // Legacy 0xA5 at 500s (in the past, <= last + limit): remains accepted by legacy 0xA5 rules.
            foreignCtrl->SendFrame(f, vehicle_sil::can::encode_send_time(SimTime{} + 500s));
        }
    });

    std::vector<ParticipantRuntime*> runtimes{&simRuntime, &senderRuntime, &foreignRuntime};
    start_together(controller, runtimes, 60s);
    for (auto* rt : runtimes)
    {
        const auto state = rt->wait_until_finished(scaled_timeout(60s));
        CHECK(state && *state == orch::ParticipantState::Shutdown);
    }
    senderPort.close();
    CHECK(!senderHost.failed());

    CHECK_EQ(txEvents.size(), 4u);
    const std::array<std::pair<std::uint64_t, SimTime>, 4> expected{{
        {0u, SimTime{} + 600s},
        {101u, SimTime{} + 1090s},
        {202u, SimTime{} + 1100s},
        {303u, SimTime{} + 1150s},
    }};
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        CHECK(txEvents[i].kind == vehicle_sil::can::TransmitEvent::Kind::Completed);
        CHECK_EQ(txEvents[i].token, expected[i].first);
        CHECK(txEvents[i].send_time == expected[i].second);
        CHECK(txEvents[i].time > expected[i].second);
    }
    CHECK_EQ(raw.statistics().outstanding, 0u);
    CHECK_EQ(senderPort.statistics().outstanding, 0u);
    CHECK_EQ(senderPort.statistics().tx_uncorrelated, 0u);

    const auto simStats = simulator.statistics(kNet);
    CHECK_EQ(simStats.transmissions, 7u);
    CHECK_EQ(simStats.untagged_requests, 2u); // ONLY the two out-of-window 0xA6 frames
}

TEST_CASE(netsim_standalone_executable_correlates_raw_channel_tokens)
{
    vehicle_sil::unit::InProcessRegistry registry;
    constexpr const char* kNet = "CAN1";
    constexpr SimTime kStopTime = SimTime{} + 30ms;

    const auto config = scratch_dir() / "netsim-corr.yaml";
    const auto logFile = scratch_dir() / "netsim-corr.log";
    std::ofstream(config) << "SchemaVersion: 1\n"
                             "Description: vehicle-sil token correlation test\n"
                             "SimulationStepSizeNanoseconds: 1000000\n"
                             "SimulatedNetworks:\n"
                             "  - Name: CAN1\n"
                             "    Type: CAN\n";
    vehicle_sil::process::ChildProcess child(
        VEHICLE_SIL_CAN_BUS_SIMULATOR,
        std::vector<std::string>{"--connect-uri", registry.uri, "--log", "info", "--start-timeout-ms", "60000",
                                 "--default-bitrate", "250000", vehicle_sil::process::utf8_from_path(config)},
        logFile);
    const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(30s);
    while (true)
    {
        std::ifstream log(logFile);
        const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        if (text.find("waiting for the simulation to start") != std::string::npos)
        {
            break;
        }
        if (child.poll() || std::chrono::steady_clock::now() > deadline)
        {
            throw std::runtime_error("vehicle_sil_can_bus_simulator did not come up, log: " + logFile.string());
        }
        std::this_thread::sleep_for(10ms);
    }

    vehicle_sil::SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"NetworkSimulator", "Sender", "Receiver"};
    controllerOptions.stop_at = kStopTime - SimTime{} + 20ms;
    SimulationController controller(controllerOptions);

    ParticipantRuntime senderRuntime(
        coordinated_options("Sender", registry.uri, VirtualTimeOptions{1ms, std::nullopt, kStopTime}));
    auto* senderCtrl = senderRuntime.participant().CreateCanController("CAN", kNet);
    SilKitCanPortOptions portOpts;
    portOpts.bitrate = 250'000;
    SilKitCanPort senderPort(*senderCtrl, portOpts);
    senderRuntime.add_communication_ready_handler([&] { senderPort.start(); });

    vehicle_sil::can::RawChannel senderRaw;
    Host senderHost;
    senderHost.add(senderRaw, senderPort);
    std::vector<vehicle_sil::can::TransmitEvent> txEvents;
    senderRaw.on_transmit([&](const vehicle_sil::can::TransmitEvent& ev) { txEvents.push_back(ev); });
    senderHost.set_application([&](SimTime now) {
        if (now == at(5ms))
        {
            Frame identical;
            identical.id = 0x18FF7020u;
            identical.format = IdFormat::Extended;
            identical.length = 8;
            identical.data = {0xAA, 0xBB, 0xCC, 0xDD, 1, 2, 3, 4};
            CHECK(senderRaw.send(identical, 0) == vehicle_sil::can::RawSendStatus::Ok);
            CHECK(senderRaw.send(identical, 11) == vehicle_sil::can::RawSendStatus::Ok);
            CHECK(senderRaw.send(identical, 22) == vehicle_sil::can::RawSendStatus::Ok);
            CHECK(senderRaw.send(identical, 33) == vehicle_sil::can::RawSendStatus::Ok);
        }
    });
    vehicle_sil::can::bind_host(senderRuntime, senderHost);

    ParticipantRuntime receiverRuntime(
        coordinated_options("Receiver", registry.uri, VirtualTimeOptions{1ms, std::nullopt, kStopTime}));
    auto* receiverCtrl = receiverRuntime.participant().CreateCanController("CAN", kNet);
    SilKitCanPort receiverPort(*receiverCtrl, portOpts);
    receiverRuntime.add_communication_ready_handler([&] { receiverPort.start(); });
    vehicle_sil::can::RawChannel receiverRaw;
    Host receiverHost;
    receiverHost.add(receiverRaw, receiverPort);
    std::vector<TimedFrame> received;
    receiverRaw.on_frame([&](const TimedFrame& f) { received.push_back(f); });
    vehicle_sil::can::bind_host(receiverRuntime, receiverHost);

    std::vector<ParticipantRuntime*> runtimes{&senderRuntime, &receiverRuntime};
    start_together(controller, runtimes, 60s);
    for (auto* rt : runtimes)
    {
        const auto state = rt->wait_until_finished(scaled_timeout(60s));
        CHECK(state && *state == orch::ParticipantState::Shutdown);
    }
    senderPort.close();
    receiverPort.close();
    const auto code = child.wait_for(scaled_timeout(30s));
    CHECK(code && *code == 0);
    CHECK(!senderHost.failed());
    CHECK(!receiverHost.failed());

    CHECK_EQ(received.size(), 4u);
    CHECK_EQ(txEvents.size(), 4u);
    const std::array<std::uint64_t, 4> expectedTokens{0u, 11u, 22u, 33u};
    for (std::size_t i = 0; i < 4; ++i)
    {
        CHECK(txEvents[i].kind == vehicle_sil::can::TransmitEvent::Kind::Completed);
        CHECK_EQ(txEvents[i].token, expectedTokens[i]);
        CHECK_EQ(txEvents[i].id, 0x18FF7020u);
        CHECK(txEvents[i].send_time == at(5ms));
        CHECK(txEvents[i].time == received[i].time);
        CHECK(txEvents[i].time > at(6ms));
    }
    CHECK_EQ(senderRaw.statistics().outstanding, 0u);
    CHECK_EQ(senderPort.statistics().outstanding, 0u);
    CHECK_EQ(senderPort.statistics().tx_uncorrelated, 0u);
}

TEST_CASE(simulator_executable_gives_the_same_frame_times)
{
    const auto peers = scenario();
    const Traces reference = run_on_virtual_bus(peers, 1ms);
    const auto silkit = run_on_silkit(peers, 1ms, Simulator::Executable);
    if (silkit.traces != reference)
    {
        print_first_difference(reference, silkit.traces);
    }
    CHECK(silkit.traces == reference);
}
