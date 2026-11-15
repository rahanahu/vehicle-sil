// SPDX-License-Identifier: MIT
#include "vehicle_sil/spy/spy.hpp"

#include <algorithm>
#include <stdexcept>

namespace vehicle_sil::spy {

namespace {

constexpr const char* kSpy = "vehicle_sil::spy::Spy";

std::string in_quotes(std::string_view text) { return "\"" + std::string(text) + "\""; }

bool starts_or_ends_with_dot(std::string_view text)
{
    return !text.empty() && (text.front() == '.' || text.back() == '.');
}

std::int64_t as_variable(std::uint64_t count) noexcept { return static_cast<std::int64_t>(count); }

} // namespace

// --- Scope -------------------------------------------------------------------------------------

void Scope::require_set_up(const char* what) const
{
    if (state_.shared->started.load(std::memory_order_relaxed))
    {
        throw std::logic_error("vehicle_sil::spy::Scope " + in_quotes(state_.name) + ": " + what +
                               " after the run started: register the handlers and variables in the watcher's "
                               "constructor (Spy::add() makes the watcher before the run)");
    }
}

std::string Scope::variable(std::string_view item, const char* what) const
{
    require_set_up(what);
    if (item.empty() || starts_or_ends_with_dot(item))
    {
        throw std::invalid_argument("vehicle_sil::spy::Scope " + in_quotes(state_.name) + ": " + what + " with the name " +
                                    in_quotes(item) + ": the variable is " + in_quotes(state_.name + ".<name>") +
                                    ", so give a name that is not empty and neither starts nor ends with '.' "
                                    "(e.g. \"count\")");
    }
    return state_.name + "." + std::string(item);
}

void Scope::keep(j1939::Listener::HandlerId id) { state_.handlers.emplace_back(state_.listener, id); }

void Scope::note_published()
{
    if (!state_.published)
    {
        state_.published = true;
        auto& published = state_.shared->published;
        published += (published.empty() ? "" : ", ") + in_quotes(state_.name + ".*");
    }
}

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

void Scope::on_transfer_end(j1939::Listener::SniffHandler handler)
{
    require_set_up("on_transfer_end()");
    state_.handlers.reserve(state_.handlers.size() + 1);
    keep(state_.listener.on_transfer_end(std::move(handler)));
}

void Scope::on_frame(j1939::Listener::FrameHandler handler)
{
    require_set_up("on_frame()");
    state_.handlers.reserve(state_.handlers.size() + 1);
    keep(state_.listener.on_frame(std::move(handler)));
}

void Scope::on_step(std::function<void(SimTime now)> fn)
{
    require_set_up("on_step()");
    if (!fn)
    {
        throw std::invalid_argument("vehicle_sil::spy::Scope " + in_quotes(state_.name) +
                                    ": on_step() with an empty function: give the function to run in every step");
    }
    state_.shared->steps.push_back(detail::StepFunction{.owner = &state_, .fn = std::move(fn)});
}

SimTime Scope::now() const noexcept { return state_.shared->now; }

bool Scope::has_variable(std::string_view item) const
{
    const std::string name = state_.name + "." + std::string(item);
    const auto variables = ecu().describe().variables; // reads the declarations without closing them
    return std::any_of(variables.begin(), variables.end(), [&](const auto& info) { return info.name == name; });
}

// --- Spy ---------------------------------------------------------------------------------------

Spy::Spy(Ecu& ecu, j1939::Network& network, SpyOptions options) : Spy(ecu, network, nullptr, std::move(options)) {}

Spy::Spy(Ecu& ecu, j1939::Network& network, j1939::Listener& listener, SpyOptions options) :
    Spy(ecu, network, &listener, std::move(options))
{
}

std::shared_ptr<detail::SpyShared> Spy::start(Ecu& ecu, const SpyOptions& options)
{
    if (starts_or_ends_with_dot(options.statistics_as))
    {
        throw std::invalid_argument(std::string(kSpy) + ": SpyOptions::statistics_as " + in_quotes(options.statistics_as) +
                                    " starts or ends with '.': the counters are " +
                                    in_quotes(options.statistics_as + ".missed") +
                                    " and so on, so give a prefix such as \"spy\" (or \"\" for none)");
    }
    // Before anything else, so that a Spy made too late says so itself (Ecu::on_step() is what
    // refuses).
    auto shared = std::make_shared<detail::SpyShared>();
    try
    {
        ecu.on_step([shared](SimTime now) {
            if (shared->is_broken.load(std::memory_order_acquire))
            {
                // Before the variables are sampled: their getters refer to what is gone.
                throw std::logic_error(shared->broken);
            }
            shared->now = now;
            shared->started.store(true, std::memory_order_relaxed);
            for (const auto& step : shared->steps)
            {
                step.fn(now);
            }
        });
    }
    catch (const std::logic_error&)
    {
        throw std::logic_error(std::string(kSpy) + " made after the run started: make the Spy in the ECU class's "
                               "constructor (a member of the class that ecu_main() or Simulation::add() makes)");
    }
    return shared;
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
    shared_(start(ecu, options)),
    own_(borrowed != nullptr ? nullptr : make_listener(network, options.listener)),
    listener_(borrowed != nullptr ? *borrowed : *own_),
    statistics_as_(std::move(options.statistics_as))
{
    publish_statistics();
}

Spy::Slot::Slot(Spy& spy, std::string name, const char* kind_) :
    state{.ecu = spy.ecu_, .listener = spy.listener_, .shared = spy.shared_, .name = std::move(name)},
    scope(state),
    kind(kind_)
{
}

Spy::~Spy()
{
    const bool published =
        statistics_published_ || std::any_of(watchers_.begin(), watchers_.end(), [](const auto& w) { return w->state.published; });
    if (published)
    {
        // Gone before the ECU (as an ECU's member it goes after the ECU's steps, and this is
        // never read): the ECU would read the variables of what is destroyed here.
        try
        {
            break_ecu(std::string(kSpy) + " was destroyed before its ECU, but its variables (" + shared_->published +
                      ") are still read: make the Spy a member of the ECU class (of the class that ecu_main() or "
                      "Simulation::add() makes, or of an object made with ecu.emplace<T>()), so that it goes with the ECU");
        }
        catch (...)
        {
            break_ecu(std::string(kSpy) + " was destroyed before its ECU, but its variables are still read");
        }
    }
    // Each watcher's handlers before the watcher, the one whose add() returned last first; the
    // Listener it made after all of them (own_ is declared before watchers_).
    while (!watchers_.empty())
    {
        drop_last();
    }
}

void Spy::break_ecu(const std::string& why) noexcept
{
    if (shared_->is_broken.load(std::memory_order_relaxed))
    {
        return; // the first reason stays
    }
    try
    {
        shared_->broken = why;
    }
    catch (...)
    {
        shared_->broken.clear(); // std::logic_error("") still stops the ECU
    }
    shared_->is_broken.store(true, std::memory_order_release);
}

void Spy::drop_last() noexcept
{
    Slot& last = *watchers_.back();
    while (!last.state.handlers.empty())
    {
        last.state.handlers.pop_back();
    }
    std::erase_if(shared_->steps, [&](const detail::StepFunction& f) { return f.owner == &last.state; });
    last.object.reset();
    watchers_.pop_back();
}

void Spy::publish_statistics()
{
    if (statistics_as_.empty())
    {
        return;
    }
    const auto& sniffer = listener_.sniffer_statistics();
    const std::string& p = statistics_as_;
    std::size_t made = 0;
    const auto made_one = [&] {
        if (made++ == 0)
        {
            statistics_published_ = true;
            auto& published = shared_->published;
            published += (published.empty() ? "" : ", ") + in_quotes(p + ".*");
        }
    };
    try
    {
        ecu_.output(p + ".missed", [this] { return as_variable(missed()); },
                    {.description = "What the Spy's Listener missed: the sum of " + p + ".no_session, .incomplete, "
                                    ".replaced, .lost and .conflicting_packets and of the network's receive_overflows, "
                                    "transport_rejected, transport_bam_dropped and transport_bam_replaced "
                                    "(j1939.<network>.*). 0: while the ECU was on its buses, no frame was dropped and "
                                    "no transfer was lost for a missing, repeated or out-of-order packet, a next "
                                    "announcement or lack of a place. Transfers whose last packets never came are not "
                                    "in it (" + p + ".timed_out, j1939.<network>.transport_timeouts)"});
        made_one();
        ecu_.output(p + ".no_session", [&sniffer] { return as_variable(sniffer.no_session); },
                    {.description = "Connections between other ECUs not followed: no free place "
                                    "(ListenerOptions::sniffer.sessions)"});
        made_one();
        ecu_.output(p + ".incomplete", [&sniffer] { return as_variable(sniffer.incomplete); },
                    {.description = "Connections between other ECUs with packets the Listener did not see"});
        made_one();
        ecu_.output(p + ".replaced", [&sniffer] { return as_variable(sniffer.replaced); },
                    {.description = "Connections between other ECUs announced again before they ended"});
        made_one();
        ecu_.output(p + ".lost", [&sniffer] { return as_variable(sniffer.lost); },
                    {.description = "Connections between other ECUs ended because a node of this participant "
                                    "took their address"});
        made_one();
        ecu_.output(p + ".conflicting_packets", [&sniffer] { return as_variable(sniffer.conflicting_packets); },
                    {.description = "Packets seen twice with different bytes: the message handed on may differ "
                                    "from the one the receiver took"});
        made_one();
        ecu_.output(p + ".timed_out", [&sniffer] { return as_variable(sniffer.timed_out); },
                    {.description = "Connections between other ECUs whose last packets never came (not in " + p +
                                    ".missed: the sender may have stopped, or the Listener missed them)"});
        made_one();
    }
    catch (const std::invalid_argument& e)
    {
        if (made > 0)
        {
            break_ecu(std::string(kSpy) + ": its constructor failed after it published " + shared_->published +
                      ", which refer to it: do not run the ECU after a failed Spy constructor");
        }
        throw std::invalid_argument(std::string(kSpy) + ": cannot publish what it missed as " + in_quotes(p + ".*") +
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
           n.transport_rejected + n.transport_bam_dropped + n.transport_bam_replaced;
}

void Spy::require_set_up(const char* who, const char* what) const
{
    if (shared_->started.load(std::memory_order_relaxed))
    {
        throw std::logic_error(std::string(who) + "::" + what +
                               " after the run started: add the watchers in the ECU class's constructor");
    }
}

Spy::Slot& Spy::begin_add(std::string_view as, const char* kind)
{
    require_set_up(kSpy, kind);
    const std::string who = std::string(kSpy) + "::" + kind + " " + in_quotes(as);
    if (as.empty() || starts_or_ends_with_dot(as))
    {
        throw std::invalid_argument(who + ": the watcher's variables are " + in_quotes(std::string(as) + ".<item>") +
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
    watchers_.reserve(watchers_.size() + 1);
    watchers_.push_back(std::make_unique<Slot>(*this, std::string(as), kind));
    return *watchers_.back();
}

void Spy::finish_add(Slot& slot, Owned object) noexcept
{
    slot.object = std::move(object);
    // After the slots its constructor added (they come after it): destroyed before them.
    const auto it = std::find_if(watchers_.begin(), watchers_.end(), [&](const auto& w) { return w.get() == &slot; });
    std::rotate(it, it + 1, watchers_.end());
}

void Spy::abandon_add(Slot& slot) noexcept
{
    // The watcher was not made: remove what its constructor registered, and the watchers it
    // added (every slot after it was begun while it was being made).
    const auto it = std::find_if(watchers_.begin(), watchers_.end(), [&](const auto& w) { return w.get() == &slot; });
    const auto first = static_cast<std::size_t>(it - watchers_.begin());
    const bool published =
        std::any_of(it, watchers_.end(), [](const auto& w) { return w->state.published; });
    std::string why;
    if (published)
    {
        try
        {
            why = std::string(kSpy) + "::" + slot.kind + " " + in_quotes(slot.state.name) +
                  ": the watcher's constructor threw after variables were published that refer to it (or to the "
                  "watchers it added): do not run the ECU after a failed " + slot.kind +
                  "; fix what made the constructor throw";
        }
        catch (...)
        {
        }
    }
    while (watchers_.size() > first)
    {
        drop_last();
    }
    if (published)
    {
        break_ecu(why);
    }
}

} // namespace vehicle_sil::spy
