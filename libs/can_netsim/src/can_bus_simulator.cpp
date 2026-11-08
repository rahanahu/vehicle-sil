// SPDX-License-Identifier: MIT
//
// The Network Simulator API is experimental in SIL Kit; this file is the only place that uses
// it. Re-run the simulator tests after updating SIL Kit.
#include "vehicle_sil/can_netsim/can_bus_simulator.hpp"

#include "vehicle_sil/can/bus_model.hpp"
#include "vehicle_sil/can_silkit/send_time_tag.hpp"
#include "vehicle_sil/can_silkit/silkit_can_port.hpp"
#include "vehicle_sil/core/step_inbox.hpp"

#include <silkit/SilKit.hpp>
#include <silkit/experimental/netsim/all.hpp>
#include <silkit/experimental/participant/ParticipantExtensions.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::can {

namespace {

namespace ns = SilKit::Experimental::NetworkSimulation;
namespace sk = SilKit::Services::Can;

struct Request
{
    ns::ControllerDescriptor sender{};
    Frame frame{};
    void* user_context = nullptr;
};

std::string describe(const Frame& frame)
{
    char text[32];
    std::snprintf(text, sizeof(text), "0x%X", static_cast<unsigned>(frame.id));
    return text;
}

} // namespace

/// The state of one simulated CAN network, shared by SIL Kit's callbacks (any thread) and
/// step() (the step handler's thread).
class CanBusSimulator::Network
{
public:
    Network(std::string name, const CanBusSimulatorOptions& options, Log log)
      : name_(std::move(name)), options_(options), log_(std::move(log)), inbox_(options.inbox_capacity),
        model_(CanBusModelOptions{options.default_bitrate, options.transmit_queue_capacity})
    {
        bitrate_.store(options.default_bitrate, std::memory_order_relaxed);
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    // --- SIL Kit's threads -----------------------------------------------------------------

    void set_producer(std::unique_ptr<ns::IEventProducer> producer)
    {
        const std::lock_guard lock(mutex_);
        producer_ = std::move(producer);
    }

    void add_controller(ns::ControllerDescriptor descriptor)
    {
        const std::lock_guard lock(mutex_);
        controllers_[descriptor] = Controller{};
    }

    void remove_controller(ns::ControllerDescriptor descriptor)
    {
        const std::lock_guard lock(mutex_);
        const auto it = controllers_.find(descriptor);
        if (it != controllers_.end())
        {
            it->second.removed = true;
        }
    }

    void set_mode(ns::ControllerDescriptor descriptor, sk::CanControllerState state)
    {
        const std::lock_guard lock(mutex_);
        const auto it = controllers_.find(descriptor);
        if (it != controllers_.end())
        {
            it->second.pending_state = state;
        }
    }

    void set_baudrate(std::uint32_t rate)
    {
        if (rate == 0)
        {
            return;
        }
        const std::lock_guard lock(mutex_);
        if (!configured_bitrate_)
        {
            configured_bitrate_ = rate;
            pending_bitrate_ = rate;
        }
        else if (*configured_bitrate_ != rate)
        {
            bitrate_conflicts_.fetch_add(1, std::memory_order_relaxed);
            warnings_.push_back("network '" + name_ + "': a controller set " + std::to_string(rate) +
                                " bit/s, but the network runs at " + std::to_string(*configured_bitrate_) +
                                " bit/s (the first rate set); ignored");
        }
    }

    void on_request(ns::ControllerDescriptor sender, const ns::Can::CanFrameRequest& request)
    {
        requests_.fetch_add(1, std::memory_order_relaxed);
        const auto frame = from_silkit_frame(request.frame);
        if (!frame)
        {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            const std::lock_guard lock(mutex_);
            if (!warned_rejected_)
            {
                warned_rejected_ = true;
                warnings_.push_back("network '" + name_ +
                                    "': dropped a request that is not a classic CAN data frame (CAN FD, remote, "
                                    "or malformed); only classic CAN data frames are simulated");
            }
            return;
        }
        const SimTime last = SimTime{} + std::chrono::nanoseconds(last_step_ns_.load(std::memory_order_acquire));
        SimTime sent = last;
        const auto tagged = decode_send_time(request.userContext);
        // At least one step ahead: a sender may be a step ahead of the simulator's last step.
        const SimDuration limit = std::max(options_.future_limit, SimDuration{step_ns_.load(std::memory_order_relaxed)});
        if (tagged && *tagged <= last + limit)
        {
            sent = *tagged;
        }
        else
        {
            untagged_.fetch_add(1, std::memory_order_relaxed);
            const std::lock_guard lock(mutex_);
            if (!warned_untagged_)
            {
                warned_untagged_ = true;
                warnings_.push_back("network '" + name_ + "': a request without a vehicle-sil send time (frame " +
                                    describe(*frame) +
                                    "); it is placed at the simulator's last step, which is not deterministic");
            }
        }
        if (!inbox_.push(sent, Request{sender, *frame, request.userContext}))
        {
            const std::lock_guard lock(mutex_);
            warnings_.push_back("network '" + name_ + "': the request inbox is full (" +
                                std::to_string(inbox_.capacity()) + "); frame " + describe(*frame) + " dropped");
        }
    }

    // --- the step handler's thread ---------------------------------------------------------

    void step(SimTime now, SimDuration step)
    {
        last_step_ns_.store(to_ns(now), std::memory_order_release);
        std::vector<std::string> warnings;
        std::vector<std::pair<ns::ControllerDescriptor, sk::CanControllerState>> states;
        std::optional<std::uint32_t> bitrate;
        bool hasProducer = false;
        step_ns_.store((step - SimDuration::zero()).count(), std::memory_order_relaxed);
        {
            const std::lock_guard lock(mutex_);
            warnings.swap(warnings_);
            bitrate = std::exchange(pending_bitrate_, std::nullopt);
            present_.clear();
            for (auto it = controllers_.begin(); it != controllers_.end();)
            {
                Controller& c = it->second;
                if (c.removed)
                {
                    if (c.model_index)
                    {
                        // A disconnected controller transmits nothing more.
                        rejected_.fetch_add(model_.clear(*c.model_index), std::memory_order_relaxed);
                    }
                    it = controllers_.erase(it);
                    continue;
                }
                if (c.pending_state)
                {
                    states.emplace_back(it->first, *c.pending_state);
                    c.pending_state.reset();
                }
                present_.push_back(it->first);
                ++it;
            }
            controller_count_.store(present_.size(), std::memory_order_relaxed);
            // SIL Kit hands the event producer over when the first controller of the network
            // appears. Until then there is nothing to receive and no request to handle.
            hasProducer = producer_ != nullptr;
        }
        for (const auto& warning : warnings)
        {
            if (log_)
            {
                log_(LogLevel::Warning, warning);
            }
        }
        if (bitrate)
        {
            model_.set_bitrate(*bitrate);
            bitrate_.store(*bitrate, std::memory_order_relaxed);
            if (log_)
            {
                log_(LogLevel::Info, "network '" + name_ + "': " + std::to_string(*bitrate) + " bit/s (set by a controller)");
            }
        }
        if (!announced_bitrate_ && hasProducer)
        {
            announced_bitrate_ = true;
            if (!bitrate && log_)
            {
                log_(LogLevel::Info, "network '" + name_ + "': " + std::to_string(model_.bitrate()) +
                     " bit/s (the default; no controller has set one yet)");
            }
        }
        if (!hasProducer)
        {
            return;
        }
        auto& producer = static_cast<ns::Can::ICanEventProducer&>(*producer_);
        for (const auto& [descriptor, state] : states)
        {
            const std::array<ns::ControllerDescriptor, 1> to{descriptor};
            producer.Produce(sk::CanStateChangeEvent{now - SimTime{}, state}, SilKit::Util::MakeSpan(to));
        }

        inbox_.drain_before(now, [&](SimTime sent, Request& request) {
            const auto index = model_index(request.sender);
            if (!index)
            {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (!model_.submit(*index, request.frame, sent, reinterpret_cast<std::uint64_t>(request.user_context)))
            {
                queue_full_.fetch_add(1, std::memory_order_relaxed);
                sk::CanFrameTransmitEvent ack{};
                ack.canId = request.frame.id;
                ack.timestamp = now - SimTime{};
                ack.status = sk::CanTransmitStatus::TransmitQueueFull;
                ack.userContext = request.user_context;
                const std::array<ns::ControllerDescriptor, 1> to{request.sender};
                producer.Produce(ack, SilKit::Util::MakeSpan(to));
            }
        });

        model_.run(now, now + step, [&](const CanBusModel::Transmission& tx) {
            transmissions_.fetch_add(1, std::memory_order_relaxed);
            const ns::ControllerDescriptor sender = senders_[tx.sender];
            void* const userContext = reinterpret_cast<void*>(tx.context);
            sk::CanFrame frame{};
            static_cast<void>(to_silkit_frame(tx.frame, frame)); // valid: checked on arrival
            sk::CanFrameEvent event{};
            event.timestamp = tx.end - SimTime{};
            event.frame = frame;
            event.direction = SilKit::Services::TransmitDirection::RX;
            event.userContext = nullptr;
            others_.clear();
            bool senderPresent = false;
            for (const auto descriptor : present_)
            {
                if (descriptor == sender)
                {
                    senderPresent = true;
                }
                else
                {
                    others_.push_back(descriptor);
                }
            }
            if (!others_.empty())
            {
                producer.Produce(event, SilKit::Util::Span<const ns::ControllerDescriptor>(others_.data(), others_.size()));
            }
            if (senderPresent)
            {
                const std::array<ns::ControllerDescriptor, 1> to{sender};
                event.direction = SilKit::Services::TransmitDirection::TX;
                event.userContext = userContext;
                producer.Produce(event, SilKit::Util::MakeSpan(to));
                sk::CanFrameTransmitEvent ack{};
                ack.canId = tx.frame.id;
                ack.timestamp = tx.end - SimTime{};
                ack.status = sk::CanTransmitStatus::Transmitted;
                ack.userContext = userContext;
                producer.Produce(ack, SilKit::Util::MakeSpan(to));
            }
        });
    }

    [[nodiscard]] CanBusSimulatorStatistics statistics() const
    {
        CanBusSimulatorStatistics s;
        s.bitrate = bitrate_.load(std::memory_order_relaxed);
        s.controllers = controller_count_.load(std::memory_order_relaxed);
        s.requests = requests_.load(std::memory_order_relaxed);
        s.transmissions = transmissions_.load(std::memory_order_relaxed);
        s.untagged_requests = untagged_.load(std::memory_order_relaxed);
        s.rejected_requests = rejected_.load(std::memory_order_relaxed);
        s.transmit_queue_full = queue_full_.load(std::memory_order_relaxed);
        s.inbox_overflows = inbox_.overflows();
        s.bitrate_conflicts = bitrate_conflicts_.load(std::memory_order_relaxed);
        return s;
    }

private:
    struct Controller
    {
        std::optional<std::size_t> model_index;
        std::optional<sk::CanControllerState> pending_state;
        bool removed = false;
    };

    /// The bus model's sender index of a connected controller (added on its first request).
    std::optional<std::size_t> model_index(ns::ControllerDescriptor descriptor)
    {
        const std::lock_guard lock(mutex_);
        const auto it = controllers_.find(descriptor);
        if (it == controllers_.end() || it->second.removed)
        {
            return std::nullopt;
        }
        if (!it->second.model_index)
        {
            it->second.model_index = model_.add_sender();
            senders_.push_back(descriptor);
        }
        return it->second.model_index;
    }

    const std::string name_;
    const CanBusSimulatorOptions options_;
    const Log log_;

    mutable std::mutex mutex_; // guards the members up to warnings_
    std::map<ns::ControllerDescriptor, Controller> controllers_;
    std::unique_ptr<ns::IEventProducer> producer_;
    std::optional<std::uint32_t> configured_bitrate_;
    std::optional<std::uint32_t> pending_bitrate_;
    bool warned_untagged_ = false;
    bool warned_rejected_ = false;
    std::vector<std::string> warnings_;

    StepInbox<Request> inbox_; // thread safe

    // The step handler's thread only.
    CanBusModel model_;
    std::vector<ns::ControllerDescriptor> senders_; ///< model index -> controller
    std::vector<ns::ControllerDescriptor> present_; ///< connected controllers of this step
    std::vector<ns::ControllerDescriptor> others_;  ///< receivers of one transmission

    bool announced_bitrate_ = false; // the step handler's thread
    std::atomic<std::int64_t> last_step_ns_{0};
    std::atomic<std::int64_t> step_ns_{0};
    std::atomic<std::uint32_t> bitrate_{0};
    std::atomic<std::size_t> controller_count_{0};
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> transmissions_{0};
    std::atomic<std::uint64_t> untagged_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> queue_full_{0};
    std::atomic<std::uint64_t> bitrate_conflicts_{0};
};

namespace {

class SimulatedController final : public ns::Can::ISimulatedCanController
{
public:
    SimulatedController(std::shared_ptr<CanBusSimulator::Network> network, ns::ControllerDescriptor descriptor)
      : network_(std::move(network)), descriptor_(descriptor)
    {
    }

    void OnSetBaudrate(const ns::Can::CanConfigureBaudrate& baudrate) override { network_->set_baudrate(baudrate.baudRate); }
    void OnFrameRequest(const ns::Can::CanFrameRequest& request) override { network_->on_request(descriptor_, request); }
    void OnSetControllerMode(const ns::Can::CanControllerMode& mode) override { network_->set_mode(descriptor_, mode.state); }

private:
    std::shared_ptr<CanBusSimulator::Network> network_;
    ns::ControllerDescriptor descriptor_;
};

/// The object SIL Kit owns for one network; it forwards to the shared state.
class SimulatedNetwork final : public ns::ISimulatedNetwork
{
public:
    explicit SimulatedNetwork(std::shared_ptr<CanBusSimulator::Network> network) : network_(std::move(network)) {}

    auto ProvideSimulatedController(ns::ControllerDescriptor descriptor) -> ns::ISimulatedController* override
    {
        network_->add_controller(descriptor);
        const std::lock_guard lock(mutex_);
        auto& slot = controllers_[descriptor];
        slot = std::make_unique<SimulatedController>(network_, descriptor);
        return slot.get();
    }

    void SimulatedControllerRemoved(ns::ControllerDescriptor descriptor) override { network_->remove_controller(descriptor); }

    void SetEventProducer(std::unique_ptr<ns::IEventProducer> producer) override { network_->set_producer(std::move(producer)); }

private:
    std::shared_ptr<CanBusSimulator::Network> network_;
    std::mutex mutex_;
    // Kept until SIL Kit destroys the network: SIL Kit may still hold the pointers.
    std::map<ns::ControllerDescriptor, std::unique_ptr<SimulatedController>> controllers_;
};

} // namespace

CanBusSimulator::CanBusSimulator(SilKit::IParticipant& participant, const std::vector<std::string>& networks,
                                 CanBusSimulatorOptions options, Log log)
{
    if (options.default_bitrate == 0 || options.transmit_queue_capacity == 0 || options.inbox_capacity == 0 ||
        options.future_limit <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::can::CanBusSimulator: the bitrate, capacities and future limit must be > 0");
    }
    if (networks.empty())
    {
        throw std::invalid_argument("vehicle_sil::can::CanBusSimulator: no network to simulate");
    }
    std::set<std::string> names;
    for (const auto& name : networks)
    {
        if (name.empty() || !names.insert(name).second)
        {
            throw std::invalid_argument("vehicle_sil::can::CanBusSimulator: network names must be non-empty and unique");
        }
    }
    for (const auto& name : networks)
    {
        networks_.push_back(std::make_shared<Network>(name, options, log));
    }
    auto* simulator = SilKit::Experimental::Participant::CreateNetworkSimulator(&participant);
    for (const auto& network : networks_)
    {
        simulator->SimulateNetwork(network->name(), ns::SimulatedNetworkType::CAN, std::make_unique<SimulatedNetwork>(network));
    }
    simulator->Start();
}

CanBusSimulator::~CanBusSimulator() = default;

void CanBusSimulator::step(SimTime now, SimDuration step)
{
    if (step <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::can::CanBusSimulator: the step size must be > 0");
    }
    for (const auto& network : networks_)
    {
        network->step(now, step);
    }
}

std::vector<std::string> CanBusSimulator::networks() const
{
    std::vector<std::string> out;
    for (const auto& network : networks_)
    {
        out.push_back(network->name());
    }
    return out;
}

CanBusSimulatorStatistics CanBusSimulator::statistics(std::string_view network) const
{
    for (const auto& n : networks_)
    {
        if (n->name() == network)
        {
            return n->statistics();
        }
    }
    throw std::invalid_argument("vehicle_sil::can::CanBusSimulator: '" + std::string(network) + "' is not simulated");
}

} // namespace vehicle_sil::can
