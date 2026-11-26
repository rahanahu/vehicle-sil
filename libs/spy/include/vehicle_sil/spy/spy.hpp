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
//             spy_.watch<cal::Write>("write", {.from = 0xF9});         // a typed message (watch.hpp):
//                                                                     // write.<signal>, write.count ...
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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace vehicle_sil::spy {

template <class M>
class Watch; // vehicle_sil/spy/watch.hpp

namespace detail {

/// true if T is a complete type here (Spy::watch() checks that watch.hpp was included).
template <class T, class = void>
inline constexpr bool is_complete = false;
template <class T>
inline constexpr bool is_complete<T, std::void_t<decltype(sizeof(T))>> = true;

/// A function of a watcher for every step (Scope::on_step()), with the ScopeState it belongs to.
struct StepFunction
{
    const void* owner = nullptr;
    std::function<void(SimTime now)> fn;
};

/// What a Spy shares with the function it gives Ecu::on_step(), which stays when the Spy goes
/// before the ECU: the step's time, the watchers' step functions, and what to say if the
/// Spy's variables refer to what is gone.
struct SpyShared
{
    SimTime now{};
    std::atomic<bool> started{false}; ///< set in the first step
    /// Scope::on_step() functions in the order registered; the Spy removes them with the handlers.
    std::vector<StepFunction> steps{};
    /// The prefixes of the variables published so far ("\"spy.*\", \"block.*\""), for `broken`.
    std::string published{};
    /// Why the ECU must not step any more (written before `is_broken`): the next step throws it
    /// as std::logic_error, before the variables are sampled.
    std::string broken{};
    std::atomic<bool> is_broken{false};
};

/// What one Scope uses: made and kept by its Spy (internal).
struct ScopeState
{
    Ecu& ecu;
    j1939::Listener& listener;
    std::shared_ptr<SpyShared> shared;
    std::string name;
    /// The handlers registered through the Scope; the Spy removes them before the watcher goes.
    std::vector<j1939::ListenerSubscription> handlers{};
    bool published = false; ///< a variable was published through the Scope
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

/// Which messages Spy::watch<M>() takes, and whether it publishes "<as>.stale" and
/// "<as>.stale_count".
struct WatchOptions
{
    /// Only the messages from this source address. Empty: from any.
    std::optional<j1939::Address> from{};
    /// Only the messages to this destination address (j1939::kGlobalAddress for a broadcast and
    /// for a group without a destination). Empty: to any.
    std::optional<j1939::Address> to{};
    /// Publishes "<as>.stale" (true while no message came for more than this) and
    /// "<as>.stale_count" (number of false -> true transitions). Above 0. Empty: neither variable.
    std::optional<SimDuration> stale_after{};
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
        note_published();
    }
    /// The variable "<name()>.<item>" read through `get` (as Ecu::output()), called in the ECU's
    /// samples, after the handlers of that step.
    template <typename Get>
        requires std::is_invocable_v<Get>
    void output(std::string_view item, Get get, control::VariableOptions options = {})
    {
        ecu().output(variable(item, "output()"), std::move(get), std::move(options));
        note_published();
    }
    /// The integer variable "<name()>.<item>" that takes the values of `items` (as
    /// Ecu::output_enum()).
    template <typename Get>
        requires std::is_invocable_v<Get>
    void output_enum(std::string_view item, Get get, std::vector<control::EnumItem> items,
                     control::VariableOptions options = {})
    {
        ecu().output_enum(variable(item, "output_enum()"), std::move(get), std::move(items), std::move(options));
        note_published();
    }

    /// Adds a handler to the Spy's Listener (j1939::Listener::on_message() and the others: the
    /// same messages, frames and order; handlers of one kind run in the order they were added,
    /// across the watchers). std::invalid_argument for an empty handler.
    void on_message(j1939::Pgn pgn, j1939::Listener::MessageHandler handler);
    void on_any_message(j1939::Listener::MessageHandler handler);
    void on_transfer_end(j1939::Listener::SniffHandler handler);
    void on_frame(j1939::Listener::FrameHandler handler);
    /// Runs `fn` in every step of the ECU, with the step's time (now() is that time too): after
    /// the step's handlers and before its variables are sampled. For what follows from time
    /// passing (e.g. counting that something stopped coming); the result depends on the ECU's
    /// step size. The functions of all watchers run in the order they were registered, from
    /// the Spy's one Ecu::on_step() function. That function takes its place when the Spy is
    /// made: the watchers' functions run after the functions the application gave to
    /// Ecu::on_step() before it made the Spy, and before those it gave later (e.g. in the body
    /// of the constructor of a class that has the Spy as a member). If `fn` throws, the
    /// functions registered after it do not run in that step, and the exception leaves the
    /// step as one thrown by an Ecu::on_step() function does. The same conditions as
    /// on_message(); the Spy removes it with the handlers. Like a handler, it only reads and
    /// updates the watcher's members (it allocates nothing).
    void on_step(std::function<void(SimTime now)> fn);

    /// The time of the ECU's latest step whose functions ran (SimTime{} before the first step).
    /// In a getter of a variable it is the time of the step being sampled, so a getter can say
    /// "nothing came for this long": now() - the latest Message::time. In a handler it is the
    /// time of the step before (the handlers of a step run before its functions): handlers use
    /// Message::time and ReceivedFrame::time.
    [[nodiscard]] SimTime now() const noexcept;
    /// The watcher's name (the `as` of Spy::add()): the prefix of its variables.
    [[nodiscard]] const std::string& name() const noexcept { return state_.name; }
    /// true if the ECU has the variable "<name()>.<item>" already (declared by other code). A
    /// watcher that publishes several variables checks them all first, so that it fails before
    /// the first rather than after some (variables cannot be taken back).
    [[nodiscard]] bool has_variable(std::string_view item) const;

private:
    [[nodiscard]] Ecu& ecu() const noexcept { return state_.ecu; }
    /// "<name>.<item>" after the checks (set up only, item well formed).
    [[nodiscard]] std::string variable(std::string_view item, const char* what) const;
    void require_set_up(const char* what) const;
    void keep(j1939::Listener::HandlerId id);
    /// After a variable was published: the Spy says so if it goes before the ECU.
    void note_published();

    detail::ScopeState& state_;
};

/// Hears a J1939 Network through a Listener and publishes it as the ECU's variables, through
/// watchers (add()) and the counters of what the Listener missed.
///
/// What the Listener missed ("<SpyOptions::statistics_as>.*", by default "spy.*"; integers that
/// only grow):
/// - "spy.missed": the sum of the five Listener counters below and of four counters of the
///   Network's j1939::NetworkStatistics: receive_overflows, transport_rejected,
///   transport_bam_dropped and transport_bam_replaced (an ECU that a Peer runs publishes them as
///   "j1939.<network>.<counter>"). While it is 0, the Network dropped no frame it received, no
///   broadcast or connection that the Network or the Listener followed was lost for a packet
///   that was repeated, out of order or missing before a later one came, for the same sender's
///   next announcement, or for lack of a place, and every message handed to the watchers has
///   the receiver's bytes. It does not say on which side a frame was lost: a broadcast packet
///   that the sender never put on the bus counts too (transport_bam_dropped or
///   transport_bam_replaced). Nor does it say that every transfer came to its end (see "Not seen
///   by it"). transport_rejected counts the broadcasts the Network did not start to assemble (a
///   malformed announcement, no free session) and the connections a node of this participant
///   refused (busy with another group, too large, malformed, no free session).
/// - "spy.no_session", "spy.incomplete", "spy.replaced", "spy.lost",
///   "spy.conflicting_packets": the Listener's j1939::SnifferStatistics of the same names.
/// - "spy.timed_out": SnifferStatistics::timed_out, connections between other ECUs whose last
///   packets did not come. Not in the sum (see below).
/// Not seen by it: frames that came while the ECU was off its buses ("ecu.connected" false:
/// they are dropped before the Network and counted nowhere), NetworkStatistics::frames_ignored
/// (11-bit frames among them, so it grows on any bus that also carries 11-bit frames),
/// connections whose announcement came before the Listener heard the bus
/// (SnifferStatistics::orphan_data), and transfers of which the end never came: a broadcast or
/// a connection whose last packets did not come times out, and whether the sender stopped or
/// the packets were missed cannot be told. Connections between other ECUs are counted in
/// "spy.timed_out", broadcasts in NetworkStatistics::transport_timeouts
/// ("j1939.<network>.transport_timeouts", which also counts the timeouts of this participant's
/// own nodes).
///
/// Lifetime. Make the Spy a member of the ECU's class (the class ecu_main() or Simulation::add()
/// makes), so that it lives as long as the ECU: the variables it and its watchers publish
/// cannot be taken back, and the ECU reads them until it goes. A Spy that published variables
/// and goes before its ECU makes the ECU's next step throw std::logic_error (before the
/// variables are read), saying so. A Spy that published no variable (statistics_as empty and
/// watchers without variables) may go earlier: its handlers and step functions are removed and
/// none of them is called afterwards. It destroys, in this order: for each watcher, the one
/// whose add() returned last first, the watcher's handlers and step functions, then the
/// watcher; then the Listener it made. A borrowed Listener may go before or after the Spy (it must outlive the Spy's
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
    /// already by another watcher or by statistics_as.
    ///
    /// W's constructor may add() other watchers through a Spy& it was given (a watcher made of
    /// others): those are destroyed after it, as the Spy destroys the watchers in the reverse
    /// order their add() returned. Exceptions of W's constructor propagate: the handlers and
    /// step functions it registered are removed, the watchers it added are destroyed, and no W
    /// is kept. Variables it declared cannot be taken back and would refer to the destroyed W:
    /// if it declared any, the ECU's next step throws std::logic_error saying so (do not run
    /// the ECU after that, as with Ecu::emplace()).
    template <class W, class... Args>
    W& add(std::string_view as, Args&&... args)
    {
        static_assert(std::is_constructible_v<W, Scope&, Args&&...>,
                      "vehicle_sil::spy::Spy::add<W>(as, args...) makes W(scope, args...): give W a constructor that "
                      "takes vehicle_sil::spy::Scope& first, then the arguments given to add() after the name");
        return make<W>("add()", as, std::forward<Args>(args)...);
    }

    /// Watches the typed message M (vehicle_sil::j1939::codec: generated by
    /// vehicle_sil_add_j1939_codec() or written by hand in that form) and publishes it as
    /// "<as>.<item>": each signal's latest valid value, and when and how often M came (see Watch
    /// in vehicle_sil/spy/watch.hpp, which this needs: include it). The same conditions as
    /// add<Watch<M>>(as, options) (the messages name watch()); std::invalid_argument also for a
    /// stale_after that is not above 0.
    template <class M>
    Watch<M>& watch(std::string_view as, WatchOptions options = {})
    {
        static_assert(detail::is_complete<Watch<M>>,
                      "vehicle_sil::spy::Spy::watch<M>() needs vehicle_sil/spy/watch.hpp: include it (it uses the "
                      "typed messages of vehicle_sil::j1939_codec, which the target of the messages links)");
        return make<Watch<M>>("watch()", as, std::move(options));
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
    /// add() and watch(): `kind` names the function in the messages.
    template <class W, class... Args>
    W& make(const char* kind, std::string_view as, Args&&... args)
    {
        Slot& slot = begin_add(as, kind);
        try
        {
            auto watcher = std::make_unique<W>(slot.scope, std::forward<Args>(args)...);
            W& ref = *watcher;
            finish_add(slot, Owned(watcher.release(), [](void* p) noexcept { delete static_cast<W*>(p); }));
            return ref;
        }
        catch (...)
        {
            abandon_add(slot);
            throw;
        }
    }
    /// Checks `options` and registers the Spy's function with Ecu::on_step().
    static std::shared_ptr<detail::SpyShared> start(Ecu& ecu, const SpyOptions& options);
    static std::unique_ptr<j1939::Listener> make_listener(j1939::Network& network, const j1939::ListenerOptions& options);
    void require_set_up(const char* who, const char* what) const;
    /// A new slot at the end of watchers_ (the watcher is made in it).
    Slot& begin_add(std::string_view as, const char* kind);
    /// Puts the watcher into `slot` and moves the slot to the end: after the watchers its
    /// constructor added, so that it is destroyed before them.
    void finish_add(Slot& slot, Owned object) noexcept;
    /// Destroys `slot` and every slot after it (the watchers its constructor added).
    void abandon_add(Slot& slot) noexcept;
    /// Removes the last slot's handlers and step functions, then destroys its watcher.
    void drop_last() noexcept;
    /// Makes the ECU's next step throw `why` (variables refer to what is gone).
    void break_ecu(const std::string& why) noexcept;
    void publish_statistics();

    Ecu& ecu_;
    j1939::Network& network_;
    /// Shared with the function the Spy gave Ecu::on_step(), which stays after a Spy that goes
    /// before the ECU.
    std::shared_ptr<detail::SpyShared> shared_;
    /// Declared before watchers_: destroyed after them (the destructor empties watchers_ first).
    std::unique_ptr<j1939::Listener> own_;
    j1939::Listener& listener_;
    std::string statistics_as_;
    bool statistics_published_ = false;
    std::vector<std::unique_ptr<Slot>> watchers_;
};

} // namespace vehicle_sil::spy
