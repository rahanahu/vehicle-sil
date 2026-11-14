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
    std::uint64_t cancel_generation = 0; ///< the sender's, when the request arrived
};

std::string describe(const Frame& frame)
{
    char text[32];
    std::snprintf(text, sizeof(text), "0x%X", static_cast<unsigned>(frame.id));
    return text;
}

/// A mode other than Started, as the warning of a dropped frame says it.
const char* describe(sk::CanControllerState state)
{
    switch (state)
    {
    case sk::CanControllerState::Stopped:
        return "stopped (Stop())";
    case sk::CanControllerState::Sleep:
        return "asleep (Sleep())";
    case sk::CanControllerState::Uninit:
        return "reset (Reset()) and not started again";
    default:
        return "not started";
    }
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

    void set_mode(ns::ControllerDescriptor descriptor, sk::CanControllerState state, bool cancelTransmitRequests)
    {
        const std::lock_guard lock(mutex_);
        const auto it = controllers_.find(descriptor);
        if (it != controllers_.end())
        {
            it->second.pending_state = state;
            // In force for the requests that arrive from now on: SIL Kit delivers a controller's
            // mode changes and requests in the order of its calls.
            it->second.state = state;
            if (cancelTransmitRequests)
            {
                ++it->second.cancel_generation;
                it->second.cancel_pending = true;
            }
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
        std::uint64_t cancelGeneration = 0;
        {
            const std::lock_guard lock(mutex_);
            const auto it = controllers_.find(sender);
            if (it != controllers_.end())
            {
                // Only a mode the simulator was told: a controller it heard nothing of may have
                // started before it found the simulator (SIL Kit does not send that again).
                if (it->second.state && *it->second.state != sk::CanControllerState::Started)
                {
                    rejected_.fetch_add(1, std::memory_order_relaxed);
                    // Once per controller, also after a Start() and another Stop(): a tool that
                    // changes its mode again and again must not fill the log.
                    if (!it->second.warned_not_started)
                    {
                        it->second.warned_not_started = true;
                        const std::string number = std::to_string(sender);
                        warnings_.push_back("network '" + name_ + "': dropped frame " + describe(*frame) + " of controller " +
                                            number + ", which is " + describe(*it->second.state) +
                                            "; the frame gets no acknowledgement. Call Start() on that controller before "
                                            "it sends. Controller " + number +
                                            " is the number SIL Kit gave it: the simulator numbers the controllers it "
                                            "finds from 1, over all networks. This is said once per controller; the "
                                            "later frames are only counted (rejected)");
                    }
                    return;
                }
                cancelGeneration = it->second.cancel_generation;
            }
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
        if (!inbox_.push(sent, Request{sender, *frame, request.userContext, cancelGeneration}))
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
        std::vector<std::pair<ns::ControllerDescriptor, std::size_t>> cancelled; // controller, model index
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
                if (std::exchange(c.cancel_pending, false) && c.model_index)
                {
                    cancelled.emplace_back(it->first, *c.model_index);
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

        // A refusal: the acknowledgement that tells the sender what became of its request.
        const auto refuse = [&](ns::ControllerDescriptor sender, std::uint32_t id, void* userContext, sk::CanTransmitStatus status) {
            sk::CanFrameTransmitEvent ack{};
            ack.canId = id;
            ack.timestamp = now - SimTime{};
            ack.status = status;
            ack.userContext = userContext;
            const std::array<ns::ControllerDescriptor, 1> to{sender};
            producer.Produce(ack, SilKit::Util::MakeSpan(to));
        };

        // Cancelled transmit requests (Reset()): the frames that wait for the bus go, before this
        // step decides its transmissions.
        for (const auto& [descriptor, index] : cancelled)
        {
            const auto removed = model_.clear(index, [&, sender = descriptor](const CanBusModel::Removed& r) {
                refuse(sender, r.frame.id, reinterpret_cast<void*>(r.context), sk::CanTransmitStatus::Canceled);
            });
            rejected_.fetch_add(removed, std::memory_order_relaxed);
        }

        inbox_.drain_before(now, [&](SimTime sent, Request& request) {
            const auto accepted = accept(request.sender, request.cancel_generation);
            if (!accepted.index)
            {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                if (accepted.cancelled)
                {
                    // Sent before the controller cancelled its requests, taken only now.
                    refuse(request.sender, request.frame.id, request.user_context, sk::CanTransmitStatus::Canceled);
                }
                return;
            }
            if (!model_.submit(*accepted.index, request.frame, sent, reinterpret_cast<std::uint64_t>(request.user_context)))
            {
                queue_full_.fetch_add(1, std::memory_order_relaxed);
                refuse(request.sender, request.frame.id, request.user_context, sk::CanTransmitStatus::TransmitQueueFull);
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
        std::optional<sk::CanControllerState> pending_state; ///< to acknowledge at the next step
        std::optional<sk::CanControllerState> state;         ///< the last mode the controller told; none: not heard
        std::uint64_t cancel_generation = 0;                 ///< counts the controller's cancellations of its requests
        bool cancel_pending = false;                         ///< a cancellation the next step applies to the model
        bool warned_not_started = false;                     ///< said once that its frames are dropped
        bool removed = false;
    };

    struct Accepted
    {
        std::optional<std::size_t> index; ///< the bus model's sender index; none: the request is dropped
        bool cancelled = false;           ///< dropped because the controller cancelled its requests after it
    };

    /// Whether a request that arrived at the sender's `cancelGeneration` goes to the bus model,
    /// with the model's sender index of the controller (added on its first request). Not for a
    /// controller that is gone.
    Accepted accept(ns::ControllerDescriptor descriptor, std::uint64_t cancelGeneration)
    {
        const std::lock_guard lock(mutex_);
        const auto it = controllers_.find(descriptor);
        if (it == controllers_.end() || it->second.removed)
        {
            return {};
        }
        if (it->second.cancel_generation != cancelGeneration)
        {
            return {std::nullopt, true};
        }
        if (!it->second.model_index)
        {
            it->second.model_index = model_.add_sender();
            senders_.push_back(descriptor);
        }
        return {it->second.model_index, false};
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
    void OnSetControllerMode(const ns::Can::CanControllerMode& mode) override
    {
        // The other flag asks to reset the error handling: error states are not modelled.
        const bool cancel =
            (mode.canControllerModeFlags & SilKit_Experimental_NetSim_CanControllerModeFlags_CancelTransmitRequests) != 0;
        network_->set_mode(descriptor_, mode.state, cancel);
    }

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
