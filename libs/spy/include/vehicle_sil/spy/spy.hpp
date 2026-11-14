// SPDX-License-Identifier: MIT
//
// Spy: a part of an ECU's application that hears a J1939 bus (a j1939::Listener) and publishes
// what it hears as variables of the ECU, for a scenario to check. What to watch is added as
// watchers: classes written against spy::Scope, which give each watcher its handlers and its
// variables ("<as>.<item>") and remove the handlers before the watcher goes.
//
//     class BlockWatch {                          // a watcher: takes Scope& and its own settings
//     public:
//         BlockWatch(vehicle_sil::spy::Scope& s, j1939::Address from) {
//             s.on_message(kPgnBlock, [this, from](const j1939::Message& m) {
//                 if (m.source != from || m.time < latest_) { return; }
//                 latest_ = m.time; ++count_; bytes_ = static_cast<int>(m.data.size());
//             });
//             s.output("count", &count_); s.output("bytes", &bytes_, {.unit = "bytes"});
//         }
//     private: vehicle_sil::SimTime latest_{}; std::int64_t count_ = 0; int bytes_ = 0;
//     };
//     class BusSpy {
//     public:
//         explicit BusSpy(vehicle_sil::Ecu& ecu) : spy_(ecu, ecu.j1939("J1939_CAN")) {
//             spy_.add<BlockWatch>("block", j1939::Address{0x21});   // block.count, block.bytes
//         }                                                          // and spy.missed ...
//     private: vehicle_sil::spy::Spy spy_;
//     };
//
// A Spy sends nothing: give its Network no node (or use it beside nodes that talk; the Listener
// does not hear what they send, see j1939::ListenerOptions::hear_local).
//
// NOT COMPARED WITH THE STANDARD. The Spy hears what its Listener hears (listener.hpp).
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/ecu/ecu.hpp"
#include "vehicle_sil/j1939/listener.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace vehicle_sil::spy {

namespace detail {

/// The time of the ECU's latest step, written by the function a Spy gives Ecu::on_step().
struct SpyClock
{
    SimTime now{};
    std::atomic<bool> started{false}; ///< set in the first step
};

/// What one Scope uses: made and kept by its Spy (internal).
struct ScopeState
{
    Ecu& ecu;
    j1939::Listener& listener;
    std::shared_ptr<const SpyClock> clock;
    std::string name;
    /// The handlers registered through the Scope; the Spy removes them before the watcher goes.
    std::vector<j1939::ListenerSubscription> handlers{};
};

} // namespace detail

struct SpyOptions
{
    /// The options of the Listener the Spy makes. Not read by the constructor that borrows a
    /// Listener.
    j1939::ListenerOptions listener{};
    /// The prefix of the variables of what the Listener missed: "<statistics_as>.missed" and
    /// its parts (see Spy). Empty: the Spy publishes none of them, e.g. for a second Spy on a
    /// borrowed Listener whose first Spy publishes them. Not empty: it may not start or end
    /// with '.'.
    std::string statistics_as = "spy";
};

/// What one watcher of a Spy gets: its variables, named "<name()>.<item>", and its handlers on
/// the Spy's Listener. The Spy makes one for each add() and keeps the handlers registered
/// through it, so a watcher keeps no ListenerSubscription: the Spy removes the handlers before
/// it destroys the watcher.
///
/// Set up only (the watcher's constructor): after the ECU's first step every function here but
/// now() and name() throws std::logic_error. Handlers only read what they are given and update
/// the watcher's members (j1939::Listener: sending or registering from a handler fails the
/// Network); they allocate nothing. A variable's pointer or getter may refer to the watcher:
/// the watcher lives as long as the Spy.
///
/// Not thread safe: the handlers and the getters run on the thread that steps the ECU.
class Scope final
{
public:
    /// Made by Spy::add() only (the state is the Spy's).
    explicit Scope(detail::ScopeState& state) noexcept : state_(state) {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
    ~Scope() = default;

    /// The variable "<name()>.<item>" bound to `*value` (as Ecu::output()). std::invalid_argument
    /// for an empty `item` or one that starts or ends with '.', and what Ecu::output() refuses.
    template <typename T>
    void output(std::string_view item, const T* value, control::VariableOptions options = {})
    {
        ecu().output(variable(item, "output()"), value, std::move(options));
    }
    /// The variable "<name()>.<item>" read through `get` (as Ecu::output()), called in the ECU's
    /// samples, after the handlers of that step.
    template <typename Get>
        requires std::is_invocable_v<Get>
    void output(std::string_view item, Get get, control::VariableOptions options = {})
    {
        ecu().output(variable(item, "output()"), std::move(get), std::move(options));
    }
    /// The integer variable "<name()>.<item>" that takes the values of `items` (as
    /// Ecu::output_enum()).
    template <typename Get>
        requires std::is_invocable_v<Get>
    void output_enum(std::string_view item, Get get, std::vector<control::EnumItem> items,
                     control::VariableOptions options = {})
    {
        ecu().output_enum(variable(item, "output_enum()"), std::move(get), std::move(items), std::move(options));
    }

    /// Adds a handler to the Spy's Listener (j1939::Listener::on_message() and the others: the
    /// same messages, frames and order; handlers of all watchers run in the order they were
    /// added). std::invalid_argument for an empty handler.
    void on_message(j1939::Pgn pgn, j1939::Listener::MessageHandler handler);
    void on_any_message(j1939::Listener::MessageHandler handler);
    void on_transfer(j1939::Listener::TransferHandler handler);
    void on_frame(j1939::Listener::FrameHandler handler);

    /// The time of the ECU's latest step whose functions ran (SimTime{} before the first step).
    /// In a getter of a variable it is the time of the step being sampled, so a getter can say
    /// "nothing came for this long": now() - the latest Message::time. In a handler it is the
    /// time of the step before (the handlers of a step run before its functions): handlers use
    /// Message::time and ReceivedFrame::time.
    [[nodiscard]] SimTime now() const noexcept;
    /// The watcher's name (the `as` of Spy::add()): the prefix of its variables.
    [[nodiscard]] const std::string& name() const noexcept { return state_.name; }

private:
    [[nodiscard]] Ecu& ecu() const noexcept { return state_.ecu; }
    /// "<name>.<item>" after the checks (set up only, item well formed).
    [[nodiscard]] std::string variable(std::string_view item, const char* what) const;
    void require_set_up(const char* what) const;
    void keep(j1939::Listener::HandlerId id);

    detail::ScopeState& state_;
};

/// Hears a J1939 Network through a Listener and publishes it as the ECU's variables, through
/// watchers (add()) and the counters of what the Listener missed.
///
/// What the Listener missed ("<SpyOptions::statistics_as>.*", by default "spy.*"; integers that
/// only grow):
/// - "spy.missed": the sum of the seven below. While it is 0, the Network took every frame,
///   assembled every broadcast, the Listener followed every connection it saw announced to its
///   end, and every message handed to the watchers has the receiver's bytes.
/// - "spy.no_session", "spy.incomplete", "spy.replaced", "spy.lost",
///   "spy.conflicting_packets": the Listener's j1939::SnifferStatistics of the same names.
/// - "spy.receive_overflows", "spy.transport_rejected": the Network's j1939::NetworkStatistics
///   of the same names.
/// Not in it: NetworkStatistics::frames_ignored (11-bit frames among them, so it grows on any
/// bus that also carries 11-bit frames), and connections whose announcement came before the
/// Listener heard the bus (SnifferStatistics::orphan_data).
///
/// Lifetime. Make the Spy a member of the ECU's class (the class ecu_main() or Simulation::add()
/// makes), so that it lives as long as the ECU: the variables it and its watchers publish
/// cannot be taken back, and the ECU reads them until it goes. A Spy that published no
/// variable (statistics_as empty and watchers without variables) may go earlier: its handlers
/// are removed and no handler of it is called afterwards. It destroys, in this order: for each
/// watcher, the last added first, the watcher's handlers, then the watcher; then the Listener
/// it made. A borrowed Listener may go before or after the Spy (it must outlive the Spy's
/// "spy.*" variables, which read it).
///
/// Make, add() and destroy it outside a step. Not copyable, not movable. Not thread safe: set it
/// up from one thread; the handlers and getters run on the thread that steps the ECU.
class Spy final
{
public:
    /// Makes a Listener on `network` (SpyOptions::listener) and publishes the counters of what
    /// it missed. `network` is one of `ecu`'s (Ecu::j1939()). std::logic_error if the run has
    /// started or the Network has a Listener already (use the other constructor);
    /// std::invalid_argument for a bad statistics_as and for "<statistics_as>.missed" taken
    /// already (e.g. by another Spy of the ECU).
    Spy(Ecu& ecu, j1939::Network& network, SpyOptions options = {});
    /// Uses `listener`, a Listener on `network` that the caller made and keeps (e.g. for several
    /// Spies or other code on one Listener). The same conditions; SpyOptions::listener is not
    /// read.
    Spy(Ecu& ecu, j1939::Network& network, j1939::Listener& listener, SpyOptions options = {});
    ~Spy();
    Spy(const Spy&) = delete;
    Spy& operator=(const Spy&) = delete;
    Spy(Spy&&) = delete;
    Spy& operator=(Spy&&) = delete;

    /// Makes a watcher W(scope, args...), whose variables are "<as>.<item>", and keeps it for
    /// the Spy's life. Watchers are called in the order they were added. Only before the run
    /// starts (the ECU class's constructor): afterwards std::logic_error, and no W is made.
    /// std::invalid_argument for an empty `as`, one that starts or ends with '.', one used
    /// already by another watcher or by statistics_as. Exceptions of W's constructor propagate:
    /// the handlers it registered are removed and no W is kept, but the variables it declared
    /// stay and refer to the destroyed W (do not run the ECU after that, as Ecu::emplace()).
    template <class W, class... Args>
    W& add(std::string_view as, Args&&... args)
    {
        static_assert(std::is_constructible_v<W, Scope&, Args&&...>,
                      "vehicle_sil::spy::Spy::add<W>(as, args...) makes W(scope, args...): give W a constructor that "
                      "takes vehicle_sil::spy::Scope& first, then the arguments given to add() after the name");
        Scope& scope = begin_add(as, "add()");
        try
        {
            auto watcher = std::make_unique<W>(scope, std::forward<Args>(args)...);
            W& ref = *watcher;
            finish_add(Owned(watcher.release(), [](void* p) noexcept { delete static_cast<W*>(p); }));
            return ref;
        }
        catch (...)
        {
            abandon_add();
            throw;
        }
    }

    /// The Listener the Spy hears through (made or borrowed).
    [[nodiscard]] j1939::Listener& listener() const noexcept { return listener_; }
    /// What "spy.missed" shows (also when statistics_as is empty).
    [[nodiscard]] std::uint64_t missed() const noexcept;

private:
    using Owned = std::unique_ptr<void, void (*)(void*) noexcept>;
    /// One watcher: its Scope and the Scope's state, then the watcher itself.
    struct Slot
    {
        Slot(Spy& spy, std::string name, const char* kind_);
        detail::ScopeState state;
        Scope scope;
        const char* kind;
        Owned object{nullptr, nullptr};
    };

    Spy(Ecu& ecu, j1939::Network& network, j1939::Listener* borrowed, SpyOptions options);
    /// Checks `options` and registers the clock with Ecu::on_step().
    static std::shared_ptr<detail::SpyClock> start(Ecu& ecu, const SpyOptions& options);
    static std::unique_ptr<j1939::Listener> make_listener(j1939::Network& network, const j1939::ListenerOptions& options);
    void require_set_up(const char* who, const char* what) const;
    Scope& begin_add(std::string_view as, const char* kind);
    void finish_add(Owned object) noexcept;
    void abandon_add() noexcept;
    void publish_statistics();

    Ecu& ecu_;
    j1939::Network& network_;
    /// The clock is shared with the function the Spy gave Ecu::on_step(), which stays after a
    /// Spy that goes before the ECU.
    std::shared_ptr<detail::SpyClock> clock_;
    /// Declared before watchers_: destroyed after them (the destructor empties watchers_ first).
    std::unique_ptr<j1939::Listener> own_;
    j1939::Listener& listener_;
    std::string statistics_as_;
    std::vector<std::unique_ptr<Slot>> watchers_;
};

} // namespace vehicle_sil::spy
