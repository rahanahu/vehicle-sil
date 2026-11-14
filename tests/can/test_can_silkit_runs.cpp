// SPDX-License-Identifier: MIT
//
// vehicle_sil::can on real SIL Kit participants (in-process registry): the same scenario gives
// the same traces on SIL Kit with virtual time as on the virtual bus, and a participant can be
// shut down in the documented order while others keep sending.
#include "silkit_test_util.hpp"
#include "test_harness.hpp"
#include "trace_protocol.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"
#include "vehicle_sil/can_silkit/silkit_can_port.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/services/can/all.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
namespace orch = SilKit::Services::Orchestration;
namespace sk = SilKit::Services::Can;
using vehicle_sil::LifecycleMode;
using vehicle_sil::ParticipantRuntime;
using vehicle_sil::ParticipantRuntimeOptions;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::SimulationController;
using vehicle_sil::SimulationControllerOptions;
using vehicle_sil::VirtualTimeOptions;
using vehicle_sil::can::Host;
using vehicle_sil::can::SilKitCanPort;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::start_together;
using vehicle_sil::unit::TraceProtocol;

namespace {

constexpr auto kEnd = SimTime{} + 100ms;
constexpr const char* kNetwork = "CAN1";

using Traces = std::vector<std::vector<std::string>>;

SimDuration period_of(std::size_t node)
{
    const SimDuration periods[] = {7ms, 11ms, 13ms};
    return periods[node % 3];
}

Traces run_on_virtual_bus(const std::vector<SimDuration>& steps)
{
    vehicle_sil::can::VirtualCanBus bus;
    std::vector<std::unique_ptr<TraceProtocol>> nodes;
    std::vector<std::unique_ptr<Host>> hosts;
    vehicle_sil::can::StepDriver driver;
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        nodes.push_back(std::make_unique<TraceProtocol>(static_cast<std::uint8_t>(i + 1), period_of(i)));
        hosts.push_back(std::make_unique<Host>());
        hosts.back()->add(*nodes.back(), bus.attach());
        driver.add(*hosts.back(), steps[i]);
    }
    driver.run_until(kEnd);
    Traces traces;
    for (const auto& node : nodes)
    {
        traces.push_back(node->trace());
    }
    return traces;
}

struct SilKitNode
{
    SilKitNode() = default;
    SilKitNode(const SilKitNode&) = delete;
    SilKitNode& operator=(const SilKitNode&) = delete;
    // Shutdown order (also when a check throws): stop the runtime, close the port, then the
    // members go in reverse order (host, protocol, port, runtime).
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
    std::unique_ptr<TraceProtocol> protocol;
    std::unique_ptr<Host> host;
};

Traces run_on_silkit(const std::vector<SimDuration>& steps)
{
    InProcessRegistry registry;
    std::vector<std::string> names;
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        names.push_back("Node" + std::to_string(i + 1));
    }
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = names;
    controllerOptions.stop_at = kEnd - SimTime{} + 20ms;
    SimulationController controller(controllerOptions);

    std::vector<SilKitNode> nodes(steps.size());
    std::vector<ParticipantRuntime*> runtimes;
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        auto& node = nodes[i];
        // `end`: only steps before kEnd are handled, independent of when the stop arrives.
        node.runtime = std::make_unique<ParticipantRuntime>(
            coordinated_options(names[i], registry.uri, VirtualTimeOptions{steps[i], std::nullopt, kEnd}));
        auto* controllerPtr = node.runtime->participant().CreateCanController("CAN", kNetwork);
        node.port = std::make_unique<SilKitCanPort>(*controllerPtr);
        node.protocol = std::make_unique<TraceProtocol>(static_cast<std::uint8_t>(i + 1), period_of(i));
        node.host = std::make_unique<Host>();
        node.host->add(*node.protocol, *node.port);
        vehicle_sil::can::bind_host(*node.runtime, *node.host);
        runtimes.push_back(node.runtime.get());
    }

    start_together(controller, runtimes, 30s);
    for (auto& node : nodes)
    {
        const auto state = node.runtime->wait_until_finished(scaled_timeout(60s));
        if (!state || *state != orch::ParticipantState::Shutdown)
        {
            throw std::runtime_error(node.runtime->name() + " did not shut down: " + node.runtime->step_error());
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
        // Shutdown order: the runtime has stopped stepping; close the port before the stack and
        // the runtime go away.
        node.port->close();
        traces.push_back(node.protocol->trace());
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

void check_same_traces(const std::vector<SimDuration>& steps)
{
    const Traces reference = run_on_virtual_bus(steps);
    for (const auto& trace : reference)
    {
        CHECK(trace.size() > 50u);
    }
    std::size_t sameTime = 0;
    for (const auto& trace : reference)
    {
        sameTime += vehicle_sil::unit::same_time_receptions(trace);
    }
    CHECK(sameTime > 0u); // frames from different senders with equal times
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

TEST_CASE(trace_matches_virtual_bus_steps_10_10)
{
    check_same_traces({10ms, 10ms, 5ms});
}

TEST_CASE(trace_matches_virtual_bus_steps_1_10)
{
    check_same_traces({1ms, 10ms, 5ms});
}

TEST_CASE(trace_matches_virtual_bus_steps_10_1)
{
    check_same_traces({10ms, 1ms, 5ms});
}

TEST_CASE(trace_matches_virtual_bus_steps_3_7)
{
    check_same_traces({3ms, 7ms, 5ms});
}

TEST_CASE(shutdown_in_order_while_a_peer_keeps_sending)
{
    // A peer without time synchronisation keeps sending while the stepped participant stops its
    // runtime, closes its port and is destroyed. Nothing may call into the destroyed objects
    // (ASan and TSan builds check that), and the closed port takes no more frames.
    InProcessRegistry registry;

    ParticipantRuntimeOptions senderOptions;
    senderOptions.participant_name = "Sender";
    senderOptions.registry_uri = registry.uri;
    ParticipantRuntime sender(senderOptions);
    auto* senderCan = sender.participant().CreateCanController("CAN", kNetwork);
    senderCan->Start();
    sender.start(10s);

    ParticipantRuntimeOptions stepperOptions;
    stepperOptions.participant_name = "Stepper";
    stepperOptions.registry_uri = registry.uri;
    stepperOptions.lifecycle_mode = LifecycleMode::Autonomous;
    stepperOptions.virtual_time = VirtualTimeOptions{1ms, 1.0};
    auto stepper = std::make_unique<ParticipantRuntime>(stepperOptions);
    auto* stepperCan = stepper->participant().CreateCanController("CAN", kNetwork);
    auto port = std::make_unique<SilKitCanPort>(*stepperCan);

    struct Counter final : vehicle_sil::can::Steppable
    {
        std::atomic<std::uint64_t> received{0};
        void begin(SimTime) override {}
        void receive(const vehicle_sil::can::TimedFrame&) override { received.fetch_add(1); }
        void process() override {}
        void flush(vehicle_sil::can::Port&) override {}
    };
    auto counter = std::make_unique<Counter>();
    auto host = std::make_unique<Host>();
    host->add(*counter, *port);
    vehicle_sil::can::bind_host(*stepper, *host);
    stepper->start(10s);

    std::atomic<bool> sending{true};
    std::thread sendLoop;
    // On every exit path (a failed CHECK too): stop the stepper and close its port before the
    // host and the stack go, and join the sender.
    struct CleanupOnExit
    {
        std::unique_ptr<ParticipantRuntime>& stepper;
        std::unique_ptr<SilKitCanPort>& port;
        std::atomic<bool>& flag;
        std::thread& thread;
        ~CleanupOnExit()
        {
            if (stepper)
            {
                stepper->stop("test cleanup");
            }
            if (port)
            {
                port->close();
            }
            flag.store(false);
            if (thread.joinable())
            {
                thread.join();
            }
        }
    } cleanupOnExit{stepper, port, sending, sendLoop};
    sendLoop = std::thread([&] {
        const std::vector<std::uint8_t> data{1, 2, 3, 4};
        sk::CanFrame frame{};
        frame.canId = 0x18FF0001;
        frame.flags = static_cast<sk::CanFrameFlagMask>(sk::CanFrameFlag::Ide);
        frame.dlc = 4;
        frame.dataField = SilKit::Util::Span<const std::uint8_t>(data.data(), data.size());
        while (sending.load())
        {
            senderCan->SendFrame(frame);
            std::this_thread::sleep_for(200us);
        }
    });

    CHECK(vehicle_sil::unit::eventually([&] { return counter->received.load() > 20; }, 10s));

    // 1. Stop the driver: no more steps.
    stepper->stop("test");
    const auto steps = stepper->steps_completed();
    // 2. Close the port: handlers removed, controller stopped.
    port->close();
    const auto receivedAtClose = port->statistics().received;
    std::this_thread::sleep_for(50ms); // the peer keeps sending
    CHECK_EQ(port->statistics().received, receivedAtClose);
    CHECK_EQ(stepper->steps_completed(), steps);
    // 3. Destroy the port, 4. the stack, 5. the runtime, while the peer still sends.
    port.reset();
    host.reset();
    counter.reset();
    stepper.reset();
    std::this_thread::sleep_for(20ms);

    sending.store(false);
    sendLoop.join();
    sender.stop("test");
}
