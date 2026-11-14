// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/listener.hpp"

#include "frame_seat.hpp"
#include "vehicle_sil/j1939/network.hpp"

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

    void add(const char* what, bool any, Pgn pgn, MessageHandler handler)
    {
        check(what, static_cast<bool>(handler));
        messages_.push_back(MessageEntry{any, pgn, std::move(handler)});
    }
    void add(const char* what, TransferHandler handler)
    {
        check(what, static_cast<bool>(handler));
        transfers_.push_back(std::move(handler));
    }
    void add(const char* what, FrameHandler handler)
    {
        check(what, static_cast<bool>(handler));
        frames_.push_back(std::move(handler));
    }

    [[nodiscard]] const TransferSniffer& sniffer() const noexcept { return sniffer_; }
    [[nodiscard]] const ListenerOptions& options() const noexcept { return options_; }

    // detail::FrameSeat
    void on_frame(SimTime time, const can::Frame& frame, bool local) override
    {
        const Identifier id = Identifier::decode(frame.id);
        if (!local || options_.hear_local)
        {
            const ReceivedFrame received{time, frame, id.pgn, id.priority, id.source, id.destination, local, frame.payload()};
            const Calling calling(calling_);
            for (const FrameHandler& handler : frames_)
            {
                handler(received);
            }
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
        if (!local || options_.hear_local)
        {
            const Calling calling(calling_);
            hand_on(message);
        }
        stop_if_misused();
    }

    void on_step_end(SimTime now) override
    {
        sniffer_.advance(now);
        stop_if_misused();
    }

private:
    struct MessageEntry
    {
        bool any;
        Pgn pgn;
        MessageHandler handler;
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
        for (const MessageEntry& entry : messages_)
        {
            if (entry.any || entry.pgn == message.pgn)
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
        for (const TransferHandler& handler : transfers_)
        {
            handler(transfer);
        }
    }

    Network& network_; ///< Used only inside the seat's functions, where the network is alive.
    ListenerOptions options_;
    TransferSniffer sniffer_;
    std::vector<MessageEntry> messages_;
    std::vector<TransferHandler> transfers_;
    std::vector<FrameHandler> frames_;
    bool calling_ = false;  ///< Inside a handler of this listener.
    std::string misuse_;    ///< Not empty: a handler registered a handler.
};

Listener::Listener(Network& network, ListenerOptions options) : seat_(std::make_unique<Seat>(network, options)) {}

Listener::~Listener() = default;

void Listener::on_message(Pgn pgn, MessageHandler handler) { seat_->add("on_message()", false, pgn, std::move(handler)); }

void Listener::on_any_message(MessageHandler handler) { seat_->add("on_any_message()", true, Pgn{}, std::move(handler)); }

void Listener::on_transfer(TransferHandler handler) { seat_->add("on_transfer()", std::move(handler)); }

void Listener::on_frame(FrameHandler handler) { seat_->add("on_frame()", std::move(handler)); }

std::size_t Listener::open_transfers() const noexcept { return seat_->sniffer().active(); }

const SnifferStatistics& Listener::sniffer_statistics() const noexcept { return seat_->sniffer().statistics(); }

const ListenerOptions& Listener::options() const noexcept { return seat_->options(); }

} // namespace vehicle_sil::j1939
