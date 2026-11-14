// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/listener.hpp"

#include "frame_seat.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::j1939 {

namespace {

[[nodiscard]] ListenerOptions without_broadcasts(ListenerOptions options) noexcept
{
    // The Network assembles the broadcasts and hands them to the seat: followed here too, each
    // one would be handed on twice.
    options.sniffer.broadcasts = false;
    return options;
}

} // namespace

/// What sits in the Network's seat for a Listener.
class Listener::Seat final : public detail::FrameSeat
{
public:
    Seat(Network& network, ListenerOptions options)
      : network_(network), options_(without_broadcasts(options)), sniffer_(options_.sniffer)
    {
        sniffer_.on_transfer([this](const SniffedTransfer& transfer) { on_sniffed(transfer); });
        sit(network);
    }

    ~Seat() override
    {
        // First: the handlers destroyed with the lists may own subscriptions of this listener.
        alive_.reset();
    }

    HandlerId add(const char* what, bool any, Pgn pgn, MessageHandler handler)
    {
        check(what, static_cast<bool>(handler));
        messages_.push_back(Entry<MessageHandler>{next_id(), false, any, pgn, std::move(handler)});
        return messages_.back().id;
    }
    HandlerId add(const char* what, TransferHandler handler)
    {
        check(what, static_cast<bool>(handler));
        transfers_.push_back(Entry<TransferHandler>{next_id(), false, false, Pgn{}, std::move(handler)});
        return transfers_.back().id;
    }
    HandlerId add(const char* what, FrameHandler handler)
    {
        check(what, static_cast<bool>(handler));
        frames_.push_back(Entry<FrameHandler>{next_id(), false, false, Pgn{}, std::move(handler)});
        return frames_.back().id;
    }

    bool remove(HandlerId id) noexcept { return remove_from(messages_, id) || remove_from(transfers_, id) || remove_from(frames_, id); }

    [[nodiscard]] std::weak_ptr<const bool> alive() const noexcept { return alive_; }

    [[nodiscard]] const TransferSniffer& sniffer() const noexcept { return sniffer_; }
    [[nodiscard]] const ListenerOptions& options() const noexcept { return options_; }

    // detail::FrameSeat
    void on_frame(SimTime time, const can::Frame& frame, bool local) override
    {
        const Sweeping sweeping(*this);
        const Identifier id = Identifier::decode(frame.id);
        if (!local || options_.hear_local)
        {
            const ReceivedFrame received{time, frame, id.pgn, id.priority, id.source, id.destination, local, frame.payload()};
            const Calling calling(calling_);
            call(frames_, received);
        }
        // A frame of this Network's own node never reaches the sniffer, not even its time: the
        // Network loops it back only when it has a second node, and what is followed must not
        // depend on that.
        if (!local)
        {
            const bool fromOurs = uses(id.source);
            const bool toOurs = id.destination != kGlobalAddress && uses(id.destination);
            if (!fromOurs && !toOurs)
            {
                sniffer_.feed(SniffFrame{time, id.pgn, id.priority, id.source, id.destination, frame.payload()});
            }
            else
            {
                // The Network's own conversation. What was followed from or to that address
                // started before a node took it and cannot be followed further.
                if (fromOurs && sniffer_.active() != 0)
                {
                    static_cast<void>(sniffer_.end_transfers_of(id.source, time));
                }
                if (toOurs && sniffer_.active() != 0)
                {
                    static_cast<void>(sniffer_.end_transfers_of(id.destination, time));
                }
            }
        }
        stop_if_misused();
    }

    void on_message(const Message& message, bool local) override
    {
        const Sweeping sweeping(*this);
        if (!local || options_.hear_local)
        {
            const Calling calling(calling_);
            hand_on(message);
        }
        stop_if_misused();
    }

    void on_step_end(SimTime now) override
    {
        const Sweeping sweeping(*this);
        sniffer_.advance(now);
        stop_if_misused();
    }

private:
    template <typename Handler>
    struct Entry
    {
        HandlerId id;
        bool removed; ///< Removed while the lists were walked: skipped, erased by sweep().
        bool any;     ///< Message handlers: for every group.
        Pgn pgn;      ///< Message handlers that are not `any`.
        Handler handler;
    };

    [[nodiscard]] HandlerId next_id() noexcept { return HandlerId{++last_id_}; }

    /// Calls the handlers of `list` in the order they were added. Nothing is added to a list
    /// while it is walked (check()), and what is removed is only marked.
    template <typename Handler, typename Argument>
    static void call(const std::vector<Entry<Handler>>& list, const Argument& argument)
    {
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            if (!list[i].removed)
            {
                list[i].handler(argument);
            }
        }
    }

    template <typename Handler>
    bool remove_from(std::vector<Entry<Handler>>& list, HandlerId id) noexcept
    {
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            if (it->id != id || it->removed)
            {
                continue;
            }
            if (calling_)
            {
                // The list may be the one being walked, and the handler the one running.
                it->removed = true;
                swept_ = false;
                return true;
            }
            // Destroyed after the erase: what the handler captured may remove other handlers.
            const Entry<Handler> gone = std::move(*it);
            list.erase(it);
            return true;
        }
        return false;
    }

    template <typename Handler>
    static void sweep(std::vector<Entry<Handler>>& list) noexcept
    {
        while (true)
        {
            const auto it = std::find_if(list.begin(), list.end(), [](const Entry<Handler>& e) { return e.removed; });
            if (it == list.end())
            {
                return;
            }
            const Entry<Handler> gone = std::move(*it); // destroyed after the erase (see remove_from())
            list.erase(it);
        }
    }

    /// Erases what was removed while handlers ran (see Sweeping).
    void sweep() noexcept
    {
        if (!swept_)
        {
            swept_ = true;
            sweep(messages_);
            sweep(transfers_);
            sweep(frames_);
        }
    }

    /// At the end of a function of the seat, however it is left: erases what was removed while
    /// handlers ran. No list is walked there.
    class Sweeping
    {
    public:
        explicit Sweeping(Seat& seat) noexcept : seat_(seat) {}
        ~Sweeping() { seat_.sweep(); }
        Sweeping(const Sweeping&) = delete;
        Sweeping& operator=(const Sweeping&) = delete;

    private:
        Seat& seat_;
    };

    /// Sets a flag for a scope and puts back what it was (handlers are called from nested scopes).
    class Calling
    {
    public:
        explicit Calling(bool& flag) noexcept : flag_(flag), outer_(flag) { flag_ = true; }
        ~Calling() { flag_ = outer_; }
        Calling(const Calling&) = delete;
        Calling& operator=(const Calling&) = delete;

    private:
        bool& flag_;
        bool outer_;
    };

    void check(const char* what, bool hasHandler)
    {
        if (calling_)
        {
            // The lists are being walked. Remembered, so that the step stops even if the
            // handler catches this.
            misuse_ = std::string("vehicle_sil::j1939::Listener: ") + what +
                      " was called from a handler of this Listener. Register the handlers before the simulation "
                      "starts (outside a step); a handler only reads what it is given";
            throw std::logic_error(misuse_);
        }
        if (!hasHandler)
        {
            throw std::invalid_argument(std::string("vehicle_sil::j1939::Listener: ") + what +
                                        " got an empty handler. Pass a callable");
        }
    }

    void stop_if_misused() const
    {
        if (!misuse_.empty())
        {
            throw std::logic_error(misuse_);
        }
    }

    /// true if a node of the network uses `address`: the condition under which the engine
    /// takes a connection to that address (a node without a NAME, or one that is claiming or
    /// has claimed it) and the addresses its nodes send from.
    [[nodiscard]] bool uses(Address address) const
    {
        const Network& network = network_;
        const std::size_t count = network.node_count();
        for (std::size_t i = 0; i < count; ++i)
        {
            const Node& node = network.node(i);
            if (node.address() != address)
            {
                continue;
            }
            const AddressState state = node.address_state();
            if (state == AddressState::Fixed || state == AddressState::Claiming || state == AddressState::Claimed)
            {
                return true;
            }
        }
        return false;
    }

    void hand_on(const Message& message) const
    {
        for (std::size_t i = 0; i < messages_.size(); ++i)
        {
            const Entry<MessageHandler>& entry = messages_[i];
            if (!entry.removed && (entry.any || entry.pgn == message.pgn))
            {
                entry.handler(message);
            }
        }
    }

    /// From the sniffer (inside feed(), end_transfers_of() or advance()): what this throws
    /// leaves that call when the sniffer is done with its frame.
    void on_sniffed(const SniffedTransfer& transfer)
    {
        const Calling calling(calling_);
        if (transfer.end == SniffEnd::Completed)
        {
            hand_on(transfer.message());
        }
        call(transfers_, transfer);
    }

    Network& network_; ///< Used only inside the seat's functions, where the network is alive.
    ListenerOptions options_;
    TransferSniffer sniffer_;
    std::vector<Entry<MessageHandler>> messages_;
    std::vector<Entry<TransferHandler>> transfers_;
    std::vector<Entry<FrameHandler>> frames_;
    std::uint64_t last_id_ = 0;
    bool swept_ = true;     ///< false: a handler was removed while handlers ran.
    /// Tells ListenerSubscriptions that the listener is gone. Declared after the lists, but
    /// reset first by the destructor.
    std::shared_ptr<const bool> alive_ = std::make_shared<const bool>(true);
    bool calling_ = false;  ///< Inside a handler of this listener.
    std::string misuse_;    ///< Not empty: a handler registered a handler.
};

Listener::Listener(Network& network, ListenerOptions options) : seat_(std::make_unique<Seat>(network, options)) {}

Listener::~Listener() = default;

Listener::HandlerId Listener::on_message(Pgn pgn, MessageHandler handler)
{
    return seat_->add("on_message()", false, pgn, std::move(handler));
}

Listener::HandlerId Listener::on_any_message(MessageHandler handler)
{
    return seat_->add("on_any_message()", true, Pgn{}, std::move(handler));
}

Listener::HandlerId Listener::on_transfer(TransferHandler handler) { return seat_->add("on_transfer()", std::move(handler)); }

Listener::HandlerId Listener::on_frame(FrameHandler handler) { return seat_->add("on_frame()", std::move(handler)); }

bool Listener::remove_handler(HandlerId id) noexcept { return seat_->remove(id); }

std::size_t Listener::open_transfers() const noexcept { return seat_->sniffer().active(); }

const SnifferStatistics& Listener::sniffer_statistics() const noexcept { return seat_->sniffer().statistics(); }

const ListenerOptions& Listener::options() const noexcept { return seat_->options(); }

ListenerSubscription::ListenerSubscription(Listener& listener, Listener::HandlerId id) noexcept
  : listener_(&listener), id_(id), alive_(listener.seat_->alive())
{
}

ListenerSubscription::ListenerSubscription(ListenerSubscription&& other) noexcept
  : listener_(std::exchange(other.listener_, nullptr)), id_(std::exchange(other.id_, Listener::HandlerId{})),
    alive_(std::move(other.alive_))
{
    other.alive_.reset();
}

ListenerSubscription& ListenerSubscription::operator=(ListenerSubscription&& other) noexcept
{
    if (this != &other)
    {
        reset();
        listener_ = std::exchange(other.listener_, nullptr);
        id_ = std::exchange(other.id_, Listener::HandlerId{});
        alive_ = std::move(other.alive_);
        other.alive_.reset();
    }
    return *this;
}

void ListenerSubscription::reset() noexcept
{
    Listener* listener = std::exchange(listener_, nullptr);
    if (listener != nullptr && !alive_.expired())
    {
        static_cast<void>(listener->remove_handler(id_));
    }
    alive_.reset();
}

} // namespace vehicle_sil::j1939
