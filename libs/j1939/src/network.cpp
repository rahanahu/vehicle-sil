// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/j1939/observer.hpp"

#include "frame_seat.hpp"
#include "transport.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::j1939 {

namespace {

constexpr SimDuration kStepSizeWarning = std::chrono::milliseconds(50);
constexpr SimDuration kStepSizeLimit = std::chrono::milliseconds(200);

[[nodiscard]] std::size_t transport_sessions(const NetworkOptions& o) noexcept
{
    return o.bam_receive_sessions + o.rts_receive_sessions + o.bam_transmit_sessions + o.rts_transmit_sessions;
}

/// Control frames one step can produce: about one per received frame, plus per node its timers
/// and what it held during the wait after its claim, plus per transport session and queued
/// response one. Twice that leaves room for a port that is full for a step.
[[nodiscard]] std::size_t control_capacity(const NetworkOptions& o) noexcept
{
    return (o.receive_capacity + o.max_nodes * (o.hold_capacity + 2) + transport_sessions(o) + o.response_queue_capacity) * 2;
}

[[nodiscard]] Address source_of(const can::Frame& frame) noexcept { return static_cast<Address>(frame.id & 0xFFu); }

/// Source 255 is never valid. A node without an address (254) may only send Address Claimed
/// (Cannot Claim) and Request (for Address Claimed); its other frames are ignored.
[[nodiscard]] bool accepted_source(const can::Frame& frame) noexcept
{
    const Address source = source_of(frame);
    if (source == kGlobalAddress)
    {
        return false;
    }
    if (source == kNullAddress)
    {
        const Pgn pgn = Identifier::decode(frame.id).pgn;
        return pgn == kPgnAddressClaimed || pgn == kPgnRequest;
    }
    return true;
}

/// A Request for Address Claimed (answered with the claim, so not an address violation).
[[nodiscard]] bool is_request_for_address_claimed(const Message& message) noexcept
{
    return message.pgn == kPgnRequest && message.data.size() >= 3 &&
           Pgn{static_cast<std::uint32_t>(message.data[0]) | (static_cast<std::uint32_t>(message.data[1]) << 8) |
               (static_cast<std::uint32_t>(message.data[2]) << 16)} == kPgnAddressClaimed;
}

} // namespace

std::optional<Name> AddressTable::name_at(Address address) const noexcept
{
    if (!is_unicast(address) || !used_[address])
    {
        return std::nullopt;
    }
    return names_[address];
}

std::optional<Address> AddressTable::address_of(Name name) const noexcept
{
    for (std::size_t a = 0; a < kCapacity; ++a)
    {
        if (used_[a] && names_[a] == name)
        {
            return static_cast<Address>(a);
        }
    }
    return std::nullopt;
}

void AddressTable::set(Address address, Name name) noexcept
{
    if (!is_unicast(address))
    {
        return;
    }
    if (!used_[address])
    {
        used_[address] = true;
        ++size_;
    }
    names_[address] = name;
}

void AddressTable::erase(Address address) noexcept
{
    if (is_unicast(address) && used_[address])
    {
        used_[address] = false;
        --size_;
    }
}

bool Network::Queue::push(const Outgoing& item) noexcept
{
    if (full())
    {
        return false;
    }
    items_[(head_ + size_) % items_.size()] = item;
    ++size_;
    return true;
}

void Network::Queue::pop() noexcept
{
    head_ = (head_ + 1) % items_.size();
    --size_;
}

Network::Network(NetworkOptions options)
  : options_(std::move(options)),
    alive_(std::make_shared<const bool>(true)),
    // Each transport session runs at most one timer, on top of the reserved ones (address
    // claiming, Timers).
    timers_(options_.timer_capacity + transport_sessions(options_), options_.timer_capacity),
    // The queue empties at every flush. Several local nodes answering the same request can
    // exceed it (dropped and counted).
    control_(control_capacity(options_)),
    data_(options_.data_queue_capacity)
{
    if (options_.receive_capacity == 0 || options_.data_queue_capacity == 0 || options_.max_frames_per_step == 0)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Network: receive_capacity, data_queue_capacity and "
                                    "max_frames_per_step must be > 0");
    }
    if (!is_unicast(options_.arbitrary_address_first) || !is_unicast(options_.arbitrary_address_last) ||
        options_.arbitrary_address_first > options_.arbitrary_address_last)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Network: the arbitrary address range must be within 0..253 "
                                    "with first <= last");
    }
    if (options_.cts_window == 0 || options_.rts_max_packets_per_cts == 0)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Network: cts_window and rts_max_packets_per_cts must be > 0");
    }
    if (options_.bam_packet_interval < std::chrono::milliseconds(10) ||
        options_.bam_packet_interval > std::chrono::milliseconds(200))
    {
        throw std::invalid_argument("vehicle_sil::j1939::Network: bam_packet_interval must be 10 .. 200 ms");
    }
    transport_ = std::make_unique<Transport>(*this, options_);
    nodes_.reserve(options_.max_nodes);
    // Data frames and transport data packets share max_frames_per_step.
    const std::size_t loopbackCapacity = control_capacity(options_) + options_.max_frames_per_step;
    loopback_.reserve(loopbackCapacity);
    const std::size_t receivedCapacity = options_.receive_capacity + loopbackCapacity;
    received_.reserve(receivedCapacity);
    order_.reserve(receivedCapacity);
    next_in_sender_.resize(receivedCapacity);
    active_senders_.reserve(256);
    std::fill(std::begin(sender_head_), std::end(sender_head_), -1);
    std::fill(std::begin(sender_tail_), std::end(sender_tail_), -1);
}

Network::~Network()
{
    // The parts first, while the nodes and the timers they use still exist; last attached first
    // (a part may use one attached before it). No handler is called. A part destroyed here may
    // attach another one: it is destroyed in turn.
    while (!attached_.empty())
    {
        const Attached part = attached_.back();
        attached_.pop_back();
        part.destroy(part.part);
    }
    if (observer_ != nullptr)
    {
        observer_->network_ = nullptr;
        observer_ = nullptr;
    }
    if (seat_ != nullptr)
    {
        seat_->network_ = nullptr;
        seat_ = nullptr;
    }
    // Subscriptions and Timers that outlive the network do nothing from now on.
    alive_.reset();
}

Observer::~Observer()
{
    if (network_ != nullptr)
    {
        network_->observer_ = nullptr;
    }
}

void Network::set_observer(Observer* observer)
{
    if (phase_ != Phase::Failed)
    {
        require_not_observing("set_observer()");
        if (phase_ != Phase::Idle || dispatching_)
        {
            throw std::logic_error("vehicle_sil::j1939::Network: set_observer() is only allowed outside a step");
        }
    }
    if (observer == observer_)
    {
        return;
    }
    if (observer != nullptr && observer->network_ != nullptr)
    {
        // Taken from another network: only while that one is between steps too (it would
        // otherwise lose the observer in the middle of what it reports).
        const Network& other = *observer->network_;
        if (other.phase_ != Phase::Failed && (other.phase_ != Phase::Idle || other.dispatching_ || other.observing_))
        {
            throw std::logic_error("vehicle_sil::j1939::Network: set_observer(): the observer watches a network that is in a step");
        }
    }
    if (observer_ != nullptr)
    {
        observer_->network_ = nullptr;
    }
    observer_ = observer;
    if (observer_ != nullptr)
    {
        if (observer_->network_ != nullptr)
        {
            observer_->network_->observer_ = nullptr;
        }
        observer_->network_ = this;
    }
}

void Network::observe(const Event& event)
{
    if (observer_ == nullptr)
    {
        return;
    }
    const bool outer = observing_;
    observing_ = true;
    try
    {
        observer_->on_event(event);
    }
    catch (const std::exception& e)
    {
        observing_ = outer;
        fail(std::string("vehicle_sil::j1939::Network: the observer threw: ") + e.what());
        throw;
    }
    catch (...)
    {
        observing_ = outer;
        fail("vehicle_sil::j1939::Network: the observer threw");
        throw;
    }
    observing_ = outer;
}

namespace detail {

FrameSeat::~FrameSeat()
{
    if (network_ != nullptr)
    {
        network_->seat_ = nullptr;
    }
}

void FrameSeat::sit(Network& network)
{
    network.set_seat(this);
}

void FrameSeat::leave()
{
    if (network_ != nullptr)
    {
        network_->set_seat(nullptr);
    }
}

} // namespace detail

void Network::set_seat(detail::FrameSeat* seat)
{
    if (phase_ != Phase::Failed)
    {
        require_not_observing("adding or removing a Listener");
        if (phase_ != Phase::Idle || dispatching_)
        {
            throw std::logic_error("vehicle_sil::j1939::Network: a Listener is only added or removed outside a step "
                                   "(create it before the simulation starts, next to the nodes)");
        }
    }
    if (seat == seat_)
    {
        return;
    }
    if (seat != nullptr && seat_ != nullptr)
    {
        throw std::logic_error("vehicle_sil::j1939::Network: this Network already has a Listener (a Network has one); "
                               "add the handlers to that Listener");
    }
    if (seat != nullptr && seat->network_ != nullptr)
    {
        throw std::logic_error("vehicle_sil::j1939::Network: the Listener already listens to another Network; "
                               "create one Listener per Network");
    }
    if (seat_ != nullptr)
    {
        seat_->network_ = nullptr;
    }
    seat_ = seat;
    if (seat_ != nullptr)
    {
        seat_->network_ = this;
    }
}

template <typename F>
void Network::tell_seat(F&& call)
{
    if (seat_ == nullptr)
    {
        return;
    }
    const bool outer = observing_;
    const bool outerSeat = in_seat_;
    observing_ = true;
    in_seat_ = true;
    try
    {
        call(*seat_);
    }
    catch (const std::exception& e)
    {
        observing_ = outer;
        in_seat_ = outerSeat;
        fail(std::string("vehicle_sil::j1939::Network: a Listener handler threw: ") + e.what());
        throw;
    }
    catch (...)
    {
        observing_ = outer;
        in_seat_ = outerSeat;
        fail("vehicle_sil::j1939::Network: a Listener handler threw");
        throw;
    }
    observing_ = outer;
    in_seat_ = outerSeat;
    // The sitter made the network fail without throwing (it caught the exception, or used a
    // path that cannot throw): stop here, as if it had thrown.
    throw_if_failed_during_step();
}

std::string Network::read_only_text(const char* what) const
{
    if (in_seat_)
    {
        return std::string("vehicle_sil::j1939::Network: ") + what +
               " called from a Listener handler (handlers only read: update variables there and send from on_step)";
    }
    return std::string("vehicle_sil::j1939::Network: ") + what + " called from an observer (observers are read only)";
}

void Network::require_not_observing(const char* what)
{
    if (observing_)
    {
        const std::string text = read_only_text(what);
        fail(text);
        throw std::logic_error(text);
    }
}

bool Network::fail_if_observing(const char* what) noexcept
{
    if (!observing_)
    {
        return false;
    }
    try
    {
        fail(read_only_text(what));
    }
    catch (...)
    {
        fail(std::string{});
    }
    return true;
}

void Network::fail(const std::string& error)
{
    if (phase_ != Phase::Failed)
    {
        phase_ = Phase::Failed;
        error_ = error;
    }
}

template <typename F>
void Network::guarded(F&& f)
{
    try
    {
        f();
    }
    catch (const std::exception& e)
    {
        dispatching_ = false;
        event_time_.reset();
        fail(e.what());
        throw;
    }
    catch (...)
    {
        dispatching_ = false;
        event_time_.reset();
        fail("unknown exception");
        throw;
    }
}

void Network::require_not_failed() const
{
    if (phase_ == Phase::Failed)
    {
        throw std::logic_error("vehicle_sil::j1939::Network: the network has failed: " + error_);
    }
}

void Network::require_not_reentrant(const char* what)
{
    require_not_observing(what);
    if (dispatching_)
    {
        const std::string text = std::string("vehicle_sil::j1939::Network: ") + what +
                                 " called from a handler, a request responder or a timer";
        fail(text);
        throw std::logic_error(text);
    }
}

void Network::require_in_step(const char* what)
{
    require_not_observing(what);
    require_not_failed();
    if (phase_ == Phase::Idle || phase_ == Phase::Flushing)
    {
        throw std::logic_error(std::string("vehicle_sil::j1939::Network: ") + what +
                               " is only allowed inside a step (a handler, a responder, a timer or the application stage)");
    }
}

void Network::warn(std::string_view text) const
{
    if (options_.warning)
    {
        options_.warning(text);
    }
    else
    {
        std::cerr << "vehicle_sil::j1939: " << text << '\n';
    }
}

Node& Network::add_node(const NodeOptions& options)
{
    require_not_reentrant("add_node()");
    require_not_failed();
    if (phase_ == Phase::Processing || phase_ == Phase::Flushing)
    {
        throw std::logic_error("vehicle_sil::j1939::Network: add_node() is only allowed outside a step or in the "
                               "application stage");
    }
    if (nodes_.size() >= options_.max_nodes)
    {
        throw std::length_error("vehicle_sil::j1939::Network: too many nodes (max_nodes)");
    }
    const bool anyAddress = options.address == kNullAddress && options.name && options.name->arbitrary_address_capable();
    if (!is_unicast(options.address) && !anyAddress)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Network: a node address must be 0..253 (254 only for an "
                                    "arbitrary address capable NAME)");
    }
    // Each claiming node may run two timers (the wait after the claim, Cannot Claim).
    const std::size_t timers = options.name ? 2u : 0u;
    if (!timers_.reserve(timers))
    {
        throw std::invalid_argument("vehicle_sil::j1939::Network: timer_capacity is too small for the claiming nodes "
                                    "and the Timers");
    }
    try
    {
        nodes_.push_back(std::unique_ptr<Node>(new Node(*this, nodes_.size(), options)));
    }
    catch (...)
    {
        timers_.release(timers);
        throw;
    }
    // Added during a step (the application stage, or a handler of another network of the host):
    // the node takes part from the next begin(), whatever stage this network is in.
    nodes_.back()->active_ = phase_ == Phase::Idle;
    return *nodes_.back();
}

void Network::start(std::optional<SimDuration> step_size)
{
    require_not_reentrant("start()");
    require_not_failed();
    guarded([&] {
        if (!step_size)
        {
            return;
        }
        const auto ms = std::chrono::duration<double, std::milli>(*step_size).count();
        if (*step_size > kStepSizeLimit)
        {
            throw std::invalid_argument("vehicle_sil::j1939::Network: step size " + std::to_string(ms) +
                                        " ms is above 200 ms; J1939 cannot keep its 200 ms response and packet intervals");
        }
        if (*step_size > kStepSizeWarning)
        {
            warn("step size " + std::to_string(ms) +
                 " ms is above 50 ms; responses and transport packets are delayed by up to one step (10 ms or less "
                 "is recommended)");
        }
    });
}

void Network::begin(SimTime now)
{
    require_not_reentrant("begin()");
    require_not_failed();
    guarded([&] {
        if (phase_ != Phase::Idle)
        {
            throw std::logic_error("vehicle_sil::j1939::Network: begin() before the previous step was flushed");
        }
        if (now_ && now <= *now_)
        {
            throw std::logic_error("vehicle_sil::j1939::Network: step time must increase (got t=" +
                                   std::to_string(to_ns(now)) + " ns after t=" + std::to_string(to_ns(*now_)) + " ns)");
        }
        now_ = now;
        for (auto& node : nodes_)
        {
            node->active_ = true;
        }
        phase_ = Phase::Receiving;
    });
}

void Network::on_port_event(const can::PortEvent& event)
{
    require_not_observing("on_port_event()");
    switch (event.kind)
    {
    case can::PortEvent::Kind::TransmitCompleted:
        ++statistics_.frames_transmitted;
        break;
    case can::PortEvent::Kind::TransmitFailed:
        ++statistics_.transmit_failures;
        break;
    case can::PortEvent::Kind::ReceiveOverflow:
        ++statistics_.port_overflow_events;
        statistics_.port_receive_overflows += event.count;
        break;
    case can::PortEvent::Kind::BusStateChanged:
        ++statistics_.bus_state_changes;
        bus_state_ = event.bus_state;
        break;
    case can::PortEvent::Kind::ErrorFrame:
        ++statistics_.error_frames;
        break;
    }
    if (observed())
    {
        Event e;
        e.kind = EventKind::PortEvent;
        e.time = event.time;
        e.port_event = event;
        observe(e);
    }
}

void Network::receive(const can::TimedFrame& frame)
{
    require_not_reentrant("receive()");
    require_not_failed();
    guarded([&] {
        if (phase_ != Phase::Receiving)
        {
            throw std::logic_error("vehicle_sil::j1939::Network: receive() is only allowed between begin() and process()");
        }
        if (frame.time >= *now_)
        {
            throw std::logic_error("vehicle_sil::j1939::Network: receive() got a frame that is not before the step time");
        }
        if (frame.frame.format != can::IdFormat::Extended || !can::is_valid(frame.frame) || !accepted_source(frame.frame))
        {
            ++statistics_.frames_ignored;
            return;
        }
        if (external_received_ >= options_.receive_capacity)
        {
            ++statistics_.receive_overflows;
            return;
        }
        received_.push_back(Received{frame.time, static_cast<std::uint32_t>(received_.size()), -1, frame.frame});
        ++external_received_;
        ++statistics_.frames_received;
    });
}

void Network::process()
{
    require_not_reentrant("process()");
    require_not_failed();
    guarded([&] {
        if (phase_ != Phase::Receiving)
        {
            throw std::logic_error("vehicle_sil::j1939::Network: process() is only allowed once, after begin()");
        }
        phase_ = Phase::Processing;
        // Local loopback after the external frames: for equal times and the same source address,
        // external frames come first.
        for (auto& looped : loopback_)
        {
            looped.arrival = static_cast<std::uint32_t>(received_.size());
            received_.push_back(looped);
        }
        loopback_.clear();
        order_received();
        for (const std::uint32_t index : order_)
        {
            fire_timers_before(received_[index].time);
            dispatch(received_[index]);
        }
        // The seat before the timers due at the step time: what a listener concludes from the
        // step's frames is there when the application's timers run.
        tell_seat([&](detail::FrameSeat& seat) { seat.on_step_end(*now_); });
        fire_due_timers();
        // Nodes start claiming after this step's frames: those were sent before the claim.
        for (auto& node : nodes_)
        {
            if (node->active_ && node->state_ == AddressState::NotStarted)
            {
                dispatching_ = true;
                node->start_claim();
                dispatching_ = false;
                throw_if_failed_during_step();
                apply_pending();
            }
        }
        received_.clear();
        order_.clear();
        external_received_ = 0;
        phase_ = Phase::Application;
    });
}

void Network::order_received()
{
    std::sort(received_.begin(), received_.end(), [](const Received& a, const Received& b) {
        return a.time != b.time ? a.time < b.time : a.arrival < b.arrival;
    });
    order_.clear();
    const std::size_t n = received_.size();
    std::size_t first = 0;
    while (first < n)
    {
        std::size_t last = first + 1;
        while (last < n && received_[last].time == received_[first].time)
        {
            ++last;
        }
        if (last - first == 1)
        {
            order_.push_back(static_cast<std::uint32_t>(first));
            first = last;
            continue;
        }
        // Equal times: one list per sender (source address) in arrival order, merged by taking
        // the list head with the lowest identifier each time.
        active_senders_.clear();
        for (std::size_t k = first; k < last; ++k)
        {
            const Address sa = source_of(received_[k].frame);
            next_in_sender_[k] = -1;
            if (sender_head_[sa] < 0)
            {
                sender_head_[sa] = static_cast<std::int32_t>(k);
                active_senders_.push_back(sa);
            }
            else
            {
                next_in_sender_[static_cast<std::size_t>(sender_tail_[sa])] = static_cast<std::int32_t>(k);
            }
            sender_tail_[sa] = static_cast<std::int32_t>(k);
        }
        while (!active_senders_.empty())
        {
            std::size_t best = 0;
            for (std::size_t a = 1; a < active_senders_.size(); ++a)
            {
                const auto head = static_cast<std::size_t>(sender_head_[active_senders_[a]]);
                const auto bestHead = static_cast<std::size_t>(sender_head_[active_senders_[best]]);
                if (received_[head].frame.id < received_[bestHead].frame.id)
                {
                    best = a;
                }
            }
            const Address sa = active_senders_[best];
            const auto k = static_cast<std::size_t>(sender_head_[sa]);
            order_.push_back(static_cast<std::uint32_t>(k));
            sender_head_[sa] = next_in_sender_[k];
            if (sender_head_[sa] < 0)
            {
                sender_tail_[sa] = -1;
                active_senders_[best] = active_senders_.back();
                active_senders_.pop_back();
            }
        }
        first = last;
    }
}

void Network::dispatch(const Received& received)
{
    tell_seat([&](detail::FrameSeat& seat) { seat.on_frame(received.time, received.frame, received.sender >= 0); });
    const Identifier id = Identifier::decode(received.frame.id);
    const Message message{id.pgn, id.priority, id.source, id.destination, received.time, received.frame.payload()};
    if (id.pgn == kPgnTransportControl || id.pgn == kPgnTransportData)
    {
        // Taken by the transport protocol, not handed to the handlers.
        dispatching_ = true;
        check_violations(message, received.sender);
        transport_->on_frame(message, received.sender);
        dispatching_ = false;
        throw_if_failed_during_step();
        apply_pending();
        return;
    }
    dispatch_message(message, received.sender);
}

namespace {

/// Another sender with this node's address. A full Address Claimed is contention (handled by the
/// node) and a Request for Address Claimed is answered with the claim anyway.
[[nodiscard]] bool violates(const Message& message, const Node& node, bool claiming) noexcept
{
    return message.source == node.address() && node.name() && claiming &&
           !(message.pgn == kPgnAddressClaimed && message.data.size() >= 8) && !is_request_for_address_claimed(message);
}

} // namespace

void Network::check_violations(const Message& message, std::int32_t sender)
{
    for (std::size_t i = 0; i < nodes_.size(); ++i)
    {
        Node& node = *nodes_[i];
        if (static_cast<std::int32_t>(i) != sender && node.active_ && violates(message, node, node.holds_address()))
        {
            ++statistics_.address_violations;
            node.handle_violation(message.time);
        }
    }
}

SimTime Network::event_time() const noexcept
{
    return event_time_ ? *event_time_ : now_.value_or(SimTime{});
}

void Network::dispatch_message(const Message& message, std::int32_t sender, bool check_violation)
{
    // Also called from the transport protocol while it handles a frame (nested).
    const bool outer = dispatching_;
    const std::optional<SimTime> outerTime = event_time_;
    dispatching_ = true;
    event_time_ = message.time;
    if (observed())
    {
        Event e;
        e.kind = EventKind::MessageReceived;
        e.time = message.time;
        e.node = sender;
        e.local = sender >= 0;
        e.pgn = message.pgn;
        e.priority = message.priority;
        e.source = message.source;
        e.destination = message.destination;
        e.size = message.data.size();
        e.data = message.data;
        observe(e);
    }
    tell_seat([&](detail::FrameSeat& seat) { seat.on_message(message, sender >= 0); });
    if (message.pgn == kPgnAddressClaimed)
    {
        if (message.data.size() < 8)
        {
            ++statistics_.address_claims_malformed;
        }
        else if (sender < 0)
        {
            // Before the nodes see the claim: a node that loses to it and moves must not choose
            // the address it lost.
            update_address_table(message);
        }
    }
    const std::size_t count = nodes_.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        if (static_cast<std::int32_t>(i) == sender)
        {
            continue;
        }
        Node& node = *nodes_[i];
        if (!node.active_)
        {
            continue;
        }
        if (check_violation && violates(message, node, node.holds_address()))
        {
            ++statistics_.address_violations;
            node.handle_violation(message.time);
        }
        // A node without an address (254) takes only frames to all nodes.
        if (message.destination != kGlobalAddress && (message.destination != node.address_ || !is_unicast(node.address_)))
        {
            continue;
        }
        node.deliver(message);
    }
    dispatching_ = outer;
    event_time_ = outerTime;
    if (!outer)
    {
        throw_if_failed_during_step();
        apply_pending();
    }
}

void Network::table_erase(Address address, SimTime time)
{
    if (address_table_.name_at(address))
    {
        address_table_.erase(address);
        transport_->close_peer_sessions(address, time);
    }
}

void Network::table_set(Address address, Name name, SimTime time)
{
    const auto current = address_table_.name_at(address);
    address_table_.set(address, name);
    if (current && *current != name)
    {
        transport_->close_peer_sessions(address, time);
    }
}

void Network::update_address_table(const Message& message)
{
    const Name name = Name::from_bytes(message.data);
    if (message.source == kNullAddress)
    {
        // Cannot Claim: the control function has no address any more.
        if (const auto old = address_table_.address_of(name))
        {
            table_erase(*old, message.time);
        }
        return;
    }
    const Address address = message.source;
    if (const auto old = address_table_.address_of(name); old && *old != address)
    {
        table_erase(*old, message.time); // the control function left its old address
    }
    if (const auto current = address_table_.name_at(address); current && *current < name)
    {
        return; // the claimant loses to the control function already there
    }
    for (const auto& node : nodes_)
    {
        if (node->address_ == address && node->name_ && node->holds_address() && *node->name_ < name)
        {
            // The claimant loses to our node. (Our node's claim already removed any higher NAME
            // the table had there.)
            return;
        }
    }
    table_set(address, name, message.time);
}

std::optional<Address> Network::address_of(Name name) const noexcept
{
    for (const auto& node : nodes_)
    {
        if (node->name_ == name && node->holds_address())
        {
            return node->address_;
        }
    }
    return address_table_.address_of(name);
}

std::optional<Name> Network::name_at(Address address) const noexcept
{
    for (const auto& node : nodes_)
    {
        if (node->address_ == address && node->name_ && node->holds_address())
        {
            return node->name_;
        }
    }
    return address_table_.name_at(address);
}

Address Network::free_address(const Node& node) const noexcept
{
    for (unsigned a = options_.arbitrary_address_first; a <= options_.arbitrary_address_last; ++a)
    {
        const auto address = static_cast<Address>(a);
        // The address the node just lost is taken: by the winner in the table or a local node.
        if (address_table_.name_at(address))
        {
            continue;
        }
        const bool local = std::any_of(nodes_.begin(), nodes_.end(), [&](const std::unique_ptr<Node>& other) {
            return other.get() != &node && other->address_ == address;
        });
        if (!local)
        {
            return address;
        }
    }
    return kNullAddress;
}

void Network::deliver_message(const Message& message)
{
    require_not_reentrant("deliver_message()");
    require_not_failed();
    guarded([&] {
        if (phase_ != Phase::Application)
        {
            throw std::logic_error("vehicle_sil::j1939::Network: deliver_message() is only allowed in the application stage");
        }
        if (message.time > *now_)
        {
            throw std::logic_error("vehicle_sil::j1939::Network: deliver_message() got a message after the step time");
        }
        // The same sources as receive() takes.
        if (!message.pgn.is_valid() || message.source == kGlobalAddress || message.pgn == kPgnTransportControl ||
            message.pgn == kPgnTransportData ||
            (message.source == kNullAddress && message.pgn != kPgnAddressClaimed && message.pgn != kPgnRequest))
        {
            ++statistics_.frames_ignored;
            return;
        }
        dispatch_message(message, -1);
    });
}

void Network::throw_if_failed_during_step() const
{
    // A handler caught the exception of a misuse that failed the network.
    if (phase_ == Phase::Failed)
    {
        throw std::logic_error("vehicle_sil::j1939::Network: the network failed during the step: " + error_);
    }
}

void Network::apply_pending()
{
    for (auto& node : nodes_)
    {
        node->apply_pending();
    }
}

void Network::fire_timers_before(SimTime time)
{
    timers_.begin_round();
    while (true)
    {
        dispatching_ = true;
        const bool fired = timers_.fire_one_before(time);
        dispatching_ = false;
        throw_if_failed_during_step();
        if (!fired)
        {
            return;
        }
        apply_pending();
    }
}

void Network::fire_due_timers()
{
    timers_.begin_round();
    while (true)
    {
        dispatching_ = true;
        const bool fired = timers_.fire_one_due(*now_);
        dispatching_ = false;
        throw_if_failed_during_step();
        if (!fired)
        {
            return;
        }
        apply_pending();
    }
}

void Network::flush(can::Port& port)
{
    require_not_reentrant("flush()");
    require_not_failed();
    guarded([&] {
        if (phase_ != Phase::Application)
        {
            throw std::logic_error("vehicle_sil::j1939::Network: flush() is only allowed once, after process()");
        }
        phase_ = Phase::Flushing;
        transport_->pump_responses();
        std::size_t sent = 0;
        if (flush_queue(control_, port, std::numeric_limits<std::size_t>::max(), sent))
        {
            std::size_t data = 0;
            if (flush_queue(data_, port, options_.max_frames_per_step, data))
            {
                // Transport data packets are made from the sessions, after the queues.
                transport_->flush_data(port, options_.max_frames_per_step - data);
            }
        }
        phase_ = Phase::Idle;
    });
}

can::SendResult Network::send_frame(const can::Frame& frame, std::int32_t sender, can::Port& port)
{
    const can::SendResult result = port.send(can::TimedFrame{*now_, frame});
    if (observed())
    {
        Event e;
        e.kind = EventKind::FrameSent;
        e.time = *now_;
        e.node = sender;
        e.frame = frame;
        e.send_result = result;
        observe(e);
    }
    switch (result)
    {
    case can::SendResult::Accepted:
        ++statistics_.frames_sent;
        if (nodes_.size() > 1)
        {
            loopback_.push_back(Received{*now_, 0, sender, frame});
        }
        break;
    case can::SendResult::QueueFull:
        ++statistics_.send_queue_full;
        break;
    case can::SendResult::Failed:
        ++statistics_.send_failures;
        break;
    }
    return result;
}

bool Network::flush_queue(Queue& queue, can::Port& port, std::size_t limit, std::size_t& sent)
{
    while (!queue.empty() && sent < limit)
    {
        const Outgoing& out = queue.front();
        const Address source = source_of(out.frame);
        const bool requestWithoutAddress = source == kNullAddress && Identifier::decode(out.frame.id).pgn == kPgnRequest;
        if (!requestWithoutAddress && out.epoch != nodes_[static_cast<std::size_t>(out.sender)]->epoch_)
        {
            // Queued before the node lost or changed its address: it must not leave from the old
            // address (also not if the node came back to it since), and a Cannot Claim must not
            // follow a claim the node made since. A Request for Address Claimed from 254 asks
            // nothing of the node's address and still leaves.
            ++statistics_.held_dropped;
            if (observed())
            {
                Event e;
                e.kind = EventKind::MessageDropped;
                e.time = *now_;
                e.node = out.sender;
                const Identifier id = Identifier::decode(out.frame.id);
                e.pgn = id.pgn;
                e.priority = id.priority;
                e.source = id.source;
                e.destination = id.destination;
                e.size = out.frame.length;
                e.frame = out.frame;
                e.drop_reason = DropReason::AddressChanged;
                queue.pop();
                observe(e);
                continue;
            }
            queue.pop();
            continue;
        }
        if (out.session != 0 && !transport_->is_current(out.session, out.generation))
        {
            // The transport session ended (or was replaced) after the frame was queued.
            ++statistics_.transport_stale_dropped;
            queue.pop();
            continue;
        }
        const can::SendResult result = send_frame(out.frame, out.sender, port);
        if (result == can::SendResult::QueueFull)
        {
            return false;
        }
        const std::uint32_t session = out.session;
        const std::uint32_t generation = out.generation;
        queue.pop();
        if (result == can::SendResult::Accepted)
        {
            ++sent;
        }
        if (session != 0)
        {
            // Timers counted from sending start now (also if the port failed it: the protocol
            // then times out).
            transport_->on_control_sent(session, generation);
        }
    }
    return true;
}

bool Network::enqueue_data(const can::Frame& frame, std::size_t sender) noexcept
{
    return data_.push(Outgoing{frame, static_cast<std::int32_t>(sender), nodes_[sender]->epoch_});
}

bool Network::enqueue_control(const can::Frame& frame, std::size_t sender, std::uint32_t session,
                              std::uint32_t generation) noexcept
{
    if (!control_.push(Outgoing{frame, static_cast<std::int32_t>(sender), nodes_[sender]->epoch_, session, generation}))
    {
        ++statistics_.control_frames_dropped;
        return false;
    }
    return true;
}

} // namespace vehicle_sil::j1939
