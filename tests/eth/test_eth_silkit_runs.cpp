// SPDX-License-Identifier: MIT
//
// vehicle_sil::eth on real SIL Kit participants (in-process registry): the same scenario gives
// the same records on SIL Kit with virtual time as on the virtual network, for several
// combinations of step sizes, with three senders that send at the same times.
#include "eth_test_util.hpp"
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/bind_host.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/eth/channel.hpp"
#include "vehicle_sil/eth/virtual_ethernet_bus.hpp"
#include "vehicle_sil/eth_silkit/silkit_eth_port.hpp"
#include "vehicle_sil/step/host.hpp"
#include "vehicle_sil/step/step_driver.hpp"

#include <silkit/experimental/netsim/all.hpp>
#include <silkit/experimental/participant/ParticipantExtensions.hpp>
#include <silkit/services/ethernet/all.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;
namespace orch = SilKit::Services::Orchestration;
using vehicle_sil::ParticipantRuntime;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::SimulationController;
using vehicle_sil::SimulationControllerOptions;
using vehicle_sil::to_ns;
using vehicle_sil::VirtualTimeOptions;
using vehicle_sil::eth::Accept;
using vehicle_sil::eth::Channel;
using vehicle_sil::eth::ChannelOptions;
using vehicle_sil::eth::kBroadcast;
using vehicle_sil::eth::MacAddress;
using vehicle_sil::eth::Port;
using vehicle_sil::eth::SendStatus;
using vehicle_sil::eth::SilKitEthPort;
using vehicle_sil::eth::TimedFrame;
using vehicle_sil::eth::TransmitCapabilities;
using vehicle_sil::eth::TransmitEvent;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::frame_bytes;
using vehicle_sil::unit::group;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::mac;
using vehicle_sil::unit::start_together;

namespace {

constexpr auto kEnd = SimTime{} + 30ms;
constexpr const char* kNetwork = "ETH1";
constexpr std::size_t kNodes = 4;

using Records = std::vector<std::vector<std::string>>;

/// The address of node n (1..4): not in the order the nodes are registered, so frames with
/// equal times reach the receivers on the virtual network in an order the channel has to
/// change (registration order is arrival order there).
MacAddress address_of(std::uint8_t number)
{
    constexpr std::uint8_t kLastByte[kNodes] = {3, 1, 4, 2};
    return mac(kLastByte[number - 1]);
}

/// One node of the scenario, on whatever port its channel has (addresses: address_of()).
/// - 1 broadcasts a frame every step (every third one 20 bytes, so it arrives padded).
/// - 2 sends a frame to 3 every step, and takes what it receives with receive().
/// - 3 accepts only frames to itself and broadcasts (Accept::Addressed), answers each frame to
///   it from its handler in the same step, and sends a frame to a multicast group every step.
/// - 4 only receives.
/// Every node records what it receives.
class Node
{
public:
    Node(std::uint8_t number, Port& port) : number_(number), channel_(port, options_for(number))
    {
        if (channel_.transmit_capabilities() != TransmitCapabilities::Correlated)
        {
            throw std::runtime_error("expected Correlated transmit capability");
        }
        channel_.on_transmit([this](const TransmitEvent& ev) { note_tx(ev); });
        if (number_ != 2)
        {
            channel_.on_frame([this](const TimedFrame& f) { on_frame(f); });
        }
    }

    [[nodiscard]] Channel& channel() noexcept { return channel_; }
    [[nodiscard]] const std::vector<std::string>& record() const noexcept { return record_; }
    [[nodiscard]] const std::vector<std::string>& tx_record() const noexcept { return tx_record_; }

    void application(SimTime)
    {
        if (number_ == 2)
        {
            while (const auto f = channel_.receive())
            {
                note(*f);
            }
        }
        const std::uint8_t seq = seq_++;
        switch (number_)
        {
        case 1:
            send(frame_bytes(kBroadcast, address_of(1), 1, seq, seq % 3 == 0 ? 20 : 100));
            break;
        case 2:
            send(frame_bytes(address_of(3), address_of(2), 1, seq, 64));
            break;
        case 3:
            send(frame_bytes(group(1), address_of(3), 3, seq, 40));
            break;
        default:
            break;
        }
    }

private:
    static ChannelOptions options_for(std::uint8_t number)
    {
        ChannelOptions options;
        options.mac = address_of(number);
        if (number == 3)
        {
            options.accept = Accept::Addressed;
        }
        return options;
    }

    void send(const std::vector<std::uint8_t>& bytes)
    {
        const std::uint64_t token = next_token_++;
        if (channel_.send(bytes, token) != SendStatus::Ok)
        {
            throw std::runtime_error("node " + std::to_string(number_) + ": send() did not take the frame");
        }
    }

    void on_frame(const TimedFrame& f)
    {
        note(f);
        if (number_ == 3 && f.frame.destination() == address_of(3))
        {
            send(frame_bytes(f.frame.source(), address_of(3), 2, f.frame.bytes()[15], 64));
        }
    }

    void note_tx(const TransmitEvent& ev)
    {
        tx_record_.push_back("tx=" + std::string(ev.kind == TransmitEvent::Kind::Completed ? "ok" : "fail") +
                             " tok=" + std::to_string(ev.token) + " send=" + std::to_string(to_ns(ev.send_time)) +
                             " t=" + std::to_string(to_ns(ev.time)));
    }

    void note(const TimedFrame& f)
    {
        const auto bytes = f.frame.bytes();
        record_.push_back("t=" + std::to_string(to_ns(f.time)) + " from=" + f.frame.source().to_string() +
                          " to=" + f.frame.destination().to_string() + " len=" + std::to_string(bytes.size()) +
                          " tag=" + std::to_string(bytes[14]) + " seq=" + std::to_string(bytes[15]));
    }

    std::uint8_t number_;
    Channel channel_;
    std::uint8_t seq_ = 0;
    std::uint64_t next_token_ = 0; // starts at 0 to exercise token == 0 end-to-end
    std::vector<std::string> record_;
    std::vector<std::string> tx_record_;
};

void check_statistics(const Channel& channel)
{
    const auto& s = channel.statistics();
    if (s.receive_overflows != 0 || s.frames_invalid != 0 || s.frames_dropped != 0 || s.transmit_failed != 0 ||
        s.port_receive_overflows != 0 || s.port_event_overflows != 0 || s.transmit_event_overflows != 0)
    {
        throw std::runtime_error("unexpected channel statistics");
    }
    if (s.frames_sent != s.transmit_completed + s.transmit_failed + s.transmit_event_overflows +
                             s.port_event_overflows + s.outstanding)
    {
        throw std::runtime_error("outstanding conservation mismatch");
    }
}

Records run_on_virtual_bus(const std::vector<SimDuration>& steps)
{
    vehicle_sil::eth::VirtualEthernetBus bus;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<vehicle_sil::step::Host>> hosts;
    vehicle_sil::step::StepDriver driver;
    for (std::size_t i = 0; i < kNodes; ++i)
    {
        nodes.push_back(std::make_unique<Node>(static_cast<std::uint8_t>(i + 1), bus.attach()));
        hosts.push_back(std::make_unique<vehicle_sil::step::Host>());
        hosts.back()->add(nodes.back()->channel());
        Node* node = nodes.back().get();
        hosts.back()->set_application([node](SimTime now) { node->application(now); });
        driver.add(*hosts.back(), steps[i]);
    }
    driver.run_until(kEnd);
    Records records;
    for (const auto& node : nodes)
    {
        check_statistics(node->channel());
        records.push_back(node->record());
        records.push_back(node->tx_record());
    }
    return records;
}

struct SilKitNode
{
    SilKitNode() = default;
    SilKitNode(const SilKitNode&) = delete;
    SilKitNode& operator=(const SilKitNode&) = delete;
    // Shutdown order (also when a check throws): stop the runtime, close the port, then the
    // members go in reverse order (host, node, port, runtime).
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
    std::unique_ptr<SilKitEthPort> port;
    std::unique_ptr<Node> node;
    std::unique_ptr<vehicle_sil::step::Host> host;
};

Records run_on_silkit(const std::vector<SimDuration>& steps)
{
    InProcessRegistry registry;
    std::vector<std::string> names;
    for (std::size_t i = 0; i < kNodes; ++i)
    {
        names.push_back("Node" + std::to_string(i + 1));
    }
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = names;
    controllerOptions.stop_at = kEnd - SimTime{} + 20ms;
    SimulationController controller(controllerOptions);

    std::vector<SilKitNode> nodes(kNodes);
    std::vector<ParticipantRuntime*> runtimes;
    for (std::size_t i = 0; i < kNodes; ++i)
    {
        auto& n = nodes[i];
        // `end`: only steps before kEnd are handled, independent of when the stop arrives.
        n.runtime = std::make_unique<ParticipantRuntime>(
            coordinated_options(names[i], registry.uri, VirtualTimeOptions{steps[i], std::nullopt, kEnd}));
        auto* ethernet = n.runtime->participant().CreateEthernetController(kNetwork, kNetwork);
        n.port = std::make_unique<SilKitEthPort>(*ethernet);
        n.runtime->add_communication_ready_handler([p = n.port.get()] { p->start(); });
        n.node = std::make_unique<Node>(static_cast<std::uint8_t>(i + 1), *n.port);
        n.host = std::make_unique<vehicle_sil::step::Host>();
        n.host->add(n.node->channel());
        Node* node = n.node.get();
        n.host->set_application([node](SimTime now) { node->application(now); });
        vehicle_sil::bind_host(*n.runtime, *n.host);
        runtimes.push_back(n.runtime.get());
    }

    start_together(controller, runtimes, 30s);
    for (auto& n : nodes)
    {
        const auto state = n.runtime->wait_until_finished(60s);
        if (!state || *state != orch::ParticipantState::Shutdown)
        {
            throw std::runtime_error(n.runtime->name() + " did not shut down: " + n.runtime->step_error());
        }
    }

    Records records;
    for (auto& n : nodes)
    {
        const auto stats = n.port->statistics();
        if (stats.receive_overflows != 0 || stats.receive_rejected != 0 || stats.send_failed != 0 ||
            stats.event_overflows != 0 || stats.received_without_time != 0)
        {
            throw std::runtime_error("unexpected port statistics");
        }
        if (n.host->failed())
        {
            throw std::runtime_error("host failed: " + n.host->error());
        }
        check_statistics(n.node->channel());
        n.port->close();
        records.push_back(n.node->record());
        records.push_back(n.node->tx_record());
    }
    return records;
}

void print_first_difference(const Records& expected, const Records& actual)
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

struct SameTime
{
    std::size_t senders_differ = 0; ///< lines whose time equals the one before, from another sender
    std::size_t out_of_order = 0;   ///< of those, lines from a sender with a lower address
};

/// Frames with equal times in `record`: how often the sender changes, and how often it goes
/// down instead of up (the channel orders them by the sender's address; the address strings
/// compare like the bytes).
SameTime same_time_senders(const std::vector<std::string>& record)
{
    SameTime result;
    for (std::size_t i = 1; i < record.size(); ++i)
    {
        const auto time = [](const std::string& line) { return line.substr(0, line.find(' ')); };
        const auto from = [](const std::string& line) { return line.substr(line.find("from="), 22); };
        if (time(record[i]) == time(record[i - 1]) && from(record[i]) != from(record[i - 1]))
        {
            ++result.senders_differ;
            result.out_of_order += from(record[i]) < from(record[i - 1]) ? 1 : 0;
        }
    }
    return result;
}

void check_same_records(const std::vector<SimDuration>& steps)
{
    const Records reference = run_on_virtual_bus(steps);
    std::size_t sameTime = 0;
    std::size_t padded = 0;
    std::size_t replies = 0;
    std::size_t txEvents = 0;
    for (std::size_t n = 0; n < kNodes; ++n)
    {
        const auto& record = reference[n * 2];
        const auto& txRecord = reference[n * 2 + 1];
        CHECK(!record.empty());
        if (n < 3)
        {
            CHECK(!txRecord.empty());
            CHECK(txRecord[0].find("tok=0 ") != std::string::npos);
            txEvents += txRecord.size();
        }
        else
        {
            CHECK(txRecord.empty());
        }
        const SameTime same = same_time_senders(record);
        sameTime += same.senders_differ;
        CHECK_EQ(same.out_of_order, 0u); // fails on the virtual network alone if the channel does not order
        for (const auto& line : record)
        {
            padded += line.find("len=60 tag=1") != std::string::npos ? 1 : 0;
            replies += line.find("tag=2") != std::string::npos ? 1 : 0;
        }
    }
    CHECK(sameTime > 0u); // frames from different senders with equal times
    CHECK(padded > 0u);   // short frames arrive padded
    CHECK(replies > 0u);  // answers sent from a handler
    CHECK(txEvents > 0u); // correlated transmit completions matched across VirtualEthernetBus & SIL Kit
    // Node 3 takes only frames to itself and broadcasts: nothing to node 3's group.
    for (const auto& line : reference[2 * 2])
    {
        CHECK(line.find("to=03:") == std::string::npos);
    }
    for (int run = 0; run < 2; ++run)
    {
        const Records silkit = run_on_silkit(steps);
        if (silkit != reference)
        {
            print_first_difference(reference, silkit);
        }
        CHECK(silkit == reference);
    }
}

} // namespace

TEST_CASE(records_match_the_virtual_network_steps_1_1_1_1)
{
    check_same_records({1ms, 1ms, 1ms, 1ms});
}

TEST_CASE(records_match_the_virtual_network_steps_1_2_3_5)
{
    check_same_records({1ms, 2ms, 3ms, 5ms});
}

TEST_CASE(records_match_the_virtual_network_steps_3_1_2_7)
{
    check_same_records({3ms, 1ms, 2ms, 7ms});
}

namespace {

namespace ns = SilKit::Experimental::NetworkSimulation;
namespace sketh = SilKit::Services::Ethernet;

struct CapturedEthRequest
{
    ns::ControllerDescriptor sender{};
    std::vector<std::uint8_t> bytes;
    void* user_context = nullptr;
};

struct EthNetSimState
{
    std::mutex mutex;
    std::unique_ptr<ns::Ethernet::IEthernetEventProducer> producer;
    std::vector<ns::ControllerDescriptor> controllers;
    std::vector<CapturedEthRequest> requests;
};

class TestSimEthController final : public ns::Ethernet::ISimulatedEthernetController
{
public:
    TestSimEthController(std::shared_ptr<EthNetSimState> state, ns::ControllerDescriptor descriptor) :
      state_(std::move(state)),
      descriptor_(descriptor)
    {
    }

    void OnFrameRequest(const ns::Ethernet::EthernetFrameRequest& request) override
    {
        const std::lock_guard lock(state_->mutex);
        state_->requests.push_back(CapturedEthRequest{
            descriptor_,
            std::vector<std::uint8_t>(request.ethernetFrame.raw.begin(), request.ethernetFrame.raw.end()),
            request.userContext,
        });
    }

    void OnSetControllerMode(const ns::Ethernet::EthernetControllerMode&) override {}

private:
    std::shared_ptr<EthNetSimState> state_;
    ns::ControllerDescriptor descriptor_;
};

class TestSimEthNetwork final : public ns::ISimulatedNetwork
{
public:
    explicit TestSimEthNetwork(std::shared_ptr<EthNetSimState> state) : state_(std::move(state)) {}

    auto ProvideSimulatedController(ns::ControllerDescriptor descriptor) -> ns::ISimulatedController* override
    {
        const std::lock_guard lock(state_->mutex);
        state_->controllers.push_back(descriptor);
        auto& slot = controllers_[descriptor];
        slot = std::make_unique<TestSimEthController>(state_, descriptor);
        return slot.get();
    }

    void SimulatedControllerRemoved(ns::ControllerDescriptor) override {}

    void SetEventProducer(std::unique_ptr<ns::IEventProducer> producer) override
    {
        const std::lock_guard lock(state_->mutex);
        state_->producer.reset(dynamic_cast<ns::Ethernet::IEthernetEventProducer*>(producer.release()));
    }

private:
    std::shared_ptr<EthNetSimState> state_;
    std::map<ns::ControllerDescriptor, std::unique_ptr<TestSimEthController>> controllers_;
};

} // namespace

TEST_CASE(network_simulator_correlates_delayed_reordered_identical_frames)
{
    // Real in-process SIL Kit INetworkSimulator (ISimulatedEthernetController): Sender emits 4
    // identical Ethernet frames in one step (tokens 0, 10, 20, 30); the simulator delays and
    // acknowledges them out of order across later steps with mixed statuses (Dropped,
    // Transmitted, LinkDown, Transmitted) and forwards only the Transmitted frames to Receiver.
    InProcessRegistry registry;
    constexpr auto kSimEnd = SimTime{} + 10ms;
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"EthNetSim", "Sender", "Receiver"};
    controllerOptions.stop_at = 15ms;
    SimulationController controller(controllerOptions);

    auto simState = std::make_shared<EthNetSimState>();
    auto simRuntime = std::make_unique<ParticipantRuntime>(
        coordinated_options("EthNetSim", registry.uri, VirtualTimeOptions{1ms, std::nullopt, kSimEnd}));
    auto* simulator = SilKit::Experimental::Participant::CreateNetworkSimulator(&simRuntime->participant());
    simulator->SimulateNetwork(kNetwork, ns::SimulatedNetworkType::Ethernet,
                               std::make_unique<TestSimEthNetwork>(simState));
    simulator->Start();

    simRuntime->set_step_handler([simState](SimTime now, SimDuration) {
        const std::lock_guard lock(simState->mutex);
        if (!simState->producer || simState->requests.size() < 4)
        {
            return;
        }
        auto emit = [&](std::size_t idx, sketh::EthernetTransmitStatus status, bool deliver) {
            const auto& req = simState->requests[idx];
            const std::chrono::nanoseconds ts(to_ns(now));
            const ns::ControllerDescriptor senderArr[1] = {req.sender};
            simState->producer->Produce(sketh::EthernetFrameTransmitEvent{ts, status, req.user_context},
                                        SilKit::Util::Span<const ns::ControllerDescriptor>(senderArr, 1));
            if (deliver)
            {
                std::vector<ns::ControllerDescriptor> others;
                for (const auto d : simState->controllers)
                {
                    if (d != req.sender)
                    {
                        others.push_back(d);
                    }
                }
                if (!others.empty())
                {
                    sketh::EthernetFrameEvent rx{};
                    rx.timestamp = ts;
                    rx.frame.raw = SilKit::Util::Span<const std::uint8_t>(req.bytes.data(), req.bytes.size());
                    rx.direction = SilKit::Services::TransmitDirection::RX;
                    rx.userContext = nullptr;
                    simState->producer->Produce(
                        rx, SilKit::Util::Span<const ns::ControllerDescriptor>(others.data(), others.size()));
                }
            }
        };
        if (now == SimTime{} + 2ms)
        {
            emit(2, sketh::EthernetTransmitStatus::Dropped, false);
        }
        else if (now == SimTime{} + 3ms)
        {
            emit(0, sketh::EthernetTransmitStatus::Transmitted, true);
        }
        else if (now == SimTime{} + 4ms)
        {
            emit(3, sketh::EthernetTransmitStatus::LinkDown, false);
        }
        else if (now == SimTime{} + 5ms)
        {
            emit(1, sketh::EthernetTransmitStatus::Transmitted, true);
        }
    });

    struct EndpointParticipant
    {
        ~EndpointParticipant()
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
        std::unique_ptr<SilKitEthPort> port;
        std::unique_ptr<Channel> channel;
        std::unique_ptr<vehicle_sil::step::Host> host;
    };

    EndpointParticipant sender;
    sender.runtime = std::make_unique<ParticipantRuntime>(
        coordinated_options("Sender", registry.uri, VirtualTimeOptions{1ms, std::nullopt, kSimEnd}));
    sender.port = std::make_unique<SilKitEthPort>(
        *sender.runtime->participant().CreateEthernetController(kNetwork, kNetwork));
    sender.runtime->add_communication_ready_handler([p = sender.port.get()] { p->start(); });
    sender.channel = std::make_unique<Channel>(*sender.port);
    sender.host = std::make_unique<vehicle_sil::step::Host>();
    sender.host->add(*sender.channel);

    std::vector<TransmitEvent> senderTx;
    sender.channel->on_transmit([&](const TransmitEvent& ev) { senderTx.push_back(ev); });
    const auto identicalBytes = frame_bytes(mac(2), mac(1), 7, 1, 64);
    sender.host->set_application([&, identicalBytes](SimTime now) {
        if (now == SimTime{} + 1ms)
        {
            for (const std::uint64_t token : {std::uint64_t{0}, std::uint64_t{10}, std::uint64_t{20}, std::uint64_t{30}})
            {
                CHECK(sender.channel->send(identicalBytes, token) == SendStatus::Ok);
            }
        }
    });
    vehicle_sil::bind_host(*sender.runtime, *sender.host);

    EndpointParticipant receiver;
    receiver.runtime = std::make_unique<ParticipantRuntime>(
        coordinated_options("Receiver", registry.uri, VirtualTimeOptions{1ms, std::nullopt, kSimEnd}));
    receiver.port = std::make_unique<SilKitEthPort>(
        *receiver.runtime->participant().CreateEthernetController(kNetwork, kNetwork));
    receiver.runtime->add_communication_ready_handler([p = receiver.port.get()] { p->start(); });
    receiver.channel = std::make_unique<Channel>(*receiver.port);
    receiver.host = std::make_unique<vehicle_sil::step::Host>();
    receiver.host->add(*receiver.channel);

    std::vector<SimTime> receivedTimes;
    receiver.channel->on_frame([&](const TimedFrame& f) { receivedTimes.push_back(f.time); });
    vehicle_sil::bind_host(*receiver.runtime, *receiver.host);

    std::vector<ParticipantRuntime*> runtimes = {simRuntime.get(), sender.runtime.get(), receiver.runtime.get()};
    start_together(controller, runtimes, 30s);
    for (ParticipantRuntime* rt : runtimes)
    {
        const auto state = rt->wait_until_finished(60s);
        CHECK(state.has_value() && *state == orch::ParticipantState::Shutdown);
    }

    CHECK(!sender.host->failed());
    CHECK(!receiver.host->failed());
    CHECK_EQ(senderTx.size(), 4u);
    CHECK((senderTx[0] == TransmitEvent{TransmitEvent::Kind::Failed, 20, SimTime{} + 1ms, SimTime{} + 2ms}));
    CHECK((senderTx[1] == TransmitEvent{TransmitEvent::Kind::Completed, 0, SimTime{} + 1ms, SimTime{} + 3ms}));
    CHECK((senderTx[2] == TransmitEvent{TransmitEvent::Kind::Failed, 30, SimTime{} + 1ms, SimTime{} + 4ms}));
    CHECK((senderTx[3] == TransmitEvent{TransmitEvent::Kind::Completed, 10, SimTime{} + 1ms, SimTime{} + 5ms}));

    const auto& chStats = sender.channel->statistics();
    CHECK_EQ(chStats.frames_sent, 4u);
    CHECK_EQ(chStats.transmit_completed, 2u);
    CHECK_EQ(chStats.transmit_failed, 2u);
    CHECK_EQ(chStats.outstanding, 0u);
    CHECK_EQ(chStats.port_event_overflows, 0u);
    CHECK_EQ(chStats.transmit_event_overflows, 0u);

    const auto portStats = sender.port->statistics();
    CHECK_EQ(portStats.outstanding, 0u);
    CHECK_EQ(portStats.tx_uncorrelated, 0u);
    CHECK_EQ(portStats.tx_transmitted, 2u);
    CHECK_EQ(portStats.tx_dropped, 1u);
    CHECK_EQ(portStats.tx_link_down, 1u);

    CHECK_EQ(receivedTimes.size(), 2u);
    CHECK(receivedTimes[0] == SimTime{} + 3ms);
    CHECK(receivedTimes[1] == SimTime{} + 5ms);

    sender.port->close();
    receiver.port->close();
    simRuntime->stop("test done");
}
