// SPDX-License-Identifier: MIT
//
// vehicle_sil::spy: a Spy on an ECU of a LocalBench hears the bus through a Listener and
// publishes its watchers' variables and the counters of what it missed. Public API only.
#include "spy_rig.hpp"

#include "vehicle_sil/j1939/listener.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/simulation/simulation.hpp"
#include "vehicle_sil/spy/spy.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace vehicle_sil::unit::spy;
using vehicle_sil::Ecu;
using vehicle_sil::LocalBench;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::j1939::Listener;
using vehicle_sil::j1939::ListenerOptions;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::ReceivedFrame;
using vehicle_sil::j1939::SniffedTransfer;
using vehicle_sil::spy::Scope;
using vehicle_sil::spy::Spy;
using vehicle_sil::spy::SpyOptions;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;

namespace {

constexpr Address kA = 0x21;
constexpr Address kB = 0x22;
constexpr Address kC = 0x23;
constexpr Address kD = 0x24;

/// A watcher like the one in the guide: the messages of kPgnPropA from one sender.
class BlockWatch
{
public:
    BlockWatch(Scope& s, Address from)
    {
        s.on_message(vehicle_sil::j1939::Pgn{kPgnPropA}, [this, from](const Message& m) {
            if (m.source != from || m.time < latest_)
            {
                return;
            }
            latest_ = m.time;
            ++count_;
            bytes_ = static_cast<int>(m.data.size());
        });
        s.output("count", &count_, {.description = "messages heard"});
        s.output("bytes", &bytes_, {.unit = "bytes"});
    }

private:
    SimTime latest_{};
    std::int64_t count_ = 0;
    int bytes_ = 0;
};

/// Writes what its handlers and destructor see into `log` (no variables).
class Logger
{
public:
    Logger(Scope& s, std::vector<std::string>& log) : log_(log), name_(s.name())
    {
        s.on_any_message([this](const Message& m) { log_.push_back(name_ + " " + std::to_string(m.pgn.value())); });
    }
    ~Logger() { log_.push_back(name_ + " destroyed"); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    std::vector<std::string>& log_;
    std::string name_;
};

/// A Spy with options, owned by the ECU (as a member of the ECU's class would be).
Spy& make_spy(Rig& rig, SpyOptions options = {}) { return rig.ecu.emplace<Spy>(rig.ecu, rig.network, std::move(options)); }

/// A connection of `message` from `sa` to `da`, with a frame each millisecond from `start`.
void inject_connection(Rig& rig, SimTime start, Address sa, Address da, const Bytes& message,
                       std::uint32_t pgn = kPgnPropA)
{
    const auto packets = packet_count(message.size());
    SimTime t = start;
    rig.inject(t, rts(sa, da, message.size(), pgn));
    rig.inject(t += std::chrono::milliseconds(1), cts(da, sa, packets, 1, pgn));
    for (const auto& f : dts(sa, da, message, 1, packets))
    {
        rig.inject(t += std::chrono::milliseconds(1), f);
    }
    rig.inject(t += std::chrono::milliseconds(1), eoma(da, sa, message.size(), pgn));
}

} // namespace

TEST_CASE(watcher_variables_are_named_after_it_and_count_what_it_hears)
{
    Rig rig;
    Spy& spy = make_spy(rig);
    spy.add<BlockWatch>("block", kA);
    rig.inject(at(1ms), frame(6, kPgnPropA, kA, kB, {1, 2, 3}));
    rig.inject(at(2ms), frame(6, kPgnPropA, kC, kB, {1})); // another sender: not counted
    rig.bench.run_until(at(20ms));
    CHECK_EQ(integer_of(rig.ecu, "block.count"), 1);
    CHECK_EQ(integer_of(rig.ecu, "block.bytes"), 3);
    // A connection between two other ECUs (RTS/CTS), 20 bytes.
    inject_connection(rig, at(21ms), kA, kB, payload(20));
    rig.bench.run_until(at(60ms));
    CHECK_EQ(integer_of(rig.ecu, "block.count"), 2);
    CHECK_EQ(integer_of(rig.ecu, "block.bytes"), 20);
    CHECK(info_of(rig.ecu, "block.count").description == "messages heard");
    CHECK(info_of(rig.ecu, "block.bytes").unit == "bytes");
    CHECK(info_of(rig.ecu, "block.count").causality == vehicle_sil::control::Causality::Output);
    // The counters of what the Listener missed, under "spy." by default.
    for (const char* name : {"spy.missed", "spy.no_session", "spy.incomplete", "spy.replaced", "spy.lost",
                             "spy.conflicting_packets", "spy.receive_overflows", "spy.transport_rejected",
                             "spy.transport_bam_dropped"})
    {
        CHECK_EQ(integer_of(rig.ecu, name), 0);
        CHECK(!info_of(rig.ecu, name).description.empty());
    }
    CHECK_EQ(spy.missed(), 0u);
    CHECK_EQ(rig.network.statistics().frames_sent, 0u);
}

TEST_CASE(watchers_are_called_in_the_order_added_and_each_hears_every_message)
{
    std::vector<std::string> log; // before the rig: the watchers write to it when the ECU goes
    Rig rig;
    Spy& spy = make_spy(rig);
    spy.add<Logger>("first", log);
    spy.add<Logger>("second", log);
    rig.inject(at(1ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
    rig.inject(at(2ms), frame(6, kPgnPropA, kA, kB, {2}));
    rig.bench.run_until(at(10ms) + 1ns);
    const std::vector<std::string> expected{"first 65296", "second 65296", "first 61184", "second 61184"};
    CHECK(log == expected);
}

TEST_CASE(getters_and_on_step_run_after_the_steps_handlers)
{
    // The order in Ecu: the networks' handlers, then the step's functions (the Spy's clock among
    // them), then the samples, all on the thread that steps the ECU. Scope::now() rests on it.
    struct Probe
    {
        Probe(Scope& s, std::vector<std::string>& log) : scope(s), log_(log)
        {
            s.on_any_message([this](const Message& m) {
                handler_thread = std::this_thread::get_id();
                log_.push_back("handler " + std::to_string(to_ns(m.time) / 1000000) + " now " +
                               std::to_string(to_ns(scope.now()) / 1000000));
                last_ms = to_ns(m.time) / 1000000;
            });
            s.output("last_ms", [this] {
                getter_thread = std::this_thread::get_id();
                log_.push_back("getter " + std::to_string(last_ms) + " now " + std::to_string(to_ns(scope.now()) / 1000000));
                return last_ms;
            });
        }
        Scope& scope;
        std::vector<std::string>& log_;
        std::int64_t last_ms = -1;
        std::thread::id handler_thread{};
        std::thread::id getter_thread{};
    };
    std::vector<std::string> log;
    Rig rig;
    Spy& spy = make_spy(rig, {.statistics_as = ""});
    Probe& probe = spy.add<Probe>("probe", log);
    rig.inject(at(15ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
    rig.bench.run_until(at(20ms) + 1ns);
    // A getter sees what the handlers of its step did, and now() is that step's time; in the
    // handler now() is the step before's.
    const std::vector<std::string> expected{"getter -1 now 0", "getter -1 now 10", "handler 15 now 10", "getter 15 now 20"};
    CHECK(log == expected);
    CHECK(probe.handler_thread == probe.getter_thread);
    CHECK_EQ(integer_of(rig.ecu, "probe.last_ms"), 15);
}

TEST_CASE(spy_that_goes_first_leaves_no_handler_behind)
{
    // A Spy on a borrowed Listener that published no variable may go before the ECU and the
    // Listener: none of its handlers is called afterwards, and the steps go on.
    Rig rig;
    Listener& listener = rig.ecu.emplace<Listener>(rig.network);
    int heard_by_others = 0;
    listener.on_any_message([&](const Message&) { ++heard_by_others; });
    std::vector<std::string> log;
    auto spy = std::make_unique<Spy>(rig.ecu, rig.network, listener, SpyOptions{.statistics_as = ""});
    spy->add<Logger>("a", log);
    spy->add<Logger>("b", log);
    rig.inject(at(1ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
    rig.bench.run_until(at(10ms) + 1ns);
    CHECK_EQ(log.size(), 2u);
    spy.reset();
    const std::vector<std::string> expected{"a 65296", "b 65296", "b destroyed", "a destroyed"};
    CHECK(log == expected);
    rig.inject(at(11ms), frame(6, kPgnPropB, kA, kGlobal, {2}));
    inject_connection(rig, at(12ms), kA, kB, payload(20));
    rig.bench.run_until(at(50ms));
    CHECK(log == expected);
    CHECK_EQ(heard_by_others, 3);
    CHECK(!rig.network.failed());
}

TEST_CASE(spy_removes_a_watchers_handlers_before_the_watcher_and_its_listener_last)
{
    struct Ordered
    {
        Ordered(Scope& s, std::vector<std::string>& log, Network& network) : log_(log), network_(network), name_(s.name())
        {
            // The handler object tells when it goes.
            struct Guard
            {
                std::vector<std::string>* log;
                std::string name;
                std::shared_ptr<int> alive = std::make_shared<int>(0);
                Guard(std::vector<std::string>* l, std::string n) : log(l), name(std::move(n)) {}
                Guard(const Guard&) = default;
                ~Guard()
                {
                    if (alive.use_count() == 1)
                    {
                        log->push_back(name + " handler destroyed");
                    }
                }
            };
            s.on_any_message([guard = Guard(&log, name_)](const Message&) {});
        }
        ~Ordered()
        {
            // The Spy's Listener still exists: a second one on its Network is refused.
            const bool listener_alive = throws<std::logic_error>([&] { Listener second(network_); });
            log_.push_back(name_ + " destroyed" + (listener_alive ? "" : " after the Listener"));
        }
        Ordered(const Ordered&) = delete;
        Ordered& operator=(const Ordered&) = delete;
        std::vector<std::string>& log_;
        Network& network_;
        std::string name_;
    };
    Rig rig;
    std::vector<std::string> log;
    {
        Spy spy(rig.ecu, rig.network, {.statistics_as = ""});
        spy.add<Ordered>("a", log, rig.network);
        spy.add<Ordered>("b", log, rig.network);
        log.clear(); // copies of the guard made while registering
    }
    const std::vector<std::string> expected{"b handler destroyed", "b destroyed", "a handler destroyed", "a destroyed"};
    CHECK(log == expected);
    // The Spy's Listener is gone with it.
    Listener again(rig.network);
}

TEST_CASE(watcher_whose_constructor_throws_leaves_no_handler)
{
    struct Failing
    {
        Failing(Scope& s, int& calls)
        {
            s.on_any_message([&calls](const Message&) { ++calls; });
            s.on_frame([&calls](const ReceivedFrame&) { ++calls; });
            throw std::runtime_error("bad settings");
        }
    };
    int calls = 0;
    std::vector<std::string> log; // before the rig: the watcher writes to it when the ECU goes
    Rig rig;
    Spy& spy = make_spy(rig);
    CHECK(throws_with<std::runtime_error>([&] { spy.add<Failing>("failing", calls); }, "bad settings"));
    // The name was not kept: no watcher has it.
    spy.add<Logger>("failing", log);
    rig.inject(at(1ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
    rig.bench.run_until(at(10ms) + 1ns);
    CHECK_EQ(calls, 0);
    CHECK_EQ(log.size(), 1u);
}

TEST_CASE(missed_counts_each_part_and_their_sum)
{
    const auto check_sum = [](Rig& rig, const Spy& spy) {
        std::int64_t parts = 0;
        for (const char* name : {"spy.no_session", "spy.incomplete", "spy.replaced", "spy.lost", "spy.conflicting_packets",
                                 "spy.receive_overflows", "spy.transport_rejected", "spy.transport_bam_dropped"})
        {
            parts += integer_of(rig.ecu, name);
        }
        CHECK_EQ(integer_of(rig.ecu, "spy.missed"), parts);
        CHECK_EQ(static_cast<std::int64_t>(spy.missed()), parts);
        CHECK(parts > 0);
    };
    {
        // No free place for a second connection.
        Rig rig;
        SpyOptions options;
        options.listener.sniffer.sessions = 1;
        Spy& spy = make_spy(rig, options);
        rig.inject(at(1ms), rts(kA, kB, 20, kPgnPropA));
        rig.inject(at(2ms), rts(kC, kD, 20, kPgnPropA));
        rig.bench.run_until(at(20ms));
        CHECK_EQ(integer_of(rig.ecu, "spy.no_session"), 1);
        check_sum(rig, spy);
    }
    {
        // A packet not seen, then the receiver's acknowledgement.
        Rig rig;
        Spy& spy = make_spy(rig);
        const Bytes message = payload(20);
        rig.inject(at(1ms), rts(kA, kB, 20, kPgnPropA));
        rig.inject(at(2ms), cts(kB, kA, 3, 1, kPgnPropA));
        rig.inject(at(3ms), dt(kA, kB, 1, message));
        rig.inject(at(4ms), dt(kA, kB, 3, message));
        rig.inject(at(5ms), eoma(kB, kA, 20, kPgnPropA));
        rig.bench.run_until(at(30ms));
        CHECK_EQ(integer_of(rig.ecu, "spy.incomplete"), 1);
        check_sum(rig, spy);
    }
    {
        // Announced again before it ended.
        Rig rig;
        Spy& spy = make_spy(rig);
        rig.inject(at(1ms), rts(kA, kB, 20, kPgnPropA));
        rig.inject(at(2ms), rts(kA, kB, 30, kPgnPropA));
        rig.bench.run_until(at(20ms));
        CHECK_EQ(integer_of(rig.ecu, "spy.replaced"), 1);
        check_sum(rig, spy);
    }
    {
        // A node of the Spy's participant takes the address of a followed connection.
        Rig rig;
        Spy& spy = make_spy(rig);
        const Name ours = Name{}.set_identity_number(7).set_manufacturer_code(0x7F0);
        rig.ecu.at(at(10ms), [&](SimTime) { rig.network.add_node(NodeOptions{kB, ours}); });
        const Bytes message = payload(30);
        rig.inject(at(1ms), rts(kA, kB, 30, kPgnPropA));
        rig.inject(at(2ms), cts(kB, kA, 5, 1, kPgnPropA));
        rig.inject(at(12ms), dt(kA, kB, 1, message));
        rig.inject(at(24ms), dt(kA, kB, 2, message));
        rig.bench.run_until(at(40ms));
        CHECK_EQ(integer_of(rig.ecu, "spy.lost"), 1);
        check_sum(rig, spy);
    }
    {
        // A packet seen twice with different bytes.
        Rig rig;
        Spy& spy = make_spy(rig);
        const Bytes message = payload(20);
        Bytes other = message;
        other[0] ^= 0xFF;
        rig.inject(at(1ms), rts(kA, kB, 20, kPgnPropA));
        rig.inject(at(2ms), cts(kB, kA, 3, 1, kPgnPropA));
        rig.inject(at(3ms), dt(kA, kB, 1, message));
        rig.inject(at(4ms), dt(kA, kB, 1, other));
        rig.inject(at(5ms), dt(kA, kB, 2, message));
        rig.inject(at(6ms), dt(kA, kB, 3, message));
        rig.inject(at(7ms), eoma(kB, kA, 20, kPgnPropA));
        rig.bench.run_until(at(30ms));
        CHECK_EQ(integer_of(rig.ecu, "spy.conflicting_packets"), 1);
        check_sum(rig, spy);
    }
    {
        // More frames in a step than the network takes.
        NetworkOptions options;
        options.receive_capacity = 4;
        Rig rig(options);
        Spy& spy = make_spy(rig);
        for (int i = 0; i < 6; ++i)
        {
            rig.inject(at(1ms) + i * 100us, frame(6, kPgnPropB, kA, kGlobal, {static_cast<std::uint8_t>(i)}));
        }
        rig.bench.run_until(at(20ms));
        CHECK_EQ(integer_of(rig.ecu, "spy.receive_overflows"), 2);
        check_sum(rig, spy);
    }
    {
        // A broadcast the network has no free session for.
        NetworkOptions options;
        options.bam_receive_sessions = 1;
        Rig rig(options);
        Spy& spy = make_spy(rig);
        rig.inject(at(1ms), bam(kA, 20, kPgnPropB));
        rig.inject(at(2ms), bam(kC, 20, kPgnPropB));
        rig.bench.run_until(at(20ms));
        CHECK_EQ(integer_of(rig.ecu, "spy.transport_rejected"), 1);
        check_sum(rig, spy);
    }
    {
        // A broadcast the network dropped while assembling it: a packet missing.
        Rig rig;
        Spy& spy = make_spy(rig);
        const Bytes message = payload(20);
        rig.inject(at(1ms), bam(kA, 20, kPgnPropB));
        rig.inject(at(2ms), dt(kA, kGlobal, 1, message));
        rig.inject(at(3ms), dt(kA, kGlobal, 3, message));
        rig.bench.run_until(at(20ms));
        CHECK_EQ(integer_of(rig.ecu, "spy.transport_bam_dropped"), 1);
        check_sum(rig, spy);
    }
}

TEST_CASE(statistics_as_names_the_counters_or_leaves_them_out)
{
    {
        Rig rig;
        static_cast<void>(make_spy(rig, {.statistics_as = "bus.heard"}));
        rig.bench.run_until(at(10ms) + 1ns);
        CHECK_EQ(integer_of(rig.ecu, "bus.heard.missed"), 0);
        CHECK(!has_variable(rig.ecu, "spy.missed"));
    }
    {
        // Two Spies on one Listener: the second one publishes no counters.
        Rig rig;
        Listener& listener = rig.ecu.emplace<Listener>(rig.network);
        rig.ecu.emplace<Spy>(rig.ecu, rig.network, listener);
        Spy& second = rig.ecu.emplace<Spy>(rig.ecu, rig.network, listener, SpyOptions{.statistics_as = ""});
        second.add<BlockWatch>("spy", kA); // the name "spy" is free in the second one
        rig.inject(at(1ms), frame(6, kPgnPropA, kA, kB, {1}));
        rig.bench.run_until(at(10ms) + 1ns);
        CHECK_EQ(integer_of(rig.ecu, "spy.missed"), 0);
        CHECK_EQ(integer_of(rig.ecu, "spy.count"), 1);
        CHECK(&second.listener() == &listener);
    }
}

TEST_CASE(errors_say_what_is_wrong_and_how_to_fix_it)
{
    struct Named
    {
        Named(Scope& s, std::string_view item) { s.output(item, &value); }
        int value = 0;
    };
    struct Empty
    {
        explicit Empty(Scope&) {}
    };
    Rig rig;
    Spy& spy = make_spy(rig);
    spy.add<Empty>("block");
    CHECK_MESSAGE(message_of<std::invalid_argument>([&] { spy.add<Empty>("block"); }),
                  "vehicle_sil::spy::Spy::add() \"block\": the name is used already, by an earlier add(): give each "
                  "watcher a name of its own");
    CHECK_MESSAGE(message_of<std::invalid_argument>([&] { spy.add<Empty>(""); }),
                  "vehicle_sil::spy::Spy::add() \"\": the watcher's variables are \".<item>\", so give a name that is "
                  "not empty and neither starts nor ends with '.' (e.g. \"block\" or \"brake.block\")");
    CHECK(throws_with<std::invalid_argument>([&] { spy.add<Empty>(".block"); }, "neither starts nor ends with '.'"));
    CHECK(throws_with<std::invalid_argument>([&] { spy.add<Empty>("block."); }, "neither starts nor ends with '.'"));
    CHECK_MESSAGE(message_of<std::invalid_argument>([&] { spy.add<Empty>("spy"); }),
                  "vehicle_sil::spy::Spy::add() \"spy\": the name is the prefix of the Spy's own counters "
                  "(SpyOptions::statistics_as): give the watcher another name, or the Spy another statistics_as");
    CHECK_MESSAGE(message_of<std::invalid_argument>([&] { spy.add<Named>("named", ""); }),
                  "vehicle_sil::spy::Scope \"named\": output() with the name \"\": the variable is \"named.<name>\", so "
                  "give a name that is not empty and neither starts nor ends with '.' (e.g. \"count\")");
    CHECK(throws_with<std::invalid_argument>([&] { spy.add<Named>("dotted", ".x"); }, "output() with the name \".x\""));
    struct EmptyStep
    {
        explicit EmptyStep(Scope& s) { s.on_step({}); }
    };
    CHECK_MESSAGE(message_of<std::invalid_argument>([&] { spy.add<EmptyStep>("empty_step"); }),
                  "vehicle_sil::spy::Scope \"empty_step\": on_step() with an empty function: give the function to run "
                  "in every step");
    CHECK(throws_with<std::invalid_argument>([] {
        Rig other;
        Spy s(other.ecu, other.network, {.statistics_as = "spy."});
    }, "vehicle_sil::spy::Spy: SpyOptions::statistics_as \"spy.\" starts or ends with '.': the counters are "
       "\"spy..missed\" and so on, so give a prefix such as \"spy\" (or \"\" for none)"));
    {
        // A second Spy on the same ECU with the same counters, and one that makes a second Listener.
        Rig other;
        Listener& listener = other.ecu.emplace<Listener>(other.network);
        other.ecu.emplace<Spy>(other.ecu, other.network, listener);
        CHECK(throws_with<std::invalid_argument>([&] { Spy s(other.ecu, other.network, listener); },
                                                 "vehicle_sil::spy::Spy: cannot publish what it missed as \"spy.*\": "));
        CHECK(throws_with<std::invalid_argument>([&] { Spy s(other.ecu, other.network, listener); },
                                                 ". For two Spies on one ECU, give the second one "
                                                 "SpyOptions::statistics_as = \"\" (or another prefix)"));
        CHECK(throws_with<std::logic_error>([&] { Spy s(other.ecu, other.network, {.statistics_as = ""}); },
                                            ". To hear through that Listener, give it to the Spy: Spy(ecu, network, "
                                            "listener)"));
    }
    // After the run started.
    struct Late
    {
        explicit Late(Scope& s) : scope(s) {}
        Scope& scope;
    };
    Late& late = spy.add<Late>("late");
    rig.bench.run_until(at(10ms) + 1ns);
    CHECK_MESSAGE(message_of<std::logic_error>([&] { spy.add<Empty>("later"); }),
                  "vehicle_sil::spy::Spy::add() after the run started: add the watchers in the ECU class's constructor");
    CHECK_MESSAGE(message_of<std::logic_error>([&] { late.scope.on_any_message([](const Message&) {}); }),
                  "vehicle_sil::spy::Scope \"late\": on_any_message() after the run started: register the handlers and "
                  "variables in the watcher's constructor (Spy::add() makes the watcher before the run)");
    CHECK(throws_with<std::logic_error>([&] { late.scope.on_message(vehicle_sil::j1939::Pgn{kPgnPropA}, [](const Message&) {}); },
                                        "on_message() after the run started"));
    CHECK(throws_with<std::logic_error>([&] { late.scope.on_transfer([](const SniffedTransfer&) {}); },
                                        "on_transfer() after the run started"));
    CHECK(throws_with<std::logic_error>([&] { late.scope.on_frame([](const ReceivedFrame&) {}); },
                                        "on_frame() after the run started"));
    CHECK(throws_with<std::logic_error>([&] { late.scope.on_step([](SimTime) {}); }, "on_step() after the run started"));
    const int x = 0;
    CHECK(throws_with<std::logic_error>([&] { late.scope.output("x", &x); }, "output() after the run started"));
    CHECK_MESSAGE(message_of<std::logic_error>([&] { Spy s(rig.ecu, rig.network); }),
                  "vehicle_sil::spy::Spy made after the run started: make the Spy in the ECU class's constructor (a "
                  "member of the class that ecu_main() or Simulation::add() makes)");
    CHECK(!rig.network.failed());
}

namespace {

/// Counts the reads of its variable through a pointer that outlives it (the getter stays with
/// the ECU's agent when the watcher goes).
struct Counted
{
    Counted(Scope& s, int* reads)
    {
        s.output("reads", [reads] {
            ++*reads;
            return *reads;
        });
    }
};

/// A Spy with a watcher that publishes a variable, destroyed before its ECU runs (a SIL Kit
/// simulation runs once): the first step says so, and the variable is never read.
void check_spy_gone_before_its_ecu(std::optional<vehicle_sil::SilKitOptions> silkit)
{
    vehicle_sil::Simulation sim(std::move(silkit));
    Ecu& ecu = sim.add_ecu("Spy", 10ms);
    Network& network = ecu.j1939(kBus);
    int reads = 0;
    auto spy = std::make_unique<Spy>(ecu, network, SpyOptions{.statistics_as = ""});
    spy->add<Counted>("counted", &reads);
    spy.reset();
    CHECK(throws_with<vehicle_sil::SimulationError>(
        [&] { sim.run_for(30ms); },
        "Spy: vehicle_sil::spy::Spy was destroyed before its ECU, but its variables (\"counted.*\") are still read: "
        "make the Spy a member of the ECU class"));
    CHECK_EQ(reads, 0);
}

} // namespace

TEST_CASE(spy_with_variables_gone_before_its_ecu_stops_the_next_step)
{
    check_spy_gone_before_its_ecu(std::nullopt);
}

TEST_CASE(spy_with_variables_gone_before_its_ecu_stops_the_next_step_on_silkit)
{
    check_spy_gone_before_its_ecu(vehicle_sil::SilKitOptions{});
}

TEST_CASE(spy_with_counters_gone_before_its_ecu_names_them)
{
    Rig rig;
    auto spy = std::make_unique<Spy>(rig.ecu, rig.network);
    rig.bench.run_until(at(10ms) + 1ns);
    spy.reset();
    CHECK(throws_with<std::logic_error>([&] { rig.bench.run_until(at(30ms)); }, "its variables (\"spy.*\") are still read"));
}

TEST_CASE(watcher_whose_constructor_throws_after_publishing_stops_the_next_step)
{
    struct Published
    {
        explicit Published(Scope& s)
        {
            s.output("value", &value);
            throw std::runtime_error("bad settings");
        }
        int value = 0;
    };
    Rig rig;
    Spy& spy = make_spy(rig, {.statistics_as = ""});
    CHECK(throws_with<std::runtime_error>([&] { spy.add<Published>("published"); }, "bad settings"));
    CHECK(throws_with<std::logic_error>([&] { rig.bench.run_until(at(10ms) + 1ns); },
                                        "vehicle_sil::spy::Spy::add() \"published\": the watcher's constructor threw "
                                        "after variables were published that refer to it"));
}

namespace {

/// A watcher made of two others, added from its constructor.
struct Pair
{
    Pair(Scope& s, Spy& spy, std::vector<std::string>& log, bool throw_after) : log_(log)
    {
        command = &spy.add<Logger>(s.name() + ".command", log);
        answer = &spy.add<Logger>(s.name() + ".answer", log);
        s.on_any_message([this](const Message&) { log_.push_back("pair"); });
        if (throw_after)
        {
            throw std::runtime_error("pair failed");
        }
    }
    ~Pair() { log_.push_back("pair destroyed"); }
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;
    std::vector<std::string>& log_;
    Logger* command = nullptr;
    Logger* answer = nullptr;
};

/// Adds a watcher whose constructor throws, catches that and goes on.
struct Tolerant
{
    Tolerant(Scope& s, Spy& spy, std::vector<std::string>& log)
    {
        struct Failing
        {
            Failing(Scope& inner, std::vector<std::string>& l)
            {
                inner.on_any_message([&l](const Message&) { l.push_back("failing heard"); });
                throw std::runtime_error("inner failed");
            }
        };
        CHECK(throws_with<std::runtime_error>([&] { spy.add<Failing>(s.name() + ".inner", log); }, "inner failed"));
        spy.add<Logger>(s.name() + ".inner", log); // the name is free again
    }
};

} // namespace

TEST_CASE(watcher_may_add_watchers_from_its_constructor)
{
    std::vector<std::string> log;
    {
        Rig rig;
        auto spy = std::make_unique<Spy>(rig.ecu, rig.network, SpyOptions{.statistics_as = ""});
        spy->add<Logger>("before", log);
        Pair& pair = spy->add<Pair>("pair", *spy, log, false);
        spy->add<Logger>("after", log);
        CHECK(pair.command != nullptr && pair.answer != nullptr);
        rig.inject(at(1ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
        rig.bench.run_until(at(10ms) + 1ns);
        // Handlers in the order registered.
        const std::vector<std::string> heard{"before 65296", "pair.command 65296", "pair.answer 65296", "pair",
                                             "after 65296"};
        CHECK(log == heard);
        log.clear();
        spy.reset();
        // The one whose add() returned last first: the pair before the watchers it is made of.
        const std::vector<std::string> destroyed{"after destroyed", "pair destroyed", "pair.answer destroyed",
                                                 "pair.command destroyed", "before destroyed"};
        CHECK(log == destroyed);
    }
    {
        // The outer constructor throws: the watchers it added go with it, the others stay.
        log.clear();
        Rig rig;
        Spy& spy = make_spy(rig, {.statistics_as = ""});
        spy.add<Logger>("before", log);
        CHECK(throws_with<std::runtime_error>([&] { spy.add<Pair>("pair", spy, log, true); }, "pair failed"));
        const std::vector<std::string> destroyed{"pair.answer destroyed", "pair.command destroyed"};
        CHECK(log == destroyed);
        spy.add<Pair>("pair", spy, log, false); // every name is free again
        log.clear();
        rig.inject(at(1ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
        rig.bench.run_until(at(10ms) + 1ns);
        const std::vector<std::string> heard{"before 65296", "pair.command 65296", "pair.answer 65296", "pair"};
        CHECK(log == heard);
        log.clear(); // the ECU destroys the watchers after this test's checks
    }
    {
        // An inner constructor throws and the outer one goes on.
        log.clear();
        Rig rig;
        Spy& spy = make_spy(rig, {.statistics_as = ""});
        spy.add<Tolerant>("tolerant", spy, log);
        rig.inject(at(1ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
        rig.bench.run_until(at(10ms) + 1ns);
        const std::vector<std::string> heard{"tolerant.inner 65296"};
        CHECK(log == heard);
        CHECK(!rig.network.failed());
        log.clear();
    }
}

TEST_CASE(step_functions_run_after_the_handlers_in_the_order_registered)
{
    struct Stepper
    {
        Stepper(Scope& s, std::vector<std::string>& log) : log_(log), scope_(s)
        {
            s.on_any_message([this](const Message& m) { log_.push_back(scope_.name() + " heard " + std::to_string(to_ns(m.time) / 1000000)); });
            s.on_step([this](SimTime now) {
                log_.push_back(scope_.name() + " step " + std::to_string(to_ns(now) / 1000000) + " now " +
                               std::to_string(to_ns(scope_.now()) / 1000000));
            });
        }
        std::vector<std::string>& log_;
        Scope& scope_;
    };
    std::vector<std::string> log;
    Rig rig;
    auto spy = std::make_unique<Spy>(rig.ecu, rig.network, SpyOptions{.statistics_as = ""});
    spy->add<Stepper>("a", log);
    spy->add<Stepper>("b", log);
    rig.inject(at(5ms), frame(6, kPgnPropB, kA, kGlobal, {1}));
    rig.bench.run_until(at(10ms) + 1ns);
    const std::vector<std::string> expected{"a step 0 now 0",  "b step 0 now 0",  "a heard 5",
                                            "b heard 5",       "a step 10 now 10", "b step 10 now 10"};
    CHECK(log == expected);
    // Gone with the Spy (it published no variable): the steps go on without them.
    spy.reset();
    log.clear();
    rig.bench.run_until(at(30ms));
    CHECK(log.empty());
}

TEST_CASE(missed_counts_a_broadcast_dropped_while_assembled)
{
    Rig rig;
    Spy& spy = make_spy(rig);
    const Bytes message = payload(20);
    rig.inject(at(1ms), bam(kA, 20, kPgnPropB));
    rig.inject(at(2ms), dt(kA, kGlobal, 1, message));
    rig.inject(at(3ms), dt(kA, kGlobal, 3, message)); // packet 2 missing
    rig.bench.run_until(at(20ms));
    CHECK_EQ(spy.missed(), 1u);
    CHECK_EQ(integer_of(rig.ecu, "spy.missed"), 1);
}

namespace {

/// What the receiving ECU recorded and the talking ECU's statistics, with or without a Spy.
struct Heard
{
    std::vector<std::string> records;
    std::uint64_t talker_frames_sent = 0;
    std::uint64_t spy_frames_sent = 0;
    std::int64_t spy_count = -1;
};

Heard run_talk(bool with_spy, SimDuration spy_step)
{
    using vehicle_sil::j1939::Node;
    LocalBench bench;
    Ecu& talker = bench.add_ecu("Talker", 10ms);
    Network& talker_network = talker.j1939(kBus);
    Node& a = talker_network.add_node(NodeOptions{.address = kA});
    Ecu& receiver = bench.add_ecu("Receiver", 10ms);
    Node& b = receiver.j1939(kBus).add_node(NodeOptions{.address = kB});
    Heard heard;
    b.on_any_message([&](const Message& m) {
        heard.records.push_back(std::to_string(to_ns(m.time)) + " " + std::to_string(m.pgn.value()) + " " +
                                std::to_string(m.data.size()));
    });
    talker.every(20ms, [&](SimTime now) {
        if (now >= at(300ms)) // the last connection ends well before the run
        {
            return;
        }
        static_cast<void>(a.send(vehicle_sil::j1939::Pgn{kPgnPropA}, payload(now < at(100ms) ? 3 : 20), kB));
    });
    Spy* spy = nullptr;
    Network* spy_network = nullptr;
    Ecu* listening = nullptr;
    if (with_spy)
    {
        listening = &bench.add_ecu("Spy", spy_step);
        spy_network = &listening->j1939(kBus);
        spy = &listening->emplace<Spy>(*listening, *spy_network);
        spy->add<BlockWatch>("block", kA);
    }
    bench.run_until(at(500ms));
    heard.talker_frames_sent = talker_network.statistics().frames_sent;
    if (with_spy)
    {
        heard.spy_frames_sent = spy_network->statistics().frames_sent;
        heard.spy_count = integer_of(*listening, "block.count");
        CHECK_EQ(spy->missed(), 0u);
    }
    return heard;
}

} // namespace

TEST_CASE(spy_changes_nothing_on_the_bus_and_sends_nothing)
{
    const Heard without = run_talk(false, 10ms);
    std::size_t long_ones = 0;
    for (const auto& r : without.records)
    {
        long_ones += r.ends_with(" 20") ? 1u : 0u;
    }
    CHECK(without.records.size() >= 8u);
    CHECK(long_ones >= 3u); // connections (RTS/CTS) too
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{10ms}, SimDuration{25ms}})
    {
        const Heard with = run_talk(true, step);
        CHECK(with.records == without.records);
        CHECK_EQ(with.talker_frames_sent, without.talker_frames_sent);
        CHECK_EQ(with.spy_frames_sent, 0u);
        CHECK_EQ(with.spy_count, static_cast<std::int64_t>(without.records.size()));
    }
}

// The example of docs/j1939-guide.md ("Listen-only participants"), as written there but main().
namespace guide {

namespace j1939 = vehicle_sil::j1939;
using namespace std::chrono_literals;

namespace cal {
inline constexpr j1939::Pgn kPgn{0xEF00}; // Proprietary A
}

class BlockWatch {                                   // a watcher: takes Scope& and its own settings; no base class, creates no Listener
public:
    BlockWatch(vehicle_sil::spy::Scope& s, j1939::Address from) {
        s.on_message(cal::kPgn, [this, from](const j1939::Message& m) {   // the Scope keeps the registration (no removal to write)
            if (m.source != from || m.time < latest_) { return; }        // the order of delivery can vary: compare times to find the newest
            latest_ = m.time; ++count_; bytes_ = static_cast<int>(m.data.size());
        });
        s.output("count", &count_); s.output("bytes", &bytes_, {.unit = "bytes"});   // becomes "block.count"
    }
private: vehicle_sil::SimTime latest_{}; std::int64_t count_ = 0; int bytes_ = 0;
};

class CalSpy {
public:
    explicit CalSpy(vehicle_sil::Ecu& ecu) : spy_(ecu, ecu.j1939("J1939_CAN")) {    // no node: sends nothing
        spy_.add<BlockWatch>("block", j1939::Address{0x21});                        // block.count, block.bytes
    }                                                                               // the Spy writes spy.missed and its breakdown
private: vehicle_sil::spy::Spy spy_;
};

} // namespace guide

TEST_CASE(guide_example_publishes_what_it_hears)
{
    LocalBench bench;
    Ecu& ecu = bench.add_ecu("CalSpy", 5ms);
    ecu.emplace<guide::CalSpy>(ecu);
    const auto inject = [&](SimTime t, const J1939Frame& f) { CHECK(bench.inject(kBus, t, to_can(f)) == vehicle_sil::can::SendResult::Accepted); };
    inject(at(1ms), frame(6, kPgnPropA, kA, kB, {1, 2, 3, 4}));
    inject(at(2ms), frame(6, kPgnPropA, kC, kB, {1})); // from another ECU: not counted
    const Bytes message = payload(20);
    SimTime t = at(10ms);
    inject(t, rts(kA, kB, 20, kPgnPropA));
    inject(t += 1ms, cts(kB, kA, 3, 1, kPgnPropA));
    for (const auto& f : dts(kA, kB, message, 1, 3))
    {
        inject(t += 1ms, f);
    }
    inject(t += 1ms, eoma(kB, kA, 20, kPgnPropA));
    bench.run_until(at(50ms));
    CHECK_EQ(integer_of(ecu, "block.count"), 2);
    CHECK_EQ(integer_of(ecu, "block.bytes"), 20);
    CHECK(info_of(ecu, "block.bytes").unit == "bytes");
    CHECK_EQ(integer_of(ecu, "spy.missed"), 0);
    CHECK_EQ(integer_of(ecu, "spy.no_session"), 0);
}
