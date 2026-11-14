// SPDX-License-Identifier: MIT
//
// The J1939 stack on real SIL Kit participants (in-process registry, virtual time): a
// scenario with single-frame broadcasts, destination-specific frames, Requests, responses,
// acknowledgements and NACKs gives the same traces on SIL Kit as on the virtual bus, for
// several step sizes, and repeats identically.
#include "j1939_transport_util.hpp"
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/can/real_time_driver.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"
#include "vehicle_sil/can_netsim/can_bus_simulator.hpp"
#include "vehicle_sil/can_silkit/silkit_can_port.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <silkit/services/can/all.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
namespace orch = SilKit::Services::Orchestration;
using vehicle_sil::ParticipantRuntime;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::SimulationController;
using vehicle_sil::SimulationControllerOptions;
using vehicle_sil::to_ns;
using vehicle_sil::VirtualTimeOptions;
using vehicle_sil::can::Host;
using vehicle_sil::can::SilKitCanPort;
using vehicle_sil::j1939::AckCode;
using vehicle_sil::j1939::Address;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::kGlobalAddress;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::Pgn;
using vehicle_sil::j1939::RequestContext;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::start_together;

namespace {

constexpr auto kEnd = SimTime{} + 150ms;
constexpr const char* kNetwork = "CAN1";
constexpr std::size_t kNodes = 3;

constexpr Pgn kStatus{0xFF30};    ///< PDU2 broadcast of every node, also answered on request
constexpr Pgn kCommand{0xEF00};   ///< PDU1: requests for it are acknowledged with AccessDenied
constexpr Pgn kReply{0xED00};     ///< PDU1: sent to the node whose status arrived
constexpr Pgn kUnsupported{0xFEF1};

Address address_of(std::size_t i) { return static_cast<Address>(0x10 * (i + 1)); }

SimDuration period_of(std::size_t i)
{
    const SimDuration periods[] = {7ms, 11ms, 13ms};
    return periods[i % 3];
}

/// The J1939 network of one participant and its application.
class Ecu
{
public:
    Ecu() = default;
    explicit Ecu(vehicle_sil::j1939::NetworkOptions options) : network(std::move(options)) {}
    virtual ~Ecu() = default;
    /// The host's application stage.
    virtual void run(SimTime now) = 0;
    [[nodiscard]] virtual std::vector<std::string> trace() const = 0;

    Network network;
};

using EcuFactory = std::function<std::unique_ptr<Ecu>(std::size_t index)>;

/// One J1939 network with one node and the scenario's application; records what it receives.
class Scenario final : public Ecu
{
public:
    explicit Scenario(std::size_t index) : index_(index), node_(network.add_node({address_of(index)}))
    {
        node_.on_any_message([this](const Message& m) { record(m); });
        node_.on_message(kStatus, [this](const Message& m) {
            // React to the status of the next node with a destination-specific reply.
            if (m.source == address_of((index_ + 1) % kNodes))
            {
                const std::array<std::uint8_t, 2> data{static_cast<std::uint8_t>(index_), m.data.empty() ? std::uint8_t{0} : m.data[0]};
                check(node_.send(kReply, data, m.source, 3));
            }
        });
        node_.on_request(kStatus, [this](RequestContext& c) {
            const std::array<std::uint8_t, 3> data{0xAA, static_cast<std::uint8_t>(index_), counter_};
            check(c.respond(data));
        });
        node_.on_request(kCommand, [](RequestContext& c) { c.acknowledge(AckCode::AccessDenied); });
        if (index_ == 0)
        {
            // A second node on the first network: it talks to its neighbour through the local
            // loopback (and is seen on the bus by the others).
            Node& local = network.add_node({static_cast<Address>(address_of(index_) + 1)});
            local.on_any_message([this](const Message& m) { record(m, "L "); });
            local.on_message(kStatus, [this, &local](const Message& m) {
                if (m.source == address_of(index_))
                {
                    check(local.request(kCommand, m.source));
                }
            });
        }
    }

    void run(SimTime now) override
    {
        while (next_send_ <= now)
        {
            next_send_ += period_of(index_);
            ++counter_;
            const std::array<std::uint8_t, 8> status{counter_, static_cast<std::uint8_t>(index_), 1, 2, 3, 4, 5, 6};
            check(node_.send(kStatus, status, kGlobalAddress, static_cast<std::uint8_t>(3 + index_)));
            const Address next = address_of((index_ + 1) % kNodes);
            switch (counter_ % 4)
            {
            case 0: check(node_.request(kStatus, next)); break;
            case 1: check(node_.request(kCommand, next)); break;
            case 2: check(node_.request(kUnsupported, next)); break;
            default: check(node_.request(kStatus)); break; // to all
            }
        }
    }

    [[nodiscard]] std::vector<std::string> trace() const override { return trace_; }

private:
    static void check(SendStatus status)
    {
        if (status != SendStatus::Ok)
        {
            throw std::runtime_error("send failed in the scenario");
        }
    }

    void record(const Message& m, const char* prefix = "")
    {
        char text[96];
        std::snprintf(text, sizeof(text), "%lld P%u %05X %02X->%02X", static_cast<long long>(to_ns(m.time)),
                      static_cast<unsigned>(m.priority), static_cast<unsigned>(m.pgn.value()), static_cast<unsigned>(m.source),
                      static_cast<unsigned>(m.destination));
        std::string line = std::string(prefix) + text;
        for (const auto b : m.data)
        {
            std::snprintf(text, sizeof(text), " %02X", static_cast<unsigned>(b));
            line += text;
        }
        trace_.push_back(line);
    }

    std::size_t index_;
    Node& node_;
    SimTime next_send_{};
    std::uint8_t counter_ = 0;
    std::vector<std::string> trace_;
};

using Traces = std::vector<std::vector<std::string>>;

std::unique_ptr<Ecu> make_scenario(std::size_t index) { return std::make_unique<Scenario>(index); }

/// Address claiming: Ecu1 and Ecu2 claim 0x80 (Ecu2 has the lower NAME and wins), Ecu3 claims
/// 0x81 and asks every node for its claim at 300 ms. Every node sends a broadcast every 50 ms,
/// held during its wait and refused once it lost its address.
class ClaimScenario final : public Ecu
{
public:
    explicit ClaimScenario(std::size_t index)
      : node_(network.add_node(NodeOptions{index == 2 ? Address{0x81} : Address{0x80}, Name{kNames[index % 3]}}))
    {
        node_.on_any_message([this](const Message& m) { record(m); });
    }

    void run(SimTime now) override
    {
        if (to_ns(now) % 50000000 == 0)
        {
            const std::array<std::uint8_t, 2> data{static_cast<std::uint8_t>(to_ns(now) / 50000000), node_.address()};
            const SendStatus status = node_.send(kStatus, data);
            if (status != SendStatus::Ok && status != SendStatus::NoAddress)
            {
                throw std::runtime_error("send failed in the claim scenario");
            }
        }
        if (now == SimTime{} + 300ms && node_.address() == 0x81)
        {
            if (node_.request(vehicle_sil::j1939::kPgnAddressClaimed) != SendStatus::Ok)
            {
                throw std::runtime_error("request failed in the claim scenario");
            }
        }
    }

    [[nodiscard]] std::vector<std::string> trace() const override
    {
        auto out = trace_;
        out.push_back("state " + std::to_string(static_cast<int>(node_.address_state())) + " address " +
                      std::to_string(node_.address()));
        return out;
    }

private:
    static constexpr std::uint64_t kNames[3] = {0x20008200FE001236u, 0x20008200FE001234u, 0x20008200FE001235u};

    void record(const Message& m)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%lld %05X %02X->%02X", static_cast<long long>(to_ns(m.time)),
                      static_cast<unsigned>(m.pgn.value()), static_cast<unsigned>(m.source), static_cast<unsigned>(m.destination));
        std::string line = text;
        for (const auto b : m.data)
        {
            std::snprintf(text, sizeof(text), " %02X", static_cast<unsigned>(b));
            line += text;
        }
        trace_.push_back(line);
    }

    Node& node_;
    std::vector<std::string> trace_;
};

/// Address management: Ecu1 has two nodes, an arbitrary address capable one claiming 0x80 and a
/// single-address one at 0x20. Ecu2 claims 0x80 with a lower NAME (also arbitrary address
/// capable), Ecu3 claims 0x81 (not arbitrary address capable, so lower than both). Ecu1's first
/// node loses 0x80, moves to 0x81, loses again and settles at 0x82. Ecu3 also has a node without
/// a NAME at 0x20 that sends once (an address violation for Ecu1's second node, which claims
/// again), and asks every node for its claim at 400 ms. Every named node broadcasts every 50 ms.
class ManagementScenario final : public Ecu
{
public:
    explicit ManagementScenario(std::size_t index) : index_(index)
    {
        if (index == 0)
        {
            add(NodeOptions{0x80, Name{kAacB}}, "a");
            add(NodeOptions{0x20, Name{0x20008200FE001240u}}, "b");
        }
        else if (index == 1)
        {
            add(NodeOptions{0x80, Name{kAacA}}, "a");
        }
        else
        {
            add(NodeOptions{0x81, Name{0x20008200FE001235u}}, "a");
            add(NodeOptions{0x20, std::nullopt}, "f");
        }
    }

    void run(SimTime now) override
    {
        // Every 50 ms: in the first step at or after each multiple of 50 ms.
        const bool due = now >= next_status_;
        while (next_status_ <= now)
        {
            next_status_ += 50ms;
        }
        for (Node* node : nodes_)
        {
            if (node->name() && due)
            {
                const std::array<std::uint8_t, 2> data{static_cast<std::uint8_t>(to_ns(now) / 50000000), node->address()};
                const SendStatus status = node->send(kStatus, data);
                if (status != SendStatus::Ok && status != SendStatus::NoAddress)
                {
                    throw std::runtime_error("send failed in the management scenario");
                }
            }
        }
        if (index_ == 2 && !violated_ && now >= SimTime{} + 105ms)
        {
            violated_ = true;
            const std::array<std::uint8_t, 1> data{0x55};
            if (nodes_[1]->send(kStatus, data) != SendStatus::Ok)
            {
                throw std::runtime_error("send failed in the management scenario");
            }
        }
        if (index_ == 2 && !requested_ && now >= SimTime{} + 400ms)
        {
            requested_ = true;
            if (nodes_[0]->request(vehicle_sil::j1939::kPgnAddressClaimed) != SendStatus::Ok)
            {
                throw std::runtime_error("request failed in the management scenario");
            }
        }
    }

    [[nodiscard]] std::vector<std::string> trace() const override
    {
        auto out = trace_;
        for (std::size_t i = 0; i < nodes_.size(); ++i)
        {
            out.push_back("state " + tags_[i] + " " + std::to_string(static_cast<int>(nodes_[i]->address_state())) +
                          " address " + std::to_string(nodes_[i]->address()));
        }
        for (unsigned a = 0; a < 254; ++a)
        {
            if (const auto name = network.address_table().name_at(static_cast<Address>(a)))
            {
                char text[64];
                std::snprintf(text, sizeof(text), "table %02X %016llX", a, static_cast<unsigned long long>(name->value()));
                out.push_back(text);
            }
        }
        out.push_back("violations " + std::to_string(network.statistics().address_violations));
        return out;
    }

    static constexpr std::uint64_t kAacA = 0xA0008200FE001234u;
    static constexpr std::uint64_t kAacB = 0xA0008200FE001236u;

private:
    void add(const NodeOptions& options, std::string tag)
    {
        Node& node = network.add_node(options);
        node.on_any_message([this, tag](const Message& m) { record(m, tag); });
        node.on_address_change([this, tag](const vehicle_sil::j1939::AddressChange& c) {
            trace_.push_back(std::to_string(to_ns(c.time)) + " change [" + tag + "] " + std::to_string(c.previous_address) +
                             " -> " + std::to_string(c.address) + " state " + std::to_string(static_cast<int>(c.state)));
        });
        nodes_.push_back(&node);
        tags_.push_back(std::move(tag));
    }

    void record(const Message& m, const std::string& tag)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%lld %05X %02X->%02X", static_cast<long long>(to_ns(m.time)),
                      static_cast<unsigned>(m.pgn.value()), static_cast<unsigned>(m.source), static_cast<unsigned>(m.destination));
        std::string line = text;
        for (const auto b : m.data)
        {
            std::snprintf(text, sizeof(text), " %02X", static_cast<unsigned>(b));
            line += text;
        }
        trace_.push_back(line + " [" + tag + "]");
    }

    std::size_t index_;
    std::vector<Node*> nodes_;
    std::vector<std::string> tags_;
    SimTime next_status_{};
    bool violated_ = false;
    bool requested_ = false;
    std::vector<std::string> trace_;
};

/// The transport protocol: every 300 ms Ecu1 broadcasts 30 bytes and sends 100 bytes to Ecu2;
/// Ecu3 sends 60 bytes to Ecu2 at the same time (Ecu2 takes one connection at a time, so one of
/// them is refused) and asks Ecu1 for a long group by a destination-specific and a global
/// request (answered by RTS/CTS and by BAM). Ecu1's second node sends 20 bytes to Ecu3 and
/// receives Ecu1's broadcasts through the local loopback.
class TransportScenario final : public Ecu
{
public:
    explicit TransportScenario(std::size_t index) : Ecu(options_for(index)), index_(index) { setup(); }

    static std::unique_ptr<Ecu> make(std::size_t index) { return std::make_unique<TransportScenario>(index); }

    void run(SimTime now) override
    {
        // Rounds at 0, 300, ... 1200 ms (the first step at or after each).
        if (now < next_round_ || next_round_ >= SimTime{} + 1500ms)
        {
            return;
        }
        const auto round = static_cast<std::uint8_t>((next_round_ - SimTime{}) / 300ms);
        next_round_ += 300ms;
        if (index_ == 0)
        {
            send(*nodes_[0], Pgn{0xFF31}, 30, round, kGlobalAddress);
            send(*nodes_[0], Pgn{0xEF00}, 100, round, 0x20);
            send(*nodes_[1], Pgn{0xEF00}, 20, round, 0x30);
        }
        else if (index_ == 2)
        {
            send(*nodes_[0], Pgn{0xEF00}, 60, round, 0x20);
            check_ok(nodes_[0]->request(Pgn{0xFF32}, 0x10));
            check_ok(nodes_[0]->request(Pgn{0xFF33}));
        }
    }

    [[nodiscard]] std::vector<std::string> trace() const override { return trace_; }

private:
    static vehicle_sil::j1939::NetworkOptions options_for(std::size_t index)
    {
        vehicle_sil::j1939::NetworkOptions options;
        if (index == 1)
        {
            options.rts_receive_sessions = 1; // Ecu2 takes one connection at a time
        }
        return options;
    }

    void setup()
    {
        nodes_.push_back(&network.add_node({address_of(index_)}));
        if (index_ == 0)
        {
            nodes_.push_back(&network.add_node({0x11}));
            nodes_[0]->on_request(Pgn{0xFF32}, [](RequestContext& c) { check_ok(c.respond(bytes(40, 7))); });
            nodes_[0]->on_request(Pgn{0xFF33}, [](RequestContext& c) { check_ok(c.respond(bytes(25, 8))); });
        }
        for (std::size_t n = 0; n < nodes_.size(); ++n)
        {
            const std::string tag = n == 0 ? "" : "L ";
            nodes_[n]->on_any_message([this, tag](const Message& m) { record(m, tag); });
            nodes_[n]->on_transfer_end([this, tag](const vehicle_sil::j1939::TransferEnd& e) {
                char text[96];
                std::snprintf(text, sizeof(text), "%sEND %lld %05X ->%02X %zu r%d a%d", tag.c_str(),
                              static_cast<long long>(to_ns(e.time)), static_cast<unsigned>(e.pgn.value()),
                              static_cast<unsigned>(e.destination), e.length, static_cast<int>(e.result),
                              static_cast<int>(e.reason));
                trace_.push_back(text);
            });
        }
    }

    static std::vector<std::uint8_t> bytes(std::size_t size, std::uint8_t seed)
    {
        std::vector<std::uint8_t> out(size);
        for (std::size_t i = 0; i < size; ++i)
        {
            out[i] = static_cast<std::uint8_t>(seed + i * 3);
        }
        return out;
    }

    static void check_ok(SendStatus status)
    {
        if (status != SendStatus::Ok)
        {
            throw std::runtime_error("send failed in the transport scenario: status " +
                                     std::to_string(static_cast<int>(status)));
        }
    }

    void send(Node& node, Pgn pgn, std::size_t size, std::uint8_t round, Address to)
    {
        const auto data = bytes(size, round);
        const SendStatus status = node.send(pgn, data, to);
        if (status != SendStatus::Ok)
        {
            // Busy: the node's previous transfer (or a long response) is still going.
            trace_.push_back("SEND " + std::to_string(node.address()) + " " + std::to_string(pgn.value()) + " status " +
                             std::to_string(static_cast<int>(status)));
        }
    }

    void record(const Message& m, const std::string& tag)
    {
        char text[96];
        std::snprintf(text, sizeof(text), "%s%lld P%u %05X %02X->%02X n%zu", tag.c_str(), static_cast<long long>(to_ns(m.time)),
                      static_cast<unsigned>(m.priority), static_cast<unsigned>(m.pgn.value()),
                      static_cast<unsigned>(m.source), static_cast<unsigned>(m.destination), m.data.size());
        std::string line = text;
        unsigned sum = 0;
        for (const auto b : m.data)
        {
            sum = sum * 31u + b;
        }
        std::snprintf(text, sizeof(text), " #%08X", sum);
        trace_.push_back(line + text);
    }

    std::size_t index_;
    SimTime next_round_{};
    std::vector<Node*> nodes_;
    std::vector<std::string> trace_;
};

void check_network(const Network& network)
{
    const auto& s = network.statistics();
    if (network.failed() || s.receive_overflows != 0 || s.send_failures != 0 || s.send_queue_full != 0 ||
        s.control_frames_dropped != 0 || s.frames_ignored != 0)
    {
        throw std::runtime_error("unexpected network state: " + network.error());
    }
}

/// The bus: SIL Kit's trivial simulation (on the virtual bus: no timing), or the simple CAN
/// network simulator at 250 kbit/s with a 1 ms step (on the virtual bus: the same timing).
enum class Bus
{
    Trivial,
    Simulator,
};
constexpr std::uint32_t kSimulatorBitrate = 250'000;
constexpr SimDuration kSimulatorStep = 1ms;

Traces run_on_virtual_bus(const std::vector<SimDuration>& steps, const EcuFactory& make = make_scenario, SimTime end = kEnd,
                          Bus kind = Bus::Trivial)
{
    vehicle_sil::can::VirtualCanBusOptions options;
    if (kind == Bus::Simulator)
    {
        options.timing = vehicle_sil::can::VirtualCanBusTiming{kSimulatorBitrate, 64, kSimulatorStep};
    }
    vehicle_sil::can::VirtualCanBus bus(options);
    std::vector<std::unique_ptr<Ecu>> scenarios;
    std::vector<std::unique_ptr<Host>> hosts;
    vehicle_sil::can::StepDriver driver;
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        scenarios.push_back(make(i));
        hosts.push_back(std::make_unique<Host>());
        hosts.back()->add(scenarios.back()->network, bus.attach());
        Ecu* s = scenarios.back().get();
        hosts.back()->set_application([s](SimTime now) { s->run(now); });
        driver.add(*hosts.back(), steps[i]);
    }
    driver.run_until(end);
    Traces traces;
    for (const auto& s : scenarios)
    {
        check_network(s->network);
        traces.push_back(s->trace());
    }
    return traces;
}

struct SilKitNode
{
    SilKitNode() = default;
    SilKitNode(const SilKitNode&) = delete;
    SilKitNode& operator=(const SilKitNode&) = delete;
    // Shutdown order (also when a check throws): stop the runtime, close the port, then the
    // members go in reverse order (host, scenario, port, runtime).
    ~SilKitNode()
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
    std::unique_ptr<Ecu> scenario;
    std::unique_ptr<Host> host;
};

Traces run_on_silkit(const std::vector<SimDuration>& steps, const EcuFactory& make = make_scenario, SimTime end = kEnd,
                     Bus kind = Bus::Trivial)
{
    InProcessRegistry registry;
    std::vector<std::string> names;
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        names.push_back("Ecu" + std::to_string(i + 1));
    }
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = names;
    controllerOptions.stop_at = end - SimTime{} + 20ms;
    if (kind == Bus::Simulator)
    {
        controllerOptions.required_participants.push_back("NetworkSimulator");
    }
    SimulationController controller(controllerOptions);

    std::vector<ParticipantRuntime*> runtimes;
    // Declared before its runtime, so it is destroyed after the runtime stopped stepping it.
    std::unique_ptr<vehicle_sil::can::CanBusSimulator> simulator;
    std::unique_ptr<ParticipantRuntime> simulatorRuntime;
    if (kind == Bus::Simulator)
    {
        simulatorRuntime = std::make_unique<ParticipantRuntime>(
            coordinated_options("NetworkSimulator", registry.uri, VirtualTimeOptions{kSimulatorStep, std::nullopt, end}));
        simulator = std::make_unique<vehicle_sil::can::CanBusSimulator>(simulatorRuntime->participant(),
                                                                        std::vector<std::string>{kNetwork});
        simulatorRuntime->set_step_handler([&simulator](SimTime now, SimDuration step) { simulator->step(now, step); });
        runtimes.push_back(simulatorRuntime.get());
    }
    std::vector<SilKitNode> nodes(steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        auto& node = nodes[i];
        node.runtime = std::make_unique<ParticipantRuntime>(
            coordinated_options(names[i], registry.uri, VirtualTimeOptions{steps[i], std::nullopt, end}));
        auto* controllerPtr = node.runtime->participant().CreateCanController("CAN", kNetwork);
        vehicle_sil::can::SilKitCanPortOptions portOptions;
        portOptions.bitrate = kind == Bus::Simulator ? kSimulatorBitrate : 0;
        node.port = std::make_unique<SilKitCanPort>(*controllerPtr, portOptions);
        node.runtime->add_communication_ready_handler([p = node.port.get()] { p->start(); });
        node.scenario = make(i);
        node.host = std::make_unique<Host>();
        node.host->add(node.scenario->network, *node.port);
        Ecu* s = node.scenario.get();
        node.host->set_application([s](SimTime now) { s->run(now); });
        vehicle_sil::can::bind_host(*node.runtime, *node.host);
        runtimes.push_back(node.runtime.get());
    }

    start_together(controller, runtimes, 30s);
    for (auto* runtime : runtimes)
    {
        const auto state = runtime->wait_until_finished(scaled_timeout(60s));
        if (!state || *state != orch::ParticipantState::Shutdown)
        {
            throw std::runtime_error(runtime->name() + " did not shut down: " + runtime->step_error());
        }
    }
    if (simulator)
    {
        const auto stats = simulator->statistics(kNetwork);
        if (stats.untagged_requests != 0 || stats.rejected_requests != 0 || stats.inbox_overflows != 0 ||
            stats.transmit_queue_full != 0 || stats.bitrate != kSimulatorBitrate || stats.transmissions == 0)
        {
            throw std::runtime_error("unexpected simulator statistics");
        }
    }

    Traces traces;
    for (auto& node : nodes)
    {
        const auto stats = node.port->statistics();
        if (stats.receive_overflows != 0 || stats.receive_rejected != 0 || stats.send_failed != 0)
        {
            throw std::runtime_error("unexpected port statistics");
        }
        if (node.host->failed())
        {
            throw std::runtime_error("host failed: " + node.host->error());
        }
        node.port->close();
        check_network(node.scenario->network);
        traces.push_back(node.scenario->trace());
    }
    return traces;
}

void print_first_difference(const Traces& expected, const Traces& actual)
{
    for (std::size_t n = 0; n < expected.size() && n < actual.size(); ++n)
    {
        for (std::size_t i = 0; i < expected[n].size() || i < actual[n].size(); ++i)
        {
            const std::string e = i < expected[n].size() ? expected[n][i] : "<none>";
            const std::string a = i < actual[n].size() ? actual[n][i] : "<none>";
            if (e != a)
            {
                std::printf("node %zu line %zu: expected '%s', got '%s'\n", n + 1, i, e.c_str(), a.c_str());
                return;
            }
        }
    }
}

/// Receptions with the same time as the previous one but from another sender.
std::size_t same_time_receptions(const std::vector<std::string>& trace)
{
    std::size_t n = 0;
    for (std::size_t i = 1; i < trace.size(); ++i)
    {
        const auto time = [](const std::string& line) {
            const std::size_t start = line.rfind("L ", 0) == 0 ? 2 : 0;
            return line.substr(start, line.find(' ', start) - start);
        };
        const auto sender = [](const std::string& line) { return line.substr(line.find("->") - 2, 2); };
        if (time(trace[i]) == time(trace[i - 1]) && sender(trace[i]) != sender(trace[i - 1]))
        {
            ++n;
        }
    }
    return n;
}

std::size_t count_containing(const Traces& traces, const std::string& text)
{
    std::size_t n = 0;
    for (const auto& trace : traces)
    {
        for (const auto& line : trace)
        {
            n += line.find(text) != std::string::npos ? 1u : 0u;
        }
    }
    return n;
}

void check_same_traces(const std::vector<SimDuration>& steps)
{
    const Traces reference = run_on_virtual_bus(steps);
    std::size_t sameTime = 0;
    for (const auto& trace : reference)
    {
        CHECK(trace.size() > 40u);
        sameTime += same_time_receptions(trace);
    }
    CHECK(sameTime > 0u);
    // The scenario covers every kind of frame of this milestone.
    CHECK(count_containing(reference, " 0EA00 ") > 0u);          // requests
    CHECK(count_containing(reference, " 0E800 ") > 0u);          // acknowledgements
    CHECK(count_containing(reference, " 0E800 30->FF 01") > 0u); // NACK
    CHECK(count_containing(reference, " 0E800 10->FF 02") > 0u); // AccessDenied
    CHECK(count_containing(reference, " 0ED00 ") > 0u);          // destination-specific replies
    CHECK(count_containing(reference, " 0FF30 ") > 0u);           // broadcasts and responses
    CHECK(count_containing(reference, " AA ") > 0u);             // responses to requests
    CHECK(count_containing(reference, " 0EA00 11->10") > 0u);    // local loopback: request ...
    CHECK(count_containing(reference, "L ") > 0u);               // ... and the second node's receptions
    for (int run = 0; run < 2; ++run)
    {
        const Traces silkit = run_on_silkit(steps);
        if (silkit != reference)
        {
            print_first_difference(reference, silkit);
        }
        CHECK(silkit == reference);
    }
}

} // namespace

TEST_CASE(j1939_trace_matches_virtual_bus_steps_10_10)
{
    check_same_traces({10ms, 10ms, 5ms});
}

TEST_CASE(j1939_trace_matches_virtual_bus_steps_1_10)
{
    check_same_traces({1ms, 10ms, 5ms});
}

TEST_CASE(j1939_trace_matches_virtual_bus_steps_10_1)
{
    check_same_traces({10ms, 1ms, 5ms});
}

TEST_CASE(j1939_trace_matches_virtual_bus_steps_3_7)
{
    check_same_traces({3ms, 7ms, 5ms});
}

namespace {

/// Frames of different senders with the same source address and the same time (Ecu1 and Ecu2
/// both claim 0x80 at time 0) arrive in an order SIL Kit does not fix; the stack handles them in
/// arrival order (one list per source address). Sorts such runs so that only that order is
/// ignored; the protocol outcome (the last line of each trace) must not depend on it.
Traces canonical(Traces traces)
{
    for (auto& trace : traces)
    {
        const auto key = [](const std::string& line) { return line.substr(0, line.find("->")); }; // time, PGN, source
        std::size_t first = 0;
        while (first < trace.size())
        {
            std::size_t last = first + 1;
            while (last < trace.size() && key(trace[last]) == key(trace[first]))
            {
                ++last;
            }
            std::sort(trace.begin() + static_cast<std::ptrdiff_t>(first), trace.begin() + static_cast<std::ptrdiff_t>(last));
            first = last;
        }
    }
    return traces;
}

void check_same_claim_traces(const std::vector<SimDuration>& steps)
{
    const EcuFactory make = [](std::size_t i) -> std::unique_ptr<Ecu> { return std::make_unique<ClaimScenario>(i); };
    const SimTime end = SimTime{} + 600ms;
    const Traces reference = canonical(run_on_virtual_bus(steps, make, end));
    // Outcome: Ecu1 lost 0x80 (CannotClaim), Ecu2 owns 0x80, Ecu3 owns 0x81.
    CHECK(reference[0].back() == "state " + std::to_string(static_cast<int>(AddressState::CannotClaim)) + " address 254");
    CHECK(reference[1].back() == "state " + std::to_string(static_cast<int>(AddressState::Claimed)) + " address 128");
    CHECK(reference[2].back() == "state " + std::to_string(static_cast<int>(AddressState::Claimed)) + " address 129");
    CHECK(count_containing(reference, " 0EE00 FE->FF") > 0u); // Cannot Claim
    CHECK(count_containing(reference, " 0EA00 81->FF") > 0u); // Request for Address Claimed
    for (int run = 0; run < 2; ++run)
    {
        const Traces silkit = canonical(run_on_silkit(steps, make, end));
        if (silkit != reference)
        {
            print_first_difference(reference, silkit);
        }
        CHECK(silkit == reference);
    }
}

std::string state_line(const char* tag, AddressState state, Address address)
{
    return std::string("state ") + tag + " " + std::to_string(static_cast<int>(state)) + " address " + std::to_string(address);
}

bool contains_line(const std::vector<std::string>& trace, const std::string& line)
{
    return std::find(trace.begin(), trace.end(), line) != trace.end();
}

void check_same_management_traces(const std::vector<SimDuration>& steps)
{
    const EcuFactory make = [](std::size_t i) -> std::unique_ptr<Ecu> { return std::make_unique<ManagementScenario>(i); };
    const SimTime end = SimTime{} + 700ms;
    const Traces reference = canonical(run_on_virtual_bus(steps, make, end));
    // Outcome: Ecu1's first node at 0x82, its second at 0x20; Ecu2 at 0x80; Ecu3 at 0x81.
    CHECK(contains_line(reference[0], state_line("a", AddressState::Claimed, 0x82)));
    CHECK(contains_line(reference[0], state_line("b", AddressState::Claimed, 0x20)));
    CHECK(contains_line(reference[1], state_line("a", AddressState::Claimed, 0x80)));
    CHECK(contains_line(reference[2], state_line("a", AddressState::Claimed, 0x81)));
    CHECK(contains_line(reference[2], state_line("f", AddressState::Fixed, 0x20)));
    // Ecu3's table: the other control functions at their final addresses.
    CHECK(contains_line(reference[2], "table 80 A0008200FE001234"));
    CHECK(contains_line(reference[2], "table 82 A0008200FE001236"));
    CHECK(contains_line(reference[2], "table 20 20008200FE001240"));
    CHECK(reference[0].back() == "violations 1");
    CHECK(count_containing(reference, " 0EE00 82->FF") > 0u);  // the move
    CHECK(count_containing(reference, " 0EE00 FE->FF") == 0u); // nobody had to give up
    for (int run = 0; run < 2; ++run)
    {
        const Traces silkit = canonical(run_on_silkit(steps, make, end));
        if (silkit != reference)
        {
            print_first_difference(reference, silkit);
        }
        CHECK(silkit == reference);
    }
}

} // namespace

TEST_CASE(j1939_address_management_trace_matches_virtual_bus_steps_10_10)
{
    check_same_management_traces({10ms, 10ms, 5ms});
}

TEST_CASE(j1939_address_management_trace_matches_virtual_bus_steps_1_10)
{
    check_same_management_traces({1ms, 10ms, 5ms});
}

TEST_CASE(j1939_address_management_trace_matches_virtual_bus_steps_7_3)
{
    check_same_management_traces({7ms, 3ms, 10ms});
}

TEST_CASE(j1939_address_claim_trace_matches_virtual_bus_steps_10_10)
{
    check_same_claim_traces({10ms, 10ms, 5ms});
}

TEST_CASE(j1939_address_claim_trace_matches_virtual_bus_steps_1_10)
{
    check_same_claim_traces({1ms, 10ms, 5ms});
}

TEST_CASE(j1939_address_claim_trace_matches_virtual_bus_steps_3_7)
{
    check_same_claim_traces({3ms, 7ms, 5ms});
}

TEST_CASE(j1939_step_size_above_200_ms_fails_the_participant)
{
    // bind_host passes the participant's step size to the network, which rejects it: the step
    // handler throws and the participant goes to Error.
    InProcessRegistry registry;
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"Ecu"};
    controllerOptions.stop_at = 2s;
    SimulationController controller(controllerOptions);
    SilKitNode node;
    node.runtime = std::make_unique<ParticipantRuntime>(
        coordinated_options("Ecu", registry.uri, VirtualTimeOptions{250ms, std::nullopt, std::nullopt}));
    node.port = std::make_unique<SilKitCanPort>(*node.runtime->participant().CreateCanController("CAN", kNetwork));
    node.runtime->add_communication_ready_handler([p = node.port.get()] { p->start(); });
    node.scenario = std::make_unique<Scenario>(0);
    node.host = std::make_unique<Host>();
    node.host->add(node.scenario->network, *node.port);
    vehicle_sil::can::bind_host(*node.runtime, *node.host);
    start_together(controller, {node.runtime.get()}, 30s);
    const auto state = node.runtime->wait_until_finished(scaled_timeout(60s));
    CHECK(state.has_value());
    CHECK(*state == orch::ParticipantState::Error);
    CHECK(node.host->failed());
    CHECK(node.scenario->network.failed());
    CHECK(node.runtime->step_error().find("200 ms") != std::string::npos);
}

namespace {

/// The protocol outcome of TransportScenario, whatever the bus timing and step sizes.
void check_transport_outcome(const Traces& reference)
{
    // Ecu1's broadcasts reach Ecu2, Ecu3 and Ecu1's second node (local loopback); some rounds
    // find the node's broadcast still taken by a long response to a global request (Busy).
    CHECK(count_containing(reference, " 0FF31 10->FF n30 ") >= 3u);
    CHECK(count_containing(reference, "L ") >= 5u);
    CHECK(count_containing(reference, "SEND 16 65329 status 5") > 0u);
    // Connections to Ecu2, which takes one at a time: each of the 5 rounds of Ecu1 and Ecu3 is
    // received or refused (which one wins depends on the step sizes).
    CHECK_EQ(count_containing(reference, " 0EF00 10->20 n100 ") + count_containing(reference, "0EF00 ->20 100 r1 a1"), 5u);
    CHECK_EQ(count_containing(reference, " 0EF00 30->20 n60 ") + count_containing(reference, "0EF00 ->20 60 r1 a1"), 5u);
    CHECK(count_containing(reference, " r1 a1") > 0u);
    CHECK(count_containing(reference, " 0EF00 11->30 n20 ") == 5u);
    CHECK(count_containing(reference, " 0FF32 10->30 n40 ") == 5u); // long response by RTS/CTS
    CHECK(count_containing(reference, " 0FF33 10->FF n25 ") == 15u); // long response by BAM, to 3 nodes
    CHECK(count_containing(reference, " r0 a0") > 20u);              // completed transfers
    CHECK(count_containing(reference, " r1 a1") > 0u);               // refused: Ecu2 is busy
}

void check_same_transport_traces(const std::vector<SimDuration>& steps)
{
    const SimTime end = SimTime{} + 2000ms;
    const Traces reference = run_on_virtual_bus(steps, TransportScenario::make, end);
    check_transport_outcome(reference);
    for (int run = 0; run < 2; ++run)
    {
        const Traces silkit = run_on_silkit(steps, TransportScenario::make, end);
        if (silkit != reference)
        {
            print_first_difference(reference, silkit);
        }
        CHECK(silkit == reference);
    }
}

} // namespace

TEST_CASE(j1939_transport_trace_matches_virtual_bus_steps_10_10)
{
    check_same_transport_traces({10ms, 10ms, 10ms});
}

TEST_CASE(j1939_transport_trace_matches_virtual_bus_steps_1_10_5)
{
    check_same_transport_traces({1ms, 10ms, 5ms});
}

TEST_CASE(j1939_transport_trace_matches_virtual_bus_steps_7_3_10)
{
    check_same_transport_traces({7ms, 3ms, 10ms});
}

// ---------------------------------------------------------------------------------------------
// On the simple CAN network simulator (vehicle_sil::can_netsim): the nodes are on separate
// participants, so their frames go through the simulator (arbitration, transmission time). The
// traces equal those of the virtual bus with the same timing (bit rate and simulator step), and
// the protocol outcome equals that of the trivial simulation.
// ---------------------------------------------------------------------------------------------

namespace {

Traces check_simulator_matches_timed_virtual_bus(const std::vector<SimDuration>& steps, const EcuFactory& make, SimTime end,
                                                bool canonicalize = false)
{
    const auto prepare = [canonicalize](Traces traces) { return canonicalize ? canonical(std::move(traces)) : traces; };
    const Traces reference = prepare(run_on_virtual_bus(steps, make, end, Bus::Simulator));
    // The bus delays every frame, so the times differ from the trivial simulation.
    CHECK(reference != prepare(run_on_virtual_bus(steps, make, end, Bus::Trivial)));
    for (int run = 0; run < 2; ++run)
    {
        const Traces silkit = prepare(run_on_silkit(steps, make, end, Bus::Simulator));
        if (silkit != reference)
        {
            print_first_difference(reference, silkit);
        }
        CHECK(silkit == reference);
    }
    return reference;
}

} // namespace

TEST_CASE(j1939_trace_on_the_simulator_matches_the_timed_virtual_bus_steps_10_10)
{
    check_simulator_matches_timed_virtual_bus({10ms, 10ms, 10ms}, make_scenario, kEnd);
}

TEST_CASE(j1939_trace_on_the_simulator_matches_the_timed_virtual_bus_steps_1_10_5)
{
    check_simulator_matches_timed_virtual_bus({1ms, 10ms, 5ms}, make_scenario, kEnd);
}

TEST_CASE(j1939_address_claim_on_the_simulator_has_the_trivial_outcome)
{
    const EcuFactory make = [](std::size_t i) -> std::unique_ptr<Ecu> { return std::make_unique<ClaimScenario>(i); };
    const SimTime end = SimTime{} + 600ms;
    const Traces timed = check_simulator_matches_timed_virtual_bus({10ms, 1ms, 7ms}, make, end, true);
    const Traces trivial = canonical(run_on_virtual_bus({10ms, 1ms, 7ms}, make, end));
    for (std::size_t i = 0; i < trivial.size(); ++i)
    {
        CHECK(timed[i].back() == trivial[i].back()); // the final state and address
    }
    CHECK(count_containing(timed, " 0EE00 FE->FF") > 0u); // Cannot Claim
}

TEST_CASE(j1939_transport_on_the_simulator_has_the_trivial_outcome)
{
    const Traces timed = check_simulator_matches_timed_virtual_bus({10ms, 3ms, 7ms}, TransportScenario::make, SimTime{} + 2000ms);
    check_transport_outcome(timed);
}

namespace {

/// One ECU of a participant without time synchronisation, stepped by a RealTimeDriver on the
/// wall clock. Members are destroyed in the shutdown order: driver, host, network, port,
/// runtime.
struct RealTimeEcu
{
    RealTimeEcu(const std::string& uri, const std::string& name, NodeOptions nodeOptions)
    {
        vehicle_sil::ParticipantRuntimeOptions options;
        options.participant_name = name;
        options.registry_uri = uri;
        runtime = std::make_unique<ParticipantRuntime>(options); // no virtual time
        vehicle_sil::can::SilKitCanPortOptions portOptions;
        portOptions.real_time_clock = &clock;
        port = std::make_unique<SilKitCanPort>(*runtime->participant().CreateCanController("CAN", kNetwork), portOptions);
        runtime->add_communication_ready_handler([p = port.get()] { p->start(); });
        node = &network.add_node(nodeOptions);
        node->on_address_change([this](const vehicle_sil::j1939::AddressChange& c) {
            if (c.state == AddressState::Claimed)
            {
                claimed.store(true);
            }
        });
        node->on_any_message([this](const Message& m) {
            if (m.pgn == Pgn{0xEE00})
            {
                heard_claim.store(true); // the other participant's claim: it sees this one
            }
            if (m.pgn == Pgn{0xEF00} && m.data.size() == 1785)
            {
                long_received.fetch_add(1);
            }
            if (m.pgn == Pgn{0xFF10})
            {
                short_received.fetch_add(1);
            }
        });
        node->on_transfer_end([this](const vehicle_sil::j1939::TransferEnd& e) {
            if (e.result == vehicle_sil::j1939::TransferResult::Completed)
            {
                completed.fetch_add(1);
            }
        });
        host.add(network, *port);
        driver = std::make_unique<vehicle_sil::can::RealTimeDriver>(host, clock);
    }

    vehicle_sil::can::SteadyRealTimeClock clock;
    std::unique_ptr<ParticipantRuntime> runtime;
    std::unique_ptr<SilKitCanPort> port;
    Network network{[] {
        vehicle_sil::j1939::NetworkOptions options;
        options.warning = [](std::string_view) {};
        return options;
    }()};
    Node* node = nullptr;
    Host host;
    std::unique_ptr<vehicle_sil::can::RealTimeDriver> driver;
    std::atomic<bool> claimed{false};
    std::atomic<bool> heard_claim{false};
    std::atomic<int> send_errors{0}; ///< CHECK would only fail the host inside a posted function
    std::atomic<int> long_received{0};
    std::atomic<int> short_received{0};
    std::atomic<int> completed{0};
};

} // namespace

// Wall clock: only the protocol results are checked, not the times.
TEST_CASE(j1939_runs_on_real_time_without_time_synchronisation)
{
    InProcessRegistry registry;
    RealTimeEcu a(registry.uri, "EcuA", NodeOptions{0x10, Name{0x1000}});
    RealTimeEcu b(registry.uri, "EcuB", NodeOptions{0x20, Name{0x2000}});
    a.runtime->start(30s);
    b.runtime->start(30s);
    a.driver->start();
    b.driver->start();
    CHECK(vehicle_sil::unit::eventually([&] { return a.claimed.load() && b.claimed.load(); }, 10s));
    // Both have discovered each other (a frame sent before that would be lost): each heard the
    // other's claim. If one claim was sent too early, a Request for Address Claimed brings it.
    if (!vehicle_sil::unit::eventually([&] { return a.heard_claim.load() && b.heard_claim.load(); }, 2s))
    {
        a.host.post([&] { static_cast<void>(a.node->request(Pgn{0xEE00}, vehicle_sil::j1939::kGlobalAddress)); });
        b.host.post([&] { static_cast<void>(b.node->request(Pgn{0xEE00}, vehicle_sil::j1939::kGlobalAddress)); });
    }
    CHECK(vehicle_sil::unit::eventually([&] { return a.heard_claim.load() && b.heard_claim.load(); }, 10s));

    // The application hands work to the stack from its own thread with post().
    const std::vector<std::uint8_t> message(1785, 0x5A);
    a.host.post([&] {
        if (a.node->send(Pgn{0xEF00}, message, 0x20) != SendStatus::Ok)
        {
            a.send_errors.fetch_add(1);
        }
    });
    for (int i = 0; i < 5; ++i)
    {
        b.host.post([&] {
            if (b.node->send(Pgn{0xFF10}, std::vector<std::uint8_t>{1, 2, 3}) != SendStatus::Ok)
            {
                b.send_errors.fetch_add(1);
            }
        });
    }
    CHECK(vehicle_sil::unit::eventually(
        [&] { return b.long_received.load() == 1 && a.completed.load() == 1 && a.short_received.load() == 5; }, 20s));

    CHECK_EQ(a.send_errors.load(), 0);
    CHECK_EQ(b.send_errors.load(), 0);
    a.driver->stop();
    b.driver->stop();
    CHECK(!a.driver->failed());
    CHECK(!b.driver->failed());
    CHECK(a.node->address_state() == AddressState::Claimed);
    CHECK_EQ(a.node->address(), Address{0x10});
    CHECK_EQ(b.node->address(), Address{0x20});
    CHECK_EQ(a.network.statistics().transport_timeouts, 0u);
    CHECK_EQ(b.network.statistics().transport_timeouts, 0u);
    CHECK_EQ(a.port->statistics().receive_overflows, 0u);
    CHECK_EQ(b.port->statistics().receive_overflows, 0u);
    a.port->close();
    b.port->close();
}

namespace {

namespace tp = vehicle_sil::unit::j1939;

/// An ECU on virtual time (a J1939 node on the SIL Kit CAN port) and a participant without time
/// synchronisation on the same network (like the Vector XL CAN bridge), which sends raw frames.
/// SIL Kit stamps the bridge's frames with std::chrono::nanoseconds::min().
struct EcuAndBridge
{
    explicit EcuAndBridge(vehicle_sil::ParticipantRuntimeOptions ecuOptions)
    {
        ecuOptions.registry_uri = registry.uri;
        ecu = std::make_unique<ParticipantRuntime>(ecuOptions);
        port = std::make_unique<SilKitCanPort>(*ecu->participant().CreateCanController("CAN", kNetwork));
        ecu->add_communication_ready_handler([p = port.get()] { p->start(); });
        Node& node = network.add_node(NodeOptions{0x20, Name{0x2000}});
        node.on_any_message([this](const Message& m) {
            if (m.pgn == Pgn{0xFF10})
            {
                singles.fetch_add(1);
                single_time.store(to_ns(m.time));
            }
            if (m.pgn == Pgn{0xFF20} && m.data.size() == 20)
            {
                longs.fetch_add(1);
            }
        });
        host.add(network, *port);
        host.set_application([this](SimTime now) {
            last_step.store(to_ns(now));
            timeouts.store(network.statistics().transport_timeouts);
        });
        vehicle_sil::bind_host(*ecu, host);

        vehicle_sil::ParticipantRuntimeOptions bridgeOptions;
        bridgeOptions.participant_name = "Bridge";
        bridgeOptions.registry_uri = registry.uri;
        bridge = std::make_unique<ParticipantRuntime>(bridgeOptions); // no virtual time
        bridge_can = bridge->participant().CreateCanController("CAN", kNetwork);
        bridge_can->Start();
    }
    EcuAndBridge(const EcuAndBridge&) = delete;
    EcuAndBridge& operator=(const EcuAndBridge&) = delete;
    ~EcuAndBridge()
    {
        ecu->stop("test cleanup");
        port->close();
        bridge->stop("test cleanup");
    }

    void send(const tp::J1939Frame& f)
    {
        const vehicle_sil::can::Frame frame = tp::to_can(f);
        SilKit::Services::Can::CanFrame out;
        CHECK(vehicle_sil::can::to_silkit_frame(frame, out));
        bridge_can->SendFrame(out);
    }

    /// Sends a single frame until the ECU's port has one (the participants have found each
    /// other).
    void send_until_received()
    {
        CHECK(vehicle_sil::unit::eventually(
            [&] {
                send(tp::frame(6, 0xFF10, 0x30, tp::kGlobal, tp::Bytes{1, 2, 3}));
                return port->statistics().received > 0;
            },
            10s, 10ms));
    }

    /// The packets of the BAM announced by bam_announcement(), each after the ECU stepped 5 ms
    /// more (so each one arrives in a later step than the one before).
    void send_bam_packets()
    {
        for (std::uint8_t sequence = 1; sequence <= 3; ++sequence)
        {
            const std::int64_t before = last_step.load();
            CHECK(vehicle_sil::unit::eventually([&] { return last_step.load() >= before + 5'000'000; }, 10s));
            send(tp::dt(0x30, tp::kGlobal, sequence, message));
        }
    }
    [[nodiscard]] tp::J1939Frame bam_announcement() const { return tp::bam(0x30, message.size(), 0xFF20); }

    /// The BAM was received and no transfer timed out; the single frame had the time of an ECU
    /// step (not -292 years).
    void check_received()
    {
        CHECK(vehicle_sil::unit::eventually([&] { return longs.load() == 1 || timeouts.load() > 0; }, 10s));
        ecu->stop("test");
        CHECK(singles.load() > 0);
        CHECK(single_time.load() >= 0);
        CHECK(single_time.load() <= last_step.load());
        CHECK_EQ(timeouts.load(), 0u);
        CHECK_EQ(longs.load(), 1);
        CHECK(!host.failed());
    }

    InProcessRegistry registry;
    std::unique_ptr<ParticipantRuntime> ecu;
    std::unique_ptr<SilKitCanPort> port;
    Network network{[] {
        vehicle_sil::j1939::NetworkOptions options;
        options.warning = [](std::string_view) {};
        return options;
    }()};
    Host host;
    std::unique_ptr<ParticipantRuntime> bridge;
    SilKit::Services::Can::ICanController* bridge_can = nullptr;
    const tp::Bytes message = tp::payload(20);
    std::atomic<int> singles{0};
    std::atomic<std::int64_t> single_time{0};
    std::atomic<int> longs{0};
    std::atomic<std::int64_t> last_step{-1};
    std::atomic<std::uint64_t> timeouts{0};
};

} // namespace

TEST_CASE(j1939_on_virtual_time_receives_a_bam_from_a_participant_without_time_synchronisation)
{
    // While the ECU steps: its SIL Kit stamps the bridge's frames with its own current time.
    vehicle_sil::ParticipantRuntimeOptions options;
    options.participant_name = "Ecu";
    options.lifecycle_mode = vehicle_sil::LifecycleMode::Autonomous;
    options.virtual_time = VirtualTimeOptions{1ms, 1.0}; // paced to the wall clock
    EcuAndBridge run(options);
    run.ecu->start(30s);
    run.bridge->start(30s);
    run.send_until_received();
    run.send(run.bam_announcement());
    run.send_bam_packets();
    run.check_received();
}

TEST_CASE(j1939_on_virtual_time_receives_a_bam_from_a_participant_without_time_synchronisation_that_starts_before_the_first_step)
{
    // The bridge sends while the ECU waits for the simulation to start (a required participant
    // is missing): the single frame and the BAM's announcement arrive before the ECU's first
    // step, its packets after it.
    EcuAndBridge run(coordinated_options("Ecu", "", VirtualTimeOptions{1ms, 1.0}));
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = run.registry.uri;
    controllerOptions.required_participants = {"Ecu", "Late"};
    SimulationController controller(controllerOptions);
    auto late = std::make_unique<ParticipantRuntime>(
        coordinated_options("Late", run.registry.uri, VirtualTimeOptions{1ms, std::nullopt}));
    late->set_step_handler([](SimTime, SimDuration) {});

    std::thread ecuStart([&] { run.ecu->start(30s); });
    std::thread controllerStart;
    std::thread lateStart;
    struct JoinOnExit
    {
        std::vector<std::thread*> threads;
        std::unique_ptr<ParticipantRuntime>& late;
        ~JoinOnExit()
        {
            late->stop("test cleanup");
            for (auto* t : threads)
            {
                if (t->joinable())
                {
                    t->join();
                }
            }
        }
    } joinOnExit{{&ecuStart, &controllerStart, &lateStart}, late};
    run.bridge->start(30s);
    controllerStart = std::thread([&] { controller.start(30s); });

    run.send_until_received();
    run.send(run.bam_announcement());
    CHECK(vehicle_sil::unit::eventually([&] { return run.port->statistics().received >= 2; }, 10s));
    CHECK_EQ(run.last_step.load(), -1); // no step yet

    lateStart = std::thread([&] { late->start(30s); });
    run.send_bam_packets();
    run.check_received();
}
