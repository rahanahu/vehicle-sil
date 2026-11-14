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

#include <silkit/services/ethernet/all.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
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
        if (number_ != 2)
        {
            channel_.on_frame([this](const TimedFrame& f) { on_frame(f); });
        }
    }

    [[nodiscard]] Channel& channel() noexcept { return channel_; }
    [[nodiscard]] const std::vector<std::string>& record() const noexcept { return record_; }

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
        if (channel_.send(bytes) != SendStatus::Ok)
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
    std::vector<std::string> record_;
};

void check_statistics(const Channel& channel)
{
    const auto& s = channel.statistics();
    if (s.receive_overflows != 0 || s.frames_invalid != 0 || s.frames_dropped != 0 || s.transmit_failed != 0 ||
        s.port_receive_overflows != 0)
    {
        throw std::runtime_error("unexpected channel statistics");
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
    for (const auto& record : reference)
    {
        CHECK(!record.empty());
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
    // Node 3 takes only frames to itself and broadcasts: nothing to node 3's group.
    for (const auto& line : reference[2])
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
