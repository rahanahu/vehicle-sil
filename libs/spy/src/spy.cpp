// SPDX-License-Identifier: MIT
#include "vehicle_sil/spy/spy.hpp"

#include <stdexcept>

namespace vehicle_sil::spy {

namespace {

constexpr const char* kSpy = "vehicle_sil::spy::Spy";

std::string quoted(std::string_view text) { return "\"" + std::string(text) + "\""; }

bool starts_or_ends_with_dot(std::string_view text)
{
    return !text.empty() && (text.front() == '.' || text.back() == '.');
}

std::int64_t as_variable(std::uint64_t count) noexcept { return static_cast<std::int64_t>(count); }

} // namespace

// --- Scope -------------------------------------------------------------------------------------

void Scope::require_set_up(const char* what) const
{
    if (state_.clock->started.load(std::memory_order_relaxed))
    {
        throw std::logic_error("vehicle_sil::spy::Scope " + quoted(state_.name) + ": " + what +
                               " after the run started: register the handlers and variables in the watcher's "
                               "constructor (Spy::add() makes the watcher before the run)");
    }
}

std::string Scope::variable(std::string_view item, const char* what) const
{
    require_set_up(what);
    if (item.empty() || starts_or_ends_with_dot(item))
    {
        throw std::invalid_argument("vehicle_sil::spy::Scope " + quoted(state_.name) + ": " + what + " with the name " +
                                    quoted(item) + ": the variable is " + quoted(state_.name + ".<name>") +
                                    ", so give a name that is not empty and neither starts nor ends with '.' "
                                    "(e.g. \"count\")");
    }
    return state_.name + "." + std::string(item);
}

void Scope::keep(j1939::Listener::HandlerId id) { state_.handlers.emplace_back(state_.listener, id); }

// Each registration reserves first, so that the one made cannot be lost.

void Scope::on_message(j1939::Pgn pgn, j1939::Listener::MessageHandler handler)
{
    require_set_up("on_message()");
    state_.handlers.reserve(state_.handlers.size() + 1);
    keep(state_.listener.on_message(pgn, std::move(handler)));
}

void Scope::on_any_message(j1939::Listener::MessageHandler handler)
{
    require_set_up("on_any_message()");
    state_.handlers.reserve(state_.handlers.size() + 1);
    keep(state_.listener.on_any_message(std::move(handler)));
}

void Scope::on_transfer(j1939::Listener::TransferHandler handler)
{
    require_set_up("on_transfer()");
    state_.handlers.reserve(state_.handlers.size() + 1);
    keep(state_.listener.on_transfer(std::move(handler)));
}

void Scope::on_frame(j1939::Listener::FrameHandler handler)
{
    require_set_up("on_frame()");
    state_.handlers.reserve(state_.handlers.size() + 1);
    keep(state_.listener.on_frame(std::move(handler)));
}

SimTime Scope::now() const noexcept { return state_.clock->now; }

// --- Spy ---------------------------------------------------------------------------------------

Spy::Spy(Ecu& ecu, j1939::Network& network, SpyOptions options) : Spy(ecu, network, nullptr, std::move(options)) {}

Spy::Spy(Ecu& ecu, j1939::Network& network, j1939::Listener& listener, SpyOptions options) :
    Spy(ecu, network, &listener, std::move(options))
{
}

std::shared_ptr<detail::SpyClock> Spy::start(Ecu& ecu, const SpyOptions& options)
{
    if (starts_or_ends_with_dot(options.statistics_as))
    {
        throw std::invalid_argument(std::string(kSpy) + ": SpyOptions::statistics_as " + quoted(options.statistics_as) +
                                    " starts or ends with '.': the counters are " +
                                    quoted(options.statistics_as + ".missed") +
                                    " and so on, so give a prefix such as \"spy\" (or \"\" for none)");
    }
    // Before anything else, so that a Spy made too late says so itself (Ecu::on_step() is what
    // refuses).
    auto clock = std::make_shared<detail::SpyClock>();
    try
    {
        ecu.on_step([clock](SimTime now) {
            clock->now = now;
            clock->started.store(true, std::memory_order_relaxed);
        });
    }
    catch (const std::logic_error&)
    {
        throw std::logic_error(std::string(kSpy) + " made after the run started: make the Spy in the ECU class's "
                               "constructor (a member of the class that ecu_main() or Simulation::add() makes)");
    }
    return clock;
}

std::unique_ptr<j1939::Listener> Spy::make_listener(j1939::Network& network, const j1939::ListenerOptions& options)
{
    try
    {
        return std::make_unique<j1939::Listener>(network, options);
    }
    catch (const std::logic_error& e)
    {
        if (dynamic_cast<const std::invalid_argument*>(&e) != nullptr)
        {
            throw;
        }
        throw std::logic_error(std::string(kSpy) + ": " + e.what() +
                               ". To hear through that Listener, give it to the Spy: Spy(ecu, network, listener)");
    }
}

Spy::Spy(Ecu& ecu, j1939::Network& network, j1939::Listener* borrowed, SpyOptions options) :
    ecu_(ecu),
    network_(network),
    clock_(start(ecu, options)),
    own_(borrowed != nullptr ? nullptr : make_listener(network, options.listener)),
    listener_(borrowed != nullptr ? *borrowed : *own_),
    statistics_as_(std::move(options.statistics_as))
{
    publish_statistics();
}

Spy::Slot::Slot(Spy& spy, std::string name, const char* kind_) :
    state{.ecu = spy.ecu_, .listener = spy.listener_, .clock = spy.clock_, .name = std::move(name)},
    scope(state),
    kind(kind_)
{
}

Spy::~Spy()
{
    // Each watcher's handlers before the watcher, the last added first (and the last
    // registered first); the Listener it made after all of them (own_ is declared before
    // watchers_).
    while (!watchers_.empty())
    {
        Slot& last = *watchers_.back();
        while (!last.state.handlers.empty())
        {
            last.state.handlers.pop_back();
        }
        last.object.reset();
        watchers_.pop_back();
    }
}

void Spy::publish_statistics()
{
    if (statistics_as_.empty())
    {
        return;
    }
    const auto& sniffer = listener_.sniffer_statistics();
    const auto& network = network_.statistics();
    const std::string& p = statistics_as_;
    try
    {
        ecu_.output(p + ".missed", [this] { return as_variable(missed()); },
                    {.description = "What the Spy's Listener missed: the sum of the seven " + p +
                                    ".* counters beside it (0: nothing missed)"});
        ecu_.output(p + ".no_session", [&sniffer] { return as_variable(sniffer.no_session); },
                    {.description = "Connections between other ECUs not followed: no free place "
                                    "(ListenerOptions::sniffer.sessions)"});
        ecu_.output(p + ".incomplete", [&sniffer] { return as_variable(sniffer.incomplete); },
                    {.description = "Connections between other ECUs with packets the Listener did not see"});
        ecu_.output(p + ".replaced", [&sniffer] { return as_variable(sniffer.replaced); },
                    {.description = "Connections between other ECUs announced again before they ended"});
        ecu_.output(p + ".lost", [&sniffer] { return as_variable(sniffer.lost); },
                    {.description = "Connections between other ECUs ended because a node of this participant "
                                    "took their address"});
        ecu_.output(p + ".conflicting_packets", [&sniffer] { return as_variable(sniffer.conflicting_packets); },
                    {.description = "Packets seen twice with different bytes: the message handed on may differ "
                                    "from the one the receiver took"});
        ecu_.output(p + ".receive_overflows", [&network] { return as_variable(network.receive_overflows); },
                    {.description = "Frames the network dropped: its receive queue was full "
                                    "(NetworkOptions::receive_capacity)"});
        ecu_.output(p + ".transport_rejected", [&network] { return as_variable(network.transport_rejected); },
                    {.description = "Broadcasts not assembled for lack of a free session (and connections a node of "
                                    "this participant refused)"});
    }
    catch (const std::invalid_argument& e)
    {
        throw std::invalid_argument(std::string(kSpy) + ": cannot publish what it missed as " + quoted(p + ".*") +
                                    ": " + e.what() +
                                    ". For two Spies on one ECU, give the second one SpyOptions::statistics_as = "
                                    "\"\" (or another prefix)");
    }
}

std::uint64_t Spy::missed() const noexcept
{
    const auto& s = listener_.sniffer_statistics();
    const auto& n = network_.statistics();
    return s.no_session + s.incomplete + s.replaced + s.lost + s.conflicting_packets + n.receive_overflows +
           n.transport_rejected;
}

void Spy::require_set_up(const char* who, const char* what) const
{
    if (clock_->started.load(std::memory_order_relaxed))
    {
        throw std::logic_error(std::string(who) + "::" + what +
                               " after the run started: add the watchers in the ECU class's constructor");
    }
}

Scope& Spy::begin_add(std::string_view as, const char* kind)
{
    require_set_up(kSpy, kind);
    const std::string who = std::string(kSpy) + "::" + kind + " " + quoted(as);
    if (as.empty() || starts_or_ends_with_dot(as))
    {
        throw std::invalid_argument(who + ": the watcher's variables are " + quoted(std::string(as) + ".<item>") +
                                    ", so give a name that is not empty and neither starts nor ends with '.' "
                                    "(e.g. \"block\" or \"brake.block\")");
    }
    if (as == statistics_as_)
    {
        throw std::invalid_argument(who + ": the name is the prefix of the Spy's own counters "
                                          "(SpyOptions::statistics_as): give the watcher another name, or the Spy "
                                          "another statistics_as");
    }
    for (const auto& w : watchers_)
    {
        if (w->state.name == as)
        {
            throw std::invalid_argument(who + ": the name is used already, by an earlier " + w->kind +
                                        ": give each watcher a name of its own");
        }
    }
    watchers_.reserve(watchers_.size() + 1); // finish_add() cannot fail
    watchers_.push_back(std::make_unique<Slot>(*this, std::string(as), kind));
    return watchers_.back()->scope;
}

void Spy::finish_add(Owned object) noexcept { watchers_.back()->object = std::move(object); }

void Spy::abandon_add() noexcept
{
    // The watcher was not made: remove what its constructor registered.
    watchers_.pop_back();
}

} // namespace vehicle_sil::spy
